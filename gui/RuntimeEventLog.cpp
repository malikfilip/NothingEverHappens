#include "RuntimeEventLog.hpp"
#include "simulator/TransmissionStartEvent.hpp"
#include "simulator/TransmissionCompleteEvent.hpp"
#include "simulator/MessageArrivalEvent.hpp"
#include "simulator/PeerJoinEvent.hpp"
#include "simulator/PeerLeaveEvent.hpp"
#include "simulator/PeerCompletedEvent.hpp"
#include "simulator/RechokeEvent.hpp"
#include "simulator/MessageEventTrace.hpp"

using namespace simulator;
namespace {
bool owns(const Network& network, PeerId peer, SwarmId swarm, std::uint32_t index)
{
    if (!network.peer(peer).hasSwarm(swarm)) return false;
    const auto& bits = network.peer(peer).swarmState(swarm).localBitfield;
    return index / 8 < bits.size() && (bits[index / 8] & (0x80 >> (index % 8)));
}
}
RuntimeEventLog::RuntimeEventLog(QObject* parent) : QAbstractTableModel(parent)
{
    flushTimer_.setSingleShot(true);
    flushTimer_.setInterval(50);
    connect(&flushTimer_, &QTimer::timeout, this, &RuntimeEventLog::flush);
}
void RuntimeEventLog::select(std::optional<Selection> selected)
{
    if (selected_ == selected) return;
    selected_ = selected;
    flushTimer_.stop();
    beginResetModel();
    rows_.clear(); pending_.clear(); afterStep_.clear();
    endResetModel();
}
void RuntimeEventLog::resetSession()
{
    flushTimer_.stop();
    beginResetModel();
    rows_.clear(); pending_.clear(); afterStep_.clear(); arriving_.clear(); selected_.reset();
    endResetModel();
}
void RuntimeEventLog::sessionStarted(const Network& network, double time)
{
    // RuntimeSession performs initial real joins during construction, before its
    // observer can be installed. This records that successful startup transition,
    // never a membership snapshot when the user selects a different peer later.
    if (selected_ && network.peer(selected_->first).isActiveInSwarm(selected_->second)) {
        append({time, "JOINED SWARM", selected_->first, selected_->first, selected_->second, {}});
        flush();
    }
}
bool RuntimeEventLog::involves(PeerId a, PeerId b, SwarmId swarm) const
{
    return selected_ && selected_->second == swarm && (selected_->first == a || selected_->first == b);
}
void RuntimeEventLog::append(RuntimeLogEntry entry)
{
    if (pending_.size() == limit) pending_.pop_front();
    pending_.push_back(std::move(entry));
}
void RuntimeEventLog::observe(const Event& event, const Network& network)
{
    const double time = event.time();
    if (const auto* complete = dynamic_cast<const TransmissionCompleteEvent*>(&event)) {
        const auto found = network.activeTransmissions().find(complete->transmissionId());
        if (found == network.activeTransmissions().end() || found->second.generation != complete->generation()) return;
        const auto& tx = found->second;
        const ArrivalKey key{time + network.links().at(tx.linkIndex).latency(), tx.swarmId,
            tx.sender, tx.receiver, tx.message.type()};
        // Keep only in-flight snapshots, not history, including while selection/filter
        // is empty so a later real arrival can still be inspected accurately.
        arriving_[key].push_back(tx);
        return;
    }
    if (const auto* arrival = dynamic_cast<const MessageArrivalEvent*>(&event)) {
        const auto d = arrival->details();
        const ArrivalKey key{time, d.swarmId, d.sender, d.receiver, d.messageType};
        auto found = arriving_.find(key);
        if (found == arriving_.end()) return;
        auto tx = std::move(found->second.front());
        found->second.pop_front();
        if (found->second.empty()) arriving_.erase(found);
        if (network.messageStale(d.swarmId, d.sender, d.receiver, tx.lifecycle)
            || !involves(d.sender, d.receiver, d.swarmId)) return;
        if (!visible || visible(d.messageType)) {
            afterStep_.push_back([this, time, d, message = tx.message] {
                append({time, "RECEIVE", d.sender, d.receiver, d.swarmId, message});
            });
        }
        if (d.messageType == MessageType::Piece && selected_->first == d.receiver) {
            const auto index = std::get<PiecePayload>(tx.message.payload()).index;
            if (!owns(network, d.receiver, d.swarmId, index)) {
                afterStep_.push_back([this, &network, time, d, index] {
                    if (owns(network, d.receiver, d.swarmId, index))
                        append({time, QStringLiteral("PIECE COMPLETED (%1)").arg(index),
                            d.receiver, d.receiver, d.swarmId, {}});
                });
            }
        }
        return;
    }
    if (!selected_) return;
    if (const auto* start = dynamic_cast<const TransmissionStartEvent*>(&event)) {
        const auto d = start->details();
        if (!involves(d.sender, d.receiver, d.swarmId) || (visible && !visible(d.messageType))) return;
        afterStep_.push_back([this, &network, time, d, message = start->message()] {
            // Confirm start actually created an active record after successful execution.
            for (const auto& [id, tx] : network.activeTransmissions())
                if (tx.sender == d.sender && tx.receiver == d.receiver && tx.swarmId == d.swarmId
                    && tx.message.type() == d.messageType) {
                    append({time, "SEND", d.sender, d.receiver, d.swarmId, message});
                    break;
                }
        });
    } else if (const auto* completed = dynamic_cast<const PeerCompletedEvent*>(&event)) {
        if (involves(completed->peerId(), completed->peerId(), completed->swarmId())) {
            const auto peer = completed->peerId(); const auto swarm = completed->swarmId();
            afterStep_.push_back([this, time, peer, swarm] {
                append({time, "DOWNLOAD COMPLETED / became seed", peer, peer, swarm, {}});
            });
        }
    } else if (const auto* rechoke = dynamic_cast<const RechokeEvent*>(&event)) {
        const auto peer = rechoke->peerId(); const auto swarm = rechoke->swarmId();
        if (!involves(peer, peer, swarm) || !network.peer(peer).isActiveInSwarm(swarm)) return;
        const auto& policy = network.peer(peer).swarmState(swarm).choking;
        const double regular = policy.nextRegularDeadline, optimistic = policy.nextOptimisticDeadline;
        afterStep_.push_back([this, &network, peer, swarm, time, regular, optimistic] {
            const auto& policy = network.peer(peer).swarmState(swarm).choking;
            if (regular == time && policy.nextRegularDeadline > regular)
                append({time, "REGULAR RECHOKE", peer, peer, swarm, {}});
            if (optimistic == time && policy.nextOptimisticDeadline > optimistic)
                append({time, "OPTIMISTIC RECHOKE", peer, peer, swarm, {}});
        });
    } else {
        const auto* join = dynamic_cast<const PeerJoinEvent*>(&event);
        const auto* leave = dynamic_cast<const PeerLeaveEvent*>(&event);
        if (!join && !leave) return;
        const auto peer = join ? join->peerId() : leave->peerId();
        const auto swarm = join ? join->swarmId() : leave->swarmId();
        if (!involves(peer, peer, swarm)) return;
        const bool wasActive = network.peer(peer).isActiveInSwarm(swarm);
        afterStep_.push_back([this, &network, peer, swarm, time, wasActive] {
            const bool active = network.peer(peer).isActiveInSwarm(swarm);
            if (active != wasActive)
                append({time, active ? "JOINED SWARM" : "LEFT SWARM", peer, peer, swarm, {}});
        });
    }
}
void RuntimeEventLog::finishStep(bool success, bool paused)
{
    auto actions = std::move(afterStep_);
    afterStep_.clear();
    if (success) for (auto& action : actions) action();
    if (paused) flush();
    else if (!pending_.empty() && !flushTimer_.isActive()) flushTimer_.start();
}
void RuntimeEventLog::flush()
{
    flushTimer_.stop();
    if (pending_.empty()) return;
    const auto remove = rows_.size() + pending_.size() > limit ? rows_.size() + pending_.size() - limit : 0;
    if (remove) {
        beginRemoveRows({}, 0, static_cast<int>(remove) - 1);
        rows_.erase(rows_.begin(), rows_.begin() + remove);
        endRemoveRows();
    }
    beginInsertRows({}, static_cast<int>(rows_.size()), static_cast<int>(rows_.size() + pending_.size()) - 1);
    for (auto& row : pending_) rows_.push_back(std::move(row));
    pending_.clear();
    endInsertRows();
}
int RuntimeEventLog::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
int RuntimeEventLog::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : 2; }
const RuntimeLogEntry* RuntimeEventLog::entry(int row) const
{
    return row >= 0 && static_cast<std::size_t>(row) < rows_.size() ? &rows_[row] : nullptr;
}
QString RuntimeEventLog::name(PeerId id) const { return peerName ? peerName(id) : QString::number(id); }
QVariant RuntimeEventLog::data(const QModelIndex& index, int role) const
{
    const auto* row = entry(index.row());
    if (!index.isValid() || !row || role != Qt::DisplayRole) return {};
    if (index.column() == 0) return QString::number(row->time, 'f', 6);
    if (row->message)
        return QStringLiteral("%1    %2 -> %3    %4").arg(row->action, name(row->sender), name(row->receiver),
            messageTypeName(row->message->type()));
    return row->action + "    " + name(row->sender);
}
QVariant RuntimeEventLog::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section == 0 ? "Time (s)" : "Event";
}
QString RuntimeEventLog::inspectionText(const RuntimeLogEntry& row) const
{
    if (!row.message) return {};
    return QStringLiteral("%1 at %2 s\nSender: %3 (engine %4)\nReceiver: %5 (engine %6)\nSwarm: %7 (engine %8)\nMessage: %9\nWire size: %10 bytes\n\n")
        .arg(row.action).arg(row.time, 0, 'g', 15).arg(name(row.sender)).arg(row.sender)
        .arg(name(row.receiver)).arg(row.receiver).arg(swarmName ? swarmName(row.swarm) : QString::number(row.swarm)).arg(row.swarm)
        .arg(messageTypeName(row.message->type())).arg(row.message->wireSize()) + inspectMessagePayloadText(*row.message);
}
