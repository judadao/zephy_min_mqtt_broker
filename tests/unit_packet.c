/*
 * Unit tests for packet.c — encode/decode, build/parse
 * Compile: see Makefile.linux "make -f Makefile.linux test"
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "include/packet.h"

/* ── mini test harness ─────────────────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; } \
    else       { printf("  [FAIL] %s  (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) == (b)) { printf("  [PASS] %s\n", msg); g_pass++; } \
    else { printf("  [FAIL] %s  expected=%lld got=%lld (line %d)\n", \
                  msg, (long long)(b), (long long)(a), __LINE__); g_fail++; } \
} while (0)

/* ── helpers ───────────────────────────────────────────────────────────────── */
static int encode_decode_roundtrip(uint32_t val)
{
    uint8_t enc[4];
    size_t enc_bytes;
    if (packet_encode_remaining_len(val, enc, &enc_bytes) != 0) return -1;

    uint32_t decoded;
    size_t   dec_bytes;
    if (packet_decode_remaining_len(enc, enc_bytes, &decoded, &dec_bytes) != 0) return -2;

    if (decoded != val)       return -3;
    if (dec_bytes != enc_bytes) return -4;
    return 0;
}

/* ── tests ─────────────────────────────────────────────────────────────────── */

static void test_remaining_len_encode(void)
{
    uint8_t buf[4];
    size_t  n;

    printf("\n--- remaining-len encode ---\n");

    /* 0 → single byte 0x00 */
    ASSERT(packet_encode_remaining_len(0, buf, &n) == 0 && n == 1 && buf[0] == 0x00,
           "encode 0");

    /* 127 → single byte 0x7F */
    ASSERT(packet_encode_remaining_len(127, buf, &n) == 0 && n == 1 && buf[0] == 0x7F,
           "encode 127");

    /* 128 → two bytes 0x80 0x01 */
    ASSERT(packet_encode_remaining_len(128, buf, &n) == 0 && n == 2 &&
           buf[0] == 0x80 && buf[1] == 0x01,
           "encode 128");

    /* 16383 → two bytes 0xFF 0x7F */
    ASSERT(packet_encode_remaining_len(16383, buf, &n) == 0 && n == 2 &&
           buf[0] == 0xFF && buf[1] == 0x7F,
           "encode 16383");

    /* 16384 → three bytes 0x80 0x80 0x01 */
    ASSERT(packet_encode_remaining_len(16384, buf, &n) == 0 && n == 3 &&
           buf[0] == 0x80 && buf[1] == 0x80 && buf[2] == 0x01,
           "encode 16384");

    /* 2097151 → three bytes 0xFF 0xFF 0x7F */
    ASSERT(packet_encode_remaining_len(2097151, buf, &n) == 0 && n == 3 &&
           buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0x7F,
           "encode 2097151");

    /* 268435455 → four bytes 0xFF 0xFF 0xFF 0x7F (max) */
    ASSERT(packet_encode_remaining_len(268435455, buf, &n) == 0 && n == 4 &&
           buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0x7F,
           "encode 268435455 (max)");

    /* overflow: 268435456 must fail */
    ASSERT(packet_encode_remaining_len(268435456, buf, &n) != 0,
           "encode overflow returns error");
}

static void test_remaining_len_decode(void)
{
    uint32_t val;
    size_t   n;

    printf("\n--- remaining-len decode ---\n");

    /* 0x00 → 0 */
    uint8_t b0[] = {0x00};
    ASSERT(packet_decode_remaining_len(b0, 1, &val, &n) == 0 && val == 0 && n == 1,
           "decode 0x00 → 0");

    /* 0x7F → 127 */
    uint8_t b1[] = {0x7F};
    ASSERT(packet_decode_remaining_len(b1, 1, &val, &n) == 0 && val == 127 && n == 1,
           "decode 0x7F → 127");

    /* 0x80 0x01 → 128 */
    uint8_t b2[] = {0x80, 0x01};
    ASSERT(packet_decode_remaining_len(b2, 2, &val, &n) == 0 && val == 128 && n == 2,
           "decode 0x80 0x01 → 128");

    /* 0xFF 0xFF 0xFF 0x7F → 268435455 */
    uint8_t b3[] = {0xFF, 0xFF, 0xFF, 0x7F};
    ASSERT(packet_decode_remaining_len(b3, 4, &val, &n) == 0 &&
           val == 268435455 && n == 4,
           "decode max 268435455");

    /* truncated → error */
    uint8_t b4[] = {0x80};
    ASSERT(packet_decode_remaining_len(b4, 1, &val, &n) != 0,
           "decode truncated returns error");

    /* empty buffer → error */
    ASSERT(packet_decode_remaining_len(b4, 0, &val, &n) != 0,
           "decode empty buffer returns error");
}

static void test_remaining_len_roundtrip(void)
{
    printf("\n--- remaining-len roundtrip ---\n");
    uint32_t cases[] = {0, 1, 63, 127, 128, 255, 16383, 16384,
                        65535, 2097151, 2097152, 268435455};
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "roundtrip %u", (unsigned)cases[i]);
        ASSERT(encode_decode_roundtrip(cases[i]) == 0, msg);
    }
}

static void test_build_connack(void)
{
    printf("\n--- build_connack ---\n");
    uint8_t out[16];

    int n = packet_build_connack(0, CONNACK_ACCEPTED, out, sizeof(out));
    ASSERT_EQ(n, 4, "connack length = 4");
    ASSERT_EQ(out[0], MQTT_CONNACK, "connack type byte");
    ASSERT_EQ(out[1], 2,            "connack remaining_len = 2");
    ASSERT_EQ(out[2], 0,            "connack session_present = 0");
    ASSERT_EQ(out[3], CONNACK_ACCEPTED, "connack return_code = 0");

    /* with session present */
    n = packet_build_connack(1, CONNACK_ACCEPTED, out, sizeof(out));
    ASSERT_EQ(n, 4, "connack sp=1 length");
    ASSERT_EQ(out[2], 1, "connack session_present = 1");

    /* bad credentials */
    n = packet_build_connack(0, CONNACK_BAD_CREDENTIALS, out, sizeof(out));
    ASSERT_EQ(out[3], CONNACK_BAD_CREDENTIALS, "connack bad-creds return code");

    /* buffer too small → error */
    n = packet_build_connack(0, 0, out, 3);
    ASSERT(n < 0, "connack small buffer returns error");
}

static void test_build_ack_packets(void)
{
    printf("\n--- build puback/pubrec/pubrel/pubcomp/unsuback ---\n");
    uint8_t out[16];
    int n;

    /* PUBACK */
    n = packet_build_puback(0x1234, out, sizeof(out));
    ASSERT_EQ(n, 4, "puback length = 4");
    ASSERT_EQ(out[0], MQTT_PUBACK, "puback type");
    ASSERT_EQ(out[1], 2,           "puback remaining_len = 2");
    ASSERT_EQ(out[2], 0x12,        "puback id hi");
    ASSERT_EQ(out[3], 0x34,        "puback id lo");

    /* PUBREC */
    n = packet_build_pubrec(0x0001, out, sizeof(out));
    ASSERT_EQ(n, 4, "pubrec length = 4");
    ASSERT_EQ(out[0], MQTT_PUBREC, "pubrec type");

    /* PUBREL — spec requires fixed header = 0x62 */
    n = packet_build_pubrel(0x0001, out, sizeof(out));
    ASSERT_EQ(n, 4, "pubrel length = 4");
    ASSERT_EQ(out[0], 0x62, "pubrel fixed header 0x62");

    /* PUBCOMP */
    n = packet_build_pubcomp(0xFFFF, out, sizeof(out));
    ASSERT_EQ(n, 4, "pubcomp length = 4");
    ASSERT_EQ(out[0], MQTT_PUBCOMP, "pubcomp type");
    ASSERT_EQ(out[2], 0xFF, "pubcomp id hi = 0xFF");
    ASSERT_EQ(out[3], 0xFF, "pubcomp id lo = 0xFF");

    /* UNSUBACK */
    n = packet_build_unsuback(42, out, sizeof(out));
    ASSERT_EQ(n, 4, "unsuback length = 4");
    ASSERT_EQ(out[0], MQTT_UNSUBACK, "unsuback type");

    /* small buffer → error */
    n = packet_build_puback(1, out, 3);
    ASSERT(n < 0, "puback small buffer returns error");
}

static void test_build_pingresp(void)
{
    printf("\n--- build_pingresp ---\n");
    uint8_t out[8];

    int n = packet_build_pingresp(out, sizeof(out));
    ASSERT_EQ(n, 2, "pingresp length = 2");
    ASSERT_EQ(out[0], MQTT_PINGRESP, "pingresp type byte");
    ASSERT_EQ(out[1], 0,             "pingresp remaining_len = 0");

    n = packet_build_pingresp(out, 1);
    ASSERT(n < 0, "pingresp small buffer returns error");
}

static void test_build_suback(void)
{
    printf("\n--- build_suback ---\n");
    uint8_t out[32];
    uint8_t rc[3] = {0x00, 0x01, 0x02};

    int n = packet_build_suback(0x0005, rc, 3, out, sizeof(out));
    ASSERT(n == 7, "suback 3 topics length = 7");
    ASSERT_EQ(out[0], MQTT_SUBACK, "suback type");
    ASSERT_EQ(out[1], 5,           "suback remaining_len = 5");
    ASSERT_EQ(out[2], 0x00,        "suback packet_id hi");
    ASSERT_EQ(out[3], 0x05,        "suback packet_id lo");
    ASSERT_EQ(out[4], 0x00,        "suback rc[0] = 0 (QoS 0)");
    ASSERT_EQ(out[5], 0x01,        "suback rc[1] = 1 (QoS 1)");
    ASSERT_EQ(out[6], 0x02,        "suback rc[2] = 2 (QoS 2)");
}

static void test_build_parse_publish_roundtrip(void)
{
    printf("\n--- publish build→parse roundtrip ---\n");

    /* QoS 0 */
    mqtt_publish_t src = {0};
    strncpy(src.topic, "sensor/temp", sizeof(src.topic) - 1);
    memcpy(src.payload, "22.5", 4);
    src.payload_len = 4;
    src.qos    = 0;
    src.retain = 0;

    uint8_t wire[MQTT_MAX_PACKET_SIZE + 8];
    int wlen = packet_build_publish(&src, wire, sizeof(wire));
    ASSERT(wlen > 0, "publish QoS0 build succeeds");

    mqtt_packet_t pkt = {0};
    pkt.type_flags = wire[0];
    /* parse remaining-len */
    uint32_t rem;
    size_t   rem_bytes;
    packet_decode_remaining_len(wire + 1, (size_t)(wlen - 1), &rem, &rem_bytes);
    pkt.buf_len = rem;
    memcpy(pkt.buf, wire + 1 + rem_bytes, rem);

    mqtt_publish_t dst = {0};
    ASSERT(packet_parse_publish(&pkt, &dst) == 0, "publish QoS0 parse succeeds");
    ASSERT(strcmp(dst.topic, "sensor/temp") == 0, "publish QoS0 topic preserved");
    ASSERT_EQ(dst.payload_len, 4, "publish QoS0 payload_len = 4");
    ASSERT(memcmp(dst.payload, "22.5", 4) == 0, "publish QoS0 payload preserved");
    ASSERT_EQ(dst.qos, 0, "publish QoS0 qos = 0");

    /* QoS 1 with packet_id */
    src.qos       = 1;
    src.packet_id = 0xABCD;
    wlen = packet_build_publish(&src, wire, sizeof(wire));
    ASSERT(wlen > 0, "publish QoS1 build succeeds");

    pkt.type_flags = wire[0];
    packet_decode_remaining_len(wire + 1, (size_t)(wlen - 1), &rem, &rem_bytes);
    pkt.buf_len = rem;
    memcpy(pkt.buf, wire + 1 + rem_bytes, rem);

    memset(&dst, 0, sizeof(dst));
    ASSERT(packet_parse_publish(&pkt, &dst) == 0, "publish QoS1 parse succeeds");
    ASSERT_EQ(dst.qos, 1, "publish QoS1 qos = 1");
    ASSERT_EQ(dst.packet_id, 0xABCD, "publish QoS1 packet_id preserved");

    /* retain flag */
    src.qos    = 0;
    src.retain = 1;
    wlen = packet_build_publish(&src, wire, sizeof(wire));
    pkt.type_flags = wire[0];
    packet_decode_remaining_len(wire + 1, (size_t)(wlen - 1), &rem, &rem_bytes);
    pkt.buf_len = rem;
    memcpy(pkt.buf, wire + 1 + rem_bytes, rem);
    memset(&dst, 0, sizeof(dst));
    packet_parse_publish(&pkt, &dst);
    ASSERT_EQ(dst.retain, 1, "publish retain flag preserved");

    /* DUP flag roundtrip */
    src.qos = 1; src.packet_id = 1; src.retain = 0;
    src.dup = 1;
    wlen = packet_build_publish(&src, wire, sizeof(wire));
    ASSERT(wlen > 0, "publish DUP=1 build succeeds");
    ASSERT((wire[0] & 0x08) != 0, "publish DUP=1: bit 3 set in fixed header");
    pkt.type_flags = wire[0];
    packet_decode_remaining_len(wire + 1, (size_t)(wlen - 1), &rem, &rem_bytes);
    pkt.buf_len = rem;
    memcpy(pkt.buf, wire + 1 + rem_bytes, rem);
    memset(&dst, 0, sizeof(dst));
    ASSERT(packet_parse_publish(&pkt, &dst) == 0, "publish DUP=1 parse succeeds");
    ASSERT_EQ(dst.dup, 1, "publish DUP=1 preserved through roundtrip");
    src.dup = 0; /* reset for subsequent tests */

    /* buffer too small → error */
    ASSERT(packet_build_publish(&src, wire, 3) < 0, "publish small buffer returns error");

    /* max-size payload (MQTT_PAYLOAD_MAX bytes) roundtrip */
    mqtt_publish_t big = {0};
    strncpy(big.topic, "big/payload", sizeof(big.topic) - 1);
    memset(big.payload, 0xAB, MQTT_PAYLOAD_MAX);
    big.payload_len = MQTT_PAYLOAD_MAX;
    big.qos = 0;
    wlen = packet_build_publish(&big, wire, sizeof(wire));
    ASSERT(wlen > 0, "max-payload publish build succeeds");
    pkt.type_flags = wire[0];
    packet_decode_remaining_len(wire + 1, (size_t)(wlen - 1), &rem, &rem_bytes);
    pkt.buf_len = rem;
    memcpy(pkt.buf, wire + 1 + rem_bytes, rem);
    memset(&dst, 0, sizeof(dst));
    ASSERT(packet_parse_publish(&pkt, &dst) == 0,    "max-payload publish parse succeeds");
    ASSERT_EQ(dst.payload_len, MQTT_PAYLOAD_MAX,     "max-payload length preserved");
    ASSERT(memcmp(dst.payload, big.payload, MQTT_PAYLOAD_MAX) == 0,
           "max-payload content preserved");
}

static void test_parse_publish_null_byte(void)
{
    printf("\n--- parse_publish: null byte in topic rejected ---\n");

    /* Build a PUBLISH packet with a null byte embedded in the topic.
     * Wire: type=0x30, rem_len, topic_len=5 "ab\x00#\x00", payload */
    uint8_t topic_wire[] = { 'a', 'b', '\x00', '#', 'x' }; /* 5 bytes, null at [2] */
    /* packet body: topic_len(2) + topic(5) + payload(1) */
    uint8_t body[10];
    body[0] = 0x00; body[1] = 5; /* topic length = 5 */
    memcpy(body + 2, topic_wire, 5);
    body[7] = 'X'; /* payload byte */

    mqtt_packet_t pkt = {0};
    pkt.type_flags = MQTT_PUBLISH;
    pkt.buf_len    = 8;
    memcpy(pkt.buf, body, 8);

    mqtt_publish_t pub = {0};
    ASSERT(packet_parse_publish(&pkt, &pub) < 0,
           "PUBLISH with null byte in topic rejected (MQTT §4.7.3)");

    /* topic without null byte should parse fine */
    uint8_t ok_topic[] = { 'a', 'b', 'c', '/', 'x' };
    body[0] = 0x00; body[1] = 5;
    memcpy(body + 2, ok_topic, 5);
    pkt.buf_len = 8;
    memcpy(pkt.buf, body, 8);
    memset(&pub, 0, sizeof(pub));
    ASSERT(packet_parse_publish(&pkt, &pub) == 0,
           "PUBLISH without null byte accepted");
    ASSERT(strcmp(pub.topic, "abc/x") == 0, "topic 'abc/x' parsed correctly");
}

static void test_parse_connect(void)
{
    printf("\n--- parse_connect ---\n");

    /*
     * Hand-craft a minimal CONNECT packet body (after fixed header):
     *   Protocol name: 0x00 0x04 'M' 'Q' 'T' 'T'
     *   Protocol level: 0x04
     *   Connect flags:  0x02  (clean session, no will, no auth)
     *   Keepalive:      0x00 0x3C  (60 s)
     *   Client ID:      0x00 0x04 't' 'e' 's' 't'
     */
    static const uint8_t body[] = {
        0x00, 0x04, 'M', 'Q', 'T', 'T',  /* protocol name */
        0x04,                              /* level */
        0x02,                              /* flags: clean session */
        0x00, 0x3C,                        /* keepalive = 60 */
        0x00, 0x04, 't', 'e', 's', 't'    /* client_id = "test" */
    };

    mqtt_packet_t pkt = {0};
    pkt.type_flags = MQTT_CONNECT;
    pkt.buf_len    = sizeof(body);
    memcpy(pkt.buf, body, sizeof(body));

    mqtt_connect_t conn = {0};
    ASSERT(packet_parse_connect(&pkt, &conn) == 0, "parse_connect basic succeeds");
    ASSERT(strcmp(conn.client_id, "test") == 0,    "parse_connect client_id = 'test'");
    ASSERT_EQ(conn.clean_session, 1, "parse_connect clean_session = 1");
    ASSERT_EQ(conn.keepalive, 60,    "parse_connect keepalive = 60");
    ASSERT_EQ(conn.has_will, 0,      "parse_connect no will");
    ASSERT_EQ(conn.has_username, 0,  "parse_connect no username");
    ASSERT_EQ(conn.has_password, 0,  "parse_connect no password");

    /*
     * CONNECT with will + username + password
     * flags = 0b11001110 = 0xCE
     *   bit7: username=1
     *   bit6: password=1
     *   bit5: will_retain=0
     *   bit4-3: will_qos=01
     *   bit2: will=1
     *   bit1: clean=1
     */
    static const uint8_t body2[] = {
        0x00, 0x04, 'M', 'Q', 'T', 'T',
        0x04,
        0xCE,                              /* flags */
        0x00, 0x1E,                        /* keepalive = 30 */
        0x00, 0x02, 'c', '1',             /* client_id = "c1" */
        0x00, 0x04, 'w', '/', 't', 'p',  /* will_topic = "w/tp" */
        0x00, 0x02, 'B', 'Y',             /* will_payload = "BY" */
        0x00, 0x04, 'u', 's', 'e', 'r',  /* username = "user" */
        0x00, 0x04, 'p', 'a', 's', 's'   /* password = "pass" */
    };

    pkt.buf_len = sizeof(body2);
    memcpy(pkt.buf, body2, sizeof(body2));
    memset(&conn, 0, sizeof(conn));

    ASSERT(packet_parse_connect(&pkt, &conn) == 0, "parse_connect with will+auth succeeds");
    ASSERT(strcmp(conn.client_id, "c1")   == 0, "parse_connect c1 client_id");
    ASSERT_EQ(conn.clean_session, 1,  "parse_connect c1 clean_session");
    ASSERT_EQ(conn.keepalive, 30,     "parse_connect c1 keepalive = 30");
    ASSERT_EQ(conn.has_will, 1,       "parse_connect c1 has_will");
    ASSERT(strcmp(conn.will_topic, "w/tp") == 0, "parse_connect c1 will_topic");
    ASSERT_EQ(conn.will_qos, 1,       "parse_connect c1 will_qos = 1");
    ASSERT_EQ(conn.has_username, 1,   "parse_connect c1 has_username");
    ASSERT(strcmp(conn.username, "user") == 0, "parse_connect c1 username");
    ASSERT_EQ(conn.has_password, 1,   "parse_connect c1 has_password");
    ASSERT(strcmp(conn.password, "pass") == 0, "parse_connect c1 password");

    /* wrong protocol level → error */
    uint8_t body3[sizeof(body)];
    memcpy(body3, body, sizeof(body));
    body3[6] = 0x03; /* level 3 */
    pkt.buf_len = sizeof(body3);
    memcpy(pkt.buf, body3, sizeof(body3));
    ASSERT(packet_parse_connect(&pkt, &conn) < 0, "parse_connect wrong proto level → error");

    /* truncated → error */
    pkt.buf_len = 5;
    memcpy(pkt.buf, body, 5);
    ASSERT(packet_parse_connect(&pkt, &conn) < 0, "parse_connect truncated → error");
}

static void test_parse_subscribe(void)
{
    printf("\n--- parse_subscribe ---\n");

    /*
     * SUBSCRIBE payload:
     *   packet_id: 0x00 0x07
     *   topic 1: 0x00 0x06 "a/b/c#" QoS 1
     *   topic 2: 0x00 0x03 "x/+" QoS 0
     */
    static const uint8_t body[] = {
        0x00, 0x07,
        0x00, 0x06, 'a', '/', 'b', '/', 'c', '#', 0x01,
        0x00, 0x03, 'x', '/', '+', 0x00
    };

    mqtt_packet_t pkt = {0};
    pkt.type_flags = MQTT_SUBSCRIBE | 0x02;
    pkt.buf_len    = sizeof(body);
    memcpy(pkt.buf, body, sizeof(body));

    uint16_t pid;
    char     topics[4][MQTT_TOPIC_MAX];
    uint8_t  qos[4];
    uint8_t  count = 0;

    ASSERT(packet_parse_subscribe(&pkt, &pid, topics, qos, &count, 4) == 0,
           "parse_subscribe succeeds");
    ASSERT_EQ(pid,   0x0007, "subscribe packet_id = 7");
    ASSERT_EQ(count, 2,      "subscribe count = 2");
    ASSERT(strcmp(topics[0], "a/b/c#") == 0, "subscribe topic[0] = a/b/c#");
    ASSERT_EQ(qos[0], 1, "subscribe qos[0] = 1");
    ASSERT(strcmp(topics[1], "x/+") == 0, "subscribe topic[1] = x/+");
    ASSERT_EQ(qos[1], 0, "subscribe qos[1] = 0");
}

static void test_parse_subscribe_edge(void)
{
    printf("\n--- parse_subscribe edge cases ---\n");

    mqtt_packet_t pkt = {0};
    uint16_t pid; char topics[4][MQTT_TOPIC_MAX]; uint8_t qos[4]; uint8_t count;

    /* empty payload (only packet_id) — count should be 0 */
    pkt.type_flags = MQTT_SUBSCRIBE | 0x02;
    pkt.buf[0] = 0x00; pkt.buf[1] = 0x01; /* packet_id = 1 */
    pkt.buf_len = 2;
    count = 99;
    ASSERT(packet_parse_subscribe(&pkt, &pid, topics, qos, &count, 4) == 0,
           "subscribe empty payload parses ok");
    ASSERT_EQ(count, 0, "subscribe empty payload: count = 0");
    ASSERT_EQ(pid,   1, "subscribe empty payload: packet_id = 1");

    /* truncated packet (only 1 byte) */
    pkt.buf_len = 1;
    ASSERT(packet_parse_subscribe(&pkt, &pid, topics, qos, &count, 4) < 0,
           "subscribe truncated → error");

    /* qos field masked: wire qos=0xFF is masked to 0x03 (reserved bits) */
    static const uint8_t body_qos3[] = {
        0x00, 0x05,                          /* packet_id = 5 */
        0x00, 0x03, 'a', '/', 'b', 0xFF      /* topic "a/b" with qos byte 0xFF */
    };
    pkt.buf_len = sizeof(body_qos3);
    memcpy(pkt.buf, body_qos3, sizeof(body_qos3));
    count = 0;
    ASSERT(packet_parse_subscribe(&pkt, &pid, topics, qos, &count, 4) == 0,
           "subscribe qos-byte masked succeeds");
    ASSERT_EQ(count,  1,    "subscribe qos-masked: count = 1");
    ASSERT_EQ(qos[0], 0x03, "subscribe qos-masked: qos[0] masked to 0x03");
}

static void test_parse_unsubscribe(void)
{
    printf("\n--- parse_unsubscribe ---\n");

    /* Build a minimal UNSUBSCRIBE packet: pkt_id=3, two topics */
    const char *t0 = "a/b";
    const char *t1 = "x/#";
    uint16_t  l0 = (uint16_t)strlen(t0);
    uint16_t  l1 = (uint16_t)strlen(t1);
    /* Variable header: pkt_id(2) + 2+t0 + 2+t1 */
    uint32_t rem_len = 2 + 2 + l0 + 2 + l1;
    uint8_t rem_enc[4]; size_t rem_bytes;
    packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes);

    uint8_t raw[64]; size_t p = 0;
    raw[p++] = MQTT_UNSUBSCRIBE | 0x02;
    memcpy(raw + p, rem_enc, rem_bytes); p += rem_bytes;
    raw[p++] = 0; raw[p++] = 3; /* pkt_id = 3 */
    raw[p++] = (uint8_t)(l0 >> 8); raw[p++] = (uint8_t)(l0 & 0xFF);
    memcpy(raw + p, t0, l0); p += l0;
    raw[p++] = (uint8_t)(l1 >> 8); raw[p++] = (uint8_t)(l1 & 0xFF);
    memcpy(raw + p, t1, l1); p += l1;

    mqtt_packet_t pkt;
    pkt.type_flags    = raw[0];
    pkt.remaining_len = rem_len;
    memcpy(pkt.buf, raw + 1 + rem_bytes, rem_len);
    pkt.buf_len       = rem_len;

    uint16_t pid; char topics[4][MQTT_TOPIC_MAX]; uint8_t count;
    int rc = packet_parse_unsubscribe(&pkt, &pid, topics, &count, 4);
    ASSERT(rc == 0,                              "parse_unsubscribe succeeds");
    ASSERT_EQ(pid,   3,                          "unsubscribe packet_id = 3");
    ASSERT_EQ(count, 2,                          "unsubscribe count = 2");
    ASSERT(strcmp(topics[0], "a/b") == 0,        "unsubscribe topic[0] = a/b");
    ASSERT(strcmp(topics[1], "x/#") == 0,        "unsubscribe topic[1] = x/#");

    /* truncated packet */
    pkt.buf_len = 1; /* only packet_id hi byte */
    ASSERT(packet_parse_unsubscribe(&pkt, &pid, topics, &count, 4) < 0,
           "parse_unsubscribe truncated → error");

    /* single topic */
    p = 0;
    const char *t2 = "single/topic";
    uint16_t l2 = (uint16_t)strlen(t2);
    rem_len = 2 + 2 + l2;
    packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes);
    raw[p++] = MQTT_UNSUBSCRIBE | 0x02;
    memcpy(raw + p, rem_enc, rem_bytes); p += rem_bytes;
    raw[p++] = 0; raw[p++] = 99;
    raw[p++] = (uint8_t)(l2 >> 8); raw[p++] = (uint8_t)(l2 & 0xFF);
    memcpy(raw + p, t2, l2);
    pkt.type_flags    = raw[0];
    pkt.remaining_len = rem_len;
    memcpy(pkt.buf, raw + 1 + rem_bytes, rem_len);
    pkt.buf_len       = rem_len;
    rc = packet_parse_unsubscribe(&pkt, &pid, topics, &count, 4);
    ASSERT(rc == 0,                          "parse_unsubscribe single topic");
    ASSERT_EQ(pid,   99,                     "unsubscribe single pkt_id = 99");
    ASSERT_EQ(count, 1,                      "unsubscribe single count = 1");
    ASSERT(strcmp(topics[0], t2) == 0,       "unsubscribe single topic correct");
}

static void test_parse_connect_edge(void)
{
    printf("\n--- parse_connect edge cases ---\n");

    /* Helper: build a CONNECT body with a given client ID */
    /* Header bytes: proto(6) + level(1) + flags(1) + keepalive(2) = 10 bytes */
    static const uint8_t hdr[] = {
        0x00, 0x04, 'M', 'Q', 'T', 'T',  /* protocol name */
        0x04,                              /* level */
        0x02,                              /* flags: clean session */
        0x00, 0x3C                         /* keepalive = 60 */
    };

    uint8_t body[128];
    mqtt_packet_t pkt = {0};
    mqtt_connect_t conn;
    size_t p;

    /* ── zero-length client ID (MQTT 3.1.1 §3.1.3.1: MUST allow with clean=1) */
    p = 0;
    memcpy(body + p, hdr, sizeof(hdr)); p += sizeof(hdr);
    body[p++] = 0x00; body[p++] = 0x00; /* client_id length = 0 */
    pkt.type_flags = MQTT_CONNECT;
    pkt.buf_len    = p;
    memcpy(pkt.buf, body, p);
    memset(&conn, 0, sizeof(conn));
    ASSERT(packet_parse_connect(&pkt, &conn) == 0,
           "zero-length client ID accepted");
    ASSERT(conn.client_id[0] == '\0',
           "zero-length client ID produces empty string");

    /* ── 23-char client ID — maximum allowed per MQTT_CLIENT_ID_MAX-1 */
    char id23[24];
    memset(id23, 'A', 23); id23[23] = '\0';
    p = 0;
    memcpy(body + p, hdr, sizeof(hdr)); p += sizeof(hdr);
    body[p++] = 0x00; body[p++] = 23;
    memcpy(body + p, id23, 23); p += 23;
    pkt.buf_len = p;
    memcpy(pkt.buf, body, p);
    memset(&conn, 0, sizeof(conn));
    ASSERT(packet_parse_connect(&pkt, &conn) == 0,
           "23-char client ID accepted");
    ASSERT(strncmp(conn.client_id, id23, 23) == 0,
           "23-char client ID stored correctly");

    /* ── 24-char client ID — one byte over MQTT_CLIENT_ID_MAX-1: must fail */
    p = 0;
    memcpy(body + p, hdr, sizeof(hdr)); p += sizeof(hdr);
    body[p++] = 0x00; body[p++] = 24;
    memset(body + p, 'B', 24); p += 24;
    pkt.buf_len = p;
    memcpy(pkt.buf, body, p);
    memset(&conn, 0, sizeof(conn));
    ASSERT(packet_parse_connect(&pkt, &conn) < 0,
           "24-char client ID rejected (> MQTT_CLIENT_ID_MAX-1)");

    /* ── CONNECT flags: will_retain set but has_will=0 — not a parse error */
    p = 0;
    memcpy(body + p, hdr, sizeof(hdr)); p += sizeof(hdr);
    body[p - 2] = 0x22; /* flags: retain bit set (bit5) but will bit (bit2) = 0 */
    body[p++] = 0x00; body[p++] = 0x02; /* client_id = "ab" */
    body[p++] = 'a'; body[p++] = 'b';
    pkt.buf_len = p;
    memcpy(pkt.buf, body, p);
    /* restore keepalive (body[-2] was changed above — fix header in-place) */
    pkt.buf[sizeof(hdr) - 2] = 0x00; /* keepalive hi */
    pkt.buf[sizeof(hdr) - 1] = 0x3C; /* keepalive lo */
    pkt.buf[sizeof(hdr)]     = 0x22; /* flags: bit5=retain, bit2=0 → no will */
    pkt.buf_len = sizeof(hdr) + 1 + 2 + 2; /* hdr + flags remap + client_id_len + "ab" */
    /* Rebuild cleanly */
    p = 0;
    memcpy(body, hdr, sizeof(hdr));
    body[sizeof(hdr) - 3] = 0x22; /* overwrite flags byte (offset 7 in hdr) */
    p = sizeof(hdr);
    body[p++] = 0x00; body[p++] = 0x02; body[p++] = 'a'; body[p++] = 'b';
    pkt.buf_len = p;
    memcpy(pkt.buf, body, p);
    memset(&conn, 0, sizeof(conn));
    /* MQTT 3.1.1 §3.1.2.4: will_retain=1 with has_will=0 is a protocol error */
    ASSERT(packet_parse_connect(&pkt, &conn) < 0,
           "will_retain=1 with has_will=0: parse fails (§3.1.2.4)");

    /* valid: will_retain=0 with has_will=0 */
    p = 0;
    memcpy(body, hdr, sizeof(hdr));
    body[sizeof(hdr) - 3] = 0x02; /* flags: clean_session=1, no will, no retain */
    p = sizeof(hdr);
    body[p++] = 0x00; body[p++] = 0x02; body[p++] = 'a'; body[p++] = 'b';
    pkt.buf_len = p;
    memcpy(pkt.buf, body, p);
    memset(&conn, 0, sizeof(conn));
    ASSERT(packet_parse_connect(&pkt, &conn) == 0,
           "no-will no-retain: parse succeeds");
    ASSERT_EQ(conn.has_will, 0, "no-will: has_will=0 confirmed");

    /* MQTT 3.1.1 §3.1.2.1: reserved bit 0 must be 0 */
    p = 0;
    memcpy(body, hdr, sizeof(hdr));
    body[sizeof(hdr) - 3] = 0x03; /* flags: reserved bit set */
    p = sizeof(hdr);
    body[p++] = 0x00; body[p++] = 0x02; body[p++] = 'c'; body[p++] = 'd';
    pkt.buf_len = p;
    memcpy(pkt.buf, body, p);
    memset(&conn, 0, sizeof(conn));
    ASSERT(packet_parse_connect(&pkt, &conn) < 0,
           "reserved bit 0 set: parse fails (§3.1.2.1)");

    /* MQTT 3.1.1 §3.1.2.9: password flag requires username flag */
    p = 0;
    memcpy(body, hdr, sizeof(hdr));
    body[sizeof(hdr) - 3] = 0x42; /* flags: password=1, username=0, clean=1 */
    p = sizeof(hdr);
    body[p++] = 0x00; body[p++] = 0x02; body[p++] = 'e'; body[p++] = 'f';
    pkt.buf_len = p;
    memcpy(pkt.buf, body, p);
    memset(&conn, 0, sizeof(conn));
    ASSERT(packet_parse_connect(&pkt, &conn) < 0,
           "password without username: parse fails (§3.1.2.9)");

    /* ── MQTT 3.1.1 §3.1.2.1: protocol name must be exactly "MQTT" */
    /* wrong case "MQTt" → error */
    {
        uint8_t bad_name[] = {
            0x00, 0x04, 'M', 'Q', 'T', 't',   /* wrong: 't' not 'T' */
            0x04, 0x02, 0x00, 0x3C,             /* level, flags, keepalive */
            0x00, 0x02, 'g', 'h'               /* client_id */
        };
        pkt.buf_len = sizeof(bad_name);
        memcpy(pkt.buf, bad_name, sizeof(bad_name));
        memset(&conn, 0, sizeof(conn));
        ASSERT(packet_parse_connect(&pkt, &conn) < 0,
               "wrong proto name 'MQTt': parse fails (§3.1.2.1)");
    }

    /* MQTT 3.1 name "MQIsdp" → error (we are strict MQTT 3.1.1 only) */
    {
        uint8_t mqisdp[] = {
            0x00, 0x06, 'M', 'Q', 'I', 's', 'd', 'p',  /* MQTT 3.1 name */
            0x03, 0x02, 0x00, 0x3C,                      /* level=3, flags, keepalive */
            0x00, 0x02, 'i', 'j'                         /* client_id */
        };
        pkt.buf_len = sizeof(mqisdp);
        memcpy(pkt.buf, mqisdp, sizeof(mqisdp));
        memset(&conn, 0, sizeof(conn));
        ASSERT(packet_parse_connect(&pkt, &conn) < 0,
               "MQTT 3.1 'MQIsdp' name: parse fails (§3.1.2.1)");
    }

    /* correct "MQTT" name with level 4 still accepted */
    {
        uint8_t good[] = {
            0x00, 0x04, 'M', 'Q', 'T', 'T',
            0x04, 0x02, 0x00, 0x3C,
            0x00, 0x02, 'k', 'l'
        };
        pkt.buf_len = sizeof(good);
        memcpy(pkt.buf, good, sizeof(good));
        memset(&conn, 0, sizeof(conn));
        ASSERT(packet_parse_connect(&pkt, &conn) == 0,
               "correct 'MQTT' proto name + level 4: parse succeeds");
    }
}

/* ── main ──────────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== unit_packet tests ===\n");

    test_remaining_len_encode();
    test_remaining_len_decode();
    test_remaining_len_roundtrip();
    test_build_connack();
    test_build_ack_packets();
    test_build_pingresp();
    test_build_suback();
    test_build_parse_publish_roundtrip();
    test_parse_publish_null_byte();
    test_parse_connect();
    test_parse_connect_edge();
    test_parse_subscribe();
    test_parse_subscribe_edge();
    test_parse_unsubscribe();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
