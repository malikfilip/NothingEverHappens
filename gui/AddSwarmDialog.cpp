#include "AddSwarmDialog.hpp"

#include <limits>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

namespace {
quint64 sizeInBytes(const QLineEdit* input, const QComboBox* unit)
{
    bool ok = false;
    const auto value = input->text().toULongLong(&ok);
    const auto multiplier = unit->currentData().toULongLong();
    if (!input->hasAcceptableInput() || !ok || value == 0 || multiplier == 0
        || value > std::numeric_limits<quint64>::max() / multiplier)
        return 0;
    return value * multiplier;
}
}

AddSwarmDialog::AddSwarmDialog(const QString& defaultName, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Add Swarm"));
    setMinimumWidth(460);
    auto* layout = new QVBoxLayout(this);
    auto* grid = new QGridLayout;
    grid->setColumnStretch(3, 1);
    layout->addLayout(grid);

    name_ = new QLineEdit(defaultName, this);
    auto* nameLabel = new QLabel(tr("Name:"), this);
    nameLabel->setBuddy(name_);
    grid->addWidget(nameLabel, 0, 0);
    grid->addWidget(name_, 0, 1, 1, 3);
    auto addSeparator = [this, grid](int row) {
        auto* line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        grid->addWidget(line, row, 0, 1, 4);
    };
    addSeparator(1);

    fileMode_ = new QRadioButton(tr("File:"), this);
    fileMode_->setChecked(true);
    browse_ = new QPushButton(tr("Browse..."), this);
    filename_ = new QLabel(tr("No file"), this);
    filename_->setTextFormat(Qt::PlainText);
    filename_->setMinimumWidth(100);
    filename_->installEventFilter(this);
    filename_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    grid->addWidget(fileMode_, 2, 0);
    grid->addWidget(browse_, 2, 1);
    grid->addWidget(filename_, 2, 2, 1, 2);

    auto makeSizeInput = [this](const QString& value) {
        auto* input = new QLineEdit(value, this);
        input->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9]{1,20}")), input));
        return input;
    };
    auto makeUnits = [this](bool includeGiB) {
        auto* units = new QComboBox(this);
        units->addItem(tr("KiB"), QVariant::fromValue(quint64{1024}));
        units->addItem(tr("MiB"), QVariant::fromValue(quint64{1024} * 1024));
        if (includeGiB)
            units->addItem(tr("GiB"), QVariant::fromValue(quint64{1024} * 1024 * 1024));
        return units;
    };
    auto* virtualMode = new QRadioButton(tr("Virtual size:"), this);
    virtualSize_ = makeSizeInput(QStringLiteral("1024"));
    virtualUnit_ = makeUnits(true);
    virtualUnit_->setCurrentIndex(1);
    grid->addWidget(virtualMode, 3, 0);
    grid->addWidget(virtualSize_, 3, 1);
    grid->addWidget(virtualUnit_, 3, 2);
    addSeparator(4);

    pieceSize_ = makeSizeInput(QStringLiteral("256"));
    pieceUnit_ = makeUnits(false);
    auto* pieceLabel = new QLabel(tr("Piece size:"), this);
    pieceLabel->setBuddy(pieceSize_);
    grid->addWidget(pieceLabel, 5, 0);
    grid->addWidget(pieceSize_, 5, 1);
    grid->addWidget(pieceUnit_, 5, 2);
    pieces_ = new QLineEdit(this);
    pieces_->setReadOnly(true);
    auto* piecesLabel = new QLabel(tr("Pieces:"), this);
    piecesLabel->setBuddy(pieces_);
    grid->addWidget(piecesLabel, 6, 0);
    grid->addWidget(pieces_, 6, 1, 1, 3);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    create_ = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    create_->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        updateValues();
        if (create_->isEnabled()) accept();
    });
    connect(browse_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Select swarm file"));
        if (path.isEmpty()) return;
        const QFileInfo info(path);
        filePath_ = info.absoluteFilePath();
        fileSizeBytes_ = info.isFile() && info.size() > 0
            ? static_cast<quint64>(info.size()) : 0;
        updateFileLabel();
        filename_->setToolTip(filePath_);
        updateValues();
    });
    connect(fileMode_, &QRadioButton::toggled, this, &AddSwarmDialog::updateValues);
    for (auto* input : {name_, virtualSize_, pieceSize_})
        connect(input, &QLineEdit::textChanged, this, &AddSwarmDialog::updateValues);
    for (auto* unit : {virtualUnit_, pieceUnit_})
        connect(unit, &QComboBox::currentIndexChanged, this, &AddSwarmDialog::updateValues);
    updateValues();
    name_->selectAll();
    name_->setFocus();
}

void AddSwarmDialog::updateValues()
{
    const bool fileMode = fileMode_->isChecked();
    browse_->setEnabled(fileMode);
    virtualSize_->setEnabled(!fileMode);
    virtualUnit_->setEnabled(!fileMode);
    totalSizeBytes_ = fileMode ? fileSizeBytes_ : sizeInBytes(virtualSize_, virtualUnit_);
    pieceSizeBytes_ = sizeInBytes(pieceSize_, pieceUnit_);
    const bool validSizes = totalSizeBytes_ > 0 && pieceSizeBytes_ > 0;
    pieceCount_ = validSizes ? totalSizeBytes_ / pieceSizeBytes_
        + (totalSizeBytes_ % pieceSizeBytes_ != 0) : 0;
    pieces_->setText(validSizes ? QString::number(pieceCount_) : QStringLiteral("?"));
    create_->setEnabled(!name_->text().trimmed().isEmpty() && validSizes);
}

ScenarioSwarm AddSwarmDialog::swarm() const
{
    ScenarioSwarm result;
    result.name = name_->text().trimmed();
    result.mode = fileMode_->isChecked() ? ScenarioSwarm::Mode::File : ScenarioSwarm::Mode::Virtual;
    result.filePath = fileMode_->isChecked() ? filePath_ : QString{};
    result.totalSizeBytes = totalSizeBytes_;
    result.pieceSizeBytes = pieceSizeBytes_;
    result.pieceCount = pieceCount_;
    if (result.mode == ScenarioSwarm::Mode::Virtual) {
        result.virtualSizeDisplayValue = virtualSize_->text();
        result.virtualSizeDisplayUnit = virtualUnit_->currentText();
    }
    result.pieceSizeDisplayValue = pieceSize_->text();
    result.pieceSizeDisplayUnit = pieceUnit_->currentText();
    return result;
}

void AddSwarmDialog::updateFileLabel()
{
    if (filePath_.isEmpty()) return;

    const QLocale locale;
    auto size = locale.formattedDataSize(static_cast<qint64>(fileSizeBytes_), 1,
        QLocale::DataSizeIecFormat);
    size.replace(locale.decimalPoint() + locale.zeroDigit() + QStringLiteral(" "),
        QStringLiteral(" "));
    const auto suffix = tr(" (%1)").arg(size);
    const auto metrics = filename_->fontMetrics();
    const int suffixWidth = metrics.horizontalAdvance(suffix);
    filename_->setMinimumWidth(qMax(100,
        suffixWidth + metrics.horizontalAdvance(QStringLiteral("..."))));
    const int availableWidth = qMax(0, filename_->contentsRect().width() - suffixWidth);
    filename_->setText(metrics.elidedText(QFileInfo(filePath_).fileName(),
        Qt::ElideMiddle, availableWidth) + suffix);
}

bool AddSwarmDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == filename_ && (event->type() == QEvent::Resize
        || event->type() == QEvent::FontChange))
        updateFileLabel();
    return QDialog::eventFilter(watched, event);
}
