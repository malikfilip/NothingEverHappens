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
    MainWindow.hpp
    MainWindow.cpp
    SimulationView.hpp
    SimulationView.cpp
)
target_link_libraries(picoTorrent_gui PRIVATE Qt6::Widgets)
set_target_properties(picoTorrent_gui PROPERTIES
    AUTORCC ON
    CXX_STANDARD_REQUIRED ON
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
