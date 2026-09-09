#pragma once

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace simulator {

    enum class MessageType {
        Handshake = -1,
        Choke = 0,
        Unchoke = 1,
        Interested = 2,
        NotInterested = 3,
        Have = 4,
        Bitfield = 5,
        Request = 6,
        Piece = 7,
        Cancel = 8
    };

    struct EmptyPayload {};

    struct HandshakePayload {
        std::array<std::uint8_t, 20> infoHash{};
        std::array<std::uint8_t, 20> peerId{};
    };

    struct HavePayload {
        std::uint32_t pieceIndex{};
    };

    struct BitfieldPayload {
        // Piece 0 is the high bit of byte 0; unused trailing bits must be zero.
        std::vector<std::uint8_t> bytes;
    };

    struct RequestPayload {
        std::uint32_t index{};
        std::uint32_t begin{};
        std::uint32_t length{};
    };

    struct PiecePayload {
        std::uint32_t index{};
        std::uint32_t begin{};
        // Simulated block size only; no block data is stored.
        std::uint32_t length{};
    };

    struct CancelPayload {
        std::uint32_t index{};
        std::uint32_t begin{};
        std::uint32_t length{};
    };

    using MessagePayload = std::variant<
        EmptyPayload, HandshakePayload, HavePayload, BitfieldPayload,
        RequestPayload, PiecePayload, CancelPayload
    >;

    class Message {
    public:
        // Throws std::invalid_argument if type and payload do not match.
        explicit Message(MessageType type, MessagePayload payload = EmptyPayload{});

        MessageType type() const;
        const MessagePayload& payload() const;

        // BEP 3 application bytes, including framing but excluding transport headers.
        std::uint64_t wireSize() const;

    private:
        MessageType type_;
        MessagePayload payload_;
    };

} // namespace simulator
