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
    blockSize_ = makeSizeInput(QStringLiteral("16"));
    blockUnit_ = makeUnits(false);
    auto* blockLabel = new QLabel(tr("Block size:"), this);
    blockLabel->setBuddy(blockSize_);
    grid->addWidget(blockLabel, 6, 0);
    grid->addWidget(blockSize_, 6, 1);
    grid->addWidget(blockUnit_, 6, 2);
    geometrySummary_ = new QLabel(this);
    geometrySummary_->setWordWrap(true);
    geometrySummary_->setTextFormat(Qt::PlainText);
    grid->addWidget(geometrySummary_, 7, 0, 1, 4);

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
    for (auto* input : {name_, virtualSize_, pieceSize_, blockSize_})
        connect(input, &QLineEdit::textChanged, this, &AddSwarmDialog::updateValues);
    for (auto* unit : {virtualUnit_, pieceUnit_, blockUnit_})
        connect(unit, &QComboBox::currentIndexChanged, this, &AddSwarmDialog::updateValues);
    updateValues();
    name_->selectAll();
    name_->setFocus();
}

AddSwarmDialog::AddSwarmDialog(const ScenarioSwarm& swarm, QWidget* parent)
    : AddSwarmDialog(swarm.name, parent)
{
    original_ = swarm;
    editing_ = true;
    setWindowTitle(tr("Edit Swarm"));
    create_->setText(tr("Save"));
    filePath_ = swarm.filePath;
    fileSizeBytes_ = swarm.totalSizeBytes;
    // Preserve saved units when valid, and never round exact byte metadata.
    auto restore = [](QLineEdit* value, QComboBox* units, quint64 bytes,
                      const QString& savedValue, const QString& savedUnit) {
        const int savedIndex = units->findText(savedUnit);
        if (savedIndex >= 0) {
            units->setCurrentIndex(savedIndex);
            value->setText(savedValue);
            if (sizeInBytes(value, units) == bytes) return;
        }
        for (int i = units->count() - 1; i >= 0; --i) {
            const auto multiplier = units->itemData(i).toULongLong();
            if (bytes >= multiplier && bytes % multiplier == 0) {
                units->setCurrentIndex(i);
                value->setText(QString::number(bytes / multiplier));
                return;
            }
        }
        // Compatibility for programmatically created scenarios with sub-KiB sizes.
        units->addItem(QStringLiteral("B"), QVariant::fromValue(quint64{1}));
        units->setCurrentIndex(units->count() - 1);
        value->setText(QString::number(bytes));
    };
    restore(virtualSize_, virtualUnit_, swarm.totalSizeBytes, swarm.virtualSizeDisplayValue, swarm.virtualSizeDisplayUnit);
    restore(pieceSize_, pieceUnit_, swarm.pieceSizeBytes, swarm.pieceSizeDisplayValue, swarm.pieceSizeDisplayUnit);
    restore(blockSize_, blockUnit_, swarm.blockSizeBytes, swarm.blockSizeDisplayValue, swarm.blockSizeDisplayUnit);
    for (auto* radio : findChildren<QRadioButton*>())
        radio->setChecked(radio == fileMode_ ? swarm.mode == ScenarioSwarm::Mode::File
                                          : swarm.mode == ScenarioSwarm::Mode::Virtual);
    // Existing ownership is indexed by piece. Keep its geometry when peers exist.
    if (!swarm.peers.empty()) {
        pieceSize_->setEnabled(false);
        pieceUnit_->setEnabled(false);
        for (auto* radio : findChildren<QRadioButton*>()) radio->setEnabled(false);
        pieceSize_->setToolTip(tr("Piece size is fixed once peers have been added."));
    }
    updateFileLabel();
    updateValues();
}

void AddSwarmDialog::updateValues()
{
    const bool fileMode = fileMode_->isChecked();
    const bool locked = editing_ && !original_.peers.empty();
    browse_->setEnabled(fileMode && !locked);
    virtualSize_->setEnabled(!fileMode && !locked);
    virtualUnit_->setEnabled(!fileMode && !locked);
    totalSizeBytes_ = locked ? original_.totalSizeBytes
        : fileMode ? fileSizeBytes_ : sizeInBytes(virtualSize_, virtualUnit_);
    pieceSizeBytes_ = locked ? original_.pieceSizeBytes : sizeInBytes(pieceSize_, pieceUnit_);
    blockSizeBytes_ = sizeInBytes(blockSize_, blockUnit_);
    const bool validBlock = blockSizeBytes_ > 0 && blockSizeBytes_ <= pieceSizeBytes_;
    const bool validSizes = totalSizeBytes_ > 0 && pieceSizeBytes_ > 0;
    pieceCount_ = validSizes ? totalSizeBytes_ / pieceSizeBytes_
        + (totalSizeBytes_ % pieceSizeBytes_ != 0) : 0;
    if (totalSizeBytes_ == 0) {
        geometrySummary_->setText(tr("Select a file or virtual size to calculate pieces and blocks."));
    } else if (!validSizes || !validBlock) {
        geometrySummary_->setText(tr("Piece and block sizes must be positive, and block size must not exceed piece size."));
    } else {
        const quint64 blocksPerPiece = pieceSizeBytes_ / blockSizeBytes_
            + (pieceSizeBytes_ % blockSizeBytes_ != 0);
        const quint64 fullPieces = totalSizeBytes_ / pieceSizeBytes_;
        const quint64 tailBytes = totalSizeBytes_ % pieceSizeBytes_;
        // Count the shorter final piece separately. Counts cannot exceed total bytes.
        const quint64 totalBlocks = fullPieces * blocksPerPiece
            + tailBytes / blockSizeBytes_ + (tailBytes % blockSizeBytes_ != 0);
        const QLocale locale;
        geometrySummary_->setText(tr("File has %1 pieces and %2 blocks (%3 per full piece).")
            .arg(locale.toString(pieceCount_), locale.toString(totalBlocks), locale.toString(blocksPerPiece)));
    }
    create_->setEnabled(!name_->text().trimmed().isEmpty() && validSizes && validBlock);
}

ScenarioSwarm AddSwarmDialog::swarm() const
{
    ScenarioSwarm result = original_;
    result.name = name_->text().trimmed();
    result.mode = fileMode_->isChecked() ? ScenarioSwarm::Mode::File : ScenarioSwarm::Mode::Virtual;
    result.filePath = fileMode_->isChecked() ? filePath_ : QString{};
    result.totalSizeBytes = totalSizeBytes_;
    result.pieceSizeBytes = pieceSizeBytes_;
    result.pieceCount = pieceCount_;
    result.blockSizeBytes = blockSizeBytes_;
    result.blockSizeDisplayValue = blockSize_->text();
    result.blockSizeDisplayUnit = blockUnit_->currentText();
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
