#pragma once

#include <utility>

#include "simulator/Event.hpp"
#include "simulator/Message.hpp"
#include "simulator/Network.hpp"
#include "simulator/Peer.hpp"

namespace simulator {

    class SendMessageEvent : public Event {
    public:
        // Network must outlive the scheduled event.
        SendMessageEvent(double time, Network& network, PeerId sender, PeerId receiver, Message message)
            : Event(time), network_(network), sender_(sender), receiver_(receiver),
              message_(std::move(message))
        {
        }

        void execute() override
        {
            network_.send(sender_, receiver_, std::move(message_));
        }

    private:
        Network& network_;
        PeerId sender_;
        PeerId receiver_;
        Message message_;
    };

} // namespace simulator
