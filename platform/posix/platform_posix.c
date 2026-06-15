#include "platform_posix.h"
#include "client.h"
#include <time.h>

int64_t plat_uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *thread_wrapper(void *arg)
{
    client_thread_fn(arg, NULL, NULL);
    return NULL;
}

void plat_thread_spawn(struct client *c)
{
    pthread_create(&c->thread, NULL, thread_wrapper, c);
    pthread_detach(c->thread);
}
