/*
 * Threads created without attributes (bell::Task, std::thread) get the
 * system's default stack, which is small for TLS handshakes and Vorbis
 * decoding. Linked with --wrap=pthread_create (APP_WRAP_SYMBOLS in the
 * Makefile), so every pthread_create call comes here first.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stddef.h>

#define TITLE_THREAD_STACK (512 * 1024)

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*start)(void *), void *argument);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*start)(void *), void *argument)
{
    if (attributes != NULL)
        return __real_pthread_create(thread, attributes, start, argument);
    pthread_attr_t sized;
    if (pthread_attr_init(&sized) != 0)
        return __real_pthread_create(thread, NULL, start, argument);
    pthread_attr_setstacksize(&sized, TITLE_THREAD_STACK);
    int result = __real_pthread_create(thread, &sized, start, argument);
    pthread_attr_destroy(&sized);
    return result;
}
