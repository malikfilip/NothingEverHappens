#pragma once

#include <utility>

#include "simulator/Event.hpp"
#include "simulator/Message.hpp"
#include "simulator/Peer.hpp"

namespace simulator {

    class MessageArrivalEvent : public Event {
    public:
        MessageArrivalEvent(double time, PeerId sender, PeerId receiver, Message message)
            : Event(time), sender_(sender), receiver_(receiver), message_(std::move(message))
        {
        }

        void execute() override {}

    private:
        PeerId sender_;
        PeerId receiver_;
        Message message_;
    };

} // namespace simulator
