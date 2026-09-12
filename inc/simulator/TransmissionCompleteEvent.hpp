#pragma once

#include <cstddef>

#include "simulator/Event.hpp"
#include "simulator/Network.hpp"

namespace simulator {

    class TransmissionCompleteEvent : public Event {
    public:
        // Network must outlive this event; its link indices remain stable.
        TransmissionCompleteEvent(double time, Network& network, std::size_t linkIndex, PeerId sender)
            : Event(time), network_(network), linkIndex_(linkIndex), sender_(sender) {}

        void execute() override
        {
            network_.completeTransmission(linkIndex_, sender_);
        }

    private:
        Network& network_;
        std::size_t linkIndex_;
        PeerId sender_;
    };

} // namespace simulator
