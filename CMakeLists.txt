cmake_minimum_required(VERSION 3.20)

project(bittorrent_simulator)

set(CMAKE_CXX_STANDARD 20)

add_executable(bittorrent_simulator
        src/main.cpp
        src/Simulation.cpp
        inc/simulator/Event.hpp
        src/Peer.cpp
        inc/simulator/Peer.hpp
        inc/simulator/Link.hpp
        src/Link.cpp
        inc/simulator/Message.hpp
        src/Message.cpp
        inc/simulator/InfoHash.hpp
        inc/simulator/Swarm.hpp
        src/Swarm.cpp
        inc/simulator/Network.hpp
        src/Network.cpp
        src/Tracker.cpp
        src/PeerDiscovery.cpp src/PeerLifecycle.cpp
        src/Choking.cpp
)

target_include_directories(bittorrent_simulator PRIVATE
        inc
)

include(CTest)
if(BUILD_TESTING)
    add_executable(simulation_tests tests/SimulationTests.cpp src/Simulation.cpp)
    target_include_directories(simulation_tests PRIVATE inc)
    add_test(NAME simulation COMMAND simulation_tests)
endif()
if(BUILD_TESTING)
    add_executable(message_delivery_tests
        tests/MessageDeliveryTests.cpp
        src/Simulation.cpp
        src/Peer.cpp
        src/Link.cpp
        src/Message.cpp
        src/Swarm.cpp
        src/Network.cpp
        src/Tracker.cpp
        src/PeerDiscovery.cpp src/PeerLifecycle.cpp
        src/Choking.cpp
    )
    target_include_directories(message_delivery_tests PRIVATE inc)
    add_test(NAME message_delivery COMMAND message_delivery_tests)
    add_executable(multi_peer_integration_tests
        tests/MultiPeerIntegrationTests.cpp
        src/Simulation.cpp
        src/Peer.cpp
        src/Link.cpp
        src/Message.cpp
        src/Swarm.cpp
        src/Network.cpp
        src/Tracker.cpp
        src/PeerDiscovery.cpp src/PeerLifecycle.cpp
        src/Choking.cpp
    )
    target_include_directories(multi_peer_integration_tests PRIVATE inc)
    add_test(NAME multi_peer_integration COMMAND multi_peer_integration_tests)
    add_executable(choking_tests tests/ChokingTests.cpp src/Simulation.cpp src/Peer.cpp
        src/Link.cpp src/Message.cpp src/Swarm.cpp src/Network.cpp
        src/Tracker.cpp
        src/PeerDiscovery.cpp src/PeerLifecycle.cpp src/Choking.cpp)
    target_include_directories(choking_tests PRIVATE inc)
    add_test(NAME choking COMMAND choking_tests)
endif()

if(BUILD_TESTING)
    add_executable(tracker_tests tests/TrackerTests.cpp src/Simulation.cpp src/Peer.cpp
        src/Link.cpp src/Message.cpp src/Swarm.cpp src/Network.cpp src/Choking.cpp
        src/Tracker.cpp src/PeerDiscovery.cpp src/PeerLifecycle.cpp)
    target_include_directories(tracker_tests PRIVATE inc)
    add_test(NAME tracker COMMAND tracker_tests)
endif()

if(BUILD_TESTING)
    add_executable(peer_lifecycle_tests tests/PeerLifecycleTests.cpp src/Simulation.cpp src/Peer.cpp
        src/Link.cpp src/Message.cpp src/Swarm.cpp src/Network.cpp src/Choking.cpp
        src/Tracker.cpp src/PeerDiscovery.cpp src/PeerLifecycle.cpp)
    target_include_directories(peer_lifecycle_tests PRIVATE inc)
    add_test(NAME peer_lifecycle COMMAND peer_lifecycle_tests)
endif()

if(BUILD_TESTING)
    add_executable(endgame_tests tests/EndgameTests.cpp src/Simulation.cpp src/Peer.cpp
        src/Link.cpp src/Message.cpp src/Swarm.cpp src/Network.cpp src/Choking.cpp
        src/Tracker.cpp src/PeerDiscovery.cpp src/PeerLifecycle.cpp)
    target_include_directories(endgame_tests PRIVATE inc)
    add_test(NAME endgame COMMAND endgame_tests)
endif()
