#pragma once

#include <vector>

#include "simulator/Link.hpp"
#include "simulator/Message.hpp"
#include "simulator/Peer.hpp"

namespace simulator {

    class Simulation;

    class Network {
    public:
        // Simulation must outlive this network, which owns its peers and links.
        Network(Simulation& simulation, std::vector<Peer> peers, std::vector<Link> links);

        // Schedules arrival; throws std::invalid_argument for missing endpoints,
        // a missing link, or invalid transfer bandwidth/latency.
        void send(PeerId sender, PeerId receiver, Message message);

    private:
        Simulation& simulation_;
        std::vector<Peer> peers_;
        std::vector<Link> links_;
    };

} // namespace simulator
