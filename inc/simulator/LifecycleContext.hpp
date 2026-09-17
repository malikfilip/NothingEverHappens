#pragma once
#include <cstdint>
namespace simulator {
    // Captured once at message creation, preserved through FIFO and propagation.
    struct LifecycleContext {
        std::uint64_t senderGeneration = 0;
        std::uint64_t receiverGeneration = 0;
    };
}
