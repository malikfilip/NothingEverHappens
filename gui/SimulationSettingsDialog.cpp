#include "SimulationSettingsDialog.hpp"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

SimulationSettingsDialog::SimulationSettingsDialog(const ScenarioSettings& settings, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Simulation Settings"));
    auto* layout = new QVBoxLayout(this);
    auto* group = new QGroupBox(tr("BitTorrent / Choking"), this);
    auto* form = new QFormLayout(group);
    auto makeInterval = [group](double value) {
        auto* input = new QDoubleSpinBox(group);
        input->setDecimals(3);
        input->setRange(0.0, 86400.0);
        input->setSingleStep(1.0);
        input->setSuffix(QStringLiteral(" s"));
        input->setKeyboardTracking(false);
        input->setValue(value);
        input->setToolTip(tr("Simulated seconds, up to 86400 s (one day)."));
        return input;
    };
    regular_ = makeInterval(settings.bitTorrent.regularRechokeInterval);
    optimistic_ = makeInterval(settings.bitTorrent.optimisticUnchokeInterval);
    form->addRow(tr("Regular rechoke interval:"), regular_);
    form->addRow(tr("Optimistic unchoke interval:"), optimistic_);
    layout->addWidget(group);
    error_ = new QLabel(this);
    error_->setWordWrap(true);
    error_->setTextFormat(Qt::PlainText);
    error_->hide();
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SimulationSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

ScenarioSettings SimulationSettingsDialog::settings() const
{
    return {{regular_->value(), optimistic_->value()}};
}

void SimulationSettingsDialog::accept()
{
    if (!regular_->hasAcceptableInput() || !optimistic_->hasAcceptableInput()) {
        error_->setText(tr("Enter valid intervals between 0.001 and 86400 simulated seconds."));
        error_->show();
        return;
    }
    regular_->interpretText();
    optimistic_->interpretText();
    if (const auto* error = settings().bitTorrent.validationError()) {
        error_->setText(QString::fromUtf8(error));
        error_->show();
        return;
    }
    QDialog::accept();
}
