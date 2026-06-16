/*
 * keepalive_client — connects with a short keepalive then goes idle.
 * Used by scripts/test_keepalive.sh to verify the broker enforces
 * keepalive timeout (1.5 * keepalive per MQTT 3.1.1 §3.1.2.10).
 *
 * Usage:
 *   keepalive_client -k KEEPALIVE_SECS [-h HOST] [-p PORT]
 *
 * Exit code:
 *   0 — connection was closed by the broker (expected)
 *   1 — error or connection stayed open past the deadline
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "include/packet.h"

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int tcp_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int send_all(int fd, const uint8_t *b, size_t n)
{
    while (n > 0) {
        ssize_t r = send(fd, b, n, 0);
        if (r <= 0) return -1;
        b += r; n -= (size_t)r;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int         port = 1883;
    int         ka   = 5; /* seconds */

    int opt;
    while ((opt = getopt(argc, argv, "h:p:k:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'k': ka   = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-h HOST] [-p PORT] -k KEEPALIVE\n",
                    argv[0]);
            return 1;
        }
    }

    int fd = tcp_connect(host, port);
    if (fd < 0) { perror("connect"); return 1; }

    /* CONNECT with configurable keepalive, no will, no auth */
    char cid[32];
    snprintf(cid, sizeof(cid), "ka_cli_%d", (int)getpid());
    uint16_t id_len = (uint16_t)strlen(cid);

    uint32_t rem_len = 10 + 2 + id_len;
    uint8_t  rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes);

    uint8_t buf[256];
    size_t  p = 0;
    buf[p++] = MQTT_CONNECT;
    memcpy(buf + p, rem_enc, rem_bytes); p += rem_bytes;
    buf[p++] = 0; buf[p++] = 4;
    buf[p++] = 'M'; buf[p++] = 'Q'; buf[p++] = 'T'; buf[p++] = 'T';
    buf[p++] = 0x04;          /* protocol level 3.1.1 */
    buf[p++] = 0x02;          /* clean session */
    buf[p++] = (uint8_t)(ka >> 8);
    buf[p++] = (uint8_t)(ka & 0xFF);
    buf[p++] = (uint8_t)(id_len >> 8);
    buf[p++] = (uint8_t)(id_len & 0xFF);
    memcpy(buf + p, cid, id_len); p += id_len;

    if (send_all(fd, buf, p) < 0) { close(fd); return 1; }

    /* receive CONNACK */
    ssize_t r = recv(fd, buf, 4, MSG_WAITALL);
    if (r < 4 || (buf[0] & 0xF0) != MQTT_CONNACK || buf[3] != 0) {
        fprintf(stderr, "CONNACK rejected or error\n");
        close(fd); return 1;
    }

    printf("connected with keepalive=%ds, going idle...\n", ka);
    fflush(stdout);

    /* Go completely idle — don't send PINGREQ.
     * Broker should close the connection after 1.5 * keepalive seconds. */
    /* 4× budget: accept anything up to 4 × keepalive to tolerate slow hosts */
    int64_t max_ms = (int64_t)(ka * 4000);

    /* Set recv timeout at 4 × keepalive */
    struct timeval tv = { .tv_sec = ka * 4, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t byte;
    int64_t t0 = now_ms();
    r = recv(fd, &byte, 1, 0);
    int64_t elapsed = now_ms() - t0;

    if (r == 0 || r < 0) {
        /* Connection closed by broker — expected */
        int64_t expected_ms = (int64_t)(ka * 1500); /* 1.5 * keepalive */
        printf("broker closed connection after %lldms (expected ~%lldms)\n",
               (long long)elapsed, (long long)expected_ms);
        close(fd);
        /* Accept: lower = 0.5× expected, upper = 4× keepalive */
        if (elapsed >= expected_ms / 2 && elapsed <= max_ms) {
            return 0;
        }
        fprintf(stderr, "timing out of range (elapsed=%lldms max=%lldms)\n",
                (long long)elapsed, (long long)max_ms);
        return 1;
    }

    fprintf(stderr, "connection NOT closed by broker within %ds\n", ka * 4);
    close(fd);
    return 1;
}
