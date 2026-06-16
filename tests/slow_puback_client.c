/*
 * slow_puback_client — connects, subscribes at QoS 1, receives a PUBLISH,
 * waits DELAY_MS before sending PUBACK, and reports whether it received
 * a DUP=1 retransmit from the broker.
 *
 * Usage:
 *   slow_puback_client [-h HOST] [-p PORT] -t TOPIC [-d DELAY_MS]
 *
 * Exit code:
 *   0 — DUP retransmit received as expected
 *   2 — no DUP received (not necessarily an error, depends on timing)
 *   1 — setup/connection error
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
#include <errno.h>

#include "include/packet.h"

static void ms_sleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int tcp_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
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

static int recv_all(int fd, uint8_t *b, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = recv(fd, b + done, n - done, 0);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int recv_pkt_fd(int fd, mqtt_packet_t *out)
{
    uint8_t b;
    if (recv_all(fd, &b, 1) < 0) return -1;
    out->type_flags = b;
    uint32_t rlen = 0, mult = 1;
    do {
        if (recv_all(fd, &b, 1) < 0) return -1;
        rlen += (b & 0x7F) * mult;
        mult *= 128;
        if (mult > 128 * 128 * 128) return -1;
    } while (b & 0x80);
    out->remaining_len = rlen;
    if (rlen > MQTT_MAX_PACKET_SIZE) return -1;
    if (rlen > 0 && recv_all(fd, out->buf, rlen) < 0) return -1;
    out->buf_len = rlen;
    return 0;
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int         port = 1883;
    const char *topic = NULL;
    long        delay_ms = 8000; /* default: bridge two broker ticks at 5s each */

    int opt;
    while ((opt = getopt(argc, argv, "h:p:t:d:")) != -1) {
        switch (opt) {
        case 'h': host  = optarg; break;
        case 'p': port  = atoi(optarg); break;
        case 't': topic = optarg; break;
        case 'd': delay_ms = atol(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-h HOST] [-p PORT] -t TOPIC [-d DELAY_MS]\n",
                    argv[0]);
            return 1;
        }
    }
    if (!topic) { fprintf(stderr, "error: -t TOPIC required\n"); return 1; }

    int fd = tcp_connect(host, port);
    if (fd < 0) { perror("connect"); return 1; }

    /* CONNECT */
    char cid[32];
    snprintf(cid, sizeof(cid), "slow_pb_%d", (int)getpid());
    uint16_t id_len   = (uint16_t)strlen(cid);
    uint32_t rem_len  = 10 + 2 + id_len;
    uint8_t  rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes);
    uint8_t buf[512]; size_t p = 0;
    buf[p++] = MQTT_CONNECT;
    memcpy(buf + p, rem_enc, rem_bytes); p += rem_bytes;
    buf[p++] = 0; buf[p++] = 4;
    buf[p++] = 'M'; buf[p++] = 'Q'; buf[p++] = 'T'; buf[p++] = 'T';
    buf[p++] = 0x04; buf[p++] = 0x02; /* clean session */
    buf[p++] = 0; buf[p++] = 30;     /* keepalive=30 */
    buf[p++] = (uint8_t)(id_len >> 8); buf[p++] = (uint8_t)(id_len & 0xFF);
    memcpy(buf + p, cid, id_len); p += id_len;
    send_all(fd, buf, p);

    uint8_t connack[4];
    if (recv_all(fd, connack, 4) < 0 || connack[3] != 0) {
        fprintf(stderr, "CONNACK failed\n"); close(fd); return 1;
    }

    /* SUBSCRIBE at QoS 1 */
    uint16_t tlen = (uint16_t)strlen(topic);
    uint32_t srem = 2 + 2 + tlen + 1;
    packet_encode_remaining_len(srem, rem_enc, &rem_bytes);
    p = 0;
    buf[p++] = MQTT_SUBSCRIBE | 0x02;
    memcpy(buf + p, rem_enc, rem_bytes); p += rem_bytes;
    buf[p++] = 0; buf[p++] = 1; /* pkt_id = 1 */
    buf[p++] = (uint8_t)(tlen >> 8); buf[p++] = (uint8_t)(tlen & 0xFF);
    memcpy(buf + p, topic, tlen); p += tlen;
    buf[p++] = 1; /* QoS 1 */
    send_all(fd, buf, p);

    /* Read SUBACK */
    mqtt_packet_t pkt;
    if (recv_pkt_fd(fd, &pkt) < 0 || (pkt.type_flags & 0xF0) != MQTT_SUBACK) {
        fprintf(stderr, "SUBACK failed\n"); close(fd); return 1;
    }
    printf("subscribed to '%s'\n", topic);
    fflush(stdout);

    /* Set recv timeout > delay so we don't timeout waiting for first PUBLISH */
    struct timeval tv = { .tv_sec = (delay_ms / 1000) + 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Wait for first PUBLISH */
    if (recv_pkt_fd(fd, &pkt) < 0 || (pkt.type_flags & 0xF0) != MQTT_PUBLISH) {
        fprintf(stderr, "error: expected PUBLISH\n"); close(fd); return 1;
    }
    mqtt_publish_t pub;
    packet_parse_publish(&pkt, &pub);
    uint16_t packet_id = pub.packet_id;
    printf("received PUBLISH id=%u dup=%d\n", packet_id,
           (pkt.type_flags & 0x08) ? 1 : 0);
    fflush(stdout);

    /* Intentionally delay PUBACK to trigger broker retransmit */
    ms_sleep(delay_ms);

    /* Now check if broker retransmitted with DUP=1.
     * Window = 10s to catch a retry from the second broker tick (~10s). */
    int got_dup = 0;
    struct timeval tv2 = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
    while (1) {
        if (recv_pkt_fd(fd, &pkt) < 0) break;
        if ((pkt.type_flags & 0xF0) == MQTT_PUBLISH) {
            int dup = (pkt.type_flags & 0x08) ? 1 : 0;
            printf("received PUBLISH dup=%d\n", dup);
            fflush(stdout);
            if (dup) { got_dup = 1; break; }
        }
    }

    /* Send PUBACK for original message */
    uint8_t ab[4];
    int al = packet_build_puback(packet_id, ab, sizeof(ab));
    send_all(fd, ab, (size_t)al);

    /* DISCONNECT */
    buf[0] = MQTT_DISCONNECT; buf[1] = 0;
    send_all(fd, buf, 2);
    close(fd);

    if (got_dup) {
        printf("SUCCESS: DUP retransmit received\n");
        return 0;
    }
    printf("NO DUP: broker did not retransmit within window\n");
    return 2;
}
