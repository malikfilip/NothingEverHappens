#pragma once

#include "simulator/Event.hpp"
#include "simulator/Network.hpp"

namespace simulator {
    class RechokeEvent : public Event {
    public:
        RechokeEvent(double time, Network& network, PeerId peer, SwarmId swarm)
            : Event(time), network_(network), peer_(peer), swarm_(swarm), generation_(network.lifecycleGeneration(swarm, peer)) {}
        PeerId peerId() const { return peer_; }
        SwarmId swarmId() const { return swarm_; }
        std::string traceDescription() const override { return "RECHOKE"; }
        void execute() override { if (network_.lifecycleCurrent(swarm_, peer_, generation_)) network_.rechoke(peer_, swarm_); }
    private:
        Network& network_;
        PeerId peer_;
        SwarmId swarm_;
        std::uint64_t generation_;
    };
}