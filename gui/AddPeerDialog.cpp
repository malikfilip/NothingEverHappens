#include "AddPeerDialog.hpp"

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

// QSpinBox is limited to int; swarm piece counts are unsigned 64-bit values.
class PieceCountSpinBox : public QAbstractSpinBox {
public:
    PieceCountSpinBox(quint64 maximum, QWidget* parent)
        : QAbstractSpinBox(parent), maximum_(maximum)
    {
        lineEdit()->setText(QStringLiteral("0"));
    }
    QLineEdit* editor() const { return lineEdit(); }
    quint64 value() const { return text().toULongLong(); }
    QValidator::State validate(QString& input, int&) const override
    {
        if (input.isEmpty()) return QValidator::Intermediate;
        for (const auto ch : input)
            if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return QValidator::Invalid;
        bool ok = false;
        const auto count = input.toULongLong(&ok);
        return ok && count <= maximum_ ? QValidator::Acceptable : QValidator::Invalid;
    }
    void stepBy(int steps) override
    {
        auto count = value();
        if (steps > 0)
            count += qMin(static_cast<quint64>(steps), maximum_ - count);
        else
            count -= qMin(static_cast<quint64>(-static_cast<qint64>(steps)), count);
        lineEdit()->setText(QString::number(count));
    }
    StepEnabled stepEnabled() const override
    {
        StepEnabled result = StepNone;
        if (value() > 0) result |= StepDownEnabled;
        if (value() < maximum_) result |= StepUpEnabled;
        return result;
    }
private:
    quint64 maximum_;
};

AddPeerDialog::AddPeerDialog(const QString& defaultName, quint64 swarmPieceCount, QWidget* parent)
    : QDialog(parent), swarmPieceCount_(swarmPieceCount)
{
    setWindowTitle(tr("Add Peer"));
    setMinimumWidth(360);
    auto* layout = new QVBoxLayout(this);
    auto* grid = new QGridLayout;
    layout->addLayout(grid);
    auto addLabel = [this, grid](const QString& text, QWidget* buddy, int row) {
        auto* label = new QLabel(text, this);
        label->setBuddy(buddy);
        grid->addWidget(label, row, 0);
    };
    auto separator = [this, grid](int row) {
        auto* line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        grid->addWidget(line, row, 0, 1, 3);
    };
    name_ = new QLineEdit(defaultName, this);
    addLabel(tr("Name:"), name_, 0);
    grid->addWidget(name_, 0, 1, 1, 2);
    separator(1);
    auto* leecher = new QRadioButton(tr("Leecher"), this);
    seeder_ = new QRadioButton(tr("Seeder"), this);
    leecher->setChecked(true);
    grid->addWidget(leecher, 2, 0, 1, 3);
    grid->addWidget(seeder_, 3, 0, 1, 3);
    pieces_ = new PieceCountSpinBox(swarmPieceCount, this);
    pieces_->setToolTip(tr("Complete pieces initially owned: 0 to %1.").arg(swarmPieceCount));
    addLabel(tr("Initial pieces:"), pieces_, 4);
    grid->addWidget(pieces_, 4, 1, 1, 2);
    separator(5);
    auto bandwidthRow = [this, grid, &addLabel](const QString& label, int row,
                                              QSpinBox*& value, QComboBox*& unit) {
        value = new QSpinBox(this);
        value->setRange(0, 1000000000);
        value->setValue(10);
        unit = new QComboBox(this);
        unit->addItem(tr("KiB/s"), QVariant::fromValue(quint64{1024}));
        unit->addItem(tr("MiB/s"), QVariant::fromValue(quint64{1024} * 1024));
        unit->addItem(tr("GiB/s"), QVariant::fromValue(quint64{1024} * 1024 * 1024));
        unit->setCurrentIndex(1);
        addLabel(label, value, row);
        grid->addWidget(value, row, 1);
        grid->addWidget(unit, row, 2);
    };
    bandwidthRow(tr("Upload:"), 6, upload_, uploadUnit_);
    bandwidthRow(tr("Download:"), 7, download_, downloadUnit_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    add_ = buttons->addButton(tr("Add"), QDialogButtonBox::AcceptRole);
    add_->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        updateValidity();
        if (add_->isEnabled()) accept();
    });
    connect(name_, &QLineEdit::textChanged, this, &AddPeerDialog::updateValidity);
    connect(seeder_, &QRadioButton::toggled, this, &AddPeerDialog::updateValidity);
    connect(pieces_->editor(), &QLineEdit::textChanged, this, &AddPeerDialog::updateValidity);
    for (auto* value : {upload_, download_})
        connect(value->findChild<QLineEdit*>(), &QLineEdit::textChanged,
            this, &AddPeerDialog::updateValidity);
    updateValidity();
    name_->selectAll();
    name_->setFocus();
}

void AddPeerDialog::updateValidity()
{
    pieces_->setEnabled(!seeder_->isChecked());
    add_->setEnabled(!name_->text().trimmed().isEmpty()
        && (seeder_->isChecked() || pieces_->hasAcceptableInput())
        && upload_->hasAcceptableInput() && upload_->value() > 0
        && download_->hasAcceptableInput() && download_->value() > 0);
}

ScenarioPeer AddPeerDialog::peer() const
{
    ScenarioPeer result;
    result.name = name_->text().trimmed();
    result.initialRole = seeder_->isChecked() ? ScenarioPeer::Role::Seeder : ScenarioPeer::Role::Leecher;
    result.initialPieceCount = seeder_->isChecked() ? swarmPieceCount_ : pieces_->value();
    // Integer inputs and bounded multipliers keep conversion exact without overflow.
    result.uploadBytesPerSecond = static_cast<quint64>(upload_->value()) * uploadUnit_->currentData().toULongLong();
    result.downloadBytesPerSecond = static_cast<quint64>(download_->value()) * downloadUnit_->currentData().toULongLong();
    result.uploadDisplayValue = QString::number(upload_->value());
    result.uploadDisplayUnit = uploadUnit_->currentText();
    result.downloadDisplayValue = QString::number(download_->value());
    result.downloadDisplayUnit = downloadUnit_->currentText();
    return result;
}
