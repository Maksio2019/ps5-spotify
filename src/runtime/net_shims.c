/*
 * Name lookup for a native title. The SDK binds getaddrinfo to
 * libScePosixForWebKit, which a title does not load (a null import), so
 * define it here on top of sceNetResolver. IPv4, one address per name.
 * From ps5-native-app-boilerplate's examples/update-check console_curl.c
 * (as vendored by SymphonyStation5), hardware-proven there.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

extern int sceNetPoolCreate(const char *name, int size, int flags);
extern int sceNetPoolDestroy(int pool);
extern int sceNetResolverCreate(const char *name, int pool, int flags);
extern int sceNetResolverStartNtoa(int resolver, const char *hostname, struct in_addr *address,
                                   int timeout_us, int retries, int flags);
extern int sceNetResolverDestroy(int resolver);

/* IPv4, one address per name, through sceNetResolver on the calling thread. */
static int title_dns_lookup(const char *name, struct in_addr *address)
{
    if (inet_pton(AF_INET, name, address) == 1)
        return 0;
    const int pool = sceNetPoolCreate("title_dns", 16 * 1024, 0);
    if (pool < 0)
        return EAI_MEMORY;
    int result = EAI_FAIL;
    const int resolver = sceNetResolverCreate("title_dns", pool, 0);
    if (resolver >= 0)
    {
        /* 5 s per try, 2 retries. */
        result =
            sceNetResolverStartNtoa(resolver, name, address, 5000000, 2, 0) < 0 ? EAI_NONAME : 0;
        (void)sceNetResolverDestroy(resolver);
    }
    (void)sceNetPoolDestroy(pool);
    return result;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    *result = NULL;
    const int family = hints != NULL ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;

    struct in_addr address;
    address.s_addr = htonl(INADDR_LOOPBACK);
    if (node != NULL)
    {
        if (hints != NULL && (hints->ai_flags & AI_NUMERICHOST) != 0)
        {
            if (inet_pton(AF_INET, node, &address) != 1)
                return EAI_NONAME;
        }
        else
        {
            const int failed = title_dns_lookup(node, &address);
            if (failed != 0)
                return failed;
        }
    }
    else if (hints != NULL && (hints->ai_flags & AI_PASSIVE) != 0)
    {
        address.s_addr = htonl(INADDR_ANY);
    }

    /* The entry and its address in one allocation: freeaddrinfo frees once. */
    struct addrinfo *entry = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
    if (entry == NULL)
        return EAI_MEMORY;
    struct sockaddr_in *in = (struct sockaddr_in *)(entry + 1);
    in->sin_len = sizeof(*in);
    in->sin_family = AF_INET;
    in->sin_port = htons((uint16_t)(service != NULL ? atoi(service) : 0));
    in->sin_addr = address;
    entry->ai_family = AF_INET;
    entry->ai_socktype =
        hints != NULL && hints->ai_socktype != 0 ? hints->ai_socktype : SOCK_STREAM;
    entry->ai_protocol = hints != NULL ? hints->ai_protocol : 0;
    entry->ai_addrlen = sizeof(*in);
    entry->ai_addr = (struct sockaddr *)in;
    *result = entry;
    return 0;
}

void freeaddrinfo(struct addrinfo *entry)
{
    while (entry != NULL)
    {
        struct addrinfo *next = entry->ai_next;
        free(entry);
        entry = next;
    }
}

const char *gai_strerror(int code)
{
    switch (code)
    {
    case 0:
        return "no error";
    case EAI_NONAME:
        return "the name was not found";
    case EAI_FAMILY:
        return "address family not supported";
    case EAI_MEMORY:
        return "out of memory";
    default:
        return "the name lookup failed";
    }
}


/* Numeric only: enough for civetweb's record of the connected address. */
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, size_t host_size,
                char *service, size_t service_size, int flags)
{
    (void)flags;
    if (address == NULL || address->sa_family != AF_INET || length < sizeof(struct sockaddr_in))
        return EAI_FAMILY;
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    if (host != NULL && host_size != 0 &&
        inet_ntop(AF_INET, &in->sin_addr, host, (socklen_t)host_size) == NULL)
        return EAI_FAIL;
    if (service != NULL && service_size != 0)
        snprintf(service, service_size, "%u", (unsigned)ntohs(in->sin_port));
    return 0;
}

/* IPv4, one address; not thread-safe, like the original. */
struct hostent *gethostbyname(const char *name)
{
    static struct in_addr address;
    static char *addresses[2];
    static char *aliases[1];
    static struct hostent entry;
    if (title_dns_lookup(name, &address) != 0)
        return NULL;
    addresses[0] = (char *)&address;
    addresses[1] = NULL;
    aliases[0] = NULL;
    entry.h_name = (char *)name;
    entry.h_aliases = aliases;
    entry.h_addrtype = AF_INET;
    entry.h_length = sizeof address;
    entry.h_addr_list = addresses;
    return &entry;
}

/* ---- Interface address (bell's mDNS asks getifaddrs for the LAN address) --- */

#include <ifaddrs.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>

/* The address the console would use to reach the internet: connect() on a UDP
 * socket picks a route without sending anything. */
static int title_lan_address(struct in_addr *address)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in remote;
    memset(&remote, 0, sizeof remote);
    remote.sin_len = sizeof remote;
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x08080808);
    struct sockaddr_in local;
    socklen_t length = sizeof local;
    int result = -1;
    if (connect(fd, (struct sockaddr *)&remote, sizeof remote) == 0 &&
        getsockname(fd, (struct sockaddr *)&local, &length) == 0 &&
        local.sin_addr.s_addr != htonl(INADDR_ANY))
    {
        *address = local.sin_addr;
        result = 0;
    }
    close(fd);
    return result;
}

/* One entry: the LAN address, up and multicast-capable. */
int getifaddrs(struct ifaddrs **result)
{
    struct in_addr address;
    if (title_lan_address(&address) != 0)
        return -1;
    struct
    {
        struct ifaddrs entry;
        struct sockaddr_in address;
        char name[8];
    } *block = calloc(1, sizeof(*block));
    if (block == NULL)
        return -1;
    block->address.sin_len = sizeof(block->address);
    block->address.sin_family = AF_INET;
    block->address.sin_addr = address;
    strcpy(block->name, "lan0");
    block->entry.ifa_name = block->name;
    block->entry.ifa_flags = IFF_UP | IFF_RUNNING | IFF_MULTICAST;
    block->entry.ifa_addr = (struct sockaddr *)&block->address;
    *result = &block->entry;
    return 0;
}

void freeifaddrs(struct ifaddrs *entries)
{
    free(entries);
}
