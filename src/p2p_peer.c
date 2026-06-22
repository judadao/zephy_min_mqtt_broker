#include "platform/platform.h"
#include <stddef.h>
#include <string.h>

#ifndef __ZEPHYR__
#include <netinet/tcp.h>
#include <stdlib.h>
#include <time.h>
#endif

#include "p2p.h"
#include "p2p_shard.h"
#include "topic.h"

LOG_MODULE_REGISTER(mqtt_p2p_peer, LOG_LEVEL_INF);

typedef struct {
    int fd;
    uint8_t in_use;
    uint8_t connected;
    uint8_t node_id[P2P_NODE_ID_LEN];
    uint8_t role;
    uint32_t addr;
    uint16_t p2p_port;
    uint8_t outbound;
    plat_thread_t thread;
} p2p_conn_t;

typedef struct {
    int fd;
    uint8_t connected;
    uint8_t node_id[P2P_NODE_ID_LEN];
} p2p_send_slot_t;

typedef struct {
    uint8_t origin_id[P2P_NODE_ID_LEN];
    uint16_t seq;
} seen_msg_t;

typedef struct {
    uint32_t addr;
    uint16_t p2p_port;
    uint8_t in_use;
} p2p_static_seed_t;

typedef struct {
    uint8_t  origin_id[P2P_NODE_ID_LEN];
    uint16_t seq;
    uint16_t topic_len;
    uint16_t payload_len;
    uint8_t  qos;
    uint8_t  retain;
} __attribute__((packed)) p2p_publish_wire_hdr_t;

#define P2P_PUBLISH_FRAME_MAX \
    (sizeof(p2p_publish_wire_hdr_t) + MQTT_TOPIC_MAX + MQTT_PAYLOAD_MAX)

static p2p_conn_t conns[P2P_PEER_MAX];
static seen_msg_t seen[P2P_SEEN_MAX];
static uint16_t local_seq;
static int listen_fd = -1;
PLAT_MUTEX_DEFINE(peer_lock);
PLAT_MUTEX_DEFINE(seen_lock);
static p2p_send_slot_t send_slots[P2P_PEER_MAX];
PLAT_MUTEX_DEFINE(send_meta_lock);
static plat_mutex_t send_locks[P2P_PEER_MAX];
static p2p_static_seed_t static_seeds[P2P_STATIC_SEED_MAX];
PLAT_MUTEX_DEFINE(seed_lock);

#ifdef __ZEPHYR__
#define P2P_STACK_SIZE 1280
static struct k_thread accept_thread;
static struct k_thread connect_thread;
static K_THREAD_STACK_DEFINE(accept_stack, P2P_STACK_SIZE);
static K_THREAD_STACK_DEFINE(connect_stack, P2P_STACK_SIZE);
static K_THREAD_STACK_ARRAY_DEFINE(peer_stacks, P2P_PEER_MAX, P2P_STACK_SIZE);
#else
static pthread_t accept_thread;
static pthread_t connect_thread;
static void *peer_main(void *arg);
#endif

static int id_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, P2P_NODE_ID_LEN) == 0;
}

static int conn_exists(const uint8_t node_id[P2P_NODE_ID_LEN], const p2p_conn_t *except);
static void send_sub_to_leaves(const p2p_sub_msg_t *msg, uint8_t type,
                               const uint8_t *exclude_node_id);

static int conn_slot(const p2p_conn_t *c)
{
    return (int)(c - conns);
}

static void init_send_locks(void)
{
#ifdef __ZEPHYR__
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        k_mutex_init(&send_locks[i]);
    }
#else
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        pthread_mutex_init(&send_locks[i], NULL);
    }
#endif
}

static void send_slot_update(const p2p_conn_t *c)
{
    int slot = conn_slot(c);

    if (slot < 0 || slot >= P2P_PEER_MAX) {
        return;
    }

    plat_mutex_lock(&send_meta_lock);
    plat_mutex_lock(&send_locks[slot]);
    if (c->connected) {
        send_slots[slot].fd = c->fd;
        send_slots[slot].connected = 1;
        memcpy(send_slots[slot].node_id, c->node_id, P2P_NODE_ID_LEN);
    } else {
        memset(&send_slots[slot], 0, sizeof(send_slots[slot]));
        send_slots[slot].fd = -1;
    }
    plat_mutex_unlock(&send_locks[slot]);
    plat_mutex_unlock(&send_meta_lock);
}

static void send_slot_clear(int slot)
{
    if (slot < 0 || slot >= P2P_PEER_MAX) {
        return;
    }

    plat_mutex_lock(&send_meta_lock);
    plat_mutex_lock(&send_locks[slot]);
    memset(&send_slots[slot], 0, sizeof(send_slots[slot]));
    send_slots[slot].fd = -1;
    plat_mutex_unlock(&send_locks[slot]);
    plat_mutex_unlock(&send_meta_lock);
}

static int id_cmp(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, P2P_NODE_ID_LEN);
}

static int cstr_len_bounded(const char *s, size_t max, size_t *out_len)
{
    size_t len = 0;

    if (!s || !out_len) {
        return 0;
    }
    while (len < max && s[len] != '\0') {
        len++;
    }
    if (len >= max) {
        return 0;
    }
    *out_len = len;
    return 1;
}

static int copy_cstr_bounded(char *dst, size_t dst_cap, const char *src)
{
    size_t len;

    if (!dst || dst_cap == 0 ||
        !cstr_len_bounded(src, dst_cap, &len)) {
        return 0;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 1;
}

void p2p_static_seed_clear(void)
{
    plat_mutex_lock(&seed_lock);
    memset(static_seeds, 0, sizeof(static_seeds));
    plat_mutex_unlock(&seed_lock);
}

int p2p_static_seed_add(uint32_t addr, uint16_t p2p_port)
{
    if (addr == INADDR_NONE || addr == 0 || p2p_port == 0) {
        return -1;
    }

    plat_mutex_lock(&seed_lock);
    for (int i = 0; i < P2P_STATIC_SEED_MAX; i++) {
        if (static_seeds[i].in_use &&
            static_seeds[i].addr == addr &&
            static_seeds[i].p2p_port == p2p_port) {
            plat_mutex_unlock(&seed_lock);
            return 0;
        }
    }
    for (int i = 0; i < P2P_STATIC_SEED_MAX; i++) {
        if (!static_seeds[i].in_use) {
            static_seeds[i].addr = addr;
            static_seeds[i].p2p_port = p2p_port;
            static_seeds[i].in_use = 1;
            plat_mutex_unlock(&seed_lock);
            return 0;
        }
    }
    plat_mutex_unlock(&seed_lock);
    return -1;
}

#if !defined(CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY)
static uint32_t fnv1a32_update(uint32_t hash, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t peer_affinity(const uint8_t self_id[P2P_NODE_ID_LEN],
                              const uint8_t peer_id[P2P_NODE_ID_LEN])
{
    uint32_t hash = 2166136261u;

    hash = fnv1a32_update(hash, self_id, P2P_NODE_ID_LEN);
    hash = fnv1a32_update(hash, peer_id, P2P_NODE_ID_LEN);
    return hash;
}

static int32_t peer_priority(const uint8_t self_id[P2P_NODE_ID_LEN],
                             const p2p_peer_score_t *peer)
{
    uint32_t affinity = peer_affinity(self_id, peer->node_id);

    return (peer->score * 256) - (int32_t)(affinity & 0x7ffu);
}
#endif

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = plat_send(fd, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = plat_recv(fd, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int build_publish_frame(const p2p_publish_msg_t *msg,
                               uint8_t *out, uint16_t out_cap,
                               uint16_t *out_len);

static int send_frame(int fd, uint8_t type, const void *payload, uint16_t len)
{
    uint8_t frame[sizeof(p2p_hdr_t) + P2P_PUBLISH_FRAME_MAX];
    p2p_hdr_t hdr = {
        .type = type,
        .len = htons(len),
    };
    int rc = 0;

    if (sizeof(hdr) + len > sizeof(frame)) {
        return -1;
    }

    memcpy(frame, &hdr, sizeof(hdr));
    if (len > 0) {
        memcpy(frame + sizeof(hdr), payload, len);
    }

    if (send_all(fd, frame, sizeof(hdr) + len) < 0) {
        rc = -1;
    }
    return rc;
}

static int send_frame_to_slot(int slot, uint8_t type, const void *payload, uint16_t len)
{
    int rc;

    plat_mutex_lock(&send_locks[slot]);
    rc = send_frame(send_slots[slot].fd, type, payload, len);
    plat_mutex_unlock(&send_locks[slot]);
    return rc;
}

/* Find the send slot for node_id, lock send_locks[slot], drop send_meta_lock.
 * Returns the slot index (>= 0) with send_locks[slot] held, or -1 if not found. */
static int find_and_lock_send_slot(const uint8_t node_id[P2P_NODE_ID_LEN])
{
    int slot = -1;

    plat_mutex_lock(&send_meta_lock);
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        if (send_slots[i].connected && id_equal(send_slots[i].node_id, node_id)) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        plat_mutex_lock(&send_locks[slot]);
    }
    plat_mutex_unlock(&send_meta_lock);
    return slot;
}

static int send_prebuilt_publish_to_node_unlocked(const uint8_t node_id[P2P_NODE_ID_LEN],
                                                  const uint8_t *frame,
                                                  uint16_t frame_len)
{
    int slot = find_and_lock_send_slot(node_id);
    if (slot < 0) return 0;
    int ok = (send_frame(send_slots[slot].fd, P2P_PUBLISH, frame, frame_len) == 0);
    plat_mutex_unlock(&send_locks[slot]);
    return ok;
}

static int p2p_send_publish_from_router_prebuilt(const p2p_publish_msg_t *msg,
                                                 const uint8_t *frame,
                                                 uint16_t frame_len,
                                                 const uint8_t *exclude_node_id)
{
    uint8_t next_hops[P2P_PEER_MAX][P2P_NODE_ID_LEN];
    int next_hop_count;
    int sent = 0;

    p2p_election_record_publish();
    next_hop_count = p2p_router_find_next_hops(msg->topic, exclude_node_id,
                                               next_hops, P2P_PEER_MAX);
    if (next_hop_count <= 0) {
        return 0;
    }

    for (int i = 0; i < next_hop_count; i++) {
        sent += send_prebuilt_publish_to_node_unlocked(next_hops[i], frame, frame_len);
    }
#ifdef P2P_BENCH_TRACE
    LOG_INF("P2P send publish topic=%s peers=%d", msg->topic, sent);
#endif
    return sent;
}

/*
 * Last-resort for leaf nodes: flood the prebuilt frame to all connected peers.
 * Routers receiving it will route via their subscription tables.
 * The seen-cache prevents redelivery loops.
 */
static void flood_publish_to_connected_peers(const uint8_t *frame, uint16_t frame_len,
                                             const uint8_t *exclude_node_id)
{
    uint8_t targets[P2P_PEER_MAX][P2P_NODE_ID_LEN];
    int count = 0;

    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX && count < P2P_PEER_MAX; i++) {
        if (!conns[i].connected) {
            continue;
        }
        if (exclude_node_id && id_equal(conns[i].node_id, exclude_node_id)) {
            continue;
        }
        memcpy(targets[count], conns[i].node_id, P2P_NODE_ID_LEN);
        count++;
    }
    plat_mutex_unlock(&peer_lock);

    for (int i = 0; i < count; i++) {
        (void)send_prebuilt_publish_to_node_unlocked(targets[i], frame, frame_len);
    }
}

static int send_sub_to_node_unlocked(const uint8_t node_id[P2P_NODE_ID_LEN],
                                     const p2p_sub_msg_t *msg, uint8_t type)
{
    int slot = find_and_lock_send_slot(node_id);
    if (slot < 0) return 0;
    int ok = (send_frame(send_slots[slot].fd, type, msg, sizeof(*msg)) == 0);
    plat_mutex_unlock(&send_locks[slot]);
    return ok;
}

static void configure_peer_socket(int fd)
{
#ifndef __ZEPHYR__
    int one = 1;
    int buf = 131072;

    (void)plat_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef TCP_QUICKACK
    (void)plat_setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
    (void)plat_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    (void)plat_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
#else
    ARG_UNUSED(fd);
#endif
}

static int recv_frame(int fd, uint8_t *type, uint8_t *payload, uint16_t cap, uint16_t *len)
{
    p2p_hdr_t hdr;

    if (recv_all(fd, &hdr, sizeof(hdr)) < 0) {
        return -1;
    }
    *type = hdr.type;
    *len = ntohs(hdr.len);
    if (*len > cap) {
        return -1;
    }
    if (*len > 0 && recv_all(fd, payload, *len) < 0) {
        return -1;
    }
    return 0;
}

static int build_publish_frame(const p2p_publish_msg_t *msg,
                               uint8_t *out, uint16_t out_cap,
                               uint16_t *out_len)
{
    p2p_publish_wire_hdr_t hdr;
    size_t topic_len;
    size_t needed;

    if (!msg || !out || !out_len ||
        !cstr_len_bounded(msg->topic, MQTT_TOPIC_MAX, &topic_len) ||
        topic_len == 0 ||
        msg->payload_len > MQTT_PAYLOAD_MAX) {
        return -1;
    }

    needed = sizeof(hdr) + topic_len + msg->payload_len;
    if (needed > out_cap || needed > UINT16_MAX) {
        return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.origin_id, msg->origin_id, P2P_NODE_ID_LEN);
    hdr.seq = msg->seq;
    hdr.topic_len = (uint16_t)topic_len;
    hdr.payload_len = msg->payload_len;
    hdr.qos = msg->qos;
    hdr.retain = msg->retain;

    memcpy(out, &hdr, sizeof(hdr));
    memcpy(out + sizeof(hdr), msg->topic, topic_len);
    memcpy(out + sizeof(hdr) + topic_len, msg->payload, msg->payload_len);
    *out_len = (uint16_t)needed;
    return 0;
}

static int parse_publish_frame(const uint8_t *buf, uint16_t len,
                               p2p_publish_msg_t *msg)
{
    p2p_publish_wire_hdr_t hdr;
    size_t needed;

    if (len < sizeof(hdr)) {
        return -1;
    }
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.topic_len == 0 || hdr.topic_len >= MQTT_TOPIC_MAX ||
        hdr.payload_len > MQTT_PAYLOAD_MAX) {
        return -1;
    }
    needed = sizeof(hdr) + hdr.topic_len + hdr.payload_len;
    if (needed != len) {
        return -1;
    }

    memset(msg, 0, sizeof(*msg));
    memcpy(msg->origin_id, hdr.origin_id, P2P_NODE_ID_LEN);
    msg->seq = hdr.seq;
    memcpy(msg->topic, buf + sizeof(hdr), hdr.topic_len);
    msg->payload_len = hdr.payload_len;
    msg->qos = hdr.qos;
    msg->retain = hdr.retain;
    memcpy(msg->payload, buf + sizeof(hdr) + hdr.topic_len, hdr.payload_len);
    return 0;
}

static int send_hello(int fd)
{
    p2p_announce_t ann;

    p2p_election_build_announce(&ann);
    /* Called before send_slot_update: only this thread uses fd here. */
    return send_frame(fd, P2P_HELLO, &ann, sizeof(ann));
}

static void send_current_hello_to_peers(void)
{
    p2p_announce_t ann;
    uint8_t targets[P2P_PEER_MAX][P2P_NODE_ID_LEN];
    int count = 0;

    p2p_election_build_announce(&ann);
    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX && count < P2P_PEER_MAX; i++) {
        if (!conns[i].connected) {
            continue;
        }
        memcpy(targets[count], conns[i].node_id, P2P_NODE_ID_LEN);
        count++;
    }
    plat_mutex_unlock(&peer_lock);

    for (int i = 0; i < count; i++) {
        int slot = find_and_lock_send_slot(targets[i]);
        if (slot < 0) {
            continue;
        }
        (void)send_frame(send_slots[slot].fd, P2P_HELLO, &ann, sizeof(ann));
        plat_mutex_unlock(&send_locks[slot]);
    }
}

/*
 * O(1) direct-mapped seen cache.  P2P_SEEN_MAX must be a power of two.
 * Collision evicts the old entry; worst case is a duplicate delivery,
 * same as the old ring buffer's time-based eviction.
 */
static int seen_before(const p2p_publish_msg_t *msg)
{
    uint32_t h = (uint32_t)(msg->origin_id[0])
               | ((uint32_t)msg->origin_id[1] << 8)
               | ((uint32_t)msg->origin_id[2] << 16)
               | ((uint32_t)msg->origin_id[3] << 24);
    h ^= (uint32_t)msg->seq;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    unsigned idx = h & (P2P_SEEN_MAX - 1u);

    plat_mutex_lock(&seen_lock);
    seen_msg_t *slot = &seen[idx];
    if (slot->seq == msg->seq && id_equal(slot->origin_id, msg->origin_id)) {
        plat_mutex_unlock(&seen_lock);
        return 1;
    }
    memcpy(slot->origin_id, msg->origin_id, P2P_NODE_ID_LEN);
    slot->seq = msg->seq;
    plat_mutex_unlock(&seen_lock);
    return 0;
}

static p2p_conn_t *alloc_conn(int fd, uint32_t addr, uint16_t p2p_port, uint8_t outbound)
{
    p2p_conn_t *c = NULL;

    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        if (!conns[i].in_use) {
            c = &conns[i];
            memset(c, 0, sizeof(*c));
            c->fd = fd;
            c->addr = addr;
            c->p2p_port = p2p_port;
            c->outbound = outbound;
            c->in_use = 1;
            break;
        }
    }
    plat_mutex_unlock(&peer_lock);
    return c;
}

static void close_conn(p2p_conn_t *c)
{
    uint8_t node_id[P2P_NODE_ID_LEN];
    uint8_t had_node = 0;
    int slot = conn_slot(c);

    plat_mutex_lock(&peer_lock);
    if (c->connected) {
        memcpy(node_id, c->node_id, P2P_NODE_ID_LEN);
        had_node = 1;
    }
    if (c->fd >= 0) {
        plat_close(c->fd);
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    plat_mutex_unlock(&peer_lock);
    send_slot_clear(slot);

    if (had_node && !conn_exists(node_id, NULL)) {
        p2p_router_remove_node(node_id);
    }
}

static int conn_exists(const uint8_t node_id[P2P_NODE_ID_LEN], const p2p_conn_t *except)
{
    int exists = 0;

    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        if (&conns[i] != except && conns[i].in_use && conns[i].connected &&
            id_equal(conns[i].node_id, node_id)) {
            exists = 1;
            break;
        }
    }
    plat_mutex_unlock(&peer_lock);
    return exists;
}

static void close_duplicate_conns(const uint8_t node_id[P2P_NODE_ID_LEN],
                                  const p2p_conn_t *except)
{
    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        if (&conns[i] != except && conns[i].in_use && conns[i].connected &&
            id_equal(conns[i].node_id, node_id)) {
            if (conns[i].fd >= 0) {
                plat_close(conns[i].fd);
            }
            memset(&conns[i], 0, sizeof(conns[i]));
            conns[i].fd = -1;
            send_slot_clear(i);
        }
    }
    plat_mutex_unlock(&peer_lock);
}

static int addr_conn_exists(uint32_t addr, uint16_t p2p_port)
{
    int exists = 0;

    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        if (conns[i].in_use && conns[i].addr == addr &&
            conns[i].p2p_port == p2p_port) {
            exists = 1;
            break;
        }
    }
    plat_mutex_unlock(&peer_lock);
    return exists;
}

static int connected_peer_count(void)
{
    int count = 0;

    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        if (conns[i].connected) {
            count++;
        }
    }
    plat_mutex_unlock(&peer_lock);
    return count;
}

static void handle_publish(p2p_conn_t *c, const p2p_publish_msg_t *msg)
{
    if (seen_before(msg)) {
        return;
    }

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, msg->topic, sizeof(pub.topic) - 1);
    if (msg->payload_len > MQTT_PAYLOAD_MAX) {
        return;
    }
    memcpy(pub.payload, msg->payload, msg->payload_len);
    pub.payload_len = msg->payload_len;
    pub.qos = msg->qos;
    pub.retain = msg->retain;
#ifdef P2P_BENCH_TRACE
    LOG_INF("P2P recv publish topic=%s from peer", pub.topic);
#endif
    topic_publish_remote(&pub);

    if (p2p_election_role() == P2P_ROLE_ROUTER) {
        /*
         * Do not tie relay eligibility to the peer that delivered the publish.
         * In a mesh, the route that learned the subscription and the route
         * that delivered the publish are often different. Forwarding from the
         * current router's route table keeps multi-hop delivery alive.
         */
        p2p_router_publish(msg, c->node_id);
    }
}

static void advertise_local_subs_to(const p2p_conn_t *c)
{
    sub_snapshot_t subs[8];
    uint8_t self_id[P2P_NODE_ID_LEN];
    int slot = conn_slot(c);
    int cursor = 0;

    if (c->role != P2P_ROLE_ROUTER) {
        return;
    }

    p2p_election_self_id(self_id);
    while (cursor < TOPIC_MAX_SUBS) {
        int n = topic_get_sub_snapshots_from(subs, 8, &cursor);

        for (int i = 0; i < n; i++) {
            p2p_sub_msg_t msg = {0};
            memcpy(msg.owner_id, self_id, P2P_NODE_ID_LEN);
            strncpy(msg.filter, subs[i].filter, sizeof(msg.filter) - 1);
            msg.qos = subs[i].qos;
            (void)send_frame_to_slot(slot, P2P_SUB_NOTIFY, &msg, sizeof(msg));
        }
    }
}

void p2p_resync_local_subscriptions(void)
{
    sub_snapshot_t subs[8];
    int cursor = 0;

    while (cursor < TOPIC_MAX_SUBS) {
        int n = topic_get_sub_snapshots_from(subs, 8, &cursor);

        for (int i = 0; i < n; i++) {
            p2p_local_subscribe(subs[i].filter, subs[i].qos);
        }
        if (n <= 0) {
            break;
        }
    }
}

static void peer_loop(void *p1, void *p2, void *p3)
{
    p2p_conn_t *c = (p2p_conn_t *)p1;
    uint8_t buf[P2P_PUBLISH_FRAME_MAX];
    uint8_t type;
    uint16_t len;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (send_hello(c->fd) < 0 ||
        recv_frame(c->fd, &type, buf, sizeof(buf), &len) < 0 ||
        type != P2P_HELLO || len != sizeof(p2p_announce_t)) {
        close_conn(c);
        return;
    }

    p2p_announce_t *ann = (p2p_announce_t *)buf;
    if (conn_exists(ann->node_id, c)) {
        uint8_t self_id[P2P_NODE_ID_LEN];
        int keep_new;

        p2p_election_self_id(self_id);
        keep_new = (id_cmp(self_id, ann->node_id) < 0) ? c->outbound : !c->outbound;
        if (!keep_new) {
            close_conn(c);
            return;
        }
        close_duplicate_conns(ann->node_id, c);
    }
    memcpy(c->node_id, ann->node_id, P2P_NODE_ID_LEN);
    c->role = ann->role;
    c->connected = 1;
    send_slot_update(c);
    p2p_election_update_peer(ann, c->addr);
    LOG_INF("P2P peer connected role=%s", c->role == P2P_ROLE_ROUTER ? "ROUTER" : "LEAF");
    advertise_local_subs_to(c);

    while (recv_frame(c->fd, &type, buf, sizeof(buf), &len) == 0) {
        if (type == P2P_SUB_NOTIFY && len == sizeof(p2p_sub_msg_t)) {
            p2p_sub_msg_t *sub = (p2p_sub_msg_t *)buf;
            int changed = p2p_router_remote_subscribe(sub->owner_id, sub->filter,
                                                      sub->qos, c->node_id);
            if (changed && p2p_election_role() == P2P_ROLE_ROUTER) {
                p2p_send_sub_to_routers(sub, P2P_SUB_NOTIFY, c->node_id);
                /* Also inform connected leaves so they build route tables
                 * and can route without flooding on publish. */
                send_sub_to_leaves(sub, P2P_SUB_NOTIFY, c->node_id);
            }
        } else if (type == P2P_UNSUB_NOTIFY && len == sizeof(p2p_sub_msg_t)) {
            p2p_sub_msg_t *sub = (p2p_sub_msg_t *)buf;
            int changed = p2p_router_remote_unsubscribe(sub->owner_id, sub->filter);
            if (changed && p2p_election_role() == P2P_ROLE_ROUTER) {
                p2p_send_sub_to_routers(sub, P2P_UNSUB_NOTIFY, c->node_id);
                send_sub_to_leaves(sub, P2P_UNSUB_NOTIFY, c->node_id);
            }
        } else if (type == P2P_PUBLISH) {
            p2p_publish_msg_t msg;
            if (parse_publish_frame(buf, len, &msg) == 0) {
                handle_publish(c, &msg);
            }
        } else if (type == P2P_HELLO && len == sizeof(p2p_announce_t)) {
            ann = (p2p_announce_t *)buf;
            c->role = ann->role;
            send_slot_update(c);
            p2p_election_update_peer(ann, c->addr);
        }
    }

    LOG_INF("P2P peer disconnected");
    close_conn(c);
}

static void spawn_peer(p2p_conn_t *c)
{
#ifdef __ZEPHYR__
    int slot = (int)(c - conns);
    k_thread_create(&c->thread, peer_stacks[slot], K_THREAD_STACK_SIZEOF(peer_stacks[slot]),
                    peer_loop, c, NULL, NULL, 6, 0, K_NO_WAIT);
#else
    int rc = pthread_create(&c->thread, NULL, peer_main, c);
    if (rc != 0) {
        LOG_ERR("P2P peer thread create failed: %d", rc);
        close_conn(c);
        return;
    }
    pthread_detach(c->thread);
#endif
}

#if !defined(CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY)
static int has_conn_to(const uint8_t node_id[P2P_NODE_ID_LEN])
{
    return conn_exists(node_id, NULL);
}
#endif

static int connect_to_addr(uint32_t peer_addr, uint16_t p2p_port)
{
    int fd;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(p2p_port),
        .sin_addr.s_addr = peer_addr,
    };
    p2p_conn_t *c;

    if (addr_conn_exists(peer_addr, p2p_port)) {
        return 0;
    }

    fd = plat_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return 0;
    }
    if (plat_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        plat_close(fd);
        return 0;
    }
    configure_peer_socket(fd);
    c = alloc_conn(fd, peer_addr, p2p_port, 1);
    if (!c) {
        plat_close(fd);
        return 0;
    }
    spawn_peer(c);
    return 1;
}

#if !defined(CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY)
static int connect_to_peer(const p2p_peer_score_t *peer)
{
    uint8_t self_id[P2P_NODE_ID_LEN];

    p2p_election_self_id(self_id);
    if (id_equal(peer->node_id, self_id) ||
        peer->role != P2P_ROLE_ROUTER ||
        has_conn_to(peer->node_id)) {
        return 0;
    }
    return connect_to_addr(peer->addr, peer->p2p_port);
}
#endif

#ifndef __ZEPHYR__
static void load_env_static_seeds_once(void)
{
    static int loaded;
    const char *seeds;
    char tmp[2048];
    char *saveptr = NULL;

    if (loaded) {
        return;
    }
    loaded = 1;

    seeds = getenv("MQTT_P2P_PEERS");
    if (!seeds || !seeds[0]) {
        return;
    }

    strncpy(tmp, seeds, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *tok = strtok_r(tmp, ",", &saveptr);
         tok;
         tok = strtok_r(NULL, ",", &saveptr)) {
        char *colon = strrchr(tok, ':');
        uint32_t addr;
        int port;

        if (!colon) {
            continue;
        }
        *colon = '\0';
        addr = inet_addr(tok);
        port = atoi(colon + 1);
        if (addr != INADDR_NONE && port > 0 && port <= 65535) {
            (void)p2p_static_seed_add(addr, (uint16_t)port);
        }
    }
}
#endif

static void accept_loop(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int fd = plat_accept(listen_fd, (struct sockaddr *)&src, &src_len);
        if (fd < 0) {
            continue;
        }
        configure_peer_socket(fd);
        p2p_conn_t *c = alloc_conn(fd, src.sin_addr.s_addr, 0, 0);
        if (!c) {
            plat_close(fd);
            continue;
        }
        spawn_peer(c);
    }
}

static void connect_loop(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        uint8_t self_id[P2P_NODE_ID_LEN];
        int connected = 0;
        int budget = 0;
        p2p_peer_score_t peers[P2P_PEER_MAX + 1];
        int n = p2p_election_snapshot(peers, P2P_PEER_MAX + 1);

#ifndef __ZEPHYR__
        load_env_static_seeds_once();
#endif

        p2p_election_self_id(self_id);
        budget = p2p_election_peer_budget(n);
#if defined(CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY)
        ARG_UNUSED(peers);
#endif

        if (budget > 0) {
            connected = connected_peer_count();
        }
#if !defined(CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY)
        if (connected < budget) {
            p2p_peer_score_t candidates[P2P_PEER_MAX + 1];
            int candidate_count = 0;

            for (int i = 0; i < n; i++) {
                if (peers[i].role != P2P_ROLE_ROUTER) {
                    continue;
                }
                if (id_equal(peers[i].node_id, self_id) || has_conn_to(peers[i].node_id)) {
                    continue;
                }
                candidates[candidate_count++] = peers[i];
            }
            int32_t priorities[P2P_PEER_MAX + 1];
            for (int i = 0; i < candidate_count; i++) {
                priorities[i] = peer_priority(self_id, &candidates[i]);
            }
            for (int i = 0; i < candidate_count; i++) {
                int best = i;
                for (int j = i + 1; j < candidate_count; j++) {
                    if (priorities[j] > priorities[best] ||
                        (priorities[j] == priorities[best] &&
                         id_cmp(candidates[j].node_id, candidates[best].node_id) < 0)) {
                        best = j;
                    }
                }
                if (best != i) {
                    p2p_peer_score_t tmp = candidates[i];
                    candidates[i] = candidates[best];
                    candidates[best] = tmp;
                    int32_t ptmp = priorities[i];
                    priorities[i] = priorities[best];
                    priorities[best] = ptmp;
                }
            }
            for (int i = 0; i < candidate_count && connected < budget; i++) {
                connected += connect_to_peer(&candidates[i]);
            }
        }
#endif

        {
            p2p_static_seed_t seeds[P2P_STATIC_SEED_MAX];
            int seed_count = 0;

            plat_mutex_lock(&seed_lock);
            for (int i = 0; i < P2P_STATIC_SEED_MAX; i++) {
                if (static_seeds[i].in_use) {
                    seeds[seed_count++] = static_seeds[i];
                }
            }
            plat_mutex_unlock(&seed_lock);

            for (int i = 0; i < seed_count; i++) {
                connected += connect_to_addr(seeds[i].addr, seeds[i].p2p_port);
                if (budget > 0 && connected >= budget) {
                    break;
                }
            }
        }

        send_current_hello_to_peers();
#ifdef __ZEPHYR__
        k_sleep(K_MSEC(P2P_CONNECT_MS));
#else
        struct timespec ts = { .tv_sec = P2P_CONNECT_MS / 1000, .tv_nsec = 0 };
        ts.tv_nsec = (P2P_CONNECT_MS % 1000) * 1000000L;
        nanosleep(&ts, NULL);
#endif
    }
}

#ifndef __ZEPHYR__
static void *peer_main(void *arg)
{
    peer_loop(arg, NULL, NULL);
    return NULL;
}

static void *accept_main(void *arg)
{
    accept_loop(arg, NULL, NULL);
    return NULL;
}

static void *connect_main(void *arg)
{
    connect_loop(arg, NULL, NULL);
    return NULL;
}

#endif

void p2p_peer_start(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(P2P_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int opt = 1;

    memset(conns, 0, sizeof(conns));
    memset(send_slots, 0, sizeof(send_slots));
    for (int i = 0; i < P2P_PEER_MAX; i++) {
        conns[i].fd = -1;
        send_slots[i].fd = -1;
    }
    init_send_locks();

    listen_fd = plat_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        LOG_ERR("P2P TCP socket failed: %d", errno);
        return;
    }
    plat_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (plat_bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        plat_listen(listen_fd, P2P_PEER_MAX) < 0) {
        LOG_ERR("P2P TCP listen failed: %d", errno);
        plat_close(listen_fd);
        listen_fd = -1;
        return;
    }

#ifdef __ZEPHYR__
    k_thread_create(&accept_thread, accept_stack, K_THREAD_STACK_SIZEOF(accept_stack),
                    accept_loop, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
    k_thread_create(&connect_thread, connect_stack, K_THREAD_STACK_SIZEOF(connect_stack),
                    connect_loop, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
#else
    {
        int rc = pthread_create(&accept_thread, NULL, accept_main, NULL);
        if (rc == 0) {
            pthread_detach(accept_thread);
        } else {
            LOG_ERR("P2P accept thread create failed: %d", rc);
        }
        rc = pthread_create(&connect_thread, NULL, connect_main, NULL);
        if (rc == 0) {
            pthread_detach(connect_thread);
        } else {
            LOG_ERR("P2P connect thread create failed: %d", rc);
        }
    }
#endif
    LOG_INF("P2P peer TCP enabled on port %d", P2P_PORT);
}

int p2p_peer_snapshot(p2p_peer_snapshot_t *out, int max)
{
    int n = 0;

    if (!out || max <= 0) {
        return 0;
    }

    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX && n < max; i++) {
        if (!conns[i].in_use || !conns[i].connected) {
            continue;
        }
        memcpy(out[n].node_id, conns[i].node_id, P2P_NODE_ID_LEN);
        out[n].addr = conns[i].addr;
        out[n].p2p_port = conns[i].p2p_port;
        out[n].role = conns[i].role;
        out[n].outbound = conns[i].outbound;
        n++;
    }
    plat_mutex_unlock(&peer_lock);
    return n;
}

static void send_sub_to_peers_by_role(const p2p_sub_msg_t *msg, uint8_t type,
                                      const uint8_t *exclude_node_id,
                                      int routers_only)
{
    uint8_t targets[P2P_PEER_MAX][P2P_NODE_ID_LEN];
    int count = 0;

    plat_mutex_lock(&peer_lock);
    for (int i = 0; i < P2P_PEER_MAX && count < P2P_PEER_MAX; i++) {
        if (!conns[i].connected) {
            continue;
        }
        if (routers_only ? conns[i].role != P2P_ROLE_ROUTER
                         : conns[i].role == P2P_ROLE_ROUTER) {
            continue;
        }
        if (exclude_node_id && id_equal(conns[i].node_id, exclude_node_id)) {
            continue;
        }
        memcpy(targets[count], conns[i].node_id, P2P_NODE_ID_LEN);
        count++;
    }
    plat_mutex_unlock(&peer_lock);

    for (int i = 0; i < count; i++) {
        (void)send_sub_to_node_unlocked(targets[i], msg, type);
    }
}

void p2p_send_sub_to_routers(const p2p_sub_msg_t *msg, uint8_t type,
                             const uint8_t *exclude_node_id)
{
    send_sub_to_peers_by_role(msg, type, exclude_node_id, 1);
}

/*
 * Forward a subscription notification to directly connected leaf nodes so
 * they can build a local route table and avoid blind-flooding on publish.
 */
static void send_sub_to_leaves(const p2p_sub_msg_t *msg, uint8_t type,
                               const uint8_t *exclude_node_id)
{
    send_sub_to_peers_by_role(msg, type, exclude_node_id, 0);
}

void p2p_local_subscribe(const char *filter, uint8_t qos)
{
    p2p_sub_msg_t msg = {0};
    uint8_t owner_id[P2P_NODE_ID_LEN];
    uint8_t self_id[P2P_NODE_ID_LEN];

    if (qos > 2 || !copy_cstr_bounded(msg.filter, sizeof(msg.filter), filter)) {
        return;
    }
    p2p_election_self_id(msg.owner_id);
    memcpy(self_id, msg.owner_id, P2P_NODE_ID_LEN);
    msg.qos = qos;
    if (p2p_shard_owner_for_filter(filter, owner_id) &&
        id_cmp(owner_id, self_id) != 0) {
        if (!send_sub_to_node_unlocked(owner_id, &msg, P2P_SUB_NOTIFY)) {
            p2p_send_sub_to_routers(&msg, P2P_SUB_NOTIFY, NULL);
        }
    }
}

void p2p_local_unsubscribe(const char *filter)
{
    p2p_sub_msg_t msg = {0};
    uint8_t owner_id[P2P_NODE_ID_LEN];
    uint8_t self_id[P2P_NODE_ID_LEN];

    if (!copy_cstr_bounded(msg.filter, sizeof(msg.filter), filter)) {
        return;
    }
    p2p_election_self_id(msg.owner_id);
    memcpy(self_id, msg.owner_id, P2P_NODE_ID_LEN);
    if (p2p_shard_owner_for_filter(filter, owner_id) &&
        id_cmp(owner_id, self_id) != 0) {
        if (!send_sub_to_node_unlocked(owner_id, &msg, P2P_UNSUB_NOTIFY)) {
            p2p_send_sub_to_routers(&msg, P2P_UNSUB_NOTIFY, NULL);
        }
    }
}

void p2p_send_publish_from_router(const p2p_publish_msg_t *msg,
                                  const uint8_t *exclude_node_id)
{
    uint8_t frame[P2P_PUBLISH_FRAME_MAX];
    uint16_t frame_len = 0;
    if (build_publish_frame(msg, frame, sizeof(frame), &frame_len) < 0) {
        return;
    }
    (void)p2p_send_publish_from_router_prebuilt(msg, frame, frame_len, exclude_node_id);
}

void p2p_publish_from_local(const mqtt_publish_t *pub)
{
    p2p_publish_msg_t msg = {0};
    uint8_t frame[P2P_PUBLISH_FRAME_MAX];
    uint16_t frame_len = 0;
    uint8_t owner_id[P2P_NODE_ID_LEN];
    uint8_t self_id[P2P_NODE_ID_LEN];

    if (!pub ||
        pub->qos > 2 ||
        pub->payload_len > MQTT_PAYLOAD_MAX ||
        !copy_cstr_bounded(msg.topic, sizeof(msg.topic), pub->topic)) {
        return;
    }
    p2p_election_self_id(msg.origin_id);
    memcpy(self_id, msg.origin_id, P2P_NODE_ID_LEN);
    msg.seq = ++local_seq;
    msg.payload_len = pub->payload_len;
    msg.qos = pub->qos;
    msg.retain = pub->retain;
    memcpy(msg.payload, pub->payload, pub->payload_len);

    if (build_publish_frame(&msg, frame, sizeof(frame), &frame_len) < 0) {
        return;
    }
    if (!p2p_shard_owner_for_topic(pub->topic, owner_id) ||
        id_cmp(owner_id, self_id) == 0) {
        p2p_router_publish(&msg, NULL);
    } else {
        if (!send_prebuilt_publish_to_node_unlocked(owner_id, frame, frame_len)) {
            /* Direct path to shard owner unavailable; try route table. */
            if (!p2p_send_publish_from_router_prebuilt(&msg, frame, frame_len, NULL)) {
                /* No routes (leaf with empty table): flood to all connected peers.
                 * They will route via their own subscription tables. */
                flood_publish_to_connected_peers(frame, frame_len, NULL);
            }
        }
    }
}
