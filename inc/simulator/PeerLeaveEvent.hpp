#pragma once
#include "simulator/Event.hpp"
#include "simulator/Network.hpp"
namespace simulator {
    class PeerLeaveEvent : public Event {
    public:
        PeerLeaveEvent(double time, Network& network, SwarmId swarm, PeerId peer)
            : Event(time), network_(network), swarm_(swarm), peer_(peer) {}
        void execute() override { network_.leaveSwarm(swarm_, peer_); }
        PeerId peerId() const { return peer_; }
        SwarmId swarmId() const { return swarm_; }
        std::string traceDescription() const override {
            return "PEER_LEAVE Peer " + std::to_string(peer_) + " Swarm " + std::to_string(swarm_);
        }
    private:
        Network& network_;
        SwarmId swarm_;
        PeerId peer_;
    };
}
