#include "platform/platform.h"
#include <string.h>

#include "p2p.h"

LOG_MODULE_REGISTER(mqtt_p2p_router, LOG_LEVEL_INF);

#ifdef __ZEPHYR__
#ifndef P2P_REMOTE_SUBS_PER_NODE
#define P2P_REMOTE_SUBS_PER_NODE 8
#endif
#else
#ifndef P2P_REMOTE_SUBS_PER_NODE
#define P2P_REMOTE_SUBS_PER_NODE 16
#endif
#endif

typedef struct {
    char filter[MQTT_TOPIC_MAX];
    uint8_t qos;
    uint8_t in_use;
} remote_sub_t;

typedef struct {
    uint8_t owner_id[P2P_NODE_ID_LEN];
    uint8_t next_hop_id[P2P_NODE_ID_LEN];
    uint8_t in_use;
    remote_sub_t subs[P2P_REMOTE_SUBS_PER_NODE];
} remote_node_t;

static remote_node_t remote_nodes[P2P_PEER_MAX + 1];
PLAT_MUTEX_DEFINE(router_lock);

static int id_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, P2P_NODE_ID_LEN) == 0;
}

static int hop_in_list(uint8_t hops[][P2P_NODE_ID_LEN], int count,
                       const uint8_t hop[P2P_NODE_ID_LEN])
{
    for (int i = 0; i < count; i++) {
        if (id_equal(hops[i], hop)) {
            return 1;
        }
    }
    return 0;
}

static remote_node_t *find_node_locked(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                       int create)
{
    remote_node_t *free_node = NULL;

    for (int i = 0; i <= P2P_PEER_MAX; i++) {
        if (remote_nodes[i].in_use &&
            id_equal(remote_nodes[i].owner_id, owner_id)) {
            return &remote_nodes[i];
        }
        if (!remote_nodes[i].in_use && !free_node) {
            free_node = &remote_nodes[i];
        }
    }

    if (create && free_node) {
        memset(free_node, 0, sizeof(*free_node));
        memcpy(free_node->owner_id, owner_id, P2P_NODE_ID_LEN);
        free_node->in_use = 1;
        return free_node;
    }
    return NULL;
}

static void clear_node_if_empty_locked(remote_node_t *node)
{
    for (int i = 0; i < P2P_REMOTE_SUBS_PER_NODE; i++) {
        if (node->subs[i].in_use) {
            return;
        }
    }
    memset(node, 0, sizeof(*node));
}

static int topic_match(const char *filter, const char *topic)
{
    const char *f = filter;
    const char *t = topic;

    while (*f && *t) {
        if (*f == '#') {
            return 1;
        }
        if (*f == '+') {
            while (*t && *t != '/') {
                t++;
            }
            f++;
        } else {
            if (*f != *t) {
                return 0;
            }
            f++;
            t++;
        }
    }
    if (*f == '#') {
        return 1;
    }
    return *f == '\0' && *t == '\0';
}

void p2p_router_remote_subscribe(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                 const char *filter, uint8_t qos,
                                 const uint8_t next_hop_id[P2P_NODE_ID_LEN])
{
    remote_node_t *node;
    int slot = -1;

    plat_mutex_lock(&router_lock);
    node = find_node_locked(owner_id, 1);
    if (!node) {
        plat_mutex_unlock(&router_lock);
        return;
    }

    memcpy(node->next_hop_id, next_hop_id, P2P_NODE_ID_LEN);
    for (int i = 0; i < P2P_REMOTE_SUBS_PER_NODE; i++) {
        if (node->subs[i].in_use &&
            strcmp(node->subs[i].filter, filter) == 0) {
            slot = i;
            break;
        }
        if (!node->subs[i].in_use && slot < 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        strncpy(node->subs[slot].filter, filter, sizeof(node->subs[slot].filter) - 1);
        node->subs[slot].qos = qos;
        node->subs[slot].in_use = 1;
    }
    plat_mutex_unlock(&router_lock);
}

void p2p_router_remote_unsubscribe(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                   const char *filter)
{
    remote_node_t *node;

    plat_mutex_lock(&router_lock);
    node = find_node_locked(owner_id, 0);
    if (node) {
        for (int i = 0; i < P2P_REMOTE_SUBS_PER_NODE; i++) {
            if (node->subs[i].in_use &&
                strcmp(node->subs[i].filter, filter) == 0) {
                node->subs[i].in_use = 0;
            }
        }
        clear_node_if_empty_locked(node);
    }
    plat_mutex_unlock(&router_lock);
}

void p2p_router_remove_node(const uint8_t owner_id[P2P_NODE_ID_LEN])
{
    plat_mutex_lock(&router_lock);
    for (int i = 0; i <= P2P_PEER_MAX; i++) {
        if (remote_nodes[i].in_use &&
            (id_equal(remote_nodes[i].owner_id, owner_id) ||
             id_equal(remote_nodes[i].next_hop_id, owner_id))) {
            memset(&remote_nodes[i], 0, sizeof(remote_nodes[i]));
        }
    }
    plat_mutex_unlock(&router_lock);
}

int p2p_router_topic_has_remote_match(const uint8_t node_id[P2P_NODE_ID_LEN],
                                      const char *topic)
{
    remote_node_t *node;
    int match = 0;

    plat_mutex_lock(&router_lock);
    node = find_node_locked(node_id, 0);
    if (node) {
        for (int i = 0; i < P2P_REMOTE_SUBS_PER_NODE; i++) {
            if (node->subs[i].in_use &&
                topic_match(node->subs[i].filter, topic)) {
                match = 1;
                break;
            }
        }
    }
    plat_mutex_unlock(&router_lock);
    return match;
}

int p2p_router_next_hop_has_remote_match(const uint8_t next_hop_id[P2P_NODE_ID_LEN],
                                         const char *topic)
{
    int match = 0;

    plat_mutex_lock(&router_lock);
    for (int i = 0; i <= P2P_PEER_MAX; i++) {
        if (!remote_nodes[i].in_use ||
            !id_equal(remote_nodes[i].next_hop_id, next_hop_id)) {
            continue;
        }
        for (int j = 0; j < P2P_REMOTE_SUBS_PER_NODE; j++) {
            if (remote_nodes[i].subs[j].in_use &&
                topic_match(remote_nodes[i].subs[j].filter, topic)) {
                match = 1;
                break;
            }
        }
        if (match) {
            break;
        }
    }
    plat_mutex_unlock(&router_lock);
    return match;
}

int p2p_router_find_next_hops(const char *topic,
                              const uint8_t *exclude_node_id,
                              uint8_t out[][P2P_NODE_ID_LEN],
                              int max)
{
    int count = 0;

    if (max <= 0) {
        return 0;
    }

    plat_mutex_lock(&router_lock);
    for (int i = 0; i <= P2P_PEER_MAX && count < max; i++) {
        if (!remote_nodes[i].in_use) {
            continue;
        }
        if (exclude_node_id &&
            id_equal(remote_nodes[i].next_hop_id, exclude_node_id)) {
            continue;
        }
        if (hop_in_list(out, count, remote_nodes[i].next_hop_id)) {
            continue;
        }
        for (int j = 0; j < P2P_REMOTE_SUBS_PER_NODE; j++) {
            if (remote_nodes[i].subs[j].in_use &&
                topic_match(remote_nodes[i].subs[j].filter, topic)) {
                memcpy(out[count], remote_nodes[i].next_hop_id, P2P_NODE_ID_LEN);
                count++;
                break;
            }
        }
    }
    plat_mutex_unlock(&router_lock);
    return count;
}

void p2p_router_publish(const p2p_publish_msg_t *msg,
                        const uint8_t *exclude_node_id)
{
    p2p_send_publish_from_router(msg, exclude_node_id);
}
