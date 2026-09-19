# Qt GUI

The GUI is independent of the simulator engine. Add Swarm creates GUI-only
file or virtual swarm metadata. The swarm selector, Swarm Info, and message-filter
checkboxes are interactive; peer and simulation controls remain disabled.

Configure with a Qt 6 Widgets kit matching your compiler:

    cmake -S . -B cmake-build-gui -DPICOTORRENT_BUILD_GUI=ON -DCMAKE_PREFIX_PATH="<Qt kit directory>"
    cmake --build cmake-build-gui --target picoTorrent_gui

Launch the executable in the build directory's gui subdirectory (or the platform
application bundle). On Windows, use the Qt kit's runtime environment or deploy
its DLLs and platform plugin with that kit's windeployqt tool.

PICOTORRENT_BUILD_GUI defaults to OFF, so existing headless builds do not require Qt.
Requesting the GUI without Qt 6 Widgets produces a required-package CMake error.

The tracker image is embedded through resources.qrc. File selection reads only
file metadata; it does not read or hash contents. Scenario state is held in memory.
