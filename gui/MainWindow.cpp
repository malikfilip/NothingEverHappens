#include "MainWindow.hpp"
#include "MessageInfoDialog.hpp"
#include "AddSwarmDialog.hpp"
#include "AddPeerDialog.hpp"
#include "SimulationView.hpp"

#include <array>
#include <algorithm>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QPixmap>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLocale>
#include <QHeaderView>
#include <QLabel>
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

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("picoTorrent Simulator"));
    resize(1280, 820);
    setMinimumSize(640, 480);
    createToolbar();

    auto* canvasPanel = new QGroupBox(tr("Simulation Canvas"));
    auto* canvasLayout = new QVBoxLayout(canvasPanel);
    canvas_ = new SimulationView(canvasPanel);
    canvasLayout->addWidget(canvas_);
    canvas_->peerPlaced = [this](quint64 swarmId, const ScenarioPeer& peer) {
        if (auto* swarm = findSwarm(swarmId)) {
            swarm->peers.push_back(peer);
            ++nextPeerId_;
        }
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
    canvas_->placementChanged = [this] {
        addPeerAction_->setEnabled(swarmSelector_->currentIndex() >= 0 && !canvas_->isPlacingPeer());
        canvas_->setToolTip(canvas_->isPlacingPeer()
            ? tr("Click to place the peer. Escape or right-click cancels placement.") : QString{});
    };

    auto* left = new QSplitter(Qt::Vertical);
    left->setObjectName("canvasLogSplitter");
    left->setChildrenCollapsible(false);
    left->addWidget(canvasPanel);
    left->addWidget(createEventLog());
    left->setStretchFactor(0, 3);
    left->setStretchFactor(1, 1);
    left->setSizes({540, 220});

    auto* right = new QSplitter(Qt::Vertical);
    right->setObjectName("inspectorFilterSplitter");
    right->setChildrenCollapsible(false);
    right->addWidget(createInspector());
    right->addWidget(createMessageFilter());
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 1);
    right->setSizes({540, 220});

    auto* columns = new QSplitter(Qt::Horizontal);
    columns->setObjectName("mainSplitter");
    columns->setChildrenCollapsible(false);
    columns->addWidget(left);
    columns->addWidget(right);
    columns->setStretchFactor(0, 4);
    columns->setStretchFactor(1, 1);
    columns->setSizes({960, 300});

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(columns);
    setCentralWidget(central);
}

void MainWindow::createToolbar()
{
    auto* toolbar = addToolBar(tr("Simulation Controls"));
    toolbar->setObjectName("simulationToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto disable = [this](QAction* action) {
        action->setEnabled(false);
        action->setToolTip(tr("Not connected yet."));
    };
    connect(toolbar->addAction(tr("+ Swarm")), &QAction::triggered, this, &MainWindow::addSwarm);
    addPeerAction_ = toolbar->addAction(tr("+ Peer"));
    addPeerAction_->setEnabled(false);
    connect(addPeerAction_, &QAction::triggered, this, &MainWindow::addPeer);
    toolbar->addSeparator();
    disable(toolbar->addAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop")));
    disable(toolbar->addAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play")));
    disable(toolbar->addAction(style()->standardIcon(QStyle::SP_MediaPause), tr("Pause")));
    disable(toolbar->addAction(style()->standardIcon(QStyle::SP_MediaSkipForward), tr("Next Event")));
    toolbar->addSeparator();

    auto* time = new QLabel(tr("Time: 0.000 s"), toolbar);
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
    connect(swarmSelector_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool active = index >= 0 && static_cast<std::size_t>(index) < swarms_.size();
        swarmInfo_->setEnabled(active);
        addPeerAction_->setEnabled(active);
        if (canvas_) canvas_->showSwarm(active ? &swarms_[index] : nullptr);
    });
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
    auto* hint = new QLabel(tr("Select a peer, tracker, or connection to inspect its state."), placeholder);
    hint->setWordWrap(true);
    placeholderLayout->addWidget(hint);
    placeholderLayout->addStretch();
    inspectorContents_->addWidget(placeholder);
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
    auto* model = new QStandardItemModel(0, 6, table);
    model->setHorizontalHeaderLabels({tr("Time"), tr("Event"), tr("From"),
        tr("To"), tr("Swarm"), tr("Details")});
    table->setModel(model);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setMinimumSectionSize(55);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    const std::array<int, 6> widths{85, 150, 75, 75, 85, 250};
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
    AddSwarmDialog dialog(tr("Swarm %1").arg(static_cast<qulonglong>(swarms_.size() + 1)), this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto swarm = dialog.swarm();
    swarm.id = static_cast<quint64>(swarms_.size()) + 1;
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
    addRow(tr("Piece count:"), locale.toString(swarm.pieceCount));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

ScenarioSwarm* MainWindow::findSwarm(quint64 id)
{
    const auto it = std::find_if(swarms_.begin(), swarms_.end(),
        [id](const ScenarioSwarm& swarm) { return swarm.id == id; });
    return it == swarms_.end() ? nullptr : &*it;
}

void MainWindow::addPeer()
{
    const int index = swarmSelector_->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= swarms_.size() || canvas_->isPlacingPeer()) return;
    AddPeerDialog dialog(tr("Peer %1").arg(nextPeerId_), swarms_[index].pieceCount, this);
    if (dialog.exec() != QDialog::Accepted) return;
    auto peer = dialog.peer();
    peer.id = nextPeerId_;
    canvas_->beginPeerPlacement(peer);
}
