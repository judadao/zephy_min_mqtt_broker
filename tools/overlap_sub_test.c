/*
 * overlap_sub_test — subscribe to two overlapping topics with one connection,
 * count how many PUBLISH packets arrive, then print "received N\n".
 *
 * Usage: overlap_sub_test [HOST [PORT]]
 * Defaults: HOST=127.0.0.1  PORT=1883
 *
 * Steps:
 *   1. CONNECT (clean_session=1, id="overlap-test-client")
 *   2. SUBSCRIBE "overlap/#" (QoS 0, pkt_id=1)
 *   3. SUBSCRIBE "overlap/test" (QoS 0, pkt_id=2)
 *   4. Wait up to 2 s for PUBLISH packets, counting each one
 *   5. Print "received N\n"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "packet.h"

#define BUF 4096

static int g_fd = -1;

static int tcp_connect(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

static int send_all(int fd, const uint8_t *b, size_t n)
{
    size_t d = 0;
    while (d < n) {
        ssize_t r = send(fd, b + d, n - d, 0);
        if (r <= 0) return -1;
        d += (size_t)r;
    }
    return 0;
}

static int recv_all(int fd, uint8_t *b, size_t n)
{
    size_t d = 0;
    while (d < n) {
        ssize_t r = recv(fd, b + d, n - d, 0);
        if (r <= 0) return -1;
        d += (size_t)r;
    }
    return 0;
}

static int recv_pkt(int fd, uint8_t *type_out, uint8_t *body, uint32_t *body_len)
{
    uint8_t b;
    if (recv_all(fd, &b, 1) < 0) return -1;
    *type_out = b & 0xF0;

    uint32_t rlen = 0, mult = 1;
    do {
        if (recv_all(fd, &b, 1) < 0) return -1;
        rlen += (b & 0x7F) * mult;
        mult *= 128;
        if (mult > 128*128*128) return -1;
    } while (b & 0x80);

    *body_len = rlen;
    if (rlen > 0 && recv_all(fd, body, rlen) < 0) return -1;
    return 0;
}

static int do_subscribe(int fd, const char *topic, uint8_t qos, uint16_t pkt_id)
{
    uint16_t tlen = (uint16_t)strlen(topic);
    /* rem_len = 2 (pkt_id) + 2 (topic len) + tlen + 1 (qos) */
    uint32_t rem = 2u + 2u + tlen + 1u;
    uint8_t buf[BUF];
    size_t p = 0;
    buf[p++] = MQTT_SUBSCRIBE | 0x02;
    /* encode remaining length (single byte since rem < 128 for short topics) */
    uint8_t rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem, rem_enc, &rem_bytes);
    memcpy(buf + p, rem_enc, rem_bytes); p += rem_bytes;
    buf[p++] = (uint8_t)(pkt_id >> 8);
    buf[p++] = (uint8_t)(pkt_id & 0xFF);
    buf[p++] = (uint8_t)(tlen >> 8);
    buf[p++] = (uint8_t)(tlen & 0xFF);
    memcpy(buf + p, topic, tlen); p += tlen;
    buf[p++] = qos & 0x03;
    return send_all(fd, buf, p);
}

int main(int argc, char *argv[])
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port = (argc > 2) ? (uint16_t)atoi(argv[2]) : 1883;

    g_fd = tcp_connect(host, port);
    if (g_fd < 0) return 1;

    /* CONNECT: clean_session=1, id="overlap-test-client", keepalive=60 */
    const char *cid = "overlap-test-client";
    uint16_t cid_len = (uint16_t)strlen(cid);
    uint32_t rem = 10u + 2u + cid_len;
    uint8_t buf[BUF];
    size_t p = 0;
    buf[p++] = MQTT_CONNECT;
    uint8_t rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem, rem_enc, &rem_bytes);
    memcpy(buf + p, rem_enc, rem_bytes); p += rem_bytes;
    /* protocol name "MQTT" */
    buf[p++] = 0; buf[p++] = 4;
    buf[p++] = 'M'; buf[p++] = 'Q'; buf[p++] = 'T'; buf[p++] = 'T';
    buf[p++] = 0x04;   /* level 3.1.1 */
    buf[p++] = 0x02;   /* flags: clean_session=1 */
    buf[p++] = 0; buf[p++] = 60; /* keepalive */
    buf[p++] = (uint8_t)(cid_len >> 8);
    buf[p++] = (uint8_t)(cid_len & 0xFF);
    memcpy(buf + p, cid, cid_len); p += cid_len;
    if (send_all(g_fd, buf, p) < 0) { close(g_fd); return 1; }

    /* read CONNACK */
    uint8_t type; uint8_t body[512]; uint32_t blen;
    if (recv_pkt(g_fd, &type, body, &blen) < 0 || type != MQTT_CONNACK) {
        fprintf(stderr, "no CONNACK\n"); close(g_fd); return 1;
    }
    if (blen >= 2 && body[1] != 0) {
        fprintf(stderr, "CONNACK refused rc=%u\n", body[1]); close(g_fd); return 1;
    }

    /* SUBSCRIBE "overlap/#" pkt_id=1 */
    if (do_subscribe(g_fd, "overlap/#", 0, 1) < 0) { close(g_fd); return 1; }
    if (recv_pkt(g_fd, &type, body, &blen) < 0 || type != MQTT_SUBACK) {
        fprintf(stderr, "no SUBACK 1\n"); close(g_fd); return 1;
    }

    /* SUBSCRIBE "overlap/test" pkt_id=2 */
    if (do_subscribe(g_fd, "overlap/test", 0, 2) < 0) { close(g_fd); return 1; }
    if (recv_pkt(g_fd, &type, body, &blen) < 0 || type != MQTT_SUBACK) {
        fprintf(stderr, "no SUBACK 2\n"); close(g_fd); return 1;
    }

    /* Set 2-second recv timeout and count PUBLISH packets */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int count = 0;
    while (recv_pkt(g_fd, &type, body, &blen) == 0) {
        if (type == MQTT_PUBLISH) {
            count++;
        }
    }

    printf("received %d\n", count);
    fflush(stdout);
    close(g_fd);
    return 0;
}
