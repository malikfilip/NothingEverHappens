#pragma once

#include "simulator/Event.hpp"
#include "simulator/Network.hpp"

namespace simulator {
    // One wake-up, with independently due regular and optimistic operations.
    class RechokeEvent : public Event {
    public:
        RechokeEvent(double time, Network& network, PeerId peer, SwarmId swarm)
            : Event(time), network_(network), peer_(peer), swarm_(swarm),
              generation_(network.lifecycleGeneration(swarm, peer)),
              cycle_(network.peer(peer).swarmState(swarm).choking.cycleGeneration),
              regular_(time == network.peer(peer).swarmState(swarm).choking.nextRegularDeadline),
              optimistic_(time == network.peer(peer).swarmState(swarm).choking.nextOptimisticDeadline) {}
        PeerId peerId() const { return peer_; }
        SwarmId swarmId() const { return swarm_; }
        bool regularDue() const { return regular_; }
        bool optimisticDue() const { return optimistic_; }
        std::string traceDescription() const override {
            return regular_ ? (optimistic_ ? "RECHOKE + OPTIMISTIC_ROTATION" : "RECHOKE")
                            : "OPTIMISTIC_ROTATION";
        }
        void execute() override {
            if (network_.lifecycleCurrent(swarm_, peer_, generation_))
                network_.rechoke(peer_, swarm_, cycle_, time());
        }
    private:
        Network& network_;
        PeerId peer_;
        SwarmId swarm_;
        std::uint64_t generation_;
        std::uint64_t cycle_;
        bool regular_;
        bool optimistic_;
    };
}
