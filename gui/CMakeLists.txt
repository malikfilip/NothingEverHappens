find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(picoTorrent_gui
    main.cpp
    ScenarioPeer.hpp
    AddPeerDialog.hpp
    AddPeerDialog.cpp
    MessageInfoDialog.hpp
    MessageInfoDialog.cpp
    AddSwarmDialog.hpp
    AddSwarmDialog.cpp
    ScenarioSwarm.hpp
    resources.qrc
    PieceBitmapDialog.hpp PieceBitmapDialog.cpp
    ScenarioSettings.hpp
    SimulationSettingsDialog.hpp SimulationSettingsDialog.cpp
    MainWindow.hpp
    MainWindow.cpp
    SimulationView.hpp
    SimulationView.cpp
)
# Reuse the existing engine sources without changing the engine or headless targets.
add_library(picotorrent_runtime STATIC)
set_target_properties(picoTorrent_gui PROPERTIES
    AUTORCC ON
    CXX_STANDARD_REQUIRED ON
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

target_sources(picotorrent_runtime PRIVATE
    RuntimeSession.hpp RuntimeSession.cpp RuntimePump.hpp PlaybackClock.hpp
    ScenarioPieces.hpp ScenarioPieces.cpp PeerInspection.hpp PeerInspection.cpp
    ../src/Simulation.cpp ../src/Peer.cpp ../src/Link.cpp ../src/Message.cpp
    ../src/Swarm.cpp ../src/Network.cpp ../src/Tracker.cpp
    ../src/PeerDiscovery.cpp ../src/PeerLifecycle.cpp ../src/Choking.cpp
)
target_include_directories(picotorrent_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ../inc)
target_link_libraries(picotorrent_runtime PUBLIC Qt6::Core)
target_link_libraries(picoTorrent_gui PRIVATE picotorrent_runtime Qt6::Widgets)

if(BUILD_TESTING)
    add_executable(runtime_session_tests ../tests/RuntimeSessionTests.cpp)
    target_link_libraries(runtime_session_tests PRIVATE picotorrent_runtime)
    add_test(NAME runtime_session COMMAND runtime_session_tests)
    add_executable(runtime_pump_tests ../tests/RuntimePumpTests.cpp)
    target_link_libraries(runtime_pump_tests PRIVATE picotorrent_runtime)
    add_test(NAME runtime_pump COMMAND runtime_pump_tests)
endif()
