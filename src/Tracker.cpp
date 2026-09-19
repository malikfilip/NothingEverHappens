#include "simulator/Tracker.hpp"
#include <algorithm>

namespace simulator {
    const std::set<PeerId>& Tracker::registeredPeers(const InfoHash& hash) const {
        static const std::set<PeerId> empty;
        const auto found = registrations_.find(hash);
        return found == registrations_.end() ? empty : found->second;
    }

    std::vector<PeerId> Tracker::announce(const InfoHash& hash, PeerId peer, std::size_t numwant, AnnounceKind kind) {
        if (kind == AnnounceKind::Stopped) {
            const auto found = registrations_.find(hash);
            if (found != registrations_.end()) {
                found->second.erase(peer);
                if (found->second.empty()) registrations_.erase(found);
            }
            return {};
        }
        auto& peers = registrations_[hash];
        peers.insert(peer);
        const auto limit = std::min(numwant, maximum_);
        if (limit == 0) return {}; // Registration-only does not consume randomness.
        std::vector<PeerId> result;
        for (const auto candidate : peers)
            if (candidate != peer) result.push_back(candidate);
        const auto count = std::min(limit, result.size());
        // Partial Fisher-Yates: uniform sampling without replacement, in random order.
        // Fixed engine and explicit rejection avoid modulo bias and library-dependent
        // shuffle/distribution mappings. Sorted registry gives a stable starting order.
        for (std::size_t i = 0; i < count; ++i) {
            const auto remaining = static_cast<std::uint64_t>(result.size() - i);
            if (remaining == 1) break;
            const auto threshold = (std::uint64_t{0} - remaining) % remaining;
            std::uint64_t draw;
            do { draw = rng_(); } while (draw < threshold);
            std::swap(result[i], result[i + static_cast<std::size_t>(draw % remaining)]);
        }
        result.resize(count);
        return result;
    }
}
