/*
 * unit_topic — unit tests for topic.c (subscribe/unsubscribe/retain/fan-out)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "topic.h"
#include "session.h"
#include "client.h"
#include "packet.h"

/* ── stubs ───────────────────────────────────────────────────────────────── */

/* Counts and last-received fields set by the fan-out path */
static int      stub_send_total;
static char     stub_recv_topics[8][MQTT_TOPIC_MAX];
static char     stub_recv_payloads[8][MQTT_PAYLOAD_MAX + 1];
static uint8_t  stub_recv_retain[8];
static int      stub_recv_count;
static int      stub_inflight_count;

int client_send(client_t *c, const uint8_t *buf, size_t len)
{
    (void)c;
    stub_send_total++;
    if ((buf[0] & 0xF0) == MQTT_PUBLISH && stub_recv_count < 8) {
        mqtt_packet_t pkt;
        pkt.type_flags = buf[0];
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
            strncpy(stub_recv_topics[stub_recv_count],   pub.topic,
                    MQTT_TOPIC_MAX - 1);
            pub.payload[pub.payload_len] = '\0';
            strncpy(stub_recv_payloads[stub_recv_count], (char *)pub.payload,
                    MQTT_PAYLOAD_MAX);
            stub_recv_retain[stub_recv_count] = pub.retain;
            stub_recv_count++;
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

int session_offline_publish(const mqtt_publish_t *pub,
                             int (*match_fn)(const char *, const char *))
{
    (void)pub; (void)match_fn;
    return 0; /* stub: no offline sessions in topic unit tests */
}

static void stub_reset(void)
{
    stub_send_total    = 0;
    stub_recv_count    = 0;
    stub_inflight_count = 0;
    memset(stub_recv_topics,   0, sizeof(stub_recv_topics));
    memset(stub_recv_payloads, 0, sizeof(stub_recv_payloads));
    memset(stub_recv_retain,   0, sizeof(stub_recv_retain));
}

/* ── fake clients ─────────────────────────────────────────────────────────── */

static client_t clients[4];

static void clients_init(void)
{
    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < 4; i++) {
        clients[i].slot = (uint8_t)i;
        clients[i].fd   = i + 10; /* non-negative fd value */
    }
}

/* ── harness ─────────────────────────────────────────────────────────────── */

static int pass_count, fail_count;
#define CHECK(label, cond) do { \
    if (cond) { printf("  [PASS] %s\n", label); pass_count++; } \
    else       { printf("  [FAIL] %s\n", label); fail_count++; } \
} while(0)

/* returns 1 if stub received a pub to topic with payload */
static int stub_received(const char *topic, const char *payload)
{
    for (int i = 0; i < stub_recv_count; i++) {
        if (strcmp(stub_recv_topics[i], topic) == 0 &&
            strcmp(stub_recv_payloads[i], payload) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void test_subscribe(void)
{
    printf("\n--- topic_subscribe ---\n");
    topic_init();
    clients_init();

    int r = topic_subscribe(&clients[0], "a/b", 0);
    CHECK("subscribe succeeds",       r == 0);

    char out[1][MQTT_TOPIC_MAX]; uint8_t oq[1];
    int n = topic_get_client_subs(&clients[0], out, oq, 1);
    CHECK("get_client_subs count=1",  n == 1);
    CHECK("filter stored",            strcmp(out[0], "a/b") == 0);
    CHECK("qos stored",               oq[0] == 0);

    /* duplicate subscribe updates QoS */
    topic_subscribe(&clients[0], "a/b", 1);
    char out2[1][MQTT_TOPIC_MAX]; uint8_t oq2[1];
    topic_get_client_subs(&clients[0], out2, oq2, 1);
    CHECK("duplicate sub updates qos", oq2[0] == 1);
}

static void test_unsubscribe(void)
{
    printf("\n--- topic_unsubscribe ---\n");
    topic_init();
    clients_init();

    topic_subscribe(&clients[0], "x/y", 0);
    topic_unsubscribe(&clients[0], "x/y");

    char out[1][MQTT_TOPIC_MAX]; uint8_t oq[1];
    int n = topic_get_client_subs(&clients[0], out, oq, 1);
    CHECK("after unsubscribe, count=0", n == 0);

    /* unsubscribing a filter we never subscribed is a no-op */
    topic_unsubscribe(&clients[0], "never/subscribed"); /* should not crash */
    CHECK("unsub non-existent: no crash", 1);
}

static void test_unsubscribe_all(void)
{
    printf("\n--- topic_unsubscribe_all ---\n");
    topic_init();
    clients_init();

    topic_subscribe(&clients[0], "t/1", 0);
    topic_subscribe(&clients[0], "t/2", 0);
    topic_subscribe(&clients[1], "t/1", 0); /* different client */

    topic_unsubscribe_all(&clients[0]);

    char out[4][MQTT_TOPIC_MAX]; uint8_t oq[4];
    int n0 = topic_get_client_subs(&clients[0], out, oq, 4);
    int n1 = topic_get_client_subs(&clients[1], out, oq, 4);
    CHECK("client[0] has 0 subs after unsubscribe_all", n0 == 0);
    CHECK("client[1] still subscribed",                 n1 == 1);
}

static void test_retain_store_clear(void)
{
    printf("\n--- retain store / clear ---\n");
    topic_init();
    clients_init();
    stub_reset();

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, "ret/x", MQTT_TOPIC_MAX - 1);
    pub.payload[0] = 'A'; pub.payload_len = 1; pub.qos = 0; pub.retain = 1;
    topic_publish(&pub);

    /* Subscribe and call deliver_retained */
    topic_subscribe(&clients[0], "ret/x", 0);
    stub_reset();
    topic_deliver_retained(&clients[0], "ret/x", 0);
    CHECK("retained message delivered on subscribe", stub_recv_count == 1);
    CHECK("retained topic correct", stub_received("ret/x", "A"));

    /* Clear with empty payload */
    mqtt_publish_t clr = {0};
    strncpy(clr.topic, "ret/x", MQTT_TOPIC_MAX - 1);
    clr.retain = 1; clr.payload_len = 0; /* empty = clear */
    topic_publish(&clr);

    stub_reset();
    topic_deliver_retained(&clients[0], "ret/x", 0);
    CHECK("retained cleared: no delivery after empty pub", stub_recv_count == 0);
}

static void test_retain_overflow(void)
{
    printf("\n--- retain slot exhaustion ---\n");
    topic_init();
    clients_init();

    for (int i = 0; i < TOPIC_RETAIN_MAX; i++) {
        char t[32]; snprintf(t, sizeof(t), "ret/%d", i);
        mqtt_publish_t p = {0};
        strncpy(p.topic, t, MQTT_TOPIC_MAX - 1);
        p.payload[0] = (uint8_t)('0' + i); p.payload_len = 1;
        p.retain = 1;
        topic_publish(&p);
    }
    /* one more should silently drop (no crash) */
    mqtt_publish_t overflow = {0};
    strncpy(overflow.topic, "ret/overflow", MQTT_TOPIC_MAX - 1);
    overflow.payload[0] = 'X'; overflow.payload_len = 1; overflow.retain = 1;
    topic_publish(&overflow); /* should not crash */
    CHECK("retain overflow: no crash", 1);

    /* overwriting an existing retained topic does NOT use a new slot */
    mqtt_publish_t update = {0};
    strncpy(update.topic, "ret/0", MQTT_TOPIC_MAX - 1); /* already stored */
    update.payload[0] = 'Z'; update.payload_len = 1; update.retain = 1;
    topic_publish(&update); /* should succeed — same slot */

    topic_subscribe(&clients[0], "ret/0", 0);
    stub_reset();
    topic_deliver_retained(&clients[0], "ret/0", 0);
    CHECK("existing retained topic updated in place", stub_received("ret/0", "Z"));
}

static void test_fanout(void)
{
    printf("\n--- fan-out ---\n");
    topic_init();
    clients_init();
    stub_reset();

    topic_subscribe(&clients[0], "f/x", 0);
    topic_subscribe(&clients[1], "f/x", 0);
    topic_subscribe(&clients[2], "f/y", 0); /* different topic */

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, "f/x", MQTT_TOPIC_MAX - 1);
    pub.payload[0] = 'M'; pub.payload[1] = 'S'; pub.payload[2] = 'G';
    pub.payload_len = 3; pub.qos = 0; pub.retain = 0;
    topic_publish(&pub);

    CHECK("fan-out: 2 sends for 2 subscribers on f/x", stub_send_total == 2);
    CHECK("client[2] on f/y did not receive f/x", stub_recv_count == 2);

    stub_reset();
    mqtt_publish_t pub2 = {0};
    strncpy(pub2.topic, "f/y", MQTT_TOPIC_MAX - 1);
    pub2.payload[0] = 'Y'; pub2.payload_len = 1;
    topic_publish(&pub2);
    CHECK("only client[2] receives f/y", stub_send_total == 1);
}

static void test_fanout_wildcard(void)
{
    printf("\n--- fan-out wildcard ---\n");
    topic_init();
    clients_init();
    stub_reset();

    topic_subscribe(&clients[0], "wc/#", 0);
    topic_subscribe(&clients[1], "wc/+", 0);
    topic_subscribe(&clients[2], "wc/a/b", 0);

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, "wc/a/b", MQTT_TOPIC_MAX - 1);
    pub.payload[0] = 'W'; pub.payload_len = 1;
    topic_publish(&pub);

    /* wc/# matches wc/a/b → client[0] receives */
    /* wc/+ does NOT match wc/a/b (two levels) → client[1] does NOT receive */
    /* wc/a/b exact match → client[2] receives */
    CHECK("wc/# matched wc/a/b",         stub_received("wc/a/b", "W"));
    CHECK("wc/a/b exact match received",  stub_recv_count >= 2);
    /* client[1] on wc/+ should NOT have received wc/a/b */

    stub_reset();
    mqtt_publish_t pub2 = {0};
    strncpy(pub2.topic, "wc/x", MQTT_TOPIC_MAX - 1);
    pub2.payload[0] = 'X'; pub2.payload_len = 1;
    topic_publish(&pub2);
    /* wc/# and wc/+ both match wc/x */
    CHECK("wc/# and wc/+ both match single-level wc/x", stub_send_total == 2);
}

static void test_qos_downgrade(void)
{
    printf("\n--- QoS downgrade on delivery ---\n");
    topic_init();
    clients_init();
    stub_reset();

    /* Subscriber at QoS 0 — even a QoS 1 publish is delivered at QoS 0 */
    topic_subscribe(&clients[0], "qd/t", 0);

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, "qd/t", MQTT_TOPIC_MAX - 1);
    pub.payload[0] = 'Q'; pub.payload_len = 1; pub.qos = 1; pub.retain = 0;
    pub.packet_id = 77;
    topic_publish(&pub);

    /* client_send called once; no inflight stored because delivery QoS = 0 */
    CHECK("send called once",              stub_send_total == 1);
    CHECK("no inflight for QoS-downgrade", stub_inflight_count == 0);

    /* Subscriber at QoS 1, pub at QoS 0 — delivery at QoS 0 */
    stub_reset();
    topic_init();
    clients_init();
    topic_subscribe(&clients[0], "qd/t2", 1);
    mqtt_publish_t pub2 = {0};
    strncpy(pub2.topic, "qd/t2", MQTT_TOPIC_MAX - 1);
    pub2.payload[0] = 'Q'; pub2.payload_len = 1; pub2.qos = 0;
    topic_publish(&pub2);
    CHECK("qos0 pub to qos1 sub: no inflight", stub_inflight_count == 0);
    CHECK("qos0 pub to qos1 sub: delivered",   stub_send_total == 1);
}

static void test_retain_wildcard_deliver(void)
{
    printf("\n--- retained deliver on wildcard subscribe ---\n");
    topic_init();
    clients_init();

    /* Publish two retained messages on different sub-topics */
    mqtt_publish_t pa = {0};
    strncpy(pa.topic, "home/living/temp", MQTT_TOPIC_MAX - 1);
    pa.payload[0] = 'A'; pa.payload_len = 1; pa.qos = 0; pa.retain = 1;
    topic_publish(&pa);

    mqtt_publish_t pb = {0};
    strncpy(pb.topic, "home/kitchen/temp", MQTT_TOPIC_MAX - 1);
    pb.payload[0] = 'B'; pb.payload_len = 1; pb.qos = 0; pb.retain = 1;
    topic_publish(&pb);

    mqtt_publish_t pc = {0};
    strncpy(pc.topic, "other/sensor", MQTT_TOPIC_MAX - 1);
    pc.payload[0] = 'C'; pc.payload_len = 1; pc.qos = 0; pc.retain = 1;
    topic_publish(&pc);

    /* Subscribe with + wildcard — should get living/temp and kitchen/temp */
    topic_subscribe(&clients[0], "home/+/temp", 0);
    stub_reset();
    topic_deliver_retained(&clients[0], "home/+/temp", 0);
    CHECK("+ wildcard: 2 retained delivered", stub_recv_count == 2);
    CHECK("+ wildcard: living/temp delivered", stub_received("home/living/temp", "A"));
    CHECK("+ wildcard: kitchen/temp delivered", stub_received("home/kitchen/temp", "B"));
    CHECK("+ wildcard: other/sensor NOT delivered", !stub_received("other/sensor", "C"));

    /* Subscribe with # wildcard — should get all 3 */
    topic_subscribe(&clients[1], "home/#", 0);
    stub_reset();
    topic_deliver_retained(&clients[1], "home/#", 0);
    CHECK("# wildcard: 2 home/* retained delivered", stub_recv_count == 2);
    CHECK("# wildcard: living/temp delivered", stub_received("home/living/temp", "A"));
    CHECK("# wildcard: kitchen/temp delivered", stub_received("home/kitchen/temp", "B"));

    /* # from root — should get all 3 */
    topic_subscribe(&clients[2], "#", 0);
    stub_reset();
    topic_deliver_retained(&clients[2], "#", 0);
    CHECK("root # wildcard: 3 retained delivered", stub_recv_count == 3);
}

static void test_duplicate_subscription(void)
{
    printf("\n--- duplicate subscription: single delivery ---\n");
    topic_init();
    clients_init();
    stub_reset();

    /* Subscribe to same filter twice — only one subscription slot should be used */
    int r1 = topic_subscribe(&clients[0], "dup/t", 0);
    int r2 = topic_subscribe(&clients[0], "dup/t", 1); /* upgrade QoS */
    CHECK("second subscribe returns success", r2 == 0);
    (void)r1;

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, "dup/t", MQTT_TOPIC_MAX - 1);
    pub.payload[0] = 'X'; pub.payload_len = 1; pub.qos = 1;
    pub.packet_id = 10;
    topic_publish(&pub);

    /* The message must arrive exactly once, regardless of how many subscribe calls */
    CHECK("duplicate sub: message delivered exactly once", stub_recv_count == 1);
}

/* ── test_retain_flag_delivery ───────────────────────────────────────────── */
/* MQTT §3.3.1.3: retain=1 when delivering stored, retain=0 in fan-out */
static void test_retain_flag_delivery(void)
{
    printf("\n-- test_retain_flag_delivery --\n");
    topic_init();
    clients_init();
    stub_reset();

    /* publish with retain=1 */
    mqtt_publish_t pub = {0};
    strncpy(pub.topic, "retain/flag", sizeof(pub.topic) - 1);
    memcpy(pub.payload, "hello", 5); pub.payload_len = 5;
    pub.retain = 1; pub.qos = 0;
    topic_publish(&pub);

    /* no subscribers yet — no stub calls */
    CHECK("no delivery before subscribe", stub_recv_count == 0);

    /* subscriber arrives */
    topic_subscribe(&clients[0], "retain/flag", 0);
    topic_deliver_retained(&clients[0], "retain/flag", 0);
    /* retained delivery must set retain=1 (MQTT §3.3.1.3) */
    CHECK("retained delivery: retain=1",
          stub_recv_count == 1 && stub_recv_retain[0] == 1);

    stub_reset();

    /* now publish a fresh message (retain=1 in wire → fan-out clears it) */
    memcpy(pub.payload, "world", 5);
    topic_publish(&pub);
    /* fan-out must clear retain flag per MQTT §3.3.1.3 */
    CHECK("fan-out: retain cleared to 0",
          stub_recv_count == 1 && stub_recv_retain[0] == 0);
}

/* ── test_filter_valid ────────────────────────────────────────────────────── */

static void test_filter_valid(void)
{
    printf("\n-- test_filter_valid --\n");

    /* valid filters */
    CHECK("simple path valid",     topic_filter_valid("sport/tennis/player1"));
    CHECK("lone hash valid",        topic_filter_valid("#"));
    CHECK("hash at end valid",      topic_filter_valid("sport/#"));
    CHECK("lone plus valid",        topic_filter_valid("+"));
    CHECK("plus at end valid",      topic_filter_valid("sport/+"));
    CHECK("plus in middle valid",   topic_filter_valid("sport/+/player1"));
    CHECK("plus then hash valid",   topic_filter_valid("+/tennis/#"));
    CHECK("path then hash valid",   topic_filter_valid("sport/tennis/#"));

    /* invalid filters */
    CHECK("empty filter invalid",   !topic_filter_valid(""));
    CHECK("hash not own level",     !topic_filter_valid("sport/tennis#"));
    CHECK("hash not after /",       !topic_filter_valid("sport#"));
    CHECK("hash not at end",        !topic_filter_valid("#/sport"));
    CHECK("plus not own level",     !topic_filter_valid("sport+"));
    CHECK("plus not end of level",  !topic_filter_valid("+sport"));
    CHECK("plus mid-level invalid", !topic_filter_valid("sport/a+b"));
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== unit_topic tests ===\n");

    test_subscribe();
    test_unsubscribe();
    test_unsubscribe_all();
    test_retain_store_clear();
    test_retain_overflow();
    test_fanout();
    test_fanout_wildcard();
    test_qos_downgrade();
    test_retain_wildcard_deliver();
    test_duplicate_subscription();
    test_retain_flag_delivery();
    test_filter_valid();

    printf("\n=== Results: %d passed, %d failed ===\n", pass_count, fail_count);
    return (fail_count > 0) ? 1 : 0;
}
