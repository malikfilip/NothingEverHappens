#pragma once
#include "simulator/Event.hpp"
#include "simulator/Network.hpp"
namespace simulator {
    class PeerJoinEvent : public Event {
    public:
        PeerJoinEvent(double time, Network& network, SwarmId swarm, PeerId peer, JoinOptions options = {})
            : Event(time), network_(network), swarm_(swarm), peer_(peer), options_(std::move(options)) {}
        void execute() override { network_.joinSwarm(swarm_, peer_, std::move(options_)); }
        PeerId peerId() const { return peer_; }
        SwarmId swarmId() const { return swarm_; }
        std::string traceDescription() const override {
            return "PEER_JOIN Peer " + std::to_string(peer_) + " Swarm " + std::to_string(swarm_);
        }
    private:
        Network& network_;
        SwarmId swarm_;
        PeerId peer_;
        JoinOptions options_;
    };
}
