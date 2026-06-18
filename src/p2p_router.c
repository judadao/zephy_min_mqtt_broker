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
#define P2P_REMOTE_SUBS_PER_NODE 32
#endif
#endif

#define P2P_REMOTE_NODE_MAX (P2P_PEER_MAX + 1)
#define P2P_REMOTE_EXACT_ROUTE_MAX \
    (P2P_REMOTE_NODE_MAX * P2P_REMOTE_SUBS_PER_NODE)
#define P2P_EXACT_ROUTE_USED    1
#define P2P_EXACT_ROUTE_DELETED 2

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

typedef struct {
    uint8_t owner_id[P2P_NODE_ID_LEN];
    uint8_t next_hop_id[P2P_NODE_ID_LEN];
    char filter[MQTT_TOPIC_MAX];
    uint8_t in_use;
} exact_route_t;

typedef struct {
    uint8_t owner_id[P2P_NODE_ID_LEN];
    uint8_t next_hop_id[P2P_NODE_ID_LEN];
    char filter[MQTT_TOPIC_MAX];
    uint8_t in_use;
} wildcard_route_t;

static remote_node_t remote_nodes[P2P_PEER_MAX + 1];
static exact_route_t exact_routes[P2P_REMOTE_EXACT_ROUTE_MAX];
static wildcard_route_t wildcard_routes[P2P_REMOTE_EXACT_ROUTE_MAX];
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

static int filter_is_exact(const char *filter)
{
    return strpbrk(filter, "+#") == NULL;
}

static int id_is_zero(const uint8_t id[P2P_NODE_ID_LEN])
{
    static const uint8_t zero[P2P_NODE_ID_LEN];

    return id_equal(id, zero);
}

static uint32_t hash_filter(const char *filter)
{
    uint32_t hash = 2166136261u;

    while (*filter) {
        hash ^= (uint8_t)*filter++;
        hash *= 16777619u;
    }
    return hash;
}

void p2p_router_init(void)
{
}

static void exact_route_upsert_locked(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                      const uint8_t next_hop_id[P2P_NODE_ID_LEN],
                                      const char *filter)
{
    uint32_t start;
    exact_route_t *free_route = NULL;

    if (!filter_is_exact(filter)) {
        return;
    }

    start = hash_filter(filter) % P2P_REMOTE_EXACT_ROUTE_MAX;
    for (int step = 0; step < P2P_REMOTE_EXACT_ROUTE_MAX; step++) {
        exact_route_t *route = &exact_routes[(start + step) % P2P_REMOTE_EXACT_ROUTE_MAX];

        if (route->in_use == P2P_EXACT_ROUTE_USED &&
            id_equal(route->owner_id, owner_id) &&
            strcmp(route->filter, filter) == 0) {
            memcpy(route->next_hop_id, next_hop_id, P2P_NODE_ID_LEN);
            return;
        }
        if (route->in_use == 0) {
            free_route = free_route ? free_route : route;
            break;
        }
        if (route->in_use == P2P_EXACT_ROUTE_DELETED && !free_route) {
            free_route = route;
        }
    }
    if (free_route) {
        memset(free_route, 0, sizeof(*free_route));
        memcpy(free_route->owner_id, owner_id, P2P_NODE_ID_LEN);
        memcpy(free_route->next_hop_id, next_hop_id, P2P_NODE_ID_LEN);
        strncpy(free_route->filter, filter, sizeof(free_route->filter) - 1);
        free_route->in_use = P2P_EXACT_ROUTE_USED;
    }
}

static void wildcard_route_upsert_locked(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                         const uint8_t next_hop_id[P2P_NODE_ID_LEN],
                                         const char *filter)
{
    wildcard_route_t *free_route = NULL;

    if (filter_is_exact(filter)) {
        return;
    }

    for (int i = 0; i < P2P_REMOTE_EXACT_ROUTE_MAX; i++) {
        wildcard_route_t *route = &wildcard_routes[i];

        if (route->in_use &&
            id_equal(route->owner_id, owner_id) &&
            strcmp(route->filter, filter) == 0) {
            memcpy(route->next_hop_id, next_hop_id, P2P_NODE_ID_LEN);
            return;
        }
        if (!route->in_use && !free_route) {
            free_route = route;
        }
    }

    if (free_route) {
        memset(free_route, 0, sizeof(*free_route));
        memcpy(free_route->owner_id, owner_id, P2P_NODE_ID_LEN);
        memcpy(free_route->next_hop_id, next_hop_id, P2P_NODE_ID_LEN);
        strncpy(free_route->filter, filter, sizeof(free_route->filter) - 1);
        free_route->in_use = 1;
    }
}

static void exact_route_remove_locked(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                      const char *filter)
{
    for (int i = 0; i < P2P_REMOTE_EXACT_ROUTE_MAX; i++) {
        if (exact_routes[i].in_use == P2P_EXACT_ROUTE_USED &&
            id_equal(exact_routes[i].owner_id, owner_id) &&
            strcmp(exact_routes[i].filter, filter) == 0) {
            exact_routes[i].in_use = P2P_EXACT_ROUTE_DELETED;
        }
    }
}

static void wildcard_route_remove_locked(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                         const char *filter)
{
    for (int i = 0; i < P2P_REMOTE_EXACT_ROUTE_MAX; i++) {
        if (wildcard_routes[i].in_use &&
            id_equal(wildcard_routes[i].owner_id, owner_id) &&
            strcmp(wildcard_routes[i].filter, filter) == 0) {
            wildcard_routes[i].in_use = 0;
        }
    }
}

static void exact_routes_remove_node_locked(const uint8_t node_id[P2P_NODE_ID_LEN])
{
    for (int i = 0; i < P2P_REMOTE_EXACT_ROUTE_MAX; i++) {
        if (exact_routes[i].in_use == P2P_EXACT_ROUTE_USED &&
            (id_equal(exact_routes[i].owner_id, node_id) ||
             id_equal(exact_routes[i].next_hop_id, node_id))) {
            exact_routes[i].in_use = P2P_EXACT_ROUTE_DELETED;
        }
    }
}

static void wildcard_routes_remove_node_locked(const uint8_t node_id[P2P_NODE_ID_LEN])
{
    for (int i = 0; i < P2P_REMOTE_EXACT_ROUTE_MAX; i++) {
        if (wildcard_routes[i].in_use &&
            (id_equal(wildcard_routes[i].owner_id, node_id) ||
             id_equal(wildcard_routes[i].next_hop_id, node_id))) {
            wildcard_routes[i].in_use = 0;
        }
    }
}

static void route_upsert_locked(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                const uint8_t next_hop_id[P2P_NODE_ID_LEN],
                                const char *filter)
{
    exact_route_upsert_locked(owner_id, next_hop_id, filter);
    wildcard_route_upsert_locked(owner_id, next_hop_id, filter);
}

static void node_routes_refresh_locked(const remote_node_t *node)
{
    for (int i = 0; i < P2P_REMOTE_SUBS_PER_NODE; i++) {
        if (node->subs[i].in_use) {
            route_upsert_locked(node->owner_id, node->next_hop_id,
                                node->subs[i].filter);
        }
    }
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

int p2p_router_remote_subscribe(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                const char *filter, uint8_t qos,
                                const uint8_t next_hop_id[P2P_NODE_ID_LEN])
{
    remote_node_t *node;
    int slot = -1;
    int found_existing = 0;
    int changed = 0;
    int route_changed = 0;
    int next_is_direct = id_equal(next_hop_id, owner_id);

    plat_mutex_lock(&router_lock);
    node = find_node_locked(owner_id, 1);
    if (!node) {
        plat_mutex_unlock(&router_lock);
        return 0;
    }

    if (id_is_zero(node->next_hop_id) ||
        (next_is_direct && !id_equal(node->next_hop_id, owner_id))) {
        memcpy(node->next_hop_id, next_hop_id, P2P_NODE_ID_LEN);
        route_changed = 1;
    }
    for (int i = 0; i < P2P_REMOTE_SUBS_PER_NODE; i++) {
        if (node->subs[i].in_use &&
            strcmp(node->subs[i].filter, filter) == 0) {
            slot = i;
            found_existing = 1;
            break;
        }
        if (!node->subs[i].in_use && slot < 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        changed = route_changed ||
                  !found_existing ||
                  node->subs[slot].qos != qos;
        strncpy(node->subs[slot].filter, filter, sizeof(node->subs[slot].filter) - 1);
        node->subs[slot].qos = qos;
        node->subs[slot].in_use = 1;
        if (route_changed) {
            node_routes_refresh_locked(node);
        } else {
            route_upsert_locked(owner_id, node->next_hop_id, filter);
        }
    }
    plat_mutex_unlock(&router_lock);
    return changed;
}

int p2p_router_remote_unsubscribe(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                  const char *filter)
{
    remote_node_t *node;
    int changed = 0;

    plat_mutex_lock(&router_lock);
    node = find_node_locked(owner_id, 0);
    if (node) {
        for (int i = 0; i < P2P_REMOTE_SUBS_PER_NODE; i++) {
            if (node->subs[i].in_use &&
                strcmp(node->subs[i].filter, filter) == 0) {
                node->subs[i].in_use = 0;
                exact_route_remove_locked(owner_id, filter);
                wildcard_route_remove_locked(owner_id, filter);
                changed = 1;
            }
        }
        clear_node_if_empty_locked(node);
    }
    plat_mutex_unlock(&router_lock);
    return changed;
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
    exact_routes_remove_node_locked(owner_id);
    wildcard_routes_remove_node_locked(owner_id);
    plat_mutex_unlock(&router_lock);
}

int p2p_router_stats(p2p_router_stats_t *out)
{
    p2p_router_stats_t stats = {0};

    if (!out) {
        return 0;
    }

    plat_mutex_lock(&router_lock);
    for (int i = 0; i <= P2P_PEER_MAX; i++) {
        if (!remote_nodes[i].in_use) {
            continue;
        }
        stats.remote_nodes++;
        for (int j = 0; j < P2P_REMOTE_SUBS_PER_NODE; j++) {
            if (!remote_nodes[i].subs[j].in_use) {
                continue;
            }
            stats.remote_subs++;
        }
    }
    for (int i = 0; i < P2P_REMOTE_EXACT_ROUTE_MAX; i++) {
        if (exact_routes[i].in_use == P2P_EXACT_ROUTE_USED) {
            stats.exact_routes++;
        }
        if (wildcard_routes[i].in_use) {
            stats.wildcard_routes++;
        }
    }
    plat_mutex_unlock(&router_lock);

    *out = stats;
    return 1;
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
    if (filter_is_exact(topic)) {
        uint32_t start = hash_filter(topic) % P2P_REMOTE_EXACT_ROUTE_MAX;

        for (int step = 0; step < P2P_REMOTE_EXACT_ROUTE_MAX && count < max; step++) {
            exact_route_t *route = &exact_routes[(start + step) % P2P_REMOTE_EXACT_ROUTE_MAX];

            if (route->in_use == 0) {
                break;
            }
            if (route->in_use != P2P_EXACT_ROUTE_USED) {
                continue;
            }
            if (strcmp(route->filter, topic) != 0) {
                continue;
            }
            if (exclude_node_id && id_equal(route->next_hop_id, exclude_node_id)) {
                continue;
            }
            if (!hop_in_list(out, count, route->next_hop_id)) {
                memcpy(out[count], route->next_hop_id, P2P_NODE_ID_LEN);
                count++;
            }
        }
        /* Also check wildcard routes: a remote subscriber on "sensors/#" must
         * receive publishes to the exact topic "sensors/node1/temp". */
        for (int i = 0; i < P2P_REMOTE_EXACT_ROUTE_MAX && count < max; i++) {
            if (!wildcard_routes[i].in_use) {
                continue;
            }
            if (exclude_node_id &&
                id_equal(wildcard_routes[i].next_hop_id, exclude_node_id)) {
                continue;
            }
            if (hop_in_list(out, count, wildcard_routes[i].next_hop_id)) {
                continue;
            }
            if (topic_match(wildcard_routes[i].filter, topic)) {
                memcpy(out[count], wildcard_routes[i].next_hop_id, P2P_NODE_ID_LEN);
                count++;
            }
        }
        plat_mutex_unlock(&router_lock);
        return count;
    }

    for (int i = 0; i < P2P_REMOTE_EXACT_ROUTE_MAX && count < max; i++) {
        if (!wildcard_routes[i].in_use) {
            continue;
        }
        if (exclude_node_id &&
            id_equal(wildcard_routes[i].next_hop_id, exclude_node_id)) {
            continue;
        }
        if (hop_in_list(out, count, wildcard_routes[i].next_hop_id)) {
            continue;
        }
        if (topic_match(wildcard_routes[i].filter, topic)) {
            memcpy(out[count], wildcard_routes[i].next_hop_id, P2P_NODE_ID_LEN);
            count++;
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
