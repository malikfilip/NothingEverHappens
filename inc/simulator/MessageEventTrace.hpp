#pragma once

#include <string>

#include "simulator/Message.hpp"
#include "simulator/Peer.hpp"
#include "simulator/SwarmId.hpp"

namespace simulator {

    inline const char* messageTypeName(MessageType type)
    {
        switch (type) {
        case MessageType::Handshake: return "HANDSHAKE";
        case MessageType::Choke: return "CHOKE";
        case MessageType::Unchoke: return "UNCHOKE";
        case MessageType::Interested: return "INTERESTED";
        case MessageType::NotInterested: return "NOT_INTERESTED";
        case MessageType::Have: return "HAVE";
        case MessageType::Bitfield: return "BITFIELD";
        case MessageType::Request: return "REQUEST";
        case MessageType::Piece: return "PIECE";
        case MessageType::Cancel: return "CANCEL";
        }
        return "UNKNOWN";
    }

    struct MessageEventDetails {
        SwarmId swarmId;
        PeerId sender;
        PeerId receiver;
        MessageType messageType;
    };

    inline std::string describeMessageEvent(const char* eventType, const MessageEventDetails& details)
    {
        return std::string(eventType) + " Peer " + std::to_string(details.sender)
            + " -> Peer " + std::to_string(details.receiver) + "  "
            + messageTypeName(details.messageType) + "  Swarm " + std::to_string(details.swarmId);
    }

} // namespace simulator