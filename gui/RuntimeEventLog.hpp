#pragma once
#include "RuntimeLinks.hpp"
#include "simulator/Event.hpp"
#include <QAbstractTableModel>
#include <QTimer>
#include <deque>
#include <tuple>

// Presentation snapshots only. Arrival payloads are retained from valid physical
// completions because MessageArrivalEvent exposes details, but not its payload.
struct RuntimeLogEntry {
    double time;
    QString action;
    simulator::PeerId sender, receiver;
    simulator::SwarmId swarm;
    std::optional<simulator::Message> message;
};
class RuntimeEventLog : public QAbstractTableModel {
public:
    explicit RuntimeEventLog(QObject* parent = nullptr);
    using Selection = std::pair<simulator::PeerId, simulator::SwarmId>;
    void select(std::optional<Selection> selected);
    void resetSession();
    // Called only after successful runtime creation, for the peer already selected at Play.
    void sessionStarted(const simulator::Network& network, double time);
    void observe(const simulator::Event& event, const simulator::Network& network);
    void finishStep(bool success, bool paused);
    void flush();
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    const RuntimeLogEntry* entry(int row) const;
    QString inspectionText(const RuntimeLogEntry& entry) const;
    MessageVisibility visible;
    std::function<QString(simulator::PeerId)> peerName;
    std::function<QString(simulator::SwarmId)> swarmName;
private:
    bool involves(simulator::PeerId a, simulator::PeerId b, simulator::SwarmId swarm) const;
    void append(RuntimeLogEntry entry);
    QString name(simulator::PeerId id) const;
    static constexpr std::size_t limit = 2000;
    std::optional<Selection> selected_;
    std::deque<RuntimeLogEntry> rows_, pending_;
    std::vector<std::function<void()>> afterStep_;
    using ArrivalKey = std::tuple<double, simulator::SwarmId, simulator::PeerId, simulator::PeerId, simulator::MessageType>;
    std::map<ArrivalKey, std::deque<simulator::ActiveTransmission>> arriving_;
    QTimer flushTimer_;
};
