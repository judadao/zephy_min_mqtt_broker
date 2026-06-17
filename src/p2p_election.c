#include "platform/platform.h"
#include <string.h>

#include "broker.h"
#include "client.h"
#include "p2p.h"

LOG_MODULE_REGISTER(mqtt_p2p_election, LOG_LEVEL_INF);

typedef struct {
    uint8_t self_id[P2P_NODE_ID_LEN];
    p2p_peer_score_t peers[P2P_PEER_MAX + 1]; /* slot 0 is self */
    p2p_role_t role;
    uint32_t publish_count;
    uint32_t publish_rate;
    int64_t publish_window_ms;
} p2p_state_t;

static p2p_state_t st;
PLAT_MUTEX_DEFINE(p2p_lock);

static int id_cmp(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, P2P_NODE_ID_LEN);
}

static int score_before(const p2p_peer_score_t *a, const p2p_peer_score_t *b)
{
    if (a->score != b->score) {
        return a->score > b->score;
    }
    return id_cmp(a->node_id, b->node_id) < 0;
}

static int router_target_count(int active_nodes)
{
    if (active_nodes <= 1) {
        return active_nodes;
    }
#if P2P_ROUTER_COUNT > 0
    return P2P_ROUTER_COUNT < active_nodes ? P2P_ROUTER_COUNT : active_nodes;
#else
    int target = 1;

    while ((target + 1) * (target + 1) <= active_nodes) {
        target++;
    }
    if (target < 2) {
        target = 2;
    }
    return target < active_nodes ? target : active_nodes;
#endif
}

static int32_t local_score(void)
{
    int free_slots = client_free_slots();
    int64_t now = plat_uptime_ms();
    int uptime_bonus = now >= 60000 ? 20 : 0;
    int active_peers = 0;
    int publish_penalty;

    for (int i = 1; i <= P2P_PEER_MAX; i++) {
        if (st.peers[i].in_use) {
            active_peers++;
        }
    }

    if (st.publish_window_ms == 0) {
        st.publish_window_ms = now;
    } else if (now - st.publish_window_ms >= 1000) {
        int64_t elapsed = now - st.publish_window_ms;
        st.publish_rate = (uint32_t)(((uint64_t)st.publish_count * 1000U) /
                                     (uint64_t)elapsed);
        st.publish_count = 0;
        st.publish_window_ms = now;
    }

    publish_penalty = st.publish_rate > 1000U ? 1000 : (int)st.publish_rate;
    return (free_slots * 10) + uptime_bonus - (active_peers * 5) - publish_penalty;
}

static void expire_old_peers(void)
{
    int64_t now = plat_uptime_ms();

    for (int i = 1; i <= P2P_PEER_MAX; i++) {
        if (st.peers[i].in_use &&
            now - st.peers[i].last_seen_ms > P2P_PEER_TIMEOUT_MS) {
            memset(&st.peers[i], 0, sizeof(st.peers[i]));
        }
    }
}

static void recompute_role_locked(void)
{
    p2p_peer_score_t sorted[P2P_PEER_MAX + 1];
    int count = 0;
    int router_count;
    p2p_role_t old_role = st.role;

    expire_old_peers();
    st.peers[0].score = local_score();
    st.peers[0].role = (uint8_t)st.role;
    st.peers[0].last_seen_ms = plat_uptime_ms();

    for (int i = 0; i <= P2P_PEER_MAX; i++) {
        if (st.peers[i].in_use) {
            sorted[count++] = st.peers[i];
        }
    }

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (!score_before(&sorted[i], &sorted[j])) {
                p2p_peer_score_t tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    router_count = router_target_count(count);
    st.role = P2P_ROLE_LEAF;
    for (int i = 0; i < count && i < router_count; i++) {
        if (id_cmp(sorted[i].node_id, st.self_id) == 0) {
            st.role = P2P_ROLE_ROUTER;
            break;
        }
    }
    st.peers[0].role = (uint8_t)st.role;

    if (old_role != st.role) {
        LOG_INF("P2P role changed: %s", st.role == P2P_ROLE_ROUTER ? "ROUTER" : "LEAF");
    }
}

void p2p_election_init(const uint8_t node_id[P2P_NODE_ID_LEN])
{
    plat_mutex_lock(&p2p_lock);
    memset(&st, 0, sizeof(st));
    memcpy(st.self_id, node_id, P2P_NODE_ID_LEN);
    memcpy(st.peers[0].node_id, node_id, P2P_NODE_ID_LEN);
    st.peers[0].mqtt_port = MQTT_BROKER_PORT;
    st.peers[0].p2p_port = P2P_PORT;
    st.peers[0].in_use = 1;
    st.role = P2P_ROLE_ROUTER;
    recompute_role_locked();
    plat_mutex_unlock(&p2p_lock);
}

void p2p_election_update_self(void)
{
    plat_mutex_lock(&p2p_lock);
    recompute_role_locked();
    plat_mutex_unlock(&p2p_lock);
}

void p2p_election_update_peer(const p2p_announce_t *ann, uint32_t addr)
{
    int slot = -1;

    if (id_cmp(ann->node_id, st.self_id) == 0) {
        return;
    }

    plat_mutex_lock(&p2p_lock);
    for (int i = 1; i <= P2P_PEER_MAX; i++) {
        if (st.peers[i].in_use && id_cmp(st.peers[i].node_id, ann->node_id) == 0) {
            slot = i;
            break;
        }
        if (!st.peers[i].in_use && slot < 0) {
            slot = i;
        }
    }

    if (slot > 0) {
        memcpy(st.peers[slot].node_id, ann->node_id, P2P_NODE_ID_LEN);
        st.peers[slot].addr = addr;
        st.peers[slot].mqtt_port = ann->mqtt_port;
        st.peers[slot].p2p_port = ann->p2p_port;
        st.peers[slot].score = ann->score;
        st.peers[slot].role = ann->role;
        st.peers[slot].last_seen_ms = plat_uptime_ms();
        st.peers[slot].in_use = 1;
        recompute_role_locked();
    }
    plat_mutex_unlock(&p2p_lock);
}

void p2p_election_record_publish(void)
{
    int64_t now = plat_uptime_ms();

    plat_mutex_lock(&p2p_lock);
    if (st.publish_window_ms == 0) {
        st.publish_window_ms = now;
    } else if (now - st.publish_window_ms >= 1000) {
        int64_t elapsed = now - st.publish_window_ms;
        st.publish_rate = (uint32_t)(((uint64_t)st.publish_count * 1000U) /
                                     (uint64_t)elapsed);
        st.publish_count = 0;
        st.publish_window_ms = now;
    }
    if (st.publish_count < UINT32_MAX) {
        st.publish_count++;
    }
    plat_mutex_unlock(&p2p_lock);
}

p2p_role_t p2p_election_role(void)
{
    p2p_role_t role;

    plat_mutex_lock(&p2p_lock);
    role = st.role;
    plat_mutex_unlock(&p2p_lock);
    return role;
}

int p2p_election_snapshot(p2p_peer_score_t *out, int max)
{
    int n = 0;

    plat_mutex_lock(&p2p_lock);
    for (int i = 0; i <= P2P_PEER_MAX && n < max; i++) {
        if (st.peers[i].in_use) {
            out[n++] = st.peers[i];
        }
    }
    plat_mutex_unlock(&p2p_lock);
    return n;
}

void p2p_election_build_announce(p2p_announce_t *out)
{
    plat_mutex_lock(&p2p_lock);
    recompute_role_locked();
    memcpy(out->node_id, st.self_id, P2P_NODE_ID_LEN);
    out->mqtt_port = MQTT_BROKER_PORT;
    out->p2p_port = P2P_PORT;
    out->score = st.peers[0].score;
    out->role = (uint8_t)st.role;
    plat_mutex_unlock(&p2p_lock);
}

void p2p_election_self_id(uint8_t out[P2P_NODE_ID_LEN])
{
    plat_mutex_lock(&p2p_lock);
    memcpy(out, st.self_id, P2P_NODE_ID_LEN);
    plat_mutex_unlock(&p2p_lock);
}
