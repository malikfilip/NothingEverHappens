find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(picoTorrent_gui
    main.cpp
    ScenarioPeer.hpp
    AddPeerDialog.hpp
    AddPeerDialog.cpp
    MessageInfoDialog.hpp
    MessageInfoDialog.cpp
    RuntimeLogDialog.hpp RuntimeLogDialog.cpp
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
    RuntimeLinks.hpp RuntimeLinks.cpp
    RuntimeEventLog.hpp RuntimeEventLog.cpp
    ScenarioPersistence.hpp ScenarioPersistence.cpp
    ScenarioPieces.hpp ScenarioPieces.cpp PeerInspection.hpp PeerInspection.cpp
    ../src/Simulation.cpp ../src/Peer.cpp ../src/Link.cpp ../src/Message.cpp
    ../src/Swarm.cpp ../src/Network.cpp ../src/Tracker.cpp
    ../src/PeerDiscovery.cpp ../src/PeerLifecycle.cpp ../src/Choking.cpp
)
target_include_directories(picotorrent_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ../inc)
target_link_libraries(picotorrent_runtime PUBLIC Qt6::Core)
target_link_libraries(picoTorrent_gui PRIVATE picotorrent_runtime Qt6::Widgets)

if(BUILD_TESTING)
    add_executable(runtime_event_log_tests ../tests/RuntimeEventLogTests.cpp RuntimeLogDialog.cpp resources.qrc)
    set_target_properties(runtime_event_log_tests PROPERTIES AUTORCC ON)
    target_link_libraries(runtime_event_log_tests PRIVATE picotorrent_runtime Qt6::Widgets)
    add_test(NAME runtime_event_log COMMAND runtime_event_log_tests)
    set_tests_properties(runtime_event_log PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        set_property(TEST runtime_event_log APPEND PROPERTY ENVIRONMENT
            "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
    endif()
    add_executable(runtime_interaction_tests ../tests/RuntimeInteractionTests.cpp SimulationView.cpp resources.qrc)
    set_target_properties(runtime_interaction_tests PROPERTIES AUTORCC ON)
    target_link_libraries(runtime_interaction_tests PRIVATE picotorrent_runtime Qt6::Widgets)
    add_test(NAME runtime_interaction COMMAND runtime_interaction_tests)
    set_tests_properties(runtime_interaction PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        set_property(TEST runtime_interaction APPEND PROPERTY ENVIRONMENT
            "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
    endif()
    add_executable(runtime_links_tests ../tests/RuntimeLinksTests.cpp)
    target_link_libraries(runtime_links_tests PRIVATE picotorrent_runtime)
    add_test(NAME runtime_links COMMAND runtime_links_tests)
    add_executable(runtime_session_tests ../tests/RuntimeSessionTests.cpp)
    target_link_libraries(runtime_session_tests PRIVATE picotorrent_runtime)
    add_test(NAME runtime_session COMMAND runtime_session_tests)
    add_executable(runtime_pump_tests ../tests/RuntimePumpTests.cpp)
    target_link_libraries(runtime_pump_tests PRIVATE picotorrent_runtime)
    add_test(NAME runtime_pump COMMAND runtime_pump_tests)
    add_executable(scenario_persistence_tests ../tests/ScenarioPersistenceTests.cpp)
    target_link_libraries(scenario_persistence_tests PRIVATE picotorrent_runtime)
    add_test(NAME scenario_persistence COMMAND scenario_persistence_tests)
endif()
