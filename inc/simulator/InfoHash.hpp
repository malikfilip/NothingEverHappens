#pragma once

#include <array>
#include <cstdint>

namespace simulator {

    // BitTorrent's 20-byte info_hash identifies the torrent.
    using InfoHash = std::array<std::uint8_t, 20>;

} // namespace simulator
