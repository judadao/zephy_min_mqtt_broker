#ifndef P2P_SHARD_H
#define P2P_SHARD_H

#include "p2p.h"

void p2p_shard_key_from_topic(const char *topic, char out[MQTT_TOPIC_MAX]);
void p2p_shard_key_from_filter(const char *filter, char out[MQTT_TOPIC_MAX]);
int  p2p_shard_owner_for_topic(const char *topic, uint8_t out[P2P_NODE_ID_LEN]);
int  p2p_shard_owner_for_filter(const char *filter, uint8_t out[P2P_NODE_ID_LEN]);
int  p2p_shard_is_local_owner_for_topic(const char *topic);
int  p2p_shard_is_local_owner_for_filter(const char *filter);

#endif /* P2P_SHARD_H */
