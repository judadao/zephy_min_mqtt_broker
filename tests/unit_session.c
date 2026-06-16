/*
 * unit_session — unit tests for session.c
 * Tests: create/find/delete, save_subs, enqueue/overflow,
 *        offline_publish (QoS 0 dropped, QoS 1 queued), drain.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "session.h"
#include "client.h"

/* ── stubs for client functions session.c depends on ──────────────────────── */

static int      stub_send_count;
static int      stub_inflight_count;
static char     stub_last_topic[MQTT_TOPIC_MAX];
static uint8_t  stub_last_payload[MQTT_PAYLOAD_MAX];
static uint16_t stub_last_payload_len;

int client_send(client_t *c, const uint8_t *buf, size_t len)
{
    (void)c;
    stub_send_count++;
    /* Parse the PUBLISH type_flags to extract the topic from the wire format */
    if (len >= 2 && (buf[0] & 0xF0) == MQTT_PUBLISH) {
        mqtt_packet_t pkt;
        pkt.type_flags = buf[0];
        /* Decode remaining length */
        uint32_t rlen = 0, mult = 1; size_t pos = 1;
        uint8_t b;
        do {
            if (pos >= len) return 0;
            b = buf[pos++];
            rlen += (b & 0x7F) * mult;
            mult *= 128;
        } while (b & 0x80);
        pkt.remaining_len = rlen;
        if (pos + rlen > len) return 0;
        memcpy(pkt.buf, buf + pos, rlen);
        pkt.buf_len = rlen;
        mqtt_publish_t pub;
        if (packet_parse_publish(&pkt, &pub) == 0) {
            strncpy(stub_last_topic,   pub.topic, MQTT_TOPIC_MAX - 1);
            memcpy(stub_last_payload,  pub.payload, pub.payload_len);
            stub_last_payload_len = pub.payload_len;
        }
    }
    return 0;
}

void client_inflight_store(client_t *c, uint16_t id,
                            const uint8_t *buf, uint16_t blen, uint8_t qos)
{
    (void)c; (void)id; (void)buf; (void)blen; (void)qos;
    stub_inflight_count++;
}

static void stub_reset(void)
{
    stub_send_count       = 0;
    stub_inflight_count   = 0;
    stub_last_topic[0]    = '\0';
    stub_last_payload[0]  = '\0';
    stub_last_payload_len = 0;
}

/* ── tiny test harness ────────────────────────────────────────────────────── */

static int pass_count, fail_count;
#define CHECK(label, cond) do { \
    if (cond) { printf("  [PASS] %s\n", label); pass_count++; } \
    else       { printf("  [FAIL] %s\n", label); fail_count++; } \
} while(0)

/* match function used by session_offline_publish */
static int exact_match(const char *filter, const char *topic)
{
    return strcmp(filter, topic) == 0;
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void test_create_find_delete(void)
{
    printf("\n--- session create / find / delete ---\n");
    session_init();

    session_t *s = session_create("client1");
    CHECK("create returns non-NULL",      s != NULL);
    CHECK("create: client_id set",        strcmp(s->client_id, "client1") == 0);
    CHECK("create: in_use = 1",           s->in_use == 1);
    CHECK("create: offline = 0",          s->offline == 0);

    session_t *f = session_find("client1");
    CHECK("find returns same pointer",    f == s);

    session_t *nf = session_find("nobody");
    CHECK("find unknown returns NULL",    nf == NULL);

    session_delete("client1");
    CHECK("delete: find returns NULL",    session_find("client1") == NULL);

    /* delete non-existent is a no-op */
    session_delete("nobody"); /* should not crash */
    CHECK("delete non-existent: no crash", 1);
}

static void test_session_max(void)
{
    printf("\n--- session pool exhaustion ---\n");
    session_init();

    for (int i = 0; i < SESSION_MAX; i++) {
        char id[32]; snprintf(id, sizeof(id), "pool_%d", i);
        session_t *s = session_create(id);
        (void)s;
    }
    session_t *overflow = session_create("overflow");
    CHECK("create beyond SESSION_MAX returns NULL", overflow == NULL);

    /* free one slot and retry */
    session_delete("pool_0");
    session_t *retry = session_create("after_delete");
    CHECK("create after delete succeeds", retry != NULL);
}

static void test_save_subs(void)
{
    printf("\n--- session_save_subs ---\n");
    session_init();
    session_t *s = session_create("sub_client");

    char filters[SESSION_SUB_MAX][MQTT_TOPIC_MAX];
    uint8_t qos[SESSION_SUB_MAX];
    strncpy(filters[0], "a/b", MQTT_TOPIC_MAX - 1);
    strncpy(filters[1], "x/#", MQTT_TOPIC_MAX - 1);
    qos[0] = 1; qos[1] = 2;

    session_save_subs(s, (const char (*)[MQTT_TOPIC_MAX])filters, qos, 2);
    CHECK("sub_count = 2",                  s->sub_count == 2);
    CHECK("filter[0] = a/b",                strcmp(s->sub_filters[0], "a/b") == 0);
    CHECK("filter[1] = x/#",                strcmp(s->sub_filters[1], "x/#") == 0);
    CHECK("qos[0] = 1",                     s->sub_qos[0] == 1);
    CHECK("qos[1] = 2",                     s->sub_qos[1] == 2);
    CHECK("offline set to 1 by save_subs",  s->offline == 1);

    /* save more than SESSION_SUB_MAX — should be clamped */
    uint8_t qos8[8] = {0,1,0,1,0,1,0,1};
    char filters8[8][MQTT_TOPIC_MAX];
    for (int i = 0; i < 8; i++) snprintf(filters8[i], MQTT_TOPIC_MAX, "t/%d", i);
    session_save_subs(s, (const char (*)[MQTT_TOPIC_MAX])filters8, qos8, 8);
    CHECK("sub_count clamped to SESSION_SUB_MAX", s->sub_count == SESSION_SUB_MAX);
}

static void test_enqueue(void)
{
    printf("\n--- session_enqueue ---\n");
    session_init();
    session_t *s = session_create("eq_client");
    s->offline = 1;

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, "t/eq", MQTT_TOPIC_MAX - 1);
    pub.payload[0] = 'A'; pub.payload_len = 1; pub.qos = 1;

    int r0 = session_enqueue(s, &pub, 1);
    CHECK("first enqueue succeeds (rc=0)", r0 == 0);
    CHECK("first entry in_use",            s->queue[0].in_use == 1);
    CHECK("first entry topic",             strcmp(s->queue[0].topic, "t/eq") == 0);
    CHECK("first entry packet_id = 1",     s->queue[0].packet_id == 1);

    strncpy(pub.topic, "t/eq2", MQTT_TOPIC_MAX - 1);
    int r1 = session_enqueue(s, &pub, 2);
    CHECK("second enqueue (at SESSION_QUEUE_MAX-1) ok", SESSION_QUEUE_MAX < 2 || r1 == 0);

    /* Fill queue to overflow */
    int overflow = 0;
    for (int i = 0; i < SESSION_QUEUE_MAX + 5; i++) {
        strncpy(pub.topic, "t/over", MQTT_TOPIC_MAX - 1);
        if (session_enqueue(s, &pub, (uint16_t)(10 + i)) < 0) {
            overflow = 1;
            break;
        }
    }
    CHECK("enqueue returns -1 when queue full", overflow);
}

static void test_offline_publish(void)
{
    printf("\n--- session_offline_publish ---\n");
    session_init();
    session_t *s = session_create("offline_c");

    char filters[1][MQTT_TOPIC_MAX];
    uint8_t qos[1];
    strncpy(filters[0], "off/t", MQTT_TOPIC_MAX - 1);
    qos[0] = 1;
    session_save_subs(s, (const char (*)[MQTT_TOPIC_MAX])filters, qos, 1);
    /* save_subs sets offline=1 */

    /* QoS 0 publish — should NOT be queued */
    mqtt_publish_t pub0 = {0};
    strncpy(pub0.topic, "off/t", MQTT_TOPIC_MAX - 1);
    pub0.payload[0] = 'X'; pub0.payload_len = 1; pub0.qos = 0;
    int n0 = session_offline_publish(&pub0, exact_match);
    CHECK("QoS 0 not queued (returns 0)", n0 == 0);
    CHECK("QoS 0: queue empty",           !s->queue[0].in_use);

    /* QoS 1 publish — should be queued */
    mqtt_publish_t pub1 = {0};
    strncpy(pub1.topic, "off/t", MQTT_TOPIC_MAX - 1);
    pub1.payload[0] = 'Y'; pub1.payload_len = 1; pub1.qos = 1;
    int n1 = session_offline_publish(&pub1, exact_match);
    CHECK("QoS 1 queued (returns 1)",     n1 == 1);
    CHECK("QoS 1: queue entry in_use",    s->queue[0].in_use);
    CHECK("QoS 1: topic correct",         strcmp(s->queue[0].topic, "off/t") == 0);

    /* Non-matching topic — should not be queued */
    mqtt_publish_t pub2 = {0};
    strncpy(pub2.topic, "other/topic", MQTT_TOPIC_MAX - 1);
    pub2.payload[0] = 'Z'; pub2.payload_len = 1; pub2.qos = 1;
    session_t *s2 = session_create("offline_c2");
    strncpy(filters[0], "off/t", MQTT_TOPIC_MAX - 1);
    qos[0] = 1;
    session_save_subs(s2, (const char (*)[MQTT_TOPIC_MAX])filters, qos, 1);
    int n2 = session_offline_publish(&pub2, exact_match);
    CHECK("non-matching topic not queued", n2 == 0);

    /* Online session — should not receive offline messages.
     * Reset the pool so only s3 is present, making n3 unambiguously 0. */
    session_init(); /* clears s and s2 */
    session_t *s3 = session_create("online_c");
    strncpy(filters[0], "off/t", MQTT_TOPIC_MAX - 1);
    qos[0] = 1;
    session_save_subs(s3, (const char (*)[MQTT_TOPIC_MAX])filters, qos, 1);
    s3->offline = 0; /* override — this client is "online" */
    int n3 = session_offline_publish(&pub1, exact_match);
    CHECK("online client not queued (returns 0)", n3 == 0);
    int s3_queued = 0;
    for (int i = 0; i < SESSION_QUEUE_MAX; i++) {
        if (s3->queue[i].in_use) { s3_queued = 1; break; }
    }
    CHECK("online client queue stays empty", !s3_queued);
}

static void test_drain(void)
{
    printf("\n--- session_drain ---\n");
    session_init();
    stub_reset();

    session_t *s = session_create("drain_c");
    mqtt_publish_t pub = {0};
    strncpy(pub.topic, "dr/t", MQTT_TOPIC_MAX - 1);
    pub.payload[0] = 'D'; pub.payload_len = 1; pub.qos = 1;
    session_enqueue(s, &pub, 42);

    /* We need a fake client_t (only fd matters for client_send stub) */
    client_t fake_client;
    memset(&fake_client, 0, sizeof(fake_client));
    fake_client.fd = -1; /* stub doesn't use fd */

    session_drain(s, &fake_client);
    CHECK("drain: client_send called once",        stub_send_count == 1);
    CHECK("drain: topic delivered",                strcmp(stub_last_topic, "dr/t") == 0);
    CHECK("drain: inflight stored for QoS1",       stub_inflight_count == 1);
    CHECK("drain: queue entry cleared after drain", !s->queue[0].in_use);

    /* Drain an empty queue — no sends */
    stub_reset();
    session_drain(s, &fake_client);
    CHECK("drain empty queue: no sends", stub_send_count == 0);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== unit_session tests ===\n");

    test_create_find_delete();
    test_session_max();
    test_save_subs();
    test_enqueue();
    test_offline_publish();
    test_drain();

    printf("\n=== Results: %d passed, %d failed ===\n", pass_count, fail_count);
    return (fail_count > 0) ? 1 : 0;
}
