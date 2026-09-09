#include "simulator/Message.hpp"

#include <stdexcept>
#include <utility>

namespace simulator {

    Message::Message(MessageType type, MessagePayload payload)
        : type_(type), payload_(std::move(payload))
    {
        bool valid = false;
        switch (type_) {
        case MessageType::Handshake:
            valid = std::holds_alternative<HandshakePayload>(payload_);
            break;
        case MessageType::Choke:
        case MessageType::Unchoke:
        case MessageType::Interested:
        case MessageType::NotInterested:
            valid = std::holds_alternative<EmptyPayload>(payload_);
            break;
        case MessageType::Have:
            valid = std::holds_alternative<HavePayload>(payload_);
            break;
        case MessageType::Bitfield:
            valid = std::holds_alternative<BitfieldPayload>(payload_);
            break;
        case MessageType::Request:
            valid = std::holds_alternative<RequestPayload>(payload_);
            break;
        case MessageType::Piece:
            valid = std::holds_alternative<PiecePayload>(payload_);
            break;
        case MessageType::Cancel:
            valid = std::holds_alternative<CancelPayload>(payload_);
            break;
        }
        if (!valid) {
            throw std::invalid_argument("Message type and payload do not match");
        }
    }

    MessageType Message::type() const
    {
        return type_;
    }

    const MessagePayload& Message::payload() const
    {
        return payload_;
    }

    std::uint64_t Message::wireSize() const
    {
        // Ordinary messages have a four-byte length prefix and one-byte ID.
        constexpr std::uint64_t framing = 5;
        switch (type_) {
        case MessageType::Handshake:
            return 1 + 19 + 8 + 20 + 20;
        case MessageType::Choke:
        case MessageType::Unchoke:
        case MessageType::Interested:
        case MessageType::NotInterested:
            return framing;
        case MessageType::Have:
            return framing + 4;
        case MessageType::Bitfield:
            return framing + std::get<BitfieldPayload>(payload_).bytes.size();
        case MessageType::Request:
        case MessageType::Cancel:
            return framing + 12;
        case MessageType::Piece:
            // Length describes the simulated block; it is not a separate wire field.
            return framing + 8 + std::get<PiecePayload>(payload_).length;
        }
        throw std::logic_error("Invalid message type");
    }

} // namespace simulator
