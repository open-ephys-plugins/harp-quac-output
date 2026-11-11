#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace harp {

enum class MessageType : uint8_t { Read = 1, Write = 2, Event = 3 };

// Wire payload-type byte without the timestamp flag. Sized registers that are
// declared as a byte array (device.yml type U8, length N) use U8.
enum class PayloadType : uint8_t {
    U8 = 1,
    U16 = 2,
    U32 = 4,
    U64 = 8,
    S8 = 0x81,
    S16 = 0x82,
    S32 = 0x84,
    S64 = 0x88,
    Float = 0x44,
};

struct RawMessage {
    MessageType type{};
    bool error = false;
    uint8_t address = 0;
    uint8_t port = 0;
    PayloadType payload_type{};
    std::optional<double> timestamp;
    std::vector<uint8_t> payload;
};

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

template <class T>
struct Register {
    uint8_t address;
    PayloadType payload_type;
};

template <class T>
struct Reply {
    MessageType message_type{};
    std::optional<double> timestamp;
    T payload{};
};

inline constexpr Register<uint16_t> WhoAmI{0, PayloadType::U16};

class Device {
public:
    // Opens port at 1_000_000 baud. When expected_who_am_i is non-zero, reads
    // the core WhoAmI register and throws Error on mismatch.
    explicit Device(std::string port, uint16_t expected_who_am_i = 0);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    uint16_t who_am_i() const;
    struct TransactionTiming {
        double write_ms = 0;
        double reply_ms = 0;
    };

    // Host-side timing of the last successful transaction, not hardware onset.
    TransactionTiming last_transaction_timing() const;

    template <class T>
    Reply<T> read(Register<T> reg) {
        return decode<T>(transact(MessageType::Read, reg.address, reg.payload_type, nullptr, 0));
    }

    template <class T>
    Reply<T> write(Register<T> reg, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        return decode<T>(transact(MessageType::Write, reg.address, reg.payload_type, &value, sizeof(T)));
    }

    class Subscription {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept : device_(other.device_), id_(other.id_) { other.device_ = nullptr; }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                unsubscribe();
                device_ = other.device_;
                id_ = other.id_;
                other.device_ = nullptr;
            }
            return *this;
        }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { unsubscribe(); }

        void unsubscribe() {
            if (device_) device_->unsubscribe(id_);
            device_ = nullptr;
        }

    private:
        friend class Device;
        Subscription(Device* device, uint64_t id) : device_(device), id_(id) {}
        Device* device_ = nullptr;
        uint64_t id_ = 0;
    };

    // Events are delivered on a background thread. The handler may call read/write.
    // Destroy the subscription before the device.
    template <class T, class Handler>
    Subscription subscribe(Register<T> reg, Handler handler) {
        return subscribe_raw(reg.address, [handler = std::move(handler)](const RawMessage& msg) {
            handler(decode<T>(msg));
        });
    }

    // Every unsolicited Event, regardless of address.
    Subscription subscribe_all(std::function<void(const RawMessage&)> handler);

private:
    struct Impl;

    template <class T>
    static Reply<T> decode(const RawMessage& msg) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (msg.payload.size() != sizeof(T))
            throw Error("payload size mismatch for register " + std::to_string(msg.address));
        Reply<T> reply;
        reply.message_type = msg.type;
        reply.timestamp = msg.timestamp;
        std::memcpy(&reply.payload, msg.payload.data(), sizeof(T));
        return reply;
    }

    RawMessage transact(MessageType type, uint8_t address, PayloadType payload_type, const void* data, size_t size);
    Subscription subscribe_raw(uint8_t address, std::function<void(const RawMessage&)> handler);
    void unsubscribe(uint64_t id);

    std::unique_ptr<Impl> impl_;
};

}  // namespace harp
