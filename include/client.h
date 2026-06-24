#ifndef CLIENT_H
#define CLIENT_H

#include "platform/platform.h"
#include "packet.h"

#ifdef __ZEPHYR__
#define CLIENT_STACK_SIZE        6144
#else
#define CLIENT_STACK_SIZE        2048
#endif
#define CLIENT_INFLIGHT_MAX      4
#define CLIENT_INFLIGHT_RETRY_MS 5000
#define CLIENT_QOS2_IN_MAX       4   /* max simultaneous inbound QoS-2 publishes */

typedef struct {
    uint16_t packet_id;
    uint8_t  buf[MQTT_MAX_PACKET_SIZE + 8];
    uint16_t len;
    int64_t  sent_at_ms;
    uint8_t  in_use;
    uint8_t  qos;
    uint8_t  waiting_pubcomp; /* QoS 2 outbound: 0=sent PUBLISH, 1=sent PUBREL */
} inflight_t;

typedef struct {
    uint16_t packet_id;
    uint8_t  in_use;
} qos2_in_t;

typedef enum {
    CLIENT_STATE_FREE = 0,
    CLIENT_STATE_CONNECTED,
    CLIENT_STATE_DISCONNECTING,
} client_state_t;

typedef struct client {
    int              fd;
    client_state_t   state;
    uint8_t          slot;          /* index in pool, stable for lifetime */

    char             client_id[MQTT_CLIENT_ID_MAX];
    uint8_t          clean_session;
    uint16_t         keepalive;     /* seconds; 0 = disabled */
    int64_t          last_seen_ms;  /* k_uptime_get() at last packet */

    mqtt_publish_t   will;
    uint8_t          has_will;

    uint16_t         next_packet_id; /* incremented per QoS-1/2 outbound */
    inflight_t       inflight[CLIENT_INFLIGHT_MAX];
    qos2_in_t        qos2_in[CLIENT_QOS2_IN_MAX]; /* inbound QoS-2 dedup table */

    plat_thread_t    thread;
} client_t;

typedef struct {
    uint8_t  slot;
    char     client_id[MQTT_CLIENT_ID_MAX];
    uint16_t keepalive;
    int64_t  last_seen_ms;
} client_snapshot_t;

/*
 * Client-pool snapshot APIs for dashboards and embedders.
 *
 * client_get_snapshots() copies up to max connected clients into out and
 * returns the number written. client_free_slots() returns the number of
 * currently available client slots in the fixed pool.
 */
int  client_get_snapshots(client_snapshot_t *out, int max);
int  client_free_slots(void);

/*
 * Client-pool lifecycle and per-client worker entry points.
 *
 * client_pool_init() must run during broker initialization before accepting
 * sockets. client_alloc() claims a fixed client slot, initializes it, and starts
 * the platform thread that runs client_thread_fn(); it returns the slot index or
 * -1 when the pool is full. client_free() releases the socket, publishes any
 * pending Will message, persists subscriptions for non-clean sessions, and
 * returns the slot to the pool.
 */
void client_pool_init(void);
int  client_alloc(int fd);
void client_free(client_t *c);
void client_thread_fn(void *p1, void *p2, void *p3);

/*
 * Internal client helpers used by topic/session code. client_send() returns 0
 * when the full frame is sent or -1 on socket error. Inflight helpers manage
 * fixed QoS 1/2 retransmit slots for outbound publishes.
 */
int  client_send(client_t *c, const uint8_t *buf, size_t len);
void client_disconnect(client_t *c);
void client_inflight_store(client_t *c, uint16_t id, const uint8_t *buf, uint16_t len, uint8_t qos);
void client_inflight_ack(client_t *c, uint16_t id);
void client_inflight_retry(client_t *c);

#endif /* CLIENT_H */
