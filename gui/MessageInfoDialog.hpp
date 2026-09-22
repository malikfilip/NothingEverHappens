#pragma once

#include <QDialog>
#include "simulator/Message.hpp"

// Static peer-wire help, reusable by any GUI message-icon surface.
class MessageInfoDialog : public QDialog {
public:
    using Type = simulator::MessageType;

    explicit MessageInfoDialog(Type type, QWidget* parent = nullptr);
    static QString messageName(Type type);
    static QString iconPath(Type type);
};
