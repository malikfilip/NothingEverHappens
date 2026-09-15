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
            : Event(time), network_(network), details_(details) {
            for (const auto& [id, active] : network.activeTransmissions()) {
                if (active.linkIndex == linkIndex && active.sender == sender) {
                    transmissionId_ = id;
                    generation_ = active.generation;
                    break;
                }
            }
        }

        TransmissionCompleteEvent(double time, Network& network, const ActiveTransmission& active)
            : Event(time), network_(network), transmissionId_(active.id),
              generation_(active.generation), details_(MessageEventDetails{
                  active.swarmId, active.sender, active.receiver, active.message.type()}) {}

        TransmissionId transmissionId() const { return transmissionId_; }
        std::uint64_t generation() const { return generation_; }

        const std::optional<MessageEventDetails>& details() const { return details_; }
        std::string traceDescription() const override
        {
            return details_ ? describeMessageEvent("TX_COMPLETE", *details_) : "TX_COMPLETE";
        }

        void execute() override
        {
            network_.completeTransmission(transmissionId_, generation_);
        }

    private:
        Network& network_;
        TransmissionId transmissionId_ = 0;
        std::uint64_t generation_ = 0;
        std::optional<MessageEventDetails> details_;
    };

} // namespace simulator
