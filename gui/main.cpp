#include <QApplication>

#include "MainWindow.hpp"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName("picoTorrent Simulator");

    MainWindow window;
    window.show();
    return application.exec();
}
