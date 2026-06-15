if(CONFIG_MQTT_MIN_BROKER)
  zephyr_library_named(mqtt_min_broker)

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

  # expose include/ and project root (for platform/platform.h)
  zephyr_library_include_directories(../include ${CMAKE_CURRENT_SOURCE_DIR}/..)
  zephyr_include_directories(../include)

endif()
