#pragma once
#include "simulator/MessageEventTrace.hpp"
#include <QString>

// Shared by the filter/help, runtime canvas and link Inspector.
inline QString messageIconPath(simulator::MessageType type)
{
    return QStringLiteral(":/resources/messages/%1.png")
        .arg(QString::fromLatin1(simulator::messageTypeName(type)).toLower());
}
