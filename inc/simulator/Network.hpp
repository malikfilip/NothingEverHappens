#pragma once

#include <vector>

#include "simulator/Link.hpp"
#include "simulator/Message.hpp"
#include "simulator/Peer.hpp"
#include "simulator/Swarm.hpp"

namespace simulator {

    class Simulation;

    class Network {
    public:
        // Simulation must outlive this network, which owns its peers, links, and swarms.
        Network(Simulation& simulation, std::vector<Peer> peers, std::vector<Link> links, std::vector<Swarm> swarms = {});

        // Schedules arrival; throws std::invalid_argument for missing endpoints,
        // a missing link, or invalid transfer bandwidth/latency.
        void send(SwarmId swarmId, PeerId sender, PeerId receiver, Message message);

        // Throws std::invalid_argument for an unknown receiver/swarm or rejected message.
        void deliver(SwarmId swarmId, PeerId sender, PeerId receiver, const Message& message);

        // Read-only lookup; throws std::invalid_argument for an unknown ID.
        const Peer& peer(PeerId id) const;
        const Swarm& swarm(SwarmId id) const;

    private:
        Simulation& simulation_;
        std::vector<Peer> peers_;
        std::vector<Link> links_;
        std::vector<Swarm> swarms_;
    };

} // namespace simulator
