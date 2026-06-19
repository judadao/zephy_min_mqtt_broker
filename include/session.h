#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include "packet.h"
#include "broker.h"

#define SESSION_MAX       MQTT_MAX_CLIENTS
/* Holds offline messages + QoS1 inflights saved on disconnect (CLIENT_INFLIGHT_MAX=4) */
#define SESSION_QUEUE_MAX 8
#define SESSION_SUB_MAX   4   /* max persisted subscriptions per session    */

typedef struct {
    char     topic[MQTT_TOPIC_MAX];
    uint8_t  payload[MQTT_PAYLOAD_MAX];
    uint16_t payload_len;
    uint8_t  qos;
    uint16_t packet_id;
    uint8_t  dup;    /* 1 if retransmit (was inflight when client disconnected) */
    uint8_t  in_use;
} session_msg_t;

typedef struct {
    char          client_id[MQTT_CLIENT_ID_MAX];
    uint8_t       in_use;
    uint8_t       offline;          /* 1 while client is disconnected */
    uint16_t      next_packet_id;   /* for assigning ids to queued msgs */
    char          sub_filters[SESSION_SUB_MAX][MQTT_TOPIC_MAX];
    uint8_t       sub_qos[SESSION_SUB_MAX];
    uint8_t       sub_count;
    session_msg_t queue[SESSION_QUEUE_MAX];
} session_t;

struct client; /* forward declaration — avoids pulling in client.h */

/*
 * Persistent-session lifecycle.
 *
 * session_init() clears the fixed session pool and must run during broker
 * initialization. Sessions are keyed by MQTT client_id and are used only for
 * clients that connect with clean_session=0. All functions copy caller data
 * into broker-owned fixed buffers; no heap ownership is transferred.
 */
void       session_init(void);
session_t *session_find(const char *client_id);
session_t *session_create(const char *client_id);

/*
 * Find an existing session or create a new one in one lock+scan.
 * Sets *was_found, when non-NULL, to 1 if found or 0 if created. Returns NULL
 * when the fixed session pool is full.
 */
session_t *session_find_or_create(const char *client_id, uint8_t *was_found);
void       session_delete(const char *client_id);

/*
 * Save a disconnected persistent client's current subscription list. The list
 * is clamped to SESSION_SUB_MAX and marks the session offline.
 */
void session_save_subs(session_t *s,
                       const char (*filters)[MQTT_TOPIC_MAX],
                       const uint8_t *qos, uint8_t count);

/*
 * Enqueue one QoS 1/2 message for later delivery. pkt_id is the packet id to
 * use when retransmitting saved inflight messages; use 0 for freshly queued
 * offline publishes so drain can allocate a new id. Returns 0 on success or -1
 * when the fixed queue is full.
 */
int  session_enqueue(session_t *s, const mqtt_publish_t *pub, uint16_t pkt_id);

/* Deliver all queued messages to a freshly reconnected persistent client. */
void session_drain(session_t *s, struct client *c);

/*
 * Iterate offline sessions and enqueue pub for each matching persisted filter.
 * QoS 0 messages are intentionally skipped. Returns the number of sessions that
 * accepted a queued message.
 */
int  session_offline_publish(const mqtt_publish_t *pub,
                              int (*match_fn)(const char *, const char *));

#endif /* SESSION_H */
