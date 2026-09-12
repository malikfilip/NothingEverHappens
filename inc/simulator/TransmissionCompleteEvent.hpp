#pragma once

#include <cstddef>
#include <optional>

#include "simulator/Event.hpp"
#include "simulator/MessageEventTrace.hpp"
#include "simulator/Network.hpp"

namespace simulator {

    class TransmissionCompleteEvent : public Event {
    public:
        // Network must outlive this event; its link indices remain stable.
        TransmissionCompleteEvent(double time, Network& network, std::size_t linkIndex, PeerId sender, std::optional<MessageEventDetails> details = std::nullopt)
            : Event(time), network_(network), linkIndex_(linkIndex), sender_(sender), details_(details) {}

        const std::optional<MessageEventDetails>& details() const { return details_; }
        std::string traceDescription() const override
        {
            return details_ ? describeMessageEvent("TX_COMPLETE", *details_) : "TX_COMPLETE";
        }

        void execute() override
        {
            network_.completeTransmission(linkIndex_, sender_);
        }

    private:
        Network& network_;
        std::size_t linkIndex_;
        PeerId sender_;
        std::optional<MessageEventDetails> details_;
    };

} // namespace simulator
