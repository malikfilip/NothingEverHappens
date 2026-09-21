#include "MainWindow.hpp"
#include "simulator/PeerCompletedEvent.hpp"
#include "SimulationSettingsDialog.hpp"
#include "MessageInfoDialog.hpp"
#include "AddSwarmDialog.hpp"
#include "AddPeerDialog.hpp"
#include "SimulationView.hpp"
#include "RuntimeSession.hpp"
#include "ScenarioPieces.hpp"
#include "ScenarioPersistence.hpp"
#include <QFileDialog>
#include <QSignalBlocker>
#include <set>
#include "PeerInspection.hpp"
#include "PieceBitmapDialog.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <utility>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLocale>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
quint64 availableScenarioId(const std::set<quint64>& used)
{
    quint64 id = 1;
    while (used.contains(id)) ++id;
    return id;
}

QIcon settingsIcon(const QPalette& palette)
{
    // QStyle has no portable gear icon. Paint a palette-aware fallback using Qt.
    QPixmap pixmap(64, 64);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath gear;
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < 32; ++i) {
        const double angle = i * 2 * pi / 32;
        const double radius = i % 4 == 0 || i % 4 == 3 ? 10.0 : 14.0;
        const QPointF point(16 + radius * std::cos(angle), 16 + radius * std::sin(angle));
        if (i == 0) gear.moveTo(point);
        else gear.lineTo(point);
    }
    gear.closeSubpath();
    gear.addEllipse(QPointF(16, 16), 5, 5);
    gear.setFillRule(Qt::OddEvenFill);
    painter.fillPath(gear, palette.color(QPalette::ButtonText));
    painter.end();
    return QIcon::fromTheme(QStringLiteral("preferences-system"), QIcon(pixmap));
}

QString simulationTimeText(double time)
{
    // Unbounded total minutes; seconds/milliseconds conversions stay bounded.
    if (!std::isfinite(time) || time < 0) return QStringLiteral("Sim Time: %1 s").arg(time);
    const double minutes = std::floor(time / 60);
    const int milliseconds = static_cast<int>(std::floor(std::fmod(time, 60.0) * 1000));
    return QStringLiteral("Sim Time: %1:%2.%3")
        .arg(QString::number(minutes, 'f', 0).rightJustified(2, QLatin1Char('0')))
        .arg(milliseconds / 1000, 2, 10, QLatin1Char('0'))
        .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("picoTorrent Simulator"));
    resize(1280, 820);
    setMinimumSize(640, 480);
    createToolbar();
    completionTimer_.setSingleShot(true);
    completionTimer_.setInterval(0);
    connect(&completionTimer_, &QTimer::timeout, this, &MainWindow::showNextCompletion);
    pump_.playbackChanged = [this] {
        simulationTime_->setText(simulationTimeText(pump_.playbackTime()));
    };
    pump_.stateChanged = [this] {
        updateRuntimeControls();
        pump_.playbackChanged();
    };
    pump_.stepped = [this] {
        simulationTime_->setText(simulationTimeText(pump_.playbackTime()));
        eventLog_->scrollToBottom();
        refreshInspector();
        presentCompletions();
    };
    pump_.failed = [this](const char* message) {
        simulationTime_->setText(simulationTimeText(pump_.playbackTime()));
        eventLog_->scrollToBottom();
        refreshInspector();
        QMessageBox::critical(this, tr("Simulation paused after an error"), QString::fromUtf8(message));
        resumeAfterCompletions_ = false;
        presentCompletions();
    };

    auto* canvasPanel = new QGroupBox(tr("Simulation Canvas"));
    auto* canvasLayout = new QVBoxLayout(canvasPanel);
    canvas_ = new SimulationView(canvasPanel);
    canvasLayout->addWidget(canvas_);
    canvas_->selectionChanged = [this] { refreshInspector(); };
    canvas_->peerPlaced = [this](quint64 swarmId, const ScenarioPeer& peer) {
        if (auto* swarm = findSwarm(swarmId)) {
            swarm->peers.push_back(peer);
            ++nextPeerId_;
        }
        refreshInspector();
    };
    canvas_->peerMoved = [this](quint64 swarmId, quint64 peerId, QPointF position) {
        if (auto* swarm = findSwarm(swarmId)) {
            for (auto& peer : swarm->peers)
                if (peer.id == peerId) { peer.position = position; break; }
        }
    };
    canvas_->peerRemoved = [this](quint64 swarmId, quint64 peerId) {
        if (auto* swarm = findSwarm(swarmId))
            std::erase_if(swarm->peers, [peerId](const ScenarioPeer& peer) { return peer.id == peerId; });
    };
    canvas_->peerInitiallyJoinedChanged = [this](quint64 swarmId, quint64 peerId, bool joined) {
        if (auto* swarm = findSwarm(swarmId)) {
            for (auto& peer : swarm->peers)
                if (peer.id == peerId) { peer.initiallyJoined = joined; break; }
        }
        refreshInspector();
    };
    canvas_->trackerMoved = [this](quint64 swarmId, QPointF position) {
        if (auto* swarm = findSwarm(swarmId)) swarm->trackerPosition = position;
    };
    canvas_->placementChanged = [this] {
        addPeerAction_->setEnabled(!runtime_ && swarmSelector_->currentIndex() >= 0 && !canvas_->isPlacingPeer());
        updateRuntimeControls();
        canvas_->setToolTip(canvas_->isPlacingPeer()
            ? tr("Click to place the peer. Escape or right-click cancels placement.") : QString{});
    };

    auto* right = new QSplitter(Qt::Vertical);
    right->setObjectName("inspectorFilterSplitter");
    right->setChildrenCollapsible(false);
    right->addWidget(createInspector());
    right->addWidget(createMessageFilter());
    right->setStretchFactor(0, 2);
    right->setStretchFactor(1, 1);
    right->setSizes({360, 180});

    auto* upper = new QSplitter(Qt::Horizontal);
    upper->setObjectName("upperSplitter");
    upper->setChildrenCollapsible(false);
    upper->addWidget(canvasPanel);
    upper->addWidget(right);
    upper->setStretchFactor(0, 4);
    upper->setStretchFactor(1, 1);
    upper->setSizes({960, 300});

    auto* main = new QSplitter(Qt::Vertical);
    main->setObjectName("mainSplitter");
    main->setChildrenCollapsible(false);
    main->addWidget(upper);
    main->addWidget(createEventLog());
    main->setStretchFactor(0, 3);
    main->setStretchFactor(1, 1);
    main->setSizes({540, 220});

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(main);
    setCentralWidget(central);
}

void MainWindow::createToolbar()
{
    auto* toolbar = addToolBar(tr("Simulation Controls"));
    toolbar->setObjectName("simulationToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    addSwarmAction_ = toolbar->addAction(tr("+ Swarm"));
    connect(addSwarmAction_, &QAction::triggered, this, &MainWindow::addSwarm);
    addPeerAction_ = toolbar->addAction(tr("+ Peer"));
    addPeerAction_->setEnabled(false);
    connect(addPeerAction_, &QAction::triggered, this, &MainWindow::addPeer);
    toolbar->addSeparator();
    stopAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop"));
    stopAction_->setEnabled(false);
    connect(stopAction_, &QAction::triggered, this, &MainWindow::stopSimulation);
    playAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"));
    connect(playAction_, &QAction::triggered, this, &MainWindow::play);
    pauseAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_MediaPause), tr("Pause"));
    pauseAction_->setEnabled(false);
    connect(pauseAction_, &QAction::triggered, this, [this] { pump_.pause(); });
    nextEventAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_MediaSkipForward), tr("Next Event"));
    nextEventAction_->setEnabled(false);
    connect(nextEventAction_, &QAction::triggered, this, [this] { pump_.nextEvent(); });
    toolbar->addSeparator();

    auto* speedLabel = new QLabel(tr("Speed:"), toolbar);
    toolbar->addWidget(speedLabel);
    auto* speed = new QComboBox(toolbar);
    speed->setObjectName("playbackSpeed");
    for (const double value : {0.5, 1.0, 2.0, 5.0, 10.0})
        speed->addItem(QString::number(value) + QStringLiteral("x"), value);
    speed->addItem(tr("Max"), 0.0);
    speed->setCurrentIndex(1);
    speedLabel->setBuddy(speed);
    toolbar->addWidget(speed);
    connect(speed, &QComboBox::currentIndexChanged, this, [this, speed] {
        pump_.setPlaybackSpeed(speed->currentData().toDouble());
    });

    auto* time = new QLabel(simulationTimeText(0), toolbar);
    simulationTime_ = time;
    time->setObjectName("simulationTime");
    time->setContentsMargins(6, 0, 6, 0);
    toolbar->addWidget(time);
    toolbar->addSeparator();

    auto* swarmLabel = new QLabel(tr("Swarm:"), toolbar);
    toolbar->addWidget(swarmLabel);
    swarmSelector_ = new QComboBox(toolbar);
    swarmSelector_->setObjectName("swarmSelector");
    swarmSelector_->setPlaceholderText(tr("No swarms"));
    swarmSelector_->setMinimumContentsLength(12);
    swarmSelector_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    swarmSelector_->setEnabled(false);
    swarmLabel->setBuddy(swarmSelector_);
    toolbar->addWidget(swarmSelector_);
    swarmInfo_ = toolbar->addAction(tr("Swarm Info"));
    swarmInfo_->setEnabled(false);
    connect(swarmInfo_, &QAction::triggered, this, &MainWindow::showSwarmInfo);
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    importAction_ = toolbar->addAction(tr("Import"));
    exportAction_ = toolbar->addAction(tr("Export"));
    connect(importAction_, &QAction::triggered, this, &MainWindow::importScenario);
    connect(exportAction_, &QAction::triggered, this, &MainWindow::exportScenario);
    settingsAction_ = toolbar->addAction(settingsIcon(palette()), tr("Settings"));
    settingsAction_->setObjectName("simulationSettingsAction");
    connect(settingsAction_, &QAction::triggered, this, &MainWindow::showSettings);
    connect(swarmSelector_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool active = index >= 0 && static_cast<std::size_t>(index) < swarms_.size();
        swarmInfo_->setEnabled(active);
        addPeerAction_->setEnabled(active && !runtime_);
        if (canvas_) {
            canvas_->showSwarm(active ? &swarms_[index] : nullptr);
            refreshInspector();
        }
    });
}

void MainWindow::showSettings()
{
    if (runtime_) return;
    SimulationSettingsDialog dialog(settings_, this);
    if (dialog.exec() == QDialog::Accepted) settings_ = dialog.settings();
}

void MainWindow::exportScenario()
{
    if (runtime_ || pump_.state() != RuntimePump::State::Edit) return;
    QFileDialog dialog(this, tr("Export Scenario"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(tr("picoTorrent Scenario (*.pt)"));
    dialog.setDefaultSuffix(QStringLiteral("pt"));
    if (dialog.exec() != QDialog::Accepted) return;
    try {
        ScenarioPersistence::save(dialog.selectedFiles().front(), {swarms_, settings_, scenarioSeed_});
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Export Scenario"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::importScenario()
{
    if (runtime_ || pump_.state() != RuntimePump::State::Edit) return;
    QFileDialog dialog(this, tr("Import Scenario"));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilter(tr("picoTorrent Scenario (*.pt)"));
    dialog.setDefaultSuffix(QStringLiteral("pt"));
    if (dialog.exec() != QDialog::Accepted) return;
    ScenarioProject imported;
    try {
        imported = ScenarioPersistence::load(dialog.selectedFiles().front());
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Import Scenario"), QString::fromUtf8(error.what()));
        return; // No editor changes until the complete document has validated.
    }
    canvas_->showSwarm(nullptr); // Cancel placement and selection from the old project.
    swarms_.swap(imported.swarms);
    settings_ = imported.settings;
    scenarioSeed_ = imported.seed;
    nextPeerId_ = 1; // addPeer chooses an unused ID across all imported swarms.
    {
        const QSignalBlocker blocked(swarmSelector_);
        swarmSelector_->clear();
        for (const auto& swarm : swarms_)
            swarmSelector_->addItem(swarm.name, QVariant::fromValue(swarm.id));
        swarmSelector_->setCurrentIndex(-1);
        swarmSelector_->setEnabled(!swarms_.empty());
    }
    swarmInfo_->setEnabled(!swarms_.empty());
    if (!swarms_.empty()) swarmSelector_->setCurrentIndex(0);
    eventLogModel_->removeRows(0, eventLogModel_->rowCount());
    pump_.stop();
    refreshInspector();
    updateRuntimeControls();
}

QWidget* MainWindow::createInspector()
{
    auto* panel = new QGroupBox(tr("Inspector"));
    panel->setMinimumWidth(210);
    auto* layout = new QVBoxLayout(panel);
    inspectorContents_ = new QStackedWidget(panel);
    inspectorContents_->setObjectName("inspectorContents");
    auto* placeholder = new QWidget(inspectorContents_);
    auto* placeholderLayout = new QVBoxLayout(placeholder);
    auto* hint = new QLabel(tr("Left-click a peer to inspect its state."), placeholder);
    hint->setWordWrap(true);
    placeholderLayout->addWidget(hint);
    placeholderLayout->addStretch();
    inspectorContents_->addWidget(placeholder);
    auto* details = new QWidget(inspectorContents_);
    auto* detailsLayout = new QVBoxLayout(details);
    auto* scroll = new QScrollArea(details);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    inspectorDetails_ = new QLabel(scroll);
    inspectorDetails_->setObjectName("peerInspectorDetails");
    inspectorDetails_->setTextFormat(Qt::PlainText);
    inspectorDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inspectorDetails_->setWordWrap(true);
    inspectorDetails_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    scroll->setWidget(inspectorDetails_);
    detailsLayout->addWidget(scroll, 1);
    viewPieces_ = new QPushButton(tr("View Pieces..."), details);
    viewPieces_->setObjectName("viewPeerPieces");
    detailsLayout->addWidget(viewPieces_);
    inspectorContents_->addWidget(details);
    piecesDialog_ = new PieceBitmapDialog(this);
    connect(viewPieces_, &QPushButton::clicked, this, [this] {
        piecesDialog_->show();
        refreshInspector();
        piecesDialog_->raise();
        piecesDialog_->activateWindow();
    });
    layout->addWidget(inspectorContents_);
    return panel;
}

QWidget* MainWindow::createEventLog()
{
    auto* panel = new QGroupBox(tr("Event Log"));
    auto* layout = new QVBoxLayout(panel);
    auto* table = new QTableView(panel);
    table->setObjectName("eventLog");
    table->setMinimumHeight(100);
    eventLog_ = table;
    auto* model = new QStandardItemModel(0, 2, table);
    eventLogModel_ = model;
    model->setHorizontalHeaderLabels({tr("Time (s)"), tr("Event")});
    table->setModel(model);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setMinimumSectionSize(55);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    const std::array<int, 2> widths{125, 650};
    for (int column = 0; column < static_cast<int>(widths.size()); ++column)
        table->setColumnWidth(column, widths[column]);
    layout->addWidget(table);
    return panel;
}

QWidget* MainWindow::createMessageFilter()
{
    auto* panel = new QGroupBox(tr("Message Filter"));
    auto* layout = new QVBoxLayout(panel);
    auto* buttons = new QHBoxLayout;
    auto* all = new QPushButton(tr("All"), panel);
    all->setObjectName("filterAll");
    auto* none = new QPushButton(tr("None"), panel);
    none->setObjectName("filterNone");
    buttons->addWidget(all);
    buttons->addWidget(none);
    buttons->addStretch();
    layout->addLayout(buttons);

    auto* scroll = new QScrollArea(panel);
    scroll->setObjectName("messageFilterScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* contents = new QWidget(scroll);
    auto* checksLayout = new QGridLayout(contents);
    checksLayout->setContentsMargins(0, 0, 0, 0);
    using MessageType = MessageInfoDialog::Type;
    const std::array<MessageType, 10> types{
        MessageType::Handshake, MessageType::Bitfield, MessageType::Have,
        MessageType::Interested, MessageType::NotInterested, MessageType::Choke,
        MessageType::Unchoke, MessageType::Request, MessageType::Piece, MessageType::Cancel
    };
    std::array<QCheckBox*, 10> checks{};
    for (std::size_t i = 0; i < types.size(); ++i) {
        checks[i] = new QCheckBox(MessageInfoDialog::messageName(types[i]), contents);
        checks[i]->setChecked(true);
        const int row = static_cast<int>(i);
        checksLayout->addWidget(checks[i], row, 0);
        const auto type = types[i];
        auto* icon = new QToolButton(contents);
        icon->setAutoRaise(true);
        icon->setToolButtonStyle(Qt::ToolButtonIconOnly);
        icon->setIcon(QIcon(QPixmap(MessageInfoDialog::iconPath(type)).scaled(24, 24,
            Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        icon->setIconSize(QSize(24, 24));
        icon->setToolTip(tr("About %1").arg(MessageInfoDialog::messageName(type)));
        icon->setAccessibleName(icon->toolTip());
        connect(icon, &QToolButton::clicked, this, [this, type] {
            MessageInfoDialog dialog(type, this);
            dialog.exec();
        });
        checksLayout->addWidget(icon, row, 1);
    }
    checksLayout->setColumnStretch(2, 1);
    checksLayout->setRowStretch(static_cast<int>(types.size()), 1);
    scroll->setWidget(contents);
    layout->addWidget(scroll);

    connect(all, &QPushButton::clicked, panel, [checks] {
        for (auto* check : checks) check->setChecked(true);
    });
    connect(none, &QPushButton::clicked, panel, [checks] {
        for (auto* check : checks) check->setChecked(false);
    });
    return panel;
}

void MainWindow::addSwarm()
{
    if (runtime_) return;
    AddSwarmDialog dialog(tr("Swarm %1").arg(static_cast<qulonglong>(swarms_.size() + 1)), this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto swarm = dialog.swarm();
    std::set<quint64> usedIds;
    for (const auto& existing : swarms_) usedIds.insert(existing.id);
    swarm.id = availableScenarioId(usedIds);
    swarms_.push_back(swarm);
    swarmSelector_->addItem(swarm.name, QVariant::fromValue(swarm.id));
    swarmSelector_->setEnabled(true);
    swarmSelector_->setCurrentIndex(swarmSelector_->count() - 1);
}

void MainWindow::showSwarmInfo()
{
    const int index = swarmSelector_->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= swarms_.size()) return;
    const auto& swarm = swarms_[index];
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Swarm Info"));
    dialog.setMinimumWidth(360);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto addRow = [&dialog, form](const QString& title, const QString& value) {
        auto* label = new QLabel(value, &dialog);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(title, label);
        return label;
    };
    const QLocale locale;
    addRow(tr("Name:"), swarm.name);
    auto* source = addRow(tr("Source:"), swarm.mode == ScenarioSwarm::Mode::File
        ? QFileInfo(swarm.filePath).fileName() : tr("Virtual"));
    source->setToolTip(swarm.filePath);
    addRow(tr("Total size:"), swarm.mode == ScenarioSwarm::Mode::File
        ? locale.formattedDataSize(static_cast<qint64>(swarm.totalSizeBytes), 1,
            QLocale::DataSizeIecFormat)
        : tr("%1 %2").arg(swarm.virtualSizeDisplayValue, swarm.virtualSizeDisplayUnit));
    addRow(tr("Piece size:"), tr("%1 %2").arg(swarm.pieceSizeDisplayValue, swarm.pieceSizeDisplayUnit));
    addRow(tr("Block size:"), tr("%1 %2").arg(swarm.blockSizeDisplayValue, swarm.blockSizeDisplayUnit));
    addRow(tr("Piece count:"), locale.toString(swarm.pieceCount));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (!runtime_) {
        auto* edit = buttons->addButton(tr("Edit Swarm..."), QDialogButtonBox::ActionRole);
        connect(edit, &QPushButton::clicked, &dialog, &QDialog::accept);
    }
    layout->addWidget(buttons);
    const auto id = swarm.id;
    if (dialog.exec() == QDialog::Accepted) editSwarm(id);
}

ScenarioSwarm* MainWindow::findSwarm(quint64 id)
{
    const auto it = std::find_if(swarms_.begin(), swarms_.end(),
        [id](const ScenarioSwarm& swarm) { return swarm.id == id; });
    return it == swarms_.end() ? nullptr : &*it;
}

void MainWindow::addPeer()
{
    if (runtime_) return;
    const int index = swarmSelector_->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= swarms_.size() || canvas_->isPlacingPeer()) return;
    std::set<quint64> usedIds;
    for (const auto& swarm : swarms_)
        for (const auto& peer : swarm.peers) usedIds.insert(peer.id);
    nextPeerId_ = availableScenarioId(usedIds);
    AddPeerDialog dialog(tr("Peer %1").arg(nextPeerId_), swarms_[index].pieceCount, this);
    if (dialog.exec() != QDialog::Accepted) return;
    auto peer = dialog.peer();
    peer.id = nextPeerId_;
    try {
        ensureScenarioPieces(swarms_[index], peer, scenarioSeed_);
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Add Peer"), QString::fromUtf8(error.what()));
        return;
    }
    canvas_->beginPeerPlacement(peer);
}

MainWindow::~MainWindow() = default;

void MainWindow::play()
{
    if (pump_.state() == RuntimePump::State::Paused) {
        pump_.resume();
        return;
    }
    if (runtime_ || canvas_->isPlacingPeer()) return;
    const auto showError = [this](const QString& message) {
        QMessageBox error(QMessageBox::Critical, tr("Start Simulation"), message, QMessageBox::Ok, this);
        error.setTextFormat(Qt::PlainText);
        error.exec();
    };
    const auto validation = RuntimeSession::preflight(swarms_);
    if (!validation.errors.isEmpty()) {
        showError(validation.errors.join(QStringLiteral("\n")));
        return;
    }
    if (validation.hasWarnings()) {
        QString message = tr("Some swarms have configuration warnings:");
        const auto append = [&message](const QString& title, const QStringList& names) {
            if (!names.isEmpty()) message += QStringLiteral("\n\n") + title
                + QStringLiteral("\n- ") + names.join(QStringLiteral("\n- "));
        };
        append(tr("Only one initially active peer (no peer-to-peer exchange can occur until another peer joins):"),
            validation.singlePeerSwarms);
        append(tr("No initially active leechers:"), validation.noLeecherSwarms);
        append(tr("Swarms with no initially joined peers will not participate:"), validation.skippedSwarms);
        message += tr("\n\nStart anyway?");
        QMessageBox warning(QMessageBox::Warning, tr("Start Simulation"), message, QMessageBox::Cancel, this);
        warning.setTextFormat(Qt::PlainText);
        auto* start = warning.addButton(tr("Start Anyway"), QMessageBox::AcceptRole);
        warning.setDefaultButton(QMessageBox::Cancel);
        warning.exec();
        if (warning.clickedButton() != start) return;
    }
    try {
        const auto canvasRect = canvas_->viewportTransform().inverted()
            .mapRect(QRectF(canvas_->viewport()->rect()));
        runtime_ = RuntimeSession::create(swarms_, canvasRect, scenarioSeed_, settings_);
    } catch (const std::exception& error) {
        showError(tr("Could not create the runtime session:\n%1").arg(QString::fromUtf8(error.what())));
        return;
    }
    canvas_->setEditingEnabled(false);
    addSwarmAction_->setEnabled(false);
    addPeerAction_->setEnabled(false);
    eventLogModel_->removeRows(0, eventLogModel_->rowCount());
    runtime_->simulation().setEventObserver([this](const simulator::Event& event) {
        if (const auto* completed = dynamic_cast<const simulator::PeerCompletedEvent*>(&event))
            queueCompletion(completed->peerId(), completed->swarmId());
        // Pre-execution notification, including synchronous executeNow dispatches.
        // Copy descriptions only; never infer post-event state or retain references.
        constexpr int maximumLogRows = 2000;
        if (eventLogModel_->rowCount() >= maximumLogRows)
            eventLogModel_->removeRow(0);
        eventLogModel_->appendRow({new QStandardItem(QString::number(event.time(), 'f', 6)),
            new QStandardItem(QString::fromStdString(event.traceDescription()))});
    });
    refreshInspector();
    pump_.start(runtime_->simulation());
}

void MainWindow::stopSimulation()
{
    if (!runtime_) return;
    // Qt callbacks run on one thread: pause prevents another step throughout
    // this transaction. Never enter a modal loop until snapshotting has finished.
    pump_.pause();
    try {
        runtime_->snapshotToScenario(swarms_);
    } catch (const std::exception& error) {
        presentingCompletions_ = false;
        resumeAfterCompletions_ = false;
        completionTimer_.stop();
        updateRuntimeControls();
        QMessageBox::critical(this, tr("Stop Simulation"),
            tr("Could not save peer state. The session remains paused:\n%1")
                .arg(QString::fromUtf8(error.what())));
        return;
    }
    pump_.stop(); // Cancel timers and detach its Simulation pointer before destruction.
    runtime_.reset();

    completionTimer_.stop();
    completionMessages_.clear();
    presentingCompletions_ = false;
    resumeAfterCompletions_ = false;
    if (completionPopup_) {
        disconnect(completionPopup_, nullptr, this, nullptr);
        completionPopup_->close();
        completionPopup_ = nullptr;
    }
    eventLogModel_->removeRows(0, eventLogModel_->rowCount());
    canvas_->setEditingEnabled(true);
    simulationTime_->setText(simulationTimeText(0));
    refreshInspector();
    updateRuntimeControls();
}

void MainWindow::queueCompletion(simulator::PeerId peerId, simulator::SwarmId swarmId)
{
    // Capture names now; do not retain engine-event references or infer from colors.
    for (const auto& [scenarioPeerId, binding] : runtime_->peerBindings()) {
        if (binding.peerId != peerId || binding.swarmId != swarmId) continue;
        if (const auto* swarm = findSwarm(binding.scenarioSwarmId)) {
            for (const auto& peer : swarm->peers) {
                if (peer.id == scenarioPeerId) {
                    completionMessages_.push_back(tr("Peer %1 has finished downloading %2.")
                        .arg(peer.name, swarm->name));
                    return;
                }
            }
        }
    }
}

void MainWindow::presentCompletions()
{
    if (presentingCompletions_ || completionMessages_.empty()) return;
    presentingCompletions_ = true;
    resumeAfterCompletions_ = pump_.state() == RuntimePump::State::Running;
    pump_.pause();
    updateRuntimeControls();
    // Leave the atomic engine step and its observer before entering any dialog.
    completionTimer_.start();
}

void MainWindow::showNextCompletion()
{
    if (!runtime_ || !presentingCompletions_) return;
    if (completionMessages_.empty()) {
        presentingCompletions_ = false;
        const bool resume = std::exchange(resumeAfterCompletions_, false);
        if (resume) pump_.resume();
        else updateRuntimeControls();
        return;
    }
    auto* popup = new QMessageBox(QMessageBox::Information, tr("Download complete"),
        completionMessages_.front(), QMessageBox::NoButton, this);
    completionPopup_ = popup;
    completionMessages_.pop_front();
    popup->setTextFormat(Qt::PlainText);
    popup->setWindowModality(Qt::ApplicationModal);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto* proceed = popup->addButton(tr("Continue"), QMessageBox::AcceptRole);
    popup->setDefaultButton(proceed);
    popup->setEscapeButton(proceed);
    auto* stop = popup->addButton(tr("Stop Simulation"), QMessageBox::DestructiveRole);
    connect(popup, &QDialog::finished, this, [this, popup, stop] {
        completionPopup_ = nullptr;
        if (popup->clickedButton() == stop) {
            stopSimulation();
            return;
        }
        completionTimer_.start();
    });
    popup->open();
}

void MainWindow::updateRuntimeControls()
{
    const auto state = pump_.state();
    stopAction_->setEnabled(runtime_ != nullptr);
    const bool editing = !runtime_ && state == RuntimePump::State::Edit;
    importAction_->setEnabled(editing);
    exportAction_->setEnabled(editing);
    addSwarmAction_->setEnabled(editing);
    addPeerAction_->setEnabled(editing && swarmSelector_->currentIndex() >= 0
        && !canvas_->isPlacingPeer());
    settingsAction_->setEnabled(!runtime_ && state == RuntimePump::State::Edit);
    playAction_->setEnabled(state != RuntimePump::State::Running && !presentingCompletions_ && !canvas_->isPlacingPeer());
    playAction_->setText(state == RuntimePump::State::Paused ? tr("Resume") : tr("Play"));
    pauseAction_->setEnabled(state == RuntimePump::State::Running);
    nextEventAction_->setEnabled(state == RuntimePump::State::Paused && !presentingCompletions_ && runtime_
        && runtime_->simulation().nextEventTime().has_value());
    setWindowTitle(state == RuntimePump::State::Edit ? tr("picoTorrent Simulator - EDIT")
        : state == RuntimePump::State::Running ? tr("picoTorrent Simulator - RUNNING")
        : tr("picoTorrent Simulator - PAUSED"));
}

void MainWindow::editSwarm(quint64 id)
{
    if (runtime_ || canvas_->isPlacingPeer()) return;
    auto* swarm = findSwarm(id);
    if (!swarm) return;
    AddSwarmDialog dialog(*swarm, this);
    if (dialog.exec() != QDialog::Accepted) return;
    *swarm = dialog.swarm();
    for (int i = 0; i < swarmSelector_->count(); ++i)
        if (swarmSelector_->itemData(i).toULongLong() == id) swarmSelector_->setItemText(i, swarm->name);
    refreshInspector();
}

void MainWindow::refreshInspector()
{
    if (!inspectorContents_) return;
    canvas_->refreshRuntimeLinks(inspectRuntimeLinks(runtime_.get(),
        canvas_->shownSwarm(), canvas_->selectedPeer()));
    // This shared path runs after each atomic step and when a swarm is shown.
    // Refresh all visible nodes even when no peer is selected in the Inspector.
    if (const auto* shown = findSwarm(canvas_->shownSwarm())) {
        canvas_->refreshPeerColors([this, shown](quint64 id) {
            const auto peer = std::find_if(shown->peers.begin(), shown->peers.end(),
                [id](const ScenarioPeer& value) { return value.id == id; });
            return peer == shown->peers.end() ? PeerInspection{} : inspectPeer(*shown, *peer, runtime_.get());
        });
    }
    const auto selected = canvas_->selectedPeer();
    const auto* swarm = selected ? findSwarm(canvas_->shownSwarm()) : nullptr;
    const ScenarioPeer* peer = nullptr;
    if (swarm) {
        const auto found = std::find_if(swarm->peers.begin(), swarm->peers.end(),
            [selected](const ScenarioPeer& value) { return value.id == *selected; });
        if (found != swarm->peers.end()) peer = &*found;
    }
    if (!peer) {
        inspectorContents_->setCurrentIndex(0);
        piecesDialog_->hide();
        return;
    }
    const auto state = inspectPeer(*swarm, *peer, runtime_.get());
    const QString status = !state.joined ? tr("Not joined")
        : state.complete() ? tr("Seeder / Complete")
        : !state.pieces && !state.runtime && peer->initialRole == ScenarioPeer::Role::Seeder
            ? tr("Seeder (configured)") : tr("Leecher");
    QString text = tr("Peer: %1\nPeer ID (scenario): %2\nSwarm: %3\n")
        .arg(peer->name).arg(peer->id).arg(swarm->name);
    if (state.enginePeerId) text += tr("Peer ID (engine): %1\n").arg(*state.enginePeerId);
    text += tr("\nStatus: %1\nJoined: %2\n\nPieces: %3 / %4\nProgress: %5%\n")
        .arg(status, state.joined ? tr("Yes") : tr("No"))
        .arg(state.ownedPieces).arg(state.pieceCount)
        .arg(state.pieceCount ? 100.0 * state.ownedPieces / state.pieceCount : 0, 0, 'f', 1);
    if (state.complete() && !state.joined) text += tr("All pieces owned (not joined).\n");
    if (!state.pieces) text += tr("Exact ownership has not been prepared for this scenario.\n");
    const auto capacity = [](double bytes) {
        return QStringLiteral("%1 MiB/s").arg(bytes / (1024 * 1024), 0, 'g', 6);
    };
    text += tr("\nUpload capacity: %1\nDownload capacity: %2")
        .arg(capacity(state.uploadBytesPerSecond), capacity(state.downloadBytesPerSecond));
    if (state.runtime && state.enginePeerId) {
        text += tr("\n\nConnections: %1 (%2 established)\nChoking us: %3\nWe choke: %4"
                   "\nInterested in us: %5\nWe are interested in: %6\nOutstanding requests: %7\nReserved requests: %8")
            .arg(static_cast<qulonglong>(state.connections)).arg(static_cast<qulonglong>(state.established))
            .arg(static_cast<qulonglong>(state.chokingUs)).arg(static_cast<qulonglong>(state.weChoke))
            .arg(static_cast<qulonglong>(state.interestedInUs)).arg(static_cast<qulonglong>(state.weInterested))
            .arg(static_cast<qulonglong>(state.outstanding)).arg(static_cast<qulonglong>(state.reserved));
    } else if (state.runtime) text += tr("\n\nSwarm did not participate in this runtime.");
    inspectorDetails_->setText(text);
    inspectorContents_->setCurrentIndex(1);
    viewPieces_->setEnabled(state.pieces.has_value());
    if (piecesDialog_->isVisible()) {
        if (state.pieces) piecesDialog_->setPieces(peer->name, state.pieceCount, state.ownedPieces, *state.pieces);
        else piecesDialog_->hide();
    }
}
