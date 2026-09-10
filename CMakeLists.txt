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
)

target_include_directories(bittorrent_simulator PRIVATE
        inc
)
