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
)

target_include_directories(bittorrent_simulator PRIVATE
        inc
)