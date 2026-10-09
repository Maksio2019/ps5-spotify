/*
 * Spotify Connect mDNS helper payload.
 *
 * The app's sandbox refuses fixed listening ports, and mDNS needs UDP 5353.
 * This payload (run with Payload Manager, like ftpsrv) announces the speaker
 * over mDNS while the app runs. The app writes its Zeroconf HTTP port and
 * device name to its /download0/zeroconf.port; this payload reads it through
 * the sandbox mount and points phones at that port. It does nothing else:
 * no Spotify traffic or audio passes through it.
 *
 * Build: make -C payload (uses the template's pinned Payload SDK).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mdnssvc.h"

#define TITLE_ID "PPSA99777"
#define PORT_FILE "/mnt/sandbox/" TITLE_ID "_000/download0/zeroconf.port"
#define SERVICE_TYPE "_spotify-connect._tcp.local"

typedef struct notify_request
{
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *format, ...)
{
    notify_request_t request;
    va_list arguments;
    memset(&request, 0, sizeof request);
    va_start(arguments, format);
    vsnprintf(request.message, sizeof request.message, format, arguments);
    va_end(arguments);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

/* The address the console uses to reach the internet (no packet is sent). */
static int lan_address(struct in_addr *address)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in remote = {0};
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

/* "<port>\n<device name>\n"; returns the port, 0 when absent or invalid. */
static int read_port_file(char *name, size_t name_size)
{
    FILE *file = fopen(PORT_FILE, "r");
    if (file == NULL)
        return 0;
    int port = 0;
    char line[128];
    if (fgets(line, sizeof line, file) != NULL)
        port = atoi(line);
    if (fgets(name, (int)name_size, file) == NULL)
        name[0] = '\0';
    fclose(file);
    name[strcspn(name, "\r\n")] = '\0';
    if (port <= 0 || port > 65535 || name[0] == '\0')
        return 0;
    return port;
}

int main(void)
{
    struct in_addr host;
    while (lan_address(&host) != 0)
        sleep(2);

    struct mdnsd *server = mdnsd_start(host, false);
    if (server == NULL)
    {
        notify("Spotify mDNS helper: cannot open UDP 5353");
        return 1;
    }
    mdnsd_set_hostname(server, "ps5-speaker.local", host);
    notify("Spotify mDNS helper running on %s", inet_ntoa(host));

    const char *txt[] = {"VERSION=1.0", "CPath=/spotify_info", "Stack=SP", NULL};
    struct mdns_service *service = NULL;
    int announced_port = 0;
    char announced_name[64] = "";

    for (;;)
    {
        char name[64];
        int port = read_port_file(name, sizeof name);
        if (port != announced_port || strcmp(name, announced_name) != 0)
        {
            if (service != NULL)
            {
                mdns_service_remove(server, service);
                service = NULL;
            }
            if (port != 0)
            {
                service = mdnsd_register_svc(server, name, SERVICE_TYPE, (unsigned short)port,
                                             NULL, txt);
                notify("Spotify: announcing \"%s\" (port %d)", name, port);
            }
            announced_port = service != NULL ? port : 0;
            strcpy(announced_name, service != NULL ? name : "");
        }
        sleep(2);
    }
}
