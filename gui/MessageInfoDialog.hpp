#pragma once

#include <QDialog>

// Static peer-wire help, reusable by any GUI message-icon surface.
class MessageInfoDialog : public QDialog {
public:
    enum class Type {
        Handshake, Bitfield, Have, Interested, NotInterested,
        Choke, Unchoke, Request, Piece, Cancel
    };

    explicit MessageInfoDialog(Type type, QWidget* parent = nullptr);
    static QString messageName(Type type);
    static QString iconPath(Type type);
};
