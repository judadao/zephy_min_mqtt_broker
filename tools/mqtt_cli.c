/*
 * mqtt_cli — minimal MQTT v3.1.1 command-line client
 *
 * Usage:
 *   mqtt_cli pub  [-h HOST] [-p PORT] [-i ID] [-u USER] [-P PASS]
 *                 -t TOPIC -m MSG [-q QOS] [-r]
 *   mqtt_cli sub  [-h HOST] [-p PORT] [-i ID] [-u USER] [-P PASS]
 *                 -t TOPIC [-q QOS]
 *   mqtt_cli status [-h HOST] [-p PORT]
 *
 * Defaults: HOST=127.0.0.1  MQTT port=1883  HTTP port=8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "packet.h"

#define CLI_KEEPALIVE   60          /* seconds */
#define CLI_BUF_SIZE    (MQTT_MAX_PACKET_SIZE + 16)
#define CLI_PING_TICKS  2           /* send PINGREQ after this many recv timeouts (×30s) */

static volatile int g_running = 1;
static void on_sigint(int s) { (void)s; g_running = 0; }

/* ------------------------------------------------------------------ */
/* TCP helpers                                                          */
/* ------------------------------------------------------------------ */

static int tcp_connect(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "error: invalid host address '%s'\n", host);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "error: connect to %s:%u failed: %s\n",
                host, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t r = send(fd, buf + done, len - done, 0);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int recv_all(int fd, uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t r = recv(fd, buf + done, len - done, 0);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

/* Read one complete MQTT packet from the wire. */
static int recv_pkt(int fd, mqtt_packet_t *out)
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

/* ------------------------------------------------------------------ */
/* Client-side packet builders                                          */
/* ------------------------------------------------------------------ */

static int build_connect(uint8_t *out, size_t cap,
                          const char *client_id, uint16_t keepalive,
                          const char *user, const char *pass,
                          uint8_t clean_session)
{
    uint16_t id_len  = (uint16_t)strlen(client_id);
    uint16_t usr_len = user ? (uint16_t)strlen(user) : 0;
    uint16_t pas_len = pass ? (uint16_t)strlen(pass) : 0;

    uint8_t flags = clean_session ? 0x02 : 0x00;
    if (user) flags |= 0x80;
    if (pass) flags |= 0x40;

    /* variable header: "MQTT"(6) + level(1) + flags(1) + keepalive(2) */
    uint32_t payload = (uint32_t)(2 + id_len);
    if (user) payload += 2 + usr_len;
    if (pass) payload += 2 + pas_len;
    uint32_t rem_len = 10 + payload;

    uint8_t rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes);
    if (cap < 1 + rem_bytes + rem_len) return -1;

    size_t p = 0;
    out[p++] = MQTT_CONNECT;
    memcpy(out + p, rem_enc, rem_bytes); p += rem_bytes;
    out[p++] = 0x00; out[p++] = 0x04;
    out[p++] = 'M'; out[p++] = 'Q'; out[p++] = 'T'; out[p++] = 'T';
    out[p++] = 0x04;            /* protocol level 3.1.1 */
    out[p++] = flags;
    out[p++] = (uint8_t)(keepalive >> 8);
    out[p++] = (uint8_t)(keepalive & 0xFF);
    out[p++] = (uint8_t)(id_len >> 8); out[p++] = (uint8_t)(id_len & 0xFF);
    memcpy(out + p, client_id, id_len); p += id_len;
    if (user) {
        out[p++] = (uint8_t)(usr_len >> 8); out[p++] = (uint8_t)(usr_len & 0xFF);
        memcpy(out + p, user, usr_len); p += usr_len;
    }
    if (pass) {
        out[p++] = (uint8_t)(pas_len >> 8); out[p++] = (uint8_t)(pas_len & 0xFF);
        memcpy(out + p, pass, pas_len); p += pas_len;
    }
    return (int)p;
}

static int build_subscribe(uint8_t *out, size_t cap,
                            const char *topic, uint8_t qos, uint16_t pkt_id)
{
    uint16_t tlen    = (uint16_t)strlen(topic);
    uint32_t rem_len = (uint32_t)(2 + 2 + tlen + 1);

    uint8_t rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes);
    if (cap < 1 + rem_bytes + rem_len) return -1;

    size_t p = 0;
    out[p++] = MQTT_SUBSCRIBE | 0x02; /* reserved bit 1 */
    memcpy(out + p, rem_enc, rem_bytes); p += rem_bytes;
    out[p++] = (uint8_t)(pkt_id >> 8); out[p++] = (uint8_t)(pkt_id & 0xFF);
    out[p++] = (uint8_t)(tlen >> 8);   out[p++] = (uint8_t)(tlen & 0xFF);
    memcpy(out + p, topic, tlen); p += tlen;
    out[p++] = qos & 0x03;
    return (int)p;
}

static int build_unsubscribe(uint8_t *out, size_t cap,
                              const char *topic, uint16_t pkt_id)
{
    uint16_t tlen    = (uint16_t)strlen(topic);
    uint32_t rem_len = (uint32_t)(2 + 2 + tlen);

    uint8_t rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes);
    if (cap < 1 + rem_bytes + rem_len) return -1;

    size_t p = 0;
    out[p++] = MQTT_UNSUBSCRIBE | 0x02; /* reserved bit 1 */
    memcpy(out + p, rem_enc, rem_bytes); p += rem_bytes;
    out[p++] = (uint8_t)(pkt_id >> 8); out[p++] = (uint8_t)(pkt_id & 0xFF);
    out[p++] = (uint8_t)(tlen >> 8);   out[p++] = (uint8_t)(tlen & 0xFF);
    memcpy(out + p, topic, tlen); p += tlen;
    return (int)p;
}

static int build_pingreq(uint8_t *out, size_t cap)
{
    if (cap < 2) return -1;
    out[0] = MQTT_PINGREQ; out[1] = 0;
    return 2;
}

static int build_disconnect(uint8_t *out, size_t cap)
{
    if (cap < 2) return -1;
    out[0] = MQTT_DISCONNECT; out[1] = 0;
    return 2;
}

/* ------------------------------------------------------------------ */
/* CONNECT + CONNACK handshake                                          */
/* ------------------------------------------------------------------ */

static int do_connect(int fd, const char *client_id,
                       const char *user, const char *pass,
                       uint8_t clean_session)
{
    uint8_t buf[CLI_BUF_SIZE];
    int     len = build_connect(buf, sizeof(buf), client_id, CLI_KEEPALIVE, user, pass,
                                clean_session);
    if (len < 0 || send_all(fd, buf, (size_t)len) < 0) return -1;

    mqtt_packet_t pkt;
    if (recv_pkt(fd, &pkt) < 0) return -1;
    if ((pkt.type_flags & 0xF0) != MQTT_CONNACK || pkt.buf_len < 2) {
        fprintf(stderr, "error: unexpected response to CONNECT\n");
        return -1;
    }

    static const char *connack_str[] = {
        "accepted", "unacceptable protocol", "client ID rejected",
        "server unavailable", "bad credentials", "not authorized"
    };
    uint8_t rc = pkt.buf[1];
    if (rc != CONNACK_ACCEPTED) {
        const char *msg = (rc < 6) ? connack_str[rc] : "refused";
        fprintf(stderr, "error: broker refused connection: %s (code %u)\n", msg, rc);
        return -1;
    }
    /* return session_present flag (bit 0 of byte 0 of variable header) */
    return (int)(pkt.buf[0] & 0x01);
}

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

static int cmd_pub(const char *host, uint16_t port,
                   const char *client_id, const char *user, const char *pass,
                   const char *topic, const char *message,
                   uint8_t qos, uint8_t retain, uint8_t clean_session)
{
    int fd = tcp_connect(host, port);
    if (fd < 0) return 1;
    if (do_connect(fd, client_id, user, pass, clean_session) < 0) {
        close(fd); return 1;
    }

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, topic, sizeof(pub.topic) - 1);
    size_t mlen = strlen(message);
    if (mlen > sizeof(pub.payload)) {
        fprintf(stderr, "error: message too long (max %zu bytes)\n",
                sizeof(pub.payload));
        close(fd); return 1;
    }
    memcpy(pub.payload, message, mlen);
    pub.payload_len = (uint16_t)mlen;
    pub.qos         = qos;
    pub.retain      = retain;
    pub.packet_id   = (qos > 0) ? 1 : 0;

    uint8_t buf[CLI_BUF_SIZE];
    int len = packet_build_publish(&pub, buf, sizeof(buf));
    if (len < 0 || send_all(fd, buf, (size_t)len) < 0) {
        fprintf(stderr, "error: publish failed\n"); close(fd); return 1;
    }

    if (qos == 1) {
        mqtt_packet_t pkt;
        if (recv_pkt(fd, &pkt) < 0 || (pkt.type_flags & 0xF0) != MQTT_PUBACK) {
            fprintf(stderr, "error: PUBACK not received\n"); close(fd); return 1;
        }
    }

    len = build_disconnect(buf, sizeof(buf));
    send_all(fd, buf, (size_t)len);
    close(fd);
    printf("published '%s' -> %s\n", message, topic);
    return 0;
}

static int cmd_sub(const char *host, uint16_t port,
                   const char *client_id, const char *user, const char *pass,
                   const char *topic, uint8_t qos, uint8_t clean_session)
{
    int fd = tcp_connect(host, port);
    if (fd < 0) return 1;
    int sp = do_connect(fd, client_id, user, pass, clean_session);
    if (sp < 0) { close(fd); return 1; }
    if (sp == 1) fprintf(stderr, "session resumed (session_present=1)\n");

    uint8_t buf[CLI_BUF_SIZE];
    int len = build_subscribe(buf, sizeof(buf), topic, qos, 1);
    if (len < 0 || send_all(fd, buf, (size_t)len) < 0) { close(fd); return 1; }

    /* Wait for SUBACK; drained PUBLISH packets may arrive first (session resume) */
    mqtt_packet_t pkt;
    for (;;) {
        if (recv_pkt(fd, &pkt) < 0) {
            fprintf(stderr, "error: SUBACK not received\n"); close(fd); return 1;
        }
        uint8_t t = pkt.type_flags & 0xF0;
        if (t == MQTT_SUBACK) {
            break;
        }
        if (t == MQTT_PUBLISH) {
            mqtt_publish_t dpub;
            if (packet_parse_publish(&pkt, &dpub) == 0) {
                dpub.payload[dpub.payload_len] = '\0';
                printf("%s %s\n", dpub.topic, (char *)dpub.payload);
                fflush(stdout);
                if (dpub.qos == 1) {
                    uint8_t ab[4];
                    int al = packet_build_puback(dpub.packet_id, ab, sizeof(ab));
                    if (al > 0) send_all(fd, ab, (size_t)al);
                }
            }
        }
        /* other packets (PINGRESP etc.) are silently ignored */
    }
    if (pkt.buf_len >= 3 && (pkt.buf[2] & 0x80)) {
        fprintf(stderr, "error: subscription refused by broker\n"); close(fd); return 1;
    }

    fprintf(stderr, "subscribed to '%s' (qos %u) — Ctrl-C to stop\n", topic, qos);
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint); /* clean DISCONNECT on kill */

    /* 30-second recv timeout; send PINGREQ after CLI_PING_TICKS timeouts */
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int idle = 0;

    while (g_running) {
        if (recv_pkt(fd, &pkt) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++idle >= CLI_PING_TICKS) {
                    idle = 0;
                    len  = build_pingreq(buf, sizeof(buf));
                    if (send_all(fd, buf, (size_t)len) < 0) break;
                }
                continue;
            }
            if (g_running) fprintf(stderr, "\nerror: connection lost\n");
            break;
        }
        idle = 0;

        uint8_t type = pkt.type_flags & 0xF0;
        if (type == MQTT_PUBLISH) {
            mqtt_publish_t pub;
            if (packet_parse_publish(&pkt, &pub) == 0) {
                pub.payload[pub.payload_len] = '\0';
                printf("%s %s\n", pub.topic, (char *)pub.payload);
                fflush(stdout);
                if (pub.qos == 1) {
                    len = packet_build_puback(pub.packet_id, buf, sizeof(buf));
                    send_all(fd, buf, (size_t)len);
                }
            }
        }
        /* PINGRESP and other control packets: ignore silently */
    }

    len = build_disconnect(buf, sizeof(buf));
    send_all(fd, buf, (size_t)len);
    close(fd);
    return 0;
}

/* Connect, subscribe, unsubscribe, then drain briefly to confirm no more msgs. */
static int cmd_unsub(const char *host, uint16_t port,
                     const char *client_id, const char *user, const char *pass,
                     const char *topic, uint8_t qos)
{
    int fd = tcp_connect(host, port);
    if (fd < 0) return 1;
    if (do_connect(fd, client_id, user, pass, 1) < 0) { close(fd); return 1; }

    uint8_t buf[CLI_BUF_SIZE];
    int len = build_subscribe(buf, sizeof(buf), topic, qos, 1);
    if (len < 0 || send_all(fd, buf, (size_t)len) < 0) { close(fd); return 1; }

    mqtt_packet_t pkt;
    if (recv_pkt(fd, &pkt) < 0 || (pkt.type_flags & 0xF0) != MQTT_SUBACK) {
        fprintf(stderr, "error: SUBACK not received\n"); close(fd); return 1;
    }
    fprintf(stderr, "subscribed to '%s'\n", topic);

    len = build_unsubscribe(buf, sizeof(buf), topic, 2);
    if (len < 0 || send_all(fd, buf, (size_t)len) < 0) { close(fd); return 1; }

    if (recv_pkt(fd, &pkt) < 0 || (pkt.type_flags & 0xF0) != MQTT_UNSUBACK) {
        fprintf(stderr, "error: UNSUBACK not received\n"); close(fd); return 1;
    }
    fprintf(stderr, "unsubscribed from '%s'\n", topic);

    /* Short drain: any messages that arrive after UNSUBACK are unexpected. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (recv_pkt(fd, &pkt) == 0) {
        if ((pkt.type_flags & 0xF0) == MQTT_PUBLISH) {
            mqtt_publish_t pub;
            if (packet_parse_publish(&pkt, &pub) == 0) {
                pub.payload[pub.payload_len] = '\0';
                /* Print to stderr so caller can detect unexpected messages */
                fprintf(stderr, "unexpected: %s %s\n", pub.topic, (char *)pub.payload);
            }
        }
    }

    len = build_disconnect(buf, sizeof(buf));
    send_all(fd, buf, (size_t)len);
    close(fd);
    return 0;
}

static int cmd_status(const char *host, uint16_t port)
{
    int fd = tcp_connect(host, port);
    if (fd < 0) return 1;

    char req[256];
    int  rlen = snprintf(req, sizeof(req),
        "GET /api/status HTTP/1.0\r\nHost: %s\r\nAccept: application/json\r\n\r\n",
        host);
    if (send_all(fd, (uint8_t *)req, (size_t)rlen) < 0) { close(fd); return 1; }

    char   resp[8192] = {0};
    size_t total = 0;
    ssize_t r;
    while (total < sizeof(resp) - 1 &&
           (r = recv(fd, resp + total, sizeof(resp) - 1 - total, 0)) > 0) {
        total += (size_t)r;
    }
    close(fd);

    char *body = strstr(resp, "\r\n\r\n");
    if (!body) { fprintf(stderr, "error: unexpected HTTP response\n"); return 1; }
    printf("%s\n", body + 4);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s pub   [-h HOST] [-p PORT] [-i ID] [-u USER] [-P PASS]\n"
        "            -t TOPIC -m MSG [-q 0|1|2] [-r] [-s]\n"
        "\n"
        "  %s sub   [-h HOST] [-p PORT] [-i ID] [-u USER] [-P PASS]\n"
        "            -t TOPIC [-q 0|1|2] [-s]\n"
        "            -s  persistent session (clean_session=0)\n"
        "\n"
        "  %s unsub [-h HOST] [-p PORT] [-i ID] [-u USER] [-P PASS]\n"
        "            -t TOPIC [-q 0|1|2]\n"
        "            subscribe, unsubscribe, then drain briefly\n"
        "\n"
        "  %s status [-h HOST] [-p PORT]\n"
        "\n"
        "Defaults: HOST=127.0.0.1  MQTT port=1883  HTTP port=8080\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];

    const char *host    = "127.0.0.1";
    uint16_t    port    = 0;
    char        cid[MQTT_CLIENT_ID_MAX];
    snprintf(cid, sizeof(cid), "mqtt_cli_%d", (int)getpid());
    const char *user    = NULL;
    const char *pass    = NULL;
    const char *topic         = NULL;
    const char *message       = NULL;
    uint8_t     qos           = 0;
    uint8_t     retain        = 0;
    uint8_t     clean_session = 1; /* default: clean session */

    /* shift argv past the command word so getopt sees only options */
    int    nargc = argc - 1;
    char **nargv = argv + 1;
    int    opt;
    while ((opt = getopt(nargc, nargv, "h:p:i:u:P:t:m:q:rs")) != -1) {
        switch (opt) {
        case 'h': host    = optarg; break;
        case 'p': port    = (uint16_t)atoi(optarg); break;
        case 'i': strncpy(cid, optarg, sizeof(cid) - 1); break;
        case 'u': user    = optarg; break;
        case 'P': pass    = optarg; break;
        case 't': topic   = optarg; break;
        case 'm': message = optarg; break;
        case 'q': qos     = (uint8_t)atoi(optarg); break;
        case 'r': retain  = 1; break;
        case 's': clean_session = 0; break; /* persistent session */
        default:  usage(argv[0]); return 1;
        }
    }

    if (strcmp(cmd, "pub") == 0) {
        if (!port) port = 1883;
        if (!topic || !message) {
            fprintf(stderr, "pub requires -t TOPIC -m MSG\n"); return 1;
        }
        return cmd_pub(host, port, cid, user, pass, topic, message, qos, retain,
                       clean_session);

    } else if (strcmp(cmd, "sub") == 0) {
        if (!port) port = 1883;
        if (!topic) {
            fprintf(stderr, "sub requires -t TOPIC\n"); return 1;
        }
        return cmd_sub(host, port, cid, user, pass, topic, qos, clean_session);

    } else if (strcmp(cmd, "unsub") == 0) {
        if (!port) port = 1883;
        if (!topic) {
            fprintf(stderr, "unsub requires -t TOPIC\n"); return 1;
        }
        return cmd_unsub(host, port, cid, user, pass, topic, qos);

    } else if (strcmp(cmd, "status") == 0) {
        if (!port) port = 8080;
        return cmd_status(host, port);

    } else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        usage(argv[0]);
        return 1;
    }
}
