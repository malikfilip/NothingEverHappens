#pragma once
#include "simulator/Event.hpp"
#include "simulator/Network.hpp"
namespace simulator {
    class TrackerAnnounceEvent : public Event {
    public:
        TrackerAnnounceEvent(double time, Network& network, SwarmId swarm, PeerId peer, std::size_t numwant,
                             AnnounceKind kind = AnnounceKind::Regular, bool periodic = false)
            : Event(time), network_(network), swarm_(swarm), peer_(peer), numwant_(numwant), kind_(kind),
              generation_(network.lifecycleGeneration(swarm, peer)), periodic_(periodic) {}
        void execute() override {
            network_.processTrackerAnnounce(swarm_, peer_, numwant_, kind_, generation_, periodic_, time());
        }
        PeerId peerId() const { return peer_; }
        SwarmId swarmId() const { return swarm_; }
        AnnounceKind kind() const { return kind_; }
        std::uint64_t generation() const { return generation_; }
        std::string traceDescription() const override {
            const char* kind = kind_ == AnnounceKind::Started ? "STARTED" : kind_ == AnnounceKind::Stopped ? "STOPPED" : "REGULAR";
            return std::string("TRACKER_ANNOUNCE ") + kind + " Peer " + std::to_string(peer_) + " Swarm " + std::to_string(swarm_);
        }
    private:
        Network& network_;
        SwarmId swarm_;
        PeerId peer_;
        std::size_t numwant_;
        AnnounceKind kind_;
        std::uint64_t generation_;
        bool periodic_;
    };
}
