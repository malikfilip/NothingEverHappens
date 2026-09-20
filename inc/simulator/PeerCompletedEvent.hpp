#pragma once

#include "simulator/Event.hpp"
#include "simulator/Peer.hpp"
#include "simulator/SwarmId.hpp"

namespace simulator {
// Observational notification dispatched immediately by the completing PIECE.
// Never queued and never changes protocol state.
class PeerCompletedEvent : public Event {
public:
    PeerCompletedEvent(double time, PeerId peer, SwarmId swarm)
        : Event(time), peer_(peer), swarm_(swarm) {}
    PeerId peerId() const { return peer_; }
    SwarmId swarmId() const { return swarm_; }
    void execute() override {}
    std::string traceDescription() const override {
        return "PEER_COMPLETED Peer " + std::to_string(peer_)
            + " Swarm " + std::to_string(swarm_);
    }
private:
    PeerId peer_;
    SwarmId swarm_;
};
}
