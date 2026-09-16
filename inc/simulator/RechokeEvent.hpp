#pragma once

#include "simulator/Event.hpp"
#include "simulator/Network.hpp"

namespace simulator {
    class RechokeEvent : public Event {
    public:
        RechokeEvent(double time, Network& network, PeerId peer, SwarmId swarm)
            : Event(time), network_(network), peer_(peer), swarm_(swarm) {}
        PeerId peerId() const { return peer_; }
        SwarmId swarmId() const { return swarm_; }
        std::string traceDescription() const override { return "RECHOKE"; }
        void execute() override { network_.rechoke(peer_, swarm_); }
    private:
        Network& network_;
        PeerId peer_;
        SwarmId swarm_;
    };
}