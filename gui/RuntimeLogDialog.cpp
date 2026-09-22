#include "RuntimeLogDialog.hpp"
#include "RuntimeEventLog.hpp"
#include "MessagePresentation.hpp"
#include <QDialogButtonBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
std::unique_ptr<QDialog> messageDialog(const QString& title, simulator::MessageType type,
    const QString& text, QWidget* parent)
{
    auto dialog = std::make_unique<QDialog>(parent);
    dialog->setWindowTitle(title);
    dialog->resize(460, 360);
    auto* layout = new QVBoxLayout(dialog.get());
    auto* icon = new QLabel(dialog.get());
    icon->setPixmap(QPixmap(messageIconPath(type)).scaled(32, 32,
        Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(icon);
    auto* scroll = new QScrollArea(dialog.get());
    scroll->setWidgetResizable(true);
    auto* details = new QLabel(text, scroll);
    details->setObjectName("messageLogDetails");
    details->setTextFormat(Qt::PlainText);
    details->setWordWrap(true);
    details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    scroll->setWidget(details);
    layout->addWidget(scroll, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog.get());
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
    layout->addWidget(buttons);
    return dialog;
}

}
std::unique_ptr<QDialog> createRuntimeLogDialog(const RuntimeEventLog& log, int row, bool paused, QWidget* parent)
{
    const auto* entry = log.entry(row);
    if (!paused || !entry || !entry->message) return {};
    return messageDialog(QObject::tr("Message %1").arg(entry->action), entry->message->type(),
        log.inspectionText(*entry), parent);
}
std::unique_ptr<QDialog> createActiveTransmissionDialog(const simulator::ActiveTransmission& active,
    double simulationTime, bool paused, QWidget* parent)
{
    if (!paused) return {};
    return messageDialog(QObject::tr("Active message"), active.message.type(),
        inspectTransmissionText(active, simulationTime), parent);
}