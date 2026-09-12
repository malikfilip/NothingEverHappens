#pragma once

#include <utility>

#include "simulator/Event.hpp"
#include "simulator/MessageEventTrace.hpp"
#include "simulator/Network.hpp"

namespace simulator {

    class TransmissionStartEvent : public Event {
    public:
        // Created by Network only when the direction is idle and the FIFO front is ready.
        TransmissionStartEvent(double time, Network& network, SwarmId swarmId,
                               PeerId sender, PeerId receiver, Message message)
            : Event(time), network_(network),
              transmission_{swarmId, sender, receiver, std::move(message)} {}

        MessageEventDetails details() const
        {
            return {transmission_.swarmId, transmission_.sender,
                    transmission_.receiver, transmission_.message.type()};
        }
        const Message& message() const { return transmission_.message; }
        std::string traceDescription() const override
        {
            return describeMessageEvent("TRANSMISSION_START", details());
        }
        void execute() override
        {
            network_.beginTransmission(std::move(transmission_));
        }

    private:
        Network& network_;
        QueuedTransmission transmission_;
    };

} // namespace simulator