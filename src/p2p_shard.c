#include "platform/platform.h"
#include <string.h>

#include "p2p_shard.h"

LOG_MODULE_REGISTER(mqtt_p2p_shard, LOG_LEVEL_INF);

#ifndef P2P_SHARD_KEY_LEVELS
#define P2P_SHARD_KEY_LEVELS 2
#endif

#ifndef P2P_SHARD_OWNER_CACHE_SIZE
#ifdef __ZEPHYR__
#define P2P_SHARD_OWNER_CACHE_SIZE 32
#else
#define P2P_SHARD_OWNER_CACHE_SIZE 256
#endif
#endif

typedef struct {
    uint8_t in_use;
    uint32_t topology_sig;
    char shard_key[MQTT_TOPIC_MAX];
    uint8_t owner_id[P2P_NODE_ID_LEN];
} shard_owner_cache_t;

static shard_owner_cache_t owner_cache[P2P_SHARD_OWNER_CACHE_SIZE];
PLAT_MUTEX_DEFINE(shard_cache_lock);

#ifndef __ZEPHYR__
static __thread shard_owner_cache_t thread_cache[8];
static __thread uint8_t thread_cache_pos;
#endif

static int id_cmp(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, P2P_NODE_ID_LEN);
}

static uint32_t fnv1a32_update(uint32_t hash, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t shard_cache_key_hash(uint32_t topology_sig, const char *shard_key)
{
    uint32_t hash = 2166136261u;

    hash = fnv1a32_update(hash, (const uint8_t *)&topology_sig, sizeof(topology_sig));
    hash = fnv1a32_update(hash, (const uint8_t *)shard_key, strlen(shard_key));
    return hash;
}

static int shard_cache_match(const shard_owner_cache_t *entry,
                             uint32_t topology_sig, const char *shard_key)
{
    return entry->in_use &&
           entry->topology_sig == topology_sig &&
           strcmp(entry->shard_key, shard_key) == 0;
}

static int shard_cache_lookup(uint32_t topology_sig, const char *shard_key,
                              uint8_t out[P2P_NODE_ID_LEN])
{
    uint32_t hash = shard_cache_key_hash(topology_sig, shard_key);

    for (int i = 0; i < P2P_SHARD_OWNER_CACHE_SIZE; i++) {
        int idx = (int)((hash + (uint32_t)i) % P2P_SHARD_OWNER_CACHE_SIZE);

        if (shard_cache_match(&owner_cache[idx], topology_sig, shard_key)) {
            memcpy(out, owner_cache[idx].owner_id, P2P_NODE_ID_LEN);
            return 1;
        }
        if (!owner_cache[idx].in_use) {
            return 0;
        }
    }
    return 0;
}

static void shard_cache_write(shard_owner_cache_t *slot, uint32_t topology_sig,
                              const char *shard_key,
                              const uint8_t owner_id[P2P_NODE_ID_LEN])
{
    slot->in_use = 1;
    slot->topology_sig = topology_sig;
    memcpy(slot->owner_id, owner_id, P2P_NODE_ID_LEN);
    strncpy(slot->shard_key, shard_key, sizeof(slot->shard_key) - 1);
    slot->shard_key[sizeof(slot->shard_key) - 1] = '\0';
}

static void shard_cache_store(uint32_t topology_sig, const char *shard_key,
                              const uint8_t owner_id[P2P_NODE_ID_LEN])
{
    uint32_t hash = shard_cache_key_hash(topology_sig, shard_key);

    for (int i = 0; i < P2P_SHARD_OWNER_CACHE_SIZE; i++) {
        int idx = (int)((hash + (uint32_t)i) % P2P_SHARD_OWNER_CACHE_SIZE);

        if (shard_cache_match(&owner_cache[idx], topology_sig, shard_key) ||
            !owner_cache[idx].in_use) {
            shard_cache_write(&owner_cache[idx], topology_sig, shard_key, owner_id);
            return;
        }
    }

    /* Table full: evict the home slot. */
    shard_cache_write(&owner_cache[hash % P2P_SHARD_OWNER_CACHE_SIZE],
                      topology_sig, shard_key, owner_id);
}

#ifndef __ZEPHYR__
static int thread_cache_lookup(uint32_t topology_sig, const char *shard_key,
                               uint8_t out[P2P_NODE_ID_LEN])
{
    for (int i = 0; i < 8; i++) {
        if (!thread_cache[i].in_use) {
            continue;
        }
        if (thread_cache[i].topology_sig == topology_sig &&
            strcmp(thread_cache[i].shard_key, shard_key) == 0) {
            memcpy(out, thread_cache[i].owner_id, P2P_NODE_ID_LEN);
            return 1;
        }
    }
    return 0;
}

static void thread_cache_store(uint32_t topology_sig, const char *shard_key,
                               const uint8_t owner_id[P2P_NODE_ID_LEN])
{
    shard_owner_cache_t *slot = &thread_cache[thread_cache_pos];

    thread_cache_pos = (uint8_t)((thread_cache_pos + 1) % 8);
    slot->in_use = 1;
    slot->topology_sig = topology_sig;
    memcpy(slot->owner_id, owner_id, P2P_NODE_ID_LEN);
    strncpy(slot->shard_key, shard_key, sizeof(slot->shard_key) - 1);
    slot->shard_key[sizeof(slot->shard_key) - 1] = '\0';
}
#endif

static void shard_key_from_str(const char *src, char out[MQTT_TOPIC_MAX])
{
    size_t len = 0;
    int slashes = 0;

    out[0] = '\0';
    if (!src || src[0] == '\0') {
        return;
    }

    for (const char *p = src; *p && len < MQTT_TOPIC_MAX - 1; p++) {
        if (*p == '+' || *p == '#') {
            break;
        }
        out[len++] = *p;
        if (*p == '/') {
            slashes++;
            if (slashes >= P2P_SHARD_KEY_LEVELS) {
                break;
            }
        }
    }

    while (len > 0 && out[len - 1] == '/') {
        len--;
    }
    out[len] = '\0';
}

void p2p_shard_key_from_topic(const char *topic, char out[MQTT_TOPIC_MAX])
{
    shard_key_from_str(topic, out);
}

void p2p_shard_key_from_filter(const char *filter, char out[MQTT_TOPIC_MAX])
{
    shard_key_from_str(filter, out);
}

static int shard_owner_id_from_key(const char *shard_key, uint8_t out[P2P_NODE_ID_LEN])
{
    p2p_peer_score_t peers[P2P_PEER_MAX + 1];
    p2p_peer_score_t routers[P2P_PEER_MAX + 1];
    uint32_t hash = 2166136261u;
    uint32_t topology_sig;
    int n;
    int router_count = 0;

    if (!out) {
        return 0;
    }

    p2p_election_self_id(out);
    if (!shard_key || shard_key[0] == '\0') {
        return 1;
    }

    topology_sig = p2p_election_topology_sig();
#ifndef __ZEPHYR__
    if (thread_cache_lookup(topology_sig, shard_key, out)) {
        return 1;
    }
#endif
    plat_mutex_lock(&shard_cache_lock);
    if (shard_cache_lookup(topology_sig, shard_key, out)) {
        plat_mutex_unlock(&shard_cache_lock);
        return 1;
    }
    plat_mutex_unlock(&shard_cache_lock);

    n = p2p_election_snapshot(peers, P2P_PEER_MAX + 1);
    for (int i = 0; i < n; i++) {
        if (peers[i].role == P2P_ROLE_ROUTER) {
            routers[router_count++] = peers[i];
        }
    }

    if (router_count == 0) {
        return 1;
    }

    for (int i = 0; i < router_count; i++) {
        for (int j = i + 1; j < router_count; j++) {
            if (id_cmp(routers[j].node_id, routers[i].node_id) < 0) {
                p2p_peer_score_t tmp = routers[i];
                routers[i] = routers[j];
                routers[j] = tmp;
            }
        }
    }

    hash = fnv1a32_update(hash, (const uint8_t *)shard_key, strlen(shard_key));
    memcpy(out, routers[hash % (uint32_t)router_count].node_id, P2P_NODE_ID_LEN);

    plat_mutex_lock(&shard_cache_lock);
    shard_cache_store(topology_sig, shard_key, out);
    plat_mutex_unlock(&shard_cache_lock);
#ifndef __ZEPHYR__
    thread_cache_store(topology_sig, shard_key, out);
#endif
    return 1;
}

int p2p_shard_owner_for_topic(const char *topic, uint8_t out[P2P_NODE_ID_LEN])
{
    char shard_key[MQTT_TOPIC_MAX];

    shard_key_from_str(topic, shard_key);
    return shard_owner_id_from_key(shard_key, out);
}

int p2p_shard_owner_for_filter(const char *filter, uint8_t out[P2P_NODE_ID_LEN])
{
    char shard_key[MQTT_TOPIC_MAX];

    shard_key_from_str(filter, shard_key);
    return shard_owner_id_from_key(shard_key, out);
}

int p2p_shard_is_local_owner_for_topic(const char *topic)
{
    uint8_t owner[P2P_NODE_ID_LEN];
    uint8_t self[P2P_NODE_ID_LEN];

    p2p_election_self_id(self);
    if (!p2p_shard_owner_for_topic(topic, owner)) {
        return 0;
    }
    return id_cmp(owner, self) == 0;
}

int p2p_shard_is_local_owner_for_filter(const char *filter)
{
    uint8_t owner[P2P_NODE_ID_LEN];
    uint8_t self[P2P_NODE_ID_LEN];

    p2p_election_self_id(self);
    if (!p2p_shard_owner_for_filter(filter, owner)) {
        return 0;
    }
    return id_cmp(owner, self) == 0;
}
