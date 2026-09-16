#pragma once
#include "simulator/Event.hpp"
#include "simulator/Network.hpp"

namespace simulator {
    // One-shot announce; no periodic timer is scheduled.
    class TrackerAnnounceEvent : public Event {
    public:
        TrackerAnnounceEvent(double time, Network& network, SwarmId swarm, PeerId peer, std::size_t numwant)
            : Event(time), network_(network), swarm_(swarm), peer_(peer), numwant_(numwant) {}
        void execute() override { network_.announceToTracker(swarm_, peer_, numwant_); }
        std::string traceDescription() const override { return "TRACKER_ANNOUNCE"; }
    private:
        Network& network_;
        SwarmId swarm_;
        PeerId peer_;
        std::size_t numwant_;
    };
}
