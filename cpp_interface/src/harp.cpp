#include "harp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace harp {
namespace {

constexpr double kTickSeconds = 32e-6;
constexpr auto kReplyTimeout = std::chrono::seconds(5);

std::vector<uint8_t> build_frame(MessageType type, uint8_t address, PayloadType payload_type, const void* data, size_t size) {
    std::vector<uint8_t> frame{uint8_t(type), 0, address, 0xFF, uint8_t(payload_type)};
    if (data && size) {
        auto bytes = static_cast<const uint8_t*>(data);
        frame.insert(frame.end(), bytes, bytes + size);
    }
    frame[1] = uint8_t(frame.size() - 1);
    uint8_t sum = 0;
    for (uint8_t byte : frame) sum = uint8_t(sum + byte);
    frame.push_back(sum);
    return frame;
}

bool valid_type(uint8_t byte) {
    if (byte & 0b11110100) return false;
    auto type = byte & 0x03;
    return type >= 1 && type <= 3;
}

std::optional<RawMessage> pop_frame(std::vector<uint8_t>& buf) {
    while (buf.size() >= 2) {
        const uint8_t length = buf[1];
        if (!valid_type(buf[0]) || length == 0) {
            buf.erase(buf.begin());
            continue;
        }
        const size_t frame_size = size_t(length) + 2;
        if (buf.size() < frame_size) return std::nullopt;

        uint8_t sum = 0;
        for (size_t i = 0; i + 1 < frame_size; ++i) sum = uint8_t(sum + buf[i]);
        if (sum != buf[frame_size - 1]) {
            buf.erase(buf.begin());
            continue;
        }

        const bool timestamped = buf[4] & 0x10;
        const size_t payload_at = timestamped ? 11 : 5;
        if (timestamped && frame_size < 12) {
            buf.erase(buf.begin());
            continue;
        }

        RawMessage msg;
        msg.type = MessageType(buf[0] & 0x03);
        msg.error = buf[0] & 0x08;
        msg.address = buf[2];
        msg.port = buf[3];
        msg.payload_type = PayloadType(buf[4] & ~uint8_t{0x10});
        if (timestamped) {
            uint32_t seconds = 0;
            uint16_t ticks = 0;
            std::memcpy(&seconds, buf.data() + 5, 4);
            std::memcpy(&ticks, buf.data() + 9, 2);
            msg.timestamp = double(seconds) + double(ticks) * kTickSeconds;
        }
        msg.payload.assign(buf.begin() + payload_at, buf.begin() + (frame_size - 1));
        buf.erase(buf.begin(), buf.begin() + frame_size);
        return msg;
    }
    return std::nullopt;
}

#ifdef _WIN32
class OverlappedOperation {
public:
    OverlappedOperation() {
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) throw Error("failed to create serial I/O event");
    }

    ~OverlappedOperation() { CloseHandle(operation.hEvent); }
    OverlappedOperation(const OverlappedOperation&) = delete;
    OverlappedOperation& operator=(const OverlappedOperation&) = delete;
    void reset() {
        const HANDLE event = operation.hEvent;
        operation = {};
        operation.hEvent = event;
        ResetEvent(event);
    }

    OVERLAPPED operation{};
};
class SerialPort {
public:
    explicit SerialPort(const std::string& port) {
        std::string path = port.rfind("\\\\.\\", 0) == 0 ? port : "\\\\.\\" + port;
        handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw Error("failed to open " + port);
        try {
            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (!GetCommState(handle_, &dcb)) throw Error("GetCommState failed");
            dcb.BaudRate = 1000000;
            dcb.ByteSize = 8;
            dcb.Parity = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            dcb.fDtrControl = DTR_CONTROL_ENABLE;
            dcb.fRtsControl = RTS_CONTROL_ENABLE;
            if (!SetCommState(handle_, &dcb)) throw Error("SetCommState failed");

            COMMTIMEOUTS timeouts{};
            // Return buffered bytes immediately, or wait only for the first
            // byte. The idle timeout bounds the reader's shutdown latency.
            timeouts.ReadIntervalTimeout = MAXDWORD;
            timeouts.ReadTotalTimeoutConstant = 20;
            timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
            timeouts.WriteTotalTimeoutConstant = 1000;
            if (!SetCommTimeouts(handle_, &timeouts)) throw Error("SetCommTimeouts failed");
            PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        } catch (...) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw;
        }
    }

    ~SerialPort() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    void write(const std::vector<uint8_t>& frame) {
        auto& io = write_io_;
        io.reset();
        DWORD written = 0;
        BOOL complete = WriteFile(handle_, frame.data(), DWORD(frame.size()), &written, &io.operation);
        if (!complete && GetLastError() == ERROR_IO_PENDING)
            complete = GetOverlappedResult(handle_, &io.operation, &written, TRUE);
        if (!complete || written != frame.size())
            throw Error("serial write failed");
    }

    // Returns 0 on timeout, -1 on failure.
    int read(uint8_t* dest, int capacity) {
        auto& io = read_io_;
        io.reset();
        DWORD n = 0;
        BOOL complete = ReadFile(handle_, dest, DWORD(capacity), &n, &io.operation);
        if (!complete && GetLastError() == ERROR_IO_PENDING)
            complete = GetOverlappedResult(handle_, &io.operation, &n, TRUE);
        if (complete) return int(n);

        const DWORD error = GetLastError();
        // COMMTIMEOUTS often surface as ERROR_TIMEOUT rather than a zero-length read.
        // A single timeout must not kill the reader; later writes would all fail.
        if (error == ERROR_TIMEOUT || error == WAIT_TIMEOUT) return 0;

        DWORD commError = 0;
        ClearCommError(handle_, &commError, nullptr);
        if (error == ERROR_OPERATION_ABORTED) return 0;
        return -1;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OverlappedOperation read_io_;
    OverlappedOperation write_io_;
};
#else
class SerialPort {
public:
    explicit SerialPort(const std::string& port) {
        fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY);
        if (fd_ < 0) throw Error("failed to open " + port);
        termios tio{};
        if (tcgetattr(fd_, &tio) != 0) throw Error("tcgetattr failed");
        cfmakeraw(&tio);
        cfsetispeed(&tio, B1000000);
        cfsetospeed(&tio, B1000000);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 1;
        if (tcsetattr(fd_, TCSANOW, &tio) != 0) throw Error("tcsetattr failed");
        tcflush(fd_, TCIOFLUSH);
    }

    ~SerialPort() {
        if (fd_ >= 0) ::close(fd_);
    }

    void write(const std::vector<uint8_t>& frame) {
        if (::write(fd_, frame.data(), frame.size()) != ssize_t(frame.size())) throw Error("serial write failed");
    }

    int read(uint8_t* dest, int capacity) {
        auto n = ::read(fd_, dest, capacity);
        return n < 0 ? -1 : int(n);
    }

private:
    int fd_ = -1;
};
#endif

}  // namespace

struct Device::Impl {
    struct Pending {
        uint8_t address = 0;
        MessageType type{};
        bool ready = false;
        RawMessage message;
    };

    struct Sub {
        uint64_t id = 0;
        uint8_t address = 0;
        bool all = false;
        std::function<void(const RawMessage&)> handler;
    };

    explicit Impl(std::string port) : serial(std::move(port)) {}

    ~Impl() { stop(); }

    void start() {
        running = true;
        reader = std::thread([this] { read_loop(); });
        events = std::thread([this] { event_loop(); });
    }

    void stop() {
        if (!running.exchange(false)) return;
        {
            std::lock_guard lock(mu);
            fault = std::make_exception_ptr(Error("device closed"));
        }
        cv.notify_all();
        event_cv.notify_all();
        if (reader.joinable()) reader.join();
        if (events.joinable()) events.join();
    }

    void read_loop() {
        std::vector<uint8_t> acc;
        uint8_t buf[256];
        while (running) {
            int n = serial.read(buf, int(sizeof buf));
            if (n < 0) {
                fail(Error("serial read failed"));
                break;
            }
            if (n == 0) continue;
            acc.insert(acc.end(), buf, buf + n);
            while (auto msg = pop_frame(acc)) dispatch(std::move(*msg));
        }
    }

    void dispatch(RawMessage msg) {
        std::lock_guard lock(mu);
        if (msg.type != MessageType::Event && pending && !pending->ready && pending->address == msg.address && pending->type == msg.type) {
            pending->message = std::move(msg);
            pending->ready = true;
            cv.notify_one();
            return;
        }
        if (msg.type == MessageType::Event) {
            queue.push(std::move(msg));
            event_cv.notify_one();
        }
    }

    void event_loop() {
        for (;;) {
            RawMessage msg;
            std::vector<std::function<void(const RawMessage&)>> handlers;
            {
                std::unique_lock lock(mu);
                event_cv.wait(lock, [&] { return !running || !queue.empty(); });
                if (!running && queue.empty()) return;
                msg = std::move(queue.front());
                queue.pop();
                for (const auto& sub : subs)
                    if (sub.all || sub.address == msg.address) handlers.push_back(sub.handler);
            }
            for (const auto& handler : handlers) {
                try {
                    handler(msg);
                } catch (...) {
                }
            }
        }
    }

    void fail(const Error& error) {
        std::lock_guard lock(mu);
        fault = std::make_exception_ptr(error);
        cv.notify_all();
    }

    SerialPort serial;
    std::atomic<bool> running{false};
    std::mutex mu;
    std::condition_variable cv;
    std::condition_variable event_cv;
    std::optional<Pending> pending;
    std::exception_ptr fault;
    std::queue<RawMessage> queue;
    std::vector<Sub> subs;
    uint64_t next_id = 1;
    uint16_t whoami = 0;
    Device::TransactionTiming last_timing;
    std::thread reader;
    std::thread events;
};

Device::Device(std::string port, uint16_t expected_who_am_i) : impl_(std::make_unique<Impl>(std::move(port))) {
    impl_->start();
    auto reply = read(WhoAmI);
    impl_->whoami = reply.payload;
    if (expected_who_am_i != 0 && impl_->whoami != expected_who_am_i)
        throw Error("WhoAmI mismatch: expected " + std::to_string(expected_who_am_i) + " but the device reported " + std::to_string(impl_->whoami));
}

Device::~Device() = default;

uint16_t Device::who_am_i() const { return impl_->whoami; }

Device::TransactionTiming Device::last_transaction_timing() const {
    std::lock_guard lock(impl_->mu);
    return impl_->last_timing;
}

RawMessage Device::transact(MessageType type, uint8_t address, PayloadType payload_type, const void* data, size_t size) {
    auto frame = build_frame(type, address, payload_type, data, size);
    {
        std::lock_guard lock(impl_->mu);
        if (impl_->fault) std::rethrow_exception(impl_->fault);
        if (impl_->pending) throw Error("a request is already in flight");
        impl_->pending = Impl::Pending{address, type};
    }
    const auto write_started = std::chrono::steady_clock::now();
    try {
        impl_->serial.write(frame);
    } catch (...) {
        std::lock_guard lock(impl_->mu);
        impl_->pending.reset();
        throw;
    }
    const auto write_finished = std::chrono::steady_clock::now();

    std::unique_lock lock(impl_->mu);
    if (!impl_->cv.wait_for(lock, kReplyTimeout, [&] { return (impl_->pending && impl_->pending->ready) || impl_->fault; })) {
        impl_->pending.reset();
        throw Error("no reply from register " + std::to_string(address));
    }
    if (impl_->fault) std::rethrow_exception(impl_->fault);
    RawMessage msg = std::move(impl_->pending->message);
    impl_->pending.reset();
    if (msg.error) throw Error("device returned an error for register " + std::to_string(address));
    impl_->last_timing = {
        std::chrono::duration<double, std::milli>(write_finished - write_started).count(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - write_finished).count()
    };
    return msg;
}

Device::Subscription Device::subscribe_raw(uint8_t address, std::function<void(const RawMessage&)> handler) {
    std::lock_guard lock(impl_->mu);
    const uint64_t id = impl_->next_id++;
    impl_->subs.push_back(Impl::Sub{id, address, false, std::move(handler)});
    return Subscription(this, id);
}

Device::Subscription Device::subscribe_all(std::function<void(const RawMessage&)> handler) {
    std::lock_guard lock(impl_->mu);
    const uint64_t id = impl_->next_id++;
    impl_->subs.push_back(Impl::Sub{id, 0, true, std::move(handler)});
    return Subscription(this, id);
}

void Device::unsubscribe(uint64_t id) {
    std::lock_guard lock(impl_->mu);
    std::erase_if(impl_->subs, [&](const Impl::Sub& sub) { return sub.id == id; });
}

}  // namespace harp
