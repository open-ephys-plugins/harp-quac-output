#pragma once

// Hand-written counterpart of software/python/device.py. The Harp toolkit
// generates the Python and Bonsai interfaces from device.yml; it does not emit C++.
// Register addresses and field layout follow Quac firmware 2.0 device.yml.
// Update this file when that metadata changes.

#include "harp.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace quac {

inline constexpr uint16_t WHO_AM_I = 1411;
inline constexpr const char* DEVICE_NAME = "Quac";

enum class DigitalInputs : uint8_t { None = 0, DI0 = 0x1, DI1 = 0x2, DI2 = 0x4, DI3 = 0x8 };
enum class DigitalOutputs : uint8_t { None = 0, DO0 = 0x1, DO1 = 0x2, DO2 = 0x4, DO3 = 0x8 };
enum class AnalogOutputs : uint8_t { None = 0, AO0 = 0x1, AO1 = 0x2, AO2 = 0x4, AO3 = 0x8 };
enum class PlayerType : uint8_t { File = 0, Sine = 1, Trapezoid = 2 };
enum class DIPinMode : uint8_t { None = 0, ExternalTrigger = 1, Blanking = 2 };
enum class DOPinMode : uint8_t { None = 0, Tracking = 1 };

constexpr DigitalInputs operator|(DigitalInputs a, DigitalInputs b) { return DigitalInputs(uint8_t(a) | uint8_t(b)); }
constexpr DigitalInputs operator&(DigitalInputs a, DigitalInputs b) { return DigitalInputs(uint8_t(a) & uint8_t(b)); }
constexpr DigitalOutputs operator|(DigitalOutputs a, DigitalOutputs b) { return DigitalOutputs(uint8_t(a) | uint8_t(b)); }
constexpr DigitalOutputs operator&(DigitalOutputs a, DigitalOutputs b) { return DigitalOutputs(uint8_t(a) & uint8_t(b)); }
constexpr AnalogOutputs operator|(AnalogOutputs a, AnalogOutputs b) { return AnalogOutputs(uint8_t(a) | uint8_t(b)); }
constexpr AnalogOutputs operator&(AnalogOutputs a, AnalogOutputs b) { return AnalogOutputs(uint8_t(a) & uint8_t(b)); }

#pragma pack(push, 1)

struct AoPortState {
    float ao_channel0 = 0;
    float ao_channel1 = 0;
    float ao_channel2 = 0;
    float ao_channel3 = 0;
};

struct FileSettings {
    uint32_t cycles = 1;
    uint32_t duration = 0;
    uint32_t update_frequency = 500000;
    char path[33]{};

    void set_path(std::string_view value) {
        std::memset(path, 0, sizeof path);
        std::memcpy(path, value.data(), value.size() < sizeof path ? value.size() : sizeof path - 1);
    }
};

struct SineSettings {
    uint32_t cycles = 1;
    uint32_t duration = 0;
    uint32_t update_frequency = 500000;
    uint32_t frequency = 1;
    float amplitude = 1;
    float vertical_shift = 0;
    float normalized_phase_shift = 0;
};

struct TrapezoidSettings {
    uint32_t cycles = 1;
    uint32_t duration = 0;
    uint32_t update_frequency = 500000;
    uint32_t frequency = 1;
    float amplitude = 1;
    float vertical_shift = 0;
    float normalized_phase_shift = 0;
    uint32_t ramp_on_duration = 0;
    uint32_t pulse_width_duration = 0;
    uint32_t ramp_off_duration = 0;
};

#pragma pack(pop)

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(AoPortState) == 16);
static_assert(sizeof(FileSettings) == 45);
static_assert(sizeof(SineSettings) == 28);
static_assert(sizeof(TrapezoidSettings) == 40);
static_assert(offsetof(AoPortState, ao_channel0) == 0);
static_assert(offsetof(AoPortState, ao_channel1) == 4);
static_assert(offsetof(AoPortState, ao_channel2) == 8);
static_assert(offsetof(AoPortState, ao_channel3) == 12);
static_assert(offsetof(FileSettings, cycles) == 0);
static_assert(offsetof(FileSettings, duration) == 4);
static_assert(offsetof(FileSettings, update_frequency) == 8);
static_assert(offsetof(FileSettings, path) == 12);
static_assert(offsetof(SineSettings, cycles) == 0);
static_assert(offsetof(SineSettings, duration) == 4);
static_assert(offsetof(SineSettings, update_frequency) == 8);
static_assert(offsetof(SineSettings, frequency) == 12);
static_assert(offsetof(SineSettings, amplitude) == 16);
static_assert(offsetof(SineSettings, vertical_shift) == 20);
static_assert(offsetof(SineSettings, normalized_phase_shift) == 24);
static_assert(offsetof(TrapezoidSettings, cycles) == 0);
static_assert(offsetof(TrapezoidSettings, duration) == 4);
static_assert(offsetof(TrapezoidSettings, update_frequency) == 8);
static_assert(offsetof(TrapezoidSettings, frequency) == 12);
static_assert(offsetof(TrapezoidSettings, amplitude) == 16);
static_assert(offsetof(TrapezoidSettings, vertical_shift) == 20);
static_assert(offsetof(TrapezoidSettings, normalized_phase_shift) == 24);
static_assert(offsetof(TrapezoidSettings, ramp_on_duration) == 28);
static_assert(offsetof(TrapezoidSettings, pulse_width_duration) == 32);
static_assert(offsetof(TrapezoidSettings, ramp_off_duration) == 36);

inline constexpr harp::Register<DigitalOutputs> DOPortState{32, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalOutputs> DOPortSet{33, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalOutputs> DOPortClear{34, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalInputs> DIPortState{35, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalInputs> DIPortInvert{36, harp::PayloadType::U8};
inline constexpr harp::Register<AoPortState> AOPortState{37, harp::PayloadType::Float};
inline constexpr harp::Register<float> AOChannel0{38, harp::PayloadType::Float};
inline constexpr harp::Register<float> AOChannel1{39, harp::PayloadType::Float};
inline constexpr harp::Register<float> AOChannel2{40, harp::PayloadType::Float};
inline constexpr harp::Register<float> AOChannel3{41, harp::PayloadType::Float};
inline constexpr harp::Register<AnalogOutputs> DacReady{42, harp::PayloadType::U8};
inline constexpr harp::Register<AnalogOutputs> DacStart{43, harp::PayloadType::U8};
inline constexpr harp::Register<AnalogOutputs> DacPause{44, harp::PayloadType::U8};
inline constexpr harp::Register<AnalogOutputs> DacAbort{45, harp::PayloadType::U8};
inline constexpr harp::Register<AnalogOutputs> DacFinished{46, harp::PayloadType::U8};
inline constexpr harp::Register<DIPinMode> PinModeDI0{47, harp::PayloadType::U8};
inline constexpr harp::Register<DIPinMode> PinModeDI1{48, harp::PayloadType::U8};
inline constexpr harp::Register<DIPinMode> PinModeDI2{49, harp::PayloadType::U8};
inline constexpr harp::Register<DIPinMode> PinModeDI3{50, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalInputs> AOChannelsAssignedToDI0{51, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalInputs> AOChannelsAssignedToDI1{52, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalInputs> AOChannelsAssignedToDI2{53, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalInputs> AOChannelsAssignedToDI3{54, harp::PayloadType::U8};
inline constexpr harp::Register<DOPinMode> DO0PinMode{55, harp::PayloadType::U8};
inline constexpr harp::Register<DOPinMode> DO1PinMode{56, harp::PayloadType::U8};
inline constexpr harp::Register<DOPinMode> DO2PinMode{57, harp::PayloadType::U8};
inline constexpr harp::Register<DOPinMode> DO3PinMode{58, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalOutputs> AOChannelsAssignedToDO0{59, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalOutputs> AOChannelsAssignedToDO1{60, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalOutputs> AOChannelsAssignedToDO2{61, harp::PayloadType::U8};
inline constexpr harp::Register<DigitalOutputs> AOChannelsAssignedToDO3{62, harp::PayloadType::U8};
inline constexpr harp::Register<PlayerType> ActivePlayer0{63, harp::PayloadType::U8};
inline constexpr harp::Register<PlayerType> ActivePlayer1{64, harp::PayloadType::U8};
inline constexpr harp::Register<PlayerType> ActivePlayer2{65, harp::PayloadType::U8};
inline constexpr harp::Register<PlayerType> ActivePlayer3{66, harp::PayloadType::U8};
inline constexpr harp::Register<FileSettings> FileSettings0{67, harp::PayloadType::U8};
inline constexpr harp::Register<FileSettings> FileSettings1{68, harp::PayloadType::U8};
inline constexpr harp::Register<FileSettings> FileSettings2{69, harp::PayloadType::U8};
inline constexpr harp::Register<FileSettings> FileSettings3{70, harp::PayloadType::U8};
inline constexpr harp::Register<SineSettings> SineSettings0{71, harp::PayloadType::U8};
inline constexpr harp::Register<SineSettings> SineSettings1{72, harp::PayloadType::U8};
inline constexpr harp::Register<SineSettings> SineSettings2{73, harp::PayloadType::U8};
inline constexpr harp::Register<SineSettings> SineSettings3{74, harp::PayloadType::U8};
inline constexpr harp::Register<TrapezoidSettings> TrapezoidSettings0{75, harp::PayloadType::U8};
inline constexpr harp::Register<TrapezoidSettings> TrapezoidSettings1{76, harp::PayloadType::U8};
inline constexpr harp::Register<TrapezoidSettings> TrapezoidSettings2{77, harp::PayloadType::U8};
inline constexpr harp::Register<TrapezoidSettings> TrapezoidSettings3{78, harp::PayloadType::U8};

class Device : public harp::Device {
public:
    explicit Device(std::string port) : harp::Device(std::move(port), WHO_AM_I) {}
};

}  // namespace quac
