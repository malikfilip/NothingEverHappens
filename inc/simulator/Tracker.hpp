#pragma once

#include <map>
#include <random>
#include <set>
#include <vector>
#include "simulator/InfoHash.hpp"
#include "simulator/Peer.hpp"

namespace simulator {
    // In-process centralized registry. Network validates simulator membership.
    class Tracker {
    public:
        explicit Tracker(std::size_t maximum = 50, std::uint64_t seed = 0) : maximum_(maximum), rng_(seed) {}
        std::vector<PeerId> announce(const InfoHash& hash, PeerId peer, std::size_t numwant);
    private:
        std::size_t maximum_;
        std::mt19937_64 rng_;
        std::map<InfoHash, std::set<PeerId>> registrations_;
    };
}
