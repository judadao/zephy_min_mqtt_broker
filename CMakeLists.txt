# Standalone app build (west build -b esp32 .)
# For module use, Zephyr picks up zephyr/CMakeLists.txt automatically.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mqtt_min_broker)

target_sources(app PRIVATE
  src/broker.c
  src/client.c
  src/packet.c
  src/topic.c
  src/session.c
  src/main.c
  src/wifi.c
)

if(CONFIG_MQTT_P2P_DYNAMIC)
  target_sources(app PRIVATE
    src/p2p_discover.c
    src/p2p_election.c
    src/p2p_peer.c
    src/p2p_router.c
  )
endif()

target_include_directories(app PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR})
