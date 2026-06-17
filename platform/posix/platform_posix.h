#ifndef PLATFORM_POSIX_H
#define PLATFORM_POSIX_H

#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

/* --- Mutex --- */
typedef pthread_mutex_t  plat_mutex_t;
#define PLAT_MUTEX_DEFINE(name)   static pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define plat_mutex_lock(m)        pthread_mutex_lock(m)
#define plat_mutex_unlock(m)      pthread_mutex_unlock(m)

/* --- Thread --- */
typedef pthread_t  plat_thread_t;
struct client;
void plat_thread_spawn(struct client *c);

/* --- Time --- */
int64_t plat_uptime_ms(void);

/* --- Sockets (BSD names) --- */
#define plat_socket      socket
#define plat_setsockopt  setsockopt
#define plat_bind        bind
#define plat_listen      listen
#define plat_accept      accept
#define plat_connect     connect
#define plat_sendto      sendto
#define plat_recv        recv
#define plat_recvfrom    recvfrom
#define plat_close       close

static inline ssize_t plat_send(int fd, const void *buf, size_t len, int flags)
{
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return send(fd, buf, len, flags);
}

/* --- Logging --- */
#define LOG_MODULE_REGISTER(name, level)  /* no-op */
#define LOG_INF(fmt, ...)  printf("[INF] " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...)  fprintf(stderr, "[WRN] " fmt "\n", ##__VA_ARGS__)
#ifdef PLATFORM_POSIX_ENABLE_DBG
#define LOG_DBG(fmt, ...)  fprintf(stderr, "[DBG] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(fmt, ...)  do { } while (0)
#endif
#define LOG_ERR(fmt, ...)  fprintf(stderr, "[ERR] " fmt "\n", ##__VA_ARGS__)

/* --- Misc --- */
#define ARG_UNUSED(x)   (void)(x)
#define LOG_LEVEL_INF   3
#define LOG_LEVEL_DBG   4

#endif /* PLATFORM_POSIX_H */
