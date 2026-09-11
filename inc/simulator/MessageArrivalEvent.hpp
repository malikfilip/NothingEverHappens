#pragma once

#include <utility>

#include "simulator/Event.hpp"
#include "simulator/Message.hpp"
#include "simulator/Network.hpp"
#include "simulator/Peer.hpp"
#include "simulator/SwarmId.hpp"

namespace simulator {

    class MessageArrivalEvent : public Event {
    public:
        // Network must outlive the scheduled event.
        MessageArrivalEvent(double time, Network& network, SwarmId swarmId, PeerId sender, PeerId receiver, Message message)
            : Event(time), network_(network), swarmId_(swarmId), sender_(sender), receiver_(receiver), message_(std::move(message))
        {
        }

        void execute() override
        {
            network_.deliver(swarmId_, sender_, receiver_, message_);
        }

    private:
        Network& network_;
        SwarmId swarmId_;
        PeerId sender_;
        PeerId receiver_;
        Message message_;
    };

} // namespace simulator
