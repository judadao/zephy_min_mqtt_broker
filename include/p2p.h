#ifndef P2P_H
#define P2P_H

#include <stdint.h>
#include "packet.h"

#define P2P_NODE_ID_LEN        16
#ifndef P2P_PORT
#define P2P_PORT              4884
#endif
#ifndef P2P_DISCOVERY_PORT
#define P2P_DISCOVERY_PORT    4885
#endif
#ifndef P2P_PEER_MAX
#ifdef __ZEPHYR__
#define P2P_PEER_MAX          5
#else
#define P2P_PEER_MAX          10
#endif
#endif
#ifndef P2P_ROUTER_COUNT
#define P2P_ROUTER_COUNT      0
#endif
#ifndef P2P_ANNOUNCE_MS
#define P2P_ANNOUNCE_MS       5000
#endif
#ifndef P2P_CONNECT_MS
#define P2P_CONNECT_MS        3000
#endif
#ifndef P2P_PEER_TIMEOUT_MS
#define P2P_PEER_TIMEOUT_MS   15000
#endif
#ifndef P2P_SEEN_MAX
#ifdef __ZEPHYR__
#define P2P_SEEN_MAX          32
#else
#define P2P_SEEN_MAX          128
#endif
#endif

#define P2P_HELLO        0x01
#define P2P_SUB_NOTIFY   0x02
#define P2P_UNSUB_NOTIFY 0x03
#define P2P_PUBLISH      0x04

typedef struct {
    uint8_t  type;
    uint16_t len;
} __attribute__((packed)) p2p_hdr_t;

typedef enum {
    P2P_ROLE_LEAF = 0,
    P2P_ROLE_ROUTER = 1,
} p2p_role_t;

typedef struct {
    uint8_t  node_id[P2P_NODE_ID_LEN];
    uint16_t mqtt_port;
    uint16_t p2p_port;
    int32_t  score;
    uint8_t  role;
} __attribute__((packed)) p2p_announce_t;

typedef struct {
    uint8_t  node_id[P2P_NODE_ID_LEN];
    uint32_t addr;
    uint16_t mqtt_port;
    uint16_t p2p_port;
    int32_t  score;
    uint8_t  role;
    int64_t  last_seen_ms;
    uint8_t  in_use;
} p2p_peer_score_t;

typedef struct {
    uint8_t remote_nodes;
    uint16_t remote_subs;
    uint16_t exact_routes;
    uint16_t wildcard_routes;
} p2p_router_stats_t;

typedef struct {
    uint8_t  owner_id[P2P_NODE_ID_LEN];
    char     filter[MQTT_TOPIC_MAX];
    uint8_t  qos;
} __attribute__((packed)) p2p_sub_msg_t;

typedef struct {
    uint8_t  origin_id[P2P_NODE_ID_LEN];
    uint16_t seq;
    char     topic[MQTT_TOPIC_MAX];
    uint16_t payload_len;
    uint8_t  qos;
    uint8_t  retain;
    uint8_t  payload[MQTT_PAYLOAD_MAX];
} __attribute__((packed)) p2p_publish_msg_t;

void p2p_start(void);
void p2p_peer_start(void);
void p2p_local_subscribe(const char *filter, uint8_t qos);
void p2p_local_unsubscribe(const char *filter);
void p2p_resync_local_subscriptions(void);
void p2p_publish_from_local(const mqtt_publish_t *pub);
void p2p_send_publish_from_router(const p2p_publish_msg_t *msg,
                                  const uint8_t *exclude_node_id);
void p2p_send_sub_to_routers(const p2p_sub_msg_t *msg, uint8_t type,
                             const uint8_t *exclude_node_id);

void p2p_election_init(const uint8_t node_id[P2P_NODE_ID_LEN]);
void p2p_election_update_self(void);
void p2p_election_update_peer(const p2p_announce_t *ann, uint32_t addr);
void p2p_election_record_publish(void);
p2p_role_t p2p_election_role(void);
uint32_t p2p_election_topology_sig(void);
int p2p_election_snapshot(p2p_peer_score_t *out, int max);
void p2p_election_build_announce(p2p_announce_t *out);
void p2p_election_self_id(uint8_t out[P2P_NODE_ID_LEN]);
int p2p_election_peer_budget(int active_nodes);

int p2p_router_remote_subscribe(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                const char *filter, uint8_t qos,
                                const uint8_t next_hop_id[P2P_NODE_ID_LEN]);
int p2p_router_remote_unsubscribe(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                  const char *filter);
void p2p_router_remove_node(const uint8_t owner_id[P2P_NODE_ID_LEN]);
int p2p_router_stats(p2p_router_stats_t *out);
void p2p_router_init(void);
int p2p_router_find_next_hops(const char *topic,
                              const uint8_t *exclude_node_id,
                              uint8_t out[][P2P_NODE_ID_LEN],
                              int max);
void p2p_router_publish(const p2p_publish_msg_t *msg,
                        const uint8_t *exclude_node_id);

#endif /* P2P_H */
