#pragma once

#include <array>
#include <cstdint>

namespace simulator {
    // BitTorrent peer_id; independent of the simulator's numeric PeerId.
    using PeerProtocolId = std::array<std::uint8_t, 20>;
}
