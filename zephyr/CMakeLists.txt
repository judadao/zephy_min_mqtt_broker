if(CONFIG_MQTT_MIN_BROKER)
  zephyr_library_named(mqtt_min_broker)
  zephyr_library_compile_definitions(MQTT_MAX_CLIENTS=${CONFIG_MQTT_MAX_CLIENTS})
  zephyr_library_compile_definitions(MQTT_ADMISSION_MAX_CLIENTS=${CONFIG_MQTT_ADMISSION_MAX_CLIENTS})
  zephyr_library_compile_definitions(CLIENT_STACK_SIZE=${CONFIG_MQTT_CLIENT_STACK_SIZE})
  if(CONFIG_MQTT_P2P_DYNAMIC)
    zephyr_library_compile_definitions(
      P2P_PEER_MAX=${CONFIG_MQTT_P2P_PEER_MAX}
      P2P_REMOTE_SUBS_PER_NODE=${CONFIG_MQTT_P2P_REMOTE_SUBS_PER_NODE}
      P2P_SHARD_OWNER_CACHE_SIZE=${CONFIG_MQTT_P2P_SHARD_OWNER_CACHE_SIZE}
    )
  endif()

  zephyr_library_sources(
    ../src/broker.c
    ../src/client.c
    ../src/packet.c
    ../src/topic.c
    ../src/session.c
  )

  zephyr_library_sources_ifdef(CONFIG_MQTT_STANDALONE
    ../src/main.c
    ../src/wifi.c
  )

  zephyr_library_sources_ifdef(CONFIG_MQTT_P2P_DYNAMIC
    ../src/p2p_discover.c
    ../src/p2p_election.c
    ../src/p2p_peer.c
    ../src/p2p_router.c
    ../src/p2p_shard.c
  )

  # expose include/ and project root (for platform/platform.h)
  zephyr_library_include_directories(../include ${CMAKE_CURRENT_SOURCE_DIR}/..)
  zephyr_include_directories(../include ${CMAKE_CURRENT_SOURCE_DIR}/..)

endif()
