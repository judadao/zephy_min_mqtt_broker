/*
 * lwt_client — minimal MQTT client that connects with a Last Will and
 * stays connected until killed.  Used by scripts/test_lwt.sh.
 *
 * Usage:
 *   lwt_client -h HOST -p PORT -t WILL_TOPIC -m WILL_MSG [-q QOS] [-r] [-P PUB_MSG]
 *
 *   -t  will topic
 *   -m  will message/payload
 *   -q  will QoS (0/1/2, default 0)
 *   -r  set will retain flag
 *   -P  if given, publish this message to the will topic then disconnect cleanly
 *       (used to test that clean disconnect does NOT trigger the will)
 *
 * On clean exit (SIGTERM / -P flag) sends DISCONNECT.
 * On SIGKILL the TCP connection drops without DISCONNECT → broker publishes will.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void ms_sleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#include "include/packet.h"

#define BUF 2048

static volatile int g_running = 1;
static void on_term(int s) { (void)s; g_running = 0; }

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

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF);
}

static size_t write_str(uint8_t *b, size_t p, const char *s)
{
    uint16_t l = (uint16_t)strlen(s);
    write_u16(b + p, l); memcpy(b + p + 2, s, l);
    return p + 2 + l;
}

static int build_connect(uint8_t *out, size_t cap,
                          const char *cid,
                          const char *will_topic,
                          const char *will_msg,
                          uint8_t will_qos,
                          uint8_t will_retain)
{
    uint16_t id_len  = (uint16_t)strlen(cid);
    uint16_t wt_len  = (uint16_t)strlen(will_topic);
    uint16_t wm_len  = (uint16_t)strlen(will_msg);

    uint8_t flags = 0x02; /* clean session */
    flags |= 0x04;                      /* has_will */
    flags |= (uint8_t)((will_qos & 0x03) << 3);
    if (will_retain) flags |= 0x20;

    /* var header: "MQTT"(6) + level(1) + flags(1) + keepalive(2) = 10 */
    uint32_t payload = (uint32_t)(2 + id_len + 2 + wt_len + 2 + wm_len);
    uint32_t rem_len = 10 + payload;

    uint8_t rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes);
    if (cap < 1 + rem_bytes + rem_len) return -1;

    size_t p = 0;
    out[p++] = MQTT_CONNECT;
    memcpy(out + p, rem_enc, rem_bytes); p += rem_bytes;
    out[p++] = 0; out[p++] = 4;
    out[p++] = 'M'; out[p++] = 'Q'; out[p++] = 'T'; out[p++] = 'T';
    out[p++] = 0x04;   /* 3.1.1 */
    out[p++] = flags;
    out[p++] = 0; out[p++] = 60; /* keepalive 60s */
    write_u16(out + p, id_len); memcpy(out + p + 2, cid, id_len); p += 2 + id_len;
    p = write_str(out, p, will_topic);
    /* will payload: 2-byte length prefix */
    write_u16(out + p, wm_len); memcpy(out + p + 2, will_msg, wm_len); p += 2 + wm_len;
    return (int)p;
}

static int build_publish_qos0(uint8_t *out, size_t cap,
                               const char *topic, const char *msg)
{
    uint16_t tlen = (uint16_t)strlen(topic);
    uint16_t mlen = (uint16_t)strlen(msg);
    uint32_t rem  = (uint32_t)(2 + tlen + mlen);
    uint8_t  enc[4]; size_t eb;
    packet_encode_remaining_len(rem, enc, &eb);
    if (cap < 1 + eb + rem) return -1;
    size_t p = 0;
    out[p++] = MQTT_PUBLISH;
    memcpy(out + p, enc, eb); p += eb;
    p = write_str(out, p, topic);
    memcpy(out + p, msg, mlen); p += mlen;
    return (int)p;
}

int main(int argc, char *argv[])
{
    const char *host       = "127.0.0.1";
    int         port       = 1883;
    const char *will_topic = NULL;
    const char *will_msg   = NULL;
    uint8_t     will_qos   = 0;
    uint8_t     will_ret   = 0;
    const char *pub_msg    = NULL; /* if set: publish then clean-disconnect */

    int opt;
    while ((opt = getopt(argc, argv, "h:p:t:m:q:rP:")) != -1) {
        switch (opt) {
        case 'h': host      = optarg; break;
        case 'p': port      = atoi(optarg); break;
        case 't': will_topic = optarg; break;
        case 'm': will_msg   = optarg; break;
        case 'q': will_qos   = (uint8_t)atoi(optarg); break;
        case 'r': will_ret   = 1; break;
        case 'P': pub_msg    = optarg; break;
        default:
            fprintf(stderr, "usage: %s -t TOPIC -m MSG [-h HOST] [-p PORT] "
                            "[-q QOS] [-r] [-P PUB_MSG]\n", argv[0]);
            return 1;
        }
    }

    if (!will_topic || !will_msg) {
        fprintf(stderr, "error: -t TOPIC and -m MSG are required\n");
        return 1;
    }

    int fd = tcp_connect(host, port);
    if (fd < 0) { perror("connect"); return 1; }

    uint8_t buf[BUF];

    /* CONNECT with will */
    char cid[32];
    snprintf(cid, sizeof(cid), "lwt_cli_%d", (int)getpid());
    int n = build_connect(buf, sizeof(buf), cid,
                          will_topic, will_msg, will_qos, will_ret);
    if (n < 0 || send_all(fd, buf, (size_t)n) < 0) {
        fprintf(stderr, "error: CONNECT failed\n"); close(fd); return 1;
    }

    /* CONNACK */
    ssize_t r = recv(fd, buf, 4, MSG_WAITALL);
    if (r < 4 || (buf[0] & 0xF0) != MQTT_CONNACK || buf[3] != 0) {
        fprintf(stderr, "error: CONNACK rejected (rc=%d)\n",
                r >= 4 ? buf[3] : -1);
        close(fd); return 1;
    }

    if (pub_msg) {
        /* clean-connect scenario: publish one message then disconnect cleanly */
        n = build_publish_qos0(buf, sizeof(buf), will_topic, pub_msg);
        if (n > 0) send_all(fd, buf, (size_t)n);
        ms_sleep(200); /* 200 ms for broker to deliver */
        /* clean DISCONNECT */
        buf[0] = MQTT_DISCONNECT; buf[1] = 0;
        send_all(fd, buf, 2);
        close(fd);
        return 0;
    }

    /* ungraceful scenario: stay connected until killed */
    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);
    while (g_running) {
        ms_sleep(100);
    }
    /* SIGTERM path: send DISCONNECT (clean) */
    buf[0] = MQTT_DISCONNECT; buf[1] = 0;
    send_all(fd, buf, 2);
    close(fd);
    return 0;
}
