#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stddef.h>

/* MQTT v3.1.1 packet type (high nibble of first byte) */
#define MQTT_CONNECT      0x10
#define MQTT_CONNACK      0x20
#define MQTT_PUBLISH      0x30
#define MQTT_PUBACK       0x40
#define MQTT_PUBREC       0x50
#define MQTT_PUBREL       0x60
#define MQTT_PUBCOMP      0x70
#define MQTT_SUBSCRIBE    0x80
#define MQTT_SUBACK       0x90
#define MQTT_UNSUBSCRIBE  0xA0
#define MQTT_UNSUBACK     0xB0
#define MQTT_PINGREQ      0xC0
#define MQTT_PINGRESP     0xD0
#define MQTT_DISCONNECT   0xE0

/* CONNACK return codes */
#define CONNACK_ACCEPTED          0x00
#define CONNACK_UNACCEPTABLE_PROTO 0x01
#define CONNACK_ID_REJECTED       0x02
#define CONNACK_SERVER_UNAVAIL    0x03
#define CONNACK_BAD_CREDENTIALS   0x04
#define CONNACK_NOT_AUTHORIZED    0x05

#define MQTT_CLIENT_ID_MAX   24
#define MQTT_USERNAME_MAX    64
#define MQTT_PASSWORD_MAX    64
#define MQTT_TOPIC_MAX       128
#define MQTT_PAYLOAD_MAX     512
#define MQTT_MAX_PACKET_SIZE (MQTT_TOPIC_MAX + MQTT_PAYLOAD_MAX + 64)

/* raw packet buffer received from wire */
typedef struct {
    uint8_t  type_flags;    /* first byte: type | flags */
    uint32_t remaining_len;
    uint8_t  buf[MQTT_MAX_PACKET_SIZE];
    uint32_t buf_len;       /* bytes in buf (== remaining_len when complete) */
} mqtt_packet_t;

typedef struct {
    char     client_id[MQTT_CLIENT_ID_MAX];
    uint8_t  clean_session;
    uint16_t keepalive;
    uint8_t  has_will;
    char     will_topic[MQTT_TOPIC_MAX];
    uint8_t  will_payload[256];
    uint16_t will_payload_len;
    uint8_t  will_qos;
    uint8_t  will_retain;
    uint8_t  has_username;
    uint8_t  has_password;
    char     username[MQTT_USERNAME_MAX];
    char     password[MQTT_PASSWORD_MAX];
} mqtt_connect_t;

typedef struct {
    char     topic[MQTT_TOPIC_MAX];
    uint8_t  payload[MQTT_PAYLOAD_MAX];
    uint16_t payload_len;
    uint8_t  qos;
    uint8_t  retain;
    uint8_t  dup;
    uint16_t packet_id;   /* only valid for qos > 0 */
} mqtt_publish_t;

/* decode/encode MQTT variable-length remaining-length field */
int packet_decode_remaining_len(const uint8_t *buf, size_t buf_len,
                                 uint32_t *out_len, size_t *out_bytes);
int packet_encode_remaining_len(uint32_t len, uint8_t *out, size_t *out_bytes);

/* parse incoming packets */
int packet_parse_connect(const mqtt_packet_t *pkt, mqtt_connect_t *out);
int packet_parse_publish(const mqtt_packet_t *pkt, mqtt_publish_t *out);
int packet_parse_subscribe(const mqtt_packet_t *pkt,
                            uint16_t *packet_id,
                            char topics[][MQTT_TOPIC_MAX],
                            uint8_t *qos, uint8_t *count, uint8_t max_topics);
int packet_parse_unsubscribe(const mqtt_packet_t *pkt,
                              uint16_t *packet_id,
                              char topics[][MQTT_TOPIC_MAX],
                              uint8_t *count, uint8_t max_topics);

/* build outgoing packets; return byte count or negative on error */
int packet_build_connack(uint8_t session_present, uint8_t return_code,
                          uint8_t *out, size_t out_cap);
int packet_build_suback(uint16_t packet_id,
                         const uint8_t *return_codes, uint8_t count,
                         uint8_t *out, size_t out_cap);
int packet_build_unsuback(uint16_t packet_id, uint8_t *out, size_t out_cap);
int packet_build_publish(const mqtt_publish_t *pub, uint8_t *out, size_t out_cap);
int packet_build_puback(uint16_t packet_id, uint8_t *out, size_t out_cap);
int packet_build_pubrec(uint16_t packet_id, uint8_t *out, size_t out_cap);
int packet_build_pubrel(uint16_t packet_id, uint8_t *out, size_t out_cap);
int packet_build_pubcomp(uint16_t packet_id, uint8_t *out, size_t out_cap);
int packet_build_pingresp(uint8_t *out, size_t out_cap);

#endif /* PACKET_H */
