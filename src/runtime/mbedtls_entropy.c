/*
 * mbedTLS entropy for the title (MBEDTLS_ENTROPY_HARDWARE_ALT, set in
 * tooling/cmake/ps5-title.cmake): /dev/urandom, or the console's sceRandom
 * when the sandbox does not let the title open it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

extern int sceRandomGetRandomNumber(void *buffer, size_t size);

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length, size_t *produced)
{
    static int reported;
    (void)data;
    *produced = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0)
    {
        ssize_t got = read(fd, output, length);
        close(fd);
        if (got > 0)
        {
            if (!reported++)
                printf("[entropy] /dev/urandom\n");
            *produced = (size_t)got;
            return 0;
        }
    }
    /* At most 64 bytes per call. */
    size_t done = 0;
    while (done < length)
    {
        size_t chunk = length - done > 64 ? 64 : length - done;
        int result = sceRandomGetRandomNumber(output + done, chunk);
        if (result < 0)
        {
            printf("[entropy] sceRandomGetRandomNumber -> 0x%08x\n", (unsigned)result);
            return -1;
        }
        done += chunk;
    }
    if (!reported++)
        printf("[entropy] sceRandom\n");
    *produced = length;
    return 0;
}
