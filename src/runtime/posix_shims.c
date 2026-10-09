/*
 * POSIX entry points civetweb references but the title's libc lacks. The
 * console has no user database and the app never changes identity.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <pwd.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

struct passwd *getpwnam(const char *name)
{
    (void)name;
    return NULL;
}

int setgid(gid_t group)
{
    (void)group;
    errno = EPERM;
    return -1;
}

/* uname() is an inline wrapper around this: five fields of namesize bytes. */
int __xuname(int namesize, void *namebuf)
{
    static const char *const fields[] = {"PS5", "ps5", "1", "1", "amd64"};
    char *out = namebuf;
    for (int i = 0; i < 5; i++)
    {
        strncpy(out + i * namesize, fields[i], (size_t)namesize - 1);
        out[i * namesize + namesize - 1] = '\0';
    }
    return 0;
}
