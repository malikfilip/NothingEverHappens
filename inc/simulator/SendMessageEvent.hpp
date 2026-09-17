#pragma once

#include <utility>

#include "simulator/Event.hpp"
#include "simulator/MessageEventTrace.hpp"
#include "simulator/Message.hpp"
#include "simulator/Network.hpp"
#include "simulator/Peer.hpp"
#include "simulator/SwarmId.hpp"

namespace simulator {

    class SendMessageEvent : public Event {
    public:
        // Requests enqueueing; this does not imply transmission has begun.
        SendMessageEvent(double time, Network& network, SwarmId swarmId, PeerId sender, PeerId receiver, Message message,
                         bool automaticRequest = false, bool automaticPiece = false)
            : Event(time), network_(network), swarmId_(swarmId), sender_(sender), receiver_(receiver),
              message_(std::move(message)), automaticRequest_(automaticRequest), automaticPiece_(automaticPiece), lifecycle_(network.lifecycleContext(swarmId, sender, receiver))
        {
        }

        MessageEventDetails details() const { return {swarmId_, sender_, receiver_, message_.type()}; }
        std::string traceDescription() const override
        {
            return describeMessageEvent("SEND_REQUEST", details());
        }
        void execute() override
        {
            if (network_.messageStale(swarmId_, sender_, receiver_, lifecycle_)) return;
            if (automaticRequest_) network_.sendScheduledRequest(swarmId_, sender_, receiver_, std::move(message_), lifecycle_);
            else if (automaticPiece_) network_.sendScheduledPiece(swarmId_, sender_, receiver_, std::move(message_), lifecycle_);
            else network_.send(swarmId_, sender_, receiver_, std::move(message_), lifecycle_);
        }

    private:
        Network& network_;
        SwarmId swarmId_;
        PeerId sender_;
        PeerId receiver_;
        Message message_;
        bool automaticRequest_;
        bool automaticPiece_;
        LifecycleContext lifecycle_;
    };

} // namespace simulator
