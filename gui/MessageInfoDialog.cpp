#include "MessageInfoDialog.hpp"
#include "MessagePresentation.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

QString MessageInfoDialog::messageName(Type type)
{
    switch (type) {
    case Type::Handshake: return QStringLiteral("HANDSHAKE");
    case Type::Bitfield: return QStringLiteral("BITFIELD");
    case Type::Have: return QStringLiteral("HAVE");
    case Type::Interested: return QStringLiteral("INTERESTED");
    case Type::NotInterested: return QStringLiteral("NOT_INTERESTED");
    case Type::Choke: return QStringLiteral("CHOKE");
    case Type::Unchoke: return QStringLiteral("UNCHOKE");
    case Type::Request: return QStringLiteral("REQUEST");
    case Type::Piece: return QStringLiteral("PIECE");
    case Type::Cancel: return QStringLiteral("CANCEL");
    }
    return {};
}

QString MessageInfoDialog::iconPath(Type type)
{
    return messageIconPath(type);
}

MessageInfoDialog::MessageInfoDialog(Type type, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Message Info"));
    resize(400, 240);
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QHBoxLayout;
    auto* icon = new QLabel(this);
    icon->setPixmap(QPixmap(iconPath(type)).scaled(40, 40,
        Qt::KeepAspectRatio, Qt::SmoothTransformation));
    heading->addWidget(icon);
    auto* title = new QLabel(messageName(type), this);
    auto font = title->font();
    font.setBold(true);
    title->setFont(font);
    heading->addWidget(title);
    heading->addStretch();
    layout->addLayout(heading);

    // Concise summaries of the peer protocol in BEP 3:
    // https://www.bittorrent.org/beps/bep_0003.html
    QString role;
    QString fields;
    switch (type) {
    case Type::Handshake:
        role = tr("Opens the connection and identifies the torrent and peer.");
        fields = tr("Protocol string and length; reserved flags (8 bytes); "
                    "info_hash (20 bytes); peer_id (20 bytes).");
        break;
    case Type::Bitfield:
        role = tr("Shares the sender's initial inventory after the handshake.");
        fields = tr("Packed bits: one per piece; 1 means available. "
                    "Piece 0 uses the highest bit; trailing unused bits are zero.");
        break;
    case Type::Have:
        role = tr("Announces a newly completed, verified piece.");
        fields = tr("index: zero-based piece number (32-bit).");
        break;
    case Type::Interested:
        role = tr("The sender wants data the recipient has.");
        break;
    case Type::NotInterested:
        role = tr("The sender currently needs no data from the recipient.");
        break;
    case Type::Choke:
        role = tr("The sender suspends uploads to the recipient.");
        break;
    case Type::Unchoke:
        role = tr("The sender permits the recipient to request data again.");
        break;
    case Type::Request:
        role = tr("Asks for a block within a piece.");
        fields = tr("index: piece number; begin: byte offset; length: byte count "
                    "(all 32-bit).");
        break;
    case Type::Piece:
        role = tr("Delivers a requested block.");
        fields = tr("index: piece number; begin: byte offset (both 32-bit); block: data bytes.");
        break;
    case Type::Cancel:
        role = tr("Withdraws a block request, often after an endgame duplicate arrives.");
        fields = tr("index, begin, length: same 32-bit fields as REQUEST.");
        break;
    }
    auto addText = [this, layout](const QString& text) {
        auto* label = new QLabel(text, this);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        layout->addWidget(label);
    };
    addText(role);
    addText(fields.isEmpty() ? tr("Payload: None.") : tr("Payload: %1").arg(fields));
    layout->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
