#pragma once

#include <map>
#include <cmath>
#include <stdexcept>
#include <random>
#include <set>
#include <vector>
#include "simulator/InfoHash.hpp"
#include "simulator/Peer.hpp"

namespace simulator {
    enum class AnnounceKind { Started, Regular, Stopped };
    // In-process centralized registry. Network validates simulator membership.
    class Tracker {
    public:
        explicit Tracker(std::size_t maximum = 50, std::uint64_t seed = 0, double interval = 1800)
            : maximum_(maximum), rng_(seed), interval_(interval) {
            if (!std::isfinite(interval) || interval <= 0)
                throw std::invalid_argument("Tracker interval must be positive and finite");
        }
        double interval() const { return interval_; }
        // Read-only registration set, or an empty set for an unknown/unregistered hash.
        // Does not register peers or consume RNG. Requery after tracker mutations:
        // removal of the last registration for a hash invalidates its set reference.
        const std::set<PeerId>& registeredPeers(const InfoHash& hash) const;
        std::vector<PeerId> announce(const InfoHash& hash, PeerId peer, std::size_t numwant, AnnounceKind kind = AnnounceKind::Regular);
    private:
        std::size_t maximum_;
        std::mt19937_64 rng_;
        double interval_;
        std::map<InfoHash, std::set<PeerId>> registrations_;
    };
}
