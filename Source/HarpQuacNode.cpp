#include "HarpQuacNode.h"

#include "AnalogPatternGenerator.h"
#include "HarpQuacEditor.h"
#include "HarpQuacPlayer.h"

#include "quac.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

constexpr harp::Register<quac::PlayerType> kPlayers[] = {
    quac::ActivePlayer0, quac::ActivePlayer1, quac::ActivePlayer2, quac::ActivePlayer3
};
constexpr harp::Register<quac::SineSettings> kSine[] = {
    quac::SineSettings0, quac::SineSettings1, quac::SineSettings2, quac::SineSettings3
};
constexpr harp::Register<quac::TrapezoidSettings> kTrapezoid[] = {
    quac::TrapezoidSettings0, quac::TrapezoidSettings1, quac::TrapezoidSettings2, quac::TrapezoidSettings3
};
constexpr harp::Register<quac::FileSettings> kFiles[] = {
    quac::FileSettings0, quac::FileSettings1, quac::FileSettings2, quac::FileSettings3
};

struct QuacWave
{
    PatternType type = PatternType::pulse;
    int channel = 0;
    int onMs = 100;
    int offMs = 100;
    int rampOnMs = 0;
    int rampOffMs = 0;
    int repeats = 1;
    float pulseVoltage = 5.0f;
    int frequency = 5;
    int cycles = 1;
    float sineVoltage = 5.0f;
    std::string path;
};

QuacWave toWave (const Pattern& pattern)
{
    QuacWave wave;
    wave.type = pattern.patternType;
    wave.channel = jlimit (0, kNumAoChannels - 1, pattern.analogOutputChannel);
    wave.onMs = pattern.pulse.onDuration;
    wave.offMs = pattern.pulse.offDuration;
    wave.rampOnMs = pattern.pulse.rampOnDuration;
    wave.rampOffMs = pattern.pulse.rampOffDuration;
    wave.repeats = jmax (1, pattern.pulse.repeatNumber);
    wave.pulseVoltage = pattern.pulse.maxVoltage;
    wave.frequency = jmax (1, pattern.sine.frequency);
    wave.cycles = jmax (1, pattern.sine.cycles);
    wave.sineVoltage = pattern.sine.maxVoltage;
    wave.path = pattern.custom.string.substring (0, 32).toStdString();
    return wave;
}

// Players are bipolar: output = 2 * vertical_shift ± amplitude.
// A 0..V wave is amplitude V/2 centered at V/2, so the shift register is V/4.
void unipolarLevels (float voltage, float& amplitude, float& verticalShift)
{
    amplitude = jlimit (0.0f, 10.0f, voltage) * 0.5f;
    verticalShift = amplitude * 0.5f;
}

uint32_t usFromMs (int64 milliseconds)
{
    if (milliseconds <= 0)
        return 0;
    const int64 microseconds = milliseconds * 1000;
    return microseconds > (int64) UINT32_MAX ? UINT32_MAX : (uint32_t) microseconds;
}

quac::AnalogOutputs channelMask (int channel)
{
    return quac::AnalogOutputs (uint8_t (1u << jlimit (0, 3, channel)));
}

QuacWave waveFromVar (const var& patternObj)
{
    QuacWave wave;
    const String type = patternObj.getProperty ("pattern_type", "").toString().toLowerCase();
    const var sine = patternObj.hasProperty ("sine") ? patternObj["sine"] : var();
    const var pulse = patternObj.hasProperty ("pulse") ? patternObj["pulse"] : var();
    const var custom = patternObj.hasProperty ("custom") ? patternObj["custom"] : var();

    auto channelFrom = [&] (const var& body)
    {
        if (patternObj.hasProperty ("analogOutputChannel"))
            return (int) patternObj["analogOutputChannel"];
        return body.isObject() ? (int) body.getProperty ("analogOutputChannel", 0) : 0;
    };

    if (type == "sine" || sine.isObject())
    {
        wave.type = PatternType::sine;
        wave.channel = jlimit (0, 3, channelFrom (sine));
        wave.frequency = jmax (1, (int) sine.getProperty ("frequency", 5));
        wave.cycles = jmax (1, (int) sine.getProperty ("cycles", 1));
        wave.sineVoltage = (float) sine.getProperty ("maxVoltage", 5.0);
    }
    else if (type == "custom" || custom.isObject())
    {
        wave.type = PatternType::custom;
        wave.channel = jlimit (0, 3, channelFrom (custom));
        wave.path = custom.getProperty ("string", "").toString().substring (0, 32).toStdString();
    }
    else
    {
        wave.type = PatternType::pulse;
        wave.channel = jlimit (0, 3, channelFrom (pulse));
        wave.onMs = (int) pulse.getProperty ("onDuration", 100);
        wave.offMs = (int) pulse.getProperty ("offDuration", 100);
        wave.rampOnMs = (int) pulse.getProperty ("rampOnDuration", 0);
        wave.rampOffMs = (int) pulse.getProperty ("rampOffDuration", 0);
        wave.repeats = jmax (1, (int) pulse.getProperty ("repeatNumber", 1));
        wave.pulseVoltage = (float) pulse.getProperty ("maxVoltage", 5.0);
    }

    return wave;
}

var nestPattern (const var& body, const char* key, double rootMaxVoltage, bool copyVoltage)
{
    auto* patternObj = new DynamicObject();
    patternObj->setProperty ("pattern_type", key);
    patternObj->setProperty ("analogOutputChannel", body.getProperty ("analogOutputChannel", 0));

    auto* copy = new DynamicObject();
    if (auto* src = body.getDynamicObject())
        copy->getProperties() = src->getProperties();
    if (copyVoltage && ! copy->hasProperty ("maxVoltage"))
        copy->setProperty ("maxVoltage", rootMaxVoltage);

    patternObj->setProperty (key, var (copy));
    return var (patternObj);
}

struct Config
{
    bool ok = false;
    bool stop = false;
    bool play = true;
    bool loop = false;
    std::vector<QuacWave> waves;
    Array<var> patterns;
    String error;
};

Config parseConfig (const String& message)
{
    Config config;
    const var root = JSON::parse (message);

    if (! root.isObject())
    {
        config.error = "Invalid JSON config message";
        return config;
    }

    if ((bool) root.getProperty ("stop", false))
    {
        config.ok = true;
        config.stop = true;
        return config;
    }

    if (root.getProperty ("command", "").toString().equalsIgnoreCase ("waveplayer"))
    {
        config.play = (bool) root.getProperty ("playImmediately", true);
        config.loop = (bool) root.getProperty ("loop", false);
        if (auto* array = root.getProperty ("patterns", var()).getArray())
            config.patterns = *array;
    }
    else if (root.hasProperty ("pulse") || root.hasProperty ("sine") || root.hasProperty ("custom"))
    {
        config.play = (bool) root.getProperty ("playImmediately", true);
        const double rootMaxVoltage = (double) root.getProperty ("maxVoltage", 5.0);
        const char* key = root.hasProperty ("pulse") ? "pulse" : root.hasProperty ("sine") ? "sine" : "custom";
        const var body = root[key];

        if (! body.isObject())
        {
            config.error = "Invalid JSON config message";
            return config;
        }

        config.patterns.add (nestPattern (body, key, rootMaxVoltage, String (key) != "custom"));
    }
    else
    {
        config.error = "Unrecognized waveplayer config (expected pulse/sine/custom or command=waveplayer)";
        return config;
    }

    if (config.patterns.isEmpty())
    {
        config.error = "Failed to translate config to a QUAC waveform";
        return config;
    }

    for (const auto& pattern : config.patterns)
        config.waves.push_back (waveFromVar (pattern));

    config.ok = true;
    return config;
}

void applyPreview (HarpQuacPlayer* player, const var& patternObj)
{
    if (player == nullptr || player->currentPattern == nullptr || ! patternObj.isObject())
        return;

    const QuacWave wave = waveFromVar (patternObj);
    Pattern* pattern = player->currentPattern;
    pattern->analogOutputChannel = wave.channel;

    if (wave.type == PatternType::sine)
    {
        const var sine = patternObj.hasProperty ("sine") ? patternObj["sine"] : patternObj;
        pattern->sine.frequency = wave.frequency;
        pattern->sine.cycles = wave.cycles;
        pattern->sine.delayDuration = (int) sine.getProperty ("delayDuration", 0);
        pattern->sine.maxVoltage = wave.sineVoltage;
        player->applyPatternType (PatternType::sine);
    }
    else if (wave.type == PatternType::custom)
    {
        pattern->custom.string = String (wave.path);
        pattern->custom.samples.clear();
        player->applyPatternType (PatternType::custom);
    }
    else
    {
        const var pulse = patternObj.hasProperty ("pulse") ? patternObj["pulse"] : patternObj;
        pattern->pulse.onDuration = wave.onMs;
        pattern->pulse.offDuration = wave.offMs;
        pattern->pulse.rampOnDuration = wave.rampOnMs;
        pattern->pulse.rampOffDuration = wave.rampOffMs;
        pattern->pulse.repeatNumber = wave.repeats;
        pattern->pulse.delayDuration = (int) pulse.getProperty ("delayDuration", 0);
        pattern->pulse.maxVoltage = wave.pulseVoltage;
        player->applyPatternType (PatternType::pulse);
    }
}

class QuacLink
{
public:
    QuacLink() : thread ([this] { run(); }) {}

    ~QuacLink()
    {
        epoch.fetch_add (1);
        {
            std::lock_guard<std::mutex> lock (mutex);
            stopping = true;
        }
        cv.notify_all();
        if (thread.joinable())
            thread.join();
    }

    void setPort (std::string port) { post ({ Job::Port, std::move (port), {}, false }); }

    void play (std::vector<QuacWave> waves, bool loop) { post ({ Job::Play, {}, std::move (waves), loop }); }

    void stop()
    {
        epoch.fetch_add (1);
        post ({ Job::Stop, {}, {}, false });
    }

private:
    struct Job
    {
        enum Type
        {
            Port,
            Play,
            Stop
        } type = Port;

        std::string port;
        std::vector<QuacWave> waves;
        bool loop = false;
        std::chrono::steady_clock::time_point queuedAt = std::chrono::steady_clock::now();
    };

    void post (Job job)
    {
        job.queuedAt = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock (mutex);
            jobs.push_back (std::move (job));
        }
        cv.notify_all();
    }

    void closeDevice (std::unique_ptr<quac::Device>& device,
                      harp::Device::Subscription& subscription,
                      std::string& openPort,
                      uint8_t& loopMask)
    {
        loopMask = 0;
        prepared = {};
        if (device != nullptr)
        {
            try
            {
                device->write (quac::DacAbort, quac::AnalogOutputs (0x0F));
            }
            catch (...)
            {
            }
        }
        subscription.unsubscribe();
        device.reset();
        openPort.clear();
    }

    static bool waitReady (quac::Device& device, uint8_t mask, const std::atomic<int>& epoch, int token)
    {
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            if (epoch.load() != token)
                return false;
            if (uint8_t (device.read (quac::DacReady).payload) & mask)
                return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        return false;
    }

    static double elapsedMs (std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
    }

    static void logTransaction (quac::Device& device, uint8_t address)
    {
        const auto timing = device.last_transaction_timing();
        LOGD ("Harp Quac register ", (int) address, ": write=", timing.write_ms,
              " ms, reply wait=", timing.reply_ms, " ms");
    }

    struct PreparedWave
    {
        quac::PlayerType player;
        uint8_t settingsAddress;
        std::vector<uint8_t> payload;
    };

    template <typename Settings>
    bool prepareWave (quac::Device& device, int channel, quac::PlayerType player,
                      harp::Register<Settings> reg, const Settings& settings)
    {
        static_assert (std::is_trivially_copyable_v<Settings>);
        const auto* bytes = reinterpret_cast<const uint8_t*> (&settings);
        const std::vector<uint8_t> payload (bytes, bytes + sizeof (settings));
        const auto& previous = prepared[channel];
        if (previous && previous->player == player
            && previous->settingsAddress == reg.address && previous->payload == payload)
            return false;

        // An inactive player's settings can be written before selecting it.
        // If the player is unchanged, writing settings alone rearms it.
        const bool selectPlayer = ! previous || previous->player != player;
        prepared[channel].reset();
        device.write (reg, settings);
        logTransaction (device, reg.address);
        if (selectPlayer)
        {
            device.write (kPlayers[channel], player);
            logTransaction (device, kPlayers[channel].address);
        }
        prepared[channel] = PreparedWave { player, reg.address, payload };
        return true;
    }

    bool startWave (quac::Device& device, const QuacWave& wave, bool loop, const std::atomic<int>& epoch, int token)
    {
        const int channel = jlimit (0, 3, wave.channel);
        const auto mask = channelMask (channel);
        const uint8_t bits = uint8_t (mask);
        const auto prepareStarted = std::chrono::steady_clock::now();
        bool changed = false;

        if (wave.type == PatternType::sine)
        {
            quac::SineSettings settings;
            // Device cycles repeat the entire duration, not individual periods.
            settings.cycles = 1;
            settings.frequency = (uint32_t) jmax (1, wave.frequency);
            settings.update_frequency = 10000;
            unipolarLevels (wave.sineVoltage, settings.amplitude, settings.vertical_shift);
            settings.normalized_phase_shift = 0;
            if (loop)
            {
                settings.duration = 0;
            }
            else
            {
                const double microseconds = double (jmax (1, wave.cycles)) / double (settings.frequency) * 1.0e6;
                settings.duration = microseconds > double (UINT32_MAX) ? UINT32_MAX : (uint32_t) microseconds;
            }
            changed = prepareWave (device, channel, quac::PlayerType::Sine, kSine[channel], settings);
        }
        else if (wave.type == PatternType::custom)
        {
            if (wave.path.empty())
            {
                LOGE ("Harp Quac: custom waveform needs a device file path.");
                return false;
            }

            quac::FileSettings settings;
            settings.cycles = 1;
            settings.duration = 0;
            settings.update_frequency = 500000;
            settings.set_path (wave.path);
            changed = prepareWave (device, channel, quac::PlayerType::File, kFiles[channel], settings);
        }
        else
        {
            const int onMs = jmax (0, wave.onMs);
            const int offMs = jmax (0, wave.offMs);
            const int rampOn = jmax (0, wave.rampOnMs);
            const int rampOff = jmax (0, wave.rampOffMs);
            const int periodMs = jmax (1, onMs + offMs);

            quac::TrapezoidSettings settings;
            // The duration already includes every requested pulse.
            settings.cycles = 1;
            settings.frequency = (uint32_t) jmax (1, (int) std::lround (1000.0 / periodMs));
            settings.update_frequency = 10000;
            unipolarLevels (wave.pulseVoltage, settings.amplitude, settings.vertical_shift);
            settings.normalized_phase_shift = 0;
            settings.ramp_on_duration = usFromMs (rampOn);
            settings.pulse_width_duration = usFromMs (jmax (0, onMs - rampOn - rampOff));
            settings.ramp_off_duration = usFromMs (rampOff);
            settings.duration = loop ? 0 : usFromMs (int64 (jmax (1, wave.repeats)) * periodMs);
            changed = prepareWave (device, channel, quac::PlayerType::Trapezoid, kTrapezoid[channel], settings);
        }
        const double prepareMs = elapsedMs (prepareStarted);
        const auto readyStarted = std::chrono::steady_clock::now();

        if (! waitReady (device, bits, epoch, token))
        {
            prepared[channel].reset();
            if (epoch.load() == token)
                LOGE ("Harp Quac: AO", channel, " did not become ready.");
            return false;
        }
        const double readyMs = elapsedMs (readyStarted);
        logTransaction (device, quac::DacReady.address);
        // Stop may have arrived while the readiness transaction was pending.
        if (epoch.load() != token)
            return false;
        const auto startSent = std::chrono::steady_clock::now();

        device.write (quac::DacStart, mask);
        const double startMs = elapsedMs (startSent);
        logTransaction (device, quac::DacStart.address);
        LOGD ("Harp Quac AO", channel, ": ", changed ? "configured" : "cached",
              ", prepare=", prepareMs, " ms, ready=", readyMs,
              " ms, start/ack=", startMs, " ms (host timing, not output onset)");
        return true;
    }

    void run()
    {
        std::unique_ptr<quac::Device> device;
        harp::Device::Subscription subscription;
        std::string openPort;
        std::string lastPort;
        uint8_t loopMask = 0;
        std::atomic<uint8_t> finished { 0 };

        auto ensure = [&] (const std::string& port)
        {
            if (port == openPort && device != nullptr)
                return;

            closeDevice (device, subscription, openPort, loopMask);
            if (port.empty())
                return;

            device = std::make_unique<quac::Device> (port);
            openPort = port;
            finished.store (0);
            subscription = device->subscribe (quac::DacFinished, [&] (const harp::Reply<quac::AnalogOutputs>& message)
            {
                finished.fetch_or (uint8_t (message.payload));
            });
            LOGC ("Harp Quac connected on ", port);
        };

        while (true)
        {
            Job job;
            bool haveJob = false;
            {
                std::unique_lock<std::mutex> lock (mutex);
                cv.wait_for (lock, std::chrono::milliseconds (30), [&] { return stopping || ! jobs.empty(); });
                if (stopping)
                {
                    lock.unlock();
                    closeDevice (device, subscription, openPort, loopMask);
                    return;
                }
                if (! jobs.empty())
                {
                    job = std::move (jobs.front());
                    jobs.pop_front();
                    haveJob = true;
                }
            }

            if (haveJob)
            {
                if (job.type == Job::Play)
                    LOGD ("Harp Quac play queue wait=", elapsedMs (job.queuedAt), " ms");
                try
                {
                    if (job.type == Job::Port)
                    {
                        lastPort = job.port;
                        ensure (job.port);
                    }
                    else if (job.type == Job::Stop)
                    {
                        loopMask = 0;
                        prepared = {};
                        if (device != nullptr)
                            device->write (quac::DacAbort, quac::AnalogOutputs (0x0F));
                    }
                    else
                    {
                        if (device == nullptr && ! lastPort.empty())
                            ensure (lastPort);
                        if (device == nullptr)
                        {
                            LOGE ("Harp Quac: no device open.");
                            continue;
                        }

                        const int token = epoch.load();
                        uint8_t touched = 0;
                        uint8_t started = 0;
                        for (const auto& wave : job.waves)
                        {
                            if (epoch.load() != token)
                                break;
                            touched = uint8_t (touched | uint8_t (channelMask (wave.channel)));
                            if (startWave (*device, wave, job.loop, epoch, token))
                                started = uint8_t (started | uint8_t (channelMask (wave.channel)));
                        }

                        if (job.loop && epoch.load() == token)
                            loopMask = started;
                        else
                            loopMask = uint8_t (loopMask & ~touched);

                        finished.fetch_and (uint8_t (~started));
                    }
                }
                catch (const std::exception& error)
                {
                    LOGE ("Harp Quac: ", error.what());
                    const std::string port = openPort.empty() ? lastPort : openPort;
                    const bool retryPlay = job.type == Job::Play && ! port.empty();
                    closeDevice (device, subscription, openPort, loopMask);
                    if (! retryPlay)
                        continue;

                    try
                    {
                        ensure (port);
                        const int token = epoch.load();
                        uint8_t started = 0;
                        for (const auto& wave : job.waves)
                        {
                            if (epoch.load() != token || device == nullptr)
                                break;
                            if (startWave (*device, wave, job.loop, epoch, token))
                                started = uint8_t (started | uint8_t (channelMask (wave.channel)));
                        }
                        loopMask = job.loop && epoch.load() == token ? started : uint8_t (0);
                    }
                    catch (const std::exception& retryError)
                    {
                        LOGE ("Harp Quac: ", retryError.what());
                        closeDevice (device, subscription, openPort, loopMask);
                    }
                }
            }

            const uint8_t again = uint8_t (finished.exchange (0) & loopMask);
            if (again != 0 && device != nullptr)
            {
                try
                {
                    device->write (quac::DacStart, quac::AnalogOutputs (again));
                }
                catch (const std::exception& error)
                {
                    prepared = {};
                    LOGE ("Harp Quac retrigger: ", error.what());
                }
            }
        }
    }

    // Accessed only by the hardware worker. Disconnect/abort invalidates all.
    std::array<std::optional<PreparedWave>, kNumAoChannels> prepared;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool stopping = false;
    std::atomic<int> epoch { 0 };
    std::thread thread;
};

struct HarpQuacNode::Impl
{
    QuacLink link;
    std::mutex mutex;
    QuacWave current;
    bool hasCurrent = false;
    std::vector<QuacWave> armed;
    bool armedLoop = false;
    String armedMessage;
};

HarpQuacNode::HarpQuacNode()
    : GenericProcessor ("Harp Quac"),
      impl (std::make_unique<Impl>())
{
}

HarpQuacNode::~HarpQuacNode() = default;

AudioProcessorEditor* HarpQuacNode::createEditor()
{
    editor = std::make_unique<HarpQuacEditor> (this);
    return editor.get();
}

void HarpQuacNode::registerParameters() {}

void HarpQuacNode::updateSettings() {}

void HarpQuacNode::process (AudioBuffer<float>&)
{
    checkForEvents (true);
}

StringArray HarpQuacNode::listSerialPorts()
{
    StringArray ports;
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return ports;

    for (DWORD index = 0;; ++index)
    {
        wchar_t name[256];
        BYTE data[256];
        DWORD nameLength = (DWORD) std::size (name);
        DWORD dataLength = (DWORD) sizeof (data);
        DWORD type = 0;
        if (RegEnumValueW (key, index, name, &nameLength, nullptr, &type, data, &dataLength) != ERROR_SUCCESS)
            break;
        if (type == REG_SZ)
            ports.addIfNotAlreadyThere (String (reinterpret_cast<const wchar_t*> (data)));
    }

    RegCloseKey (key);
    ports.sort (true);
#endif
    return ports;
}

void HarpQuacNode::setSelectedPort (const String& port)
{
    if (port == selectedPort)
        return;

    selectedPort = port;
    impl->link.setPort (port.toStdString());
}

void HarpQuacNode::updateArmedState (HarpQuacPlayer* player)
{
    if (player == nullptr || player->currentPattern == nullptr)
        return;

    outputMode.store ((int) player->getMode());
    triggerLine.store (player->currentPattern->triggerChannel);

    std::lock_guard<std::mutex> lock (impl->mutex);
    impl->current = toWave (*player->currentPattern);
    impl->hasCurrent = true;
}

void HarpQuacNode::playCurrent (HarpQuacPlayer* player, bool loop)
{
    if (player == nullptr || player->currentPattern == nullptr || player->getMode() != WaveOutputMode::GENERATE)
        return;

    impl->link.play ({ toWave (*player->currentPattern) }, loop);
}

void HarpQuacNode::playAll (HarpQuacPlayer* player, bool loop)
{
    if (player == nullptr || player->getMode() != WaveOutputMode::GENERATE)
        return;

    std::vector<QuacWave> waves;
    for (auto* pattern : player->availablePatterns)
        if (pattern != nullptr)
            waves.push_back (toWave (*pattern));

    if (! waves.empty())
        impl->link.play (std::move (waves), loop);
}

void HarpQuacNode::stopPlayback()
{
    impl->link.stop();
}

void HarpQuacNode::replayArmed()
{
    std::vector<QuacWave> waves;
    bool loop = false;
    {
        std::lock_guard<std::mutex> lock (impl->mutex);
        waves = impl->armed;
        loop = impl->armedLoop;
    }

    if (waves.empty())
    {
        LOGE ("Harp Quac: enable_output received but no config is armed.");
        return;
    }

    impl->link.play (std::move (waves), loop);
}

void HarpQuacNode::wire (HarpQuacPlayer* player)
{
    if (player == nullptr)
        return;

    player->onWaveformChanged = [this, player]
    {
        updateArmedState (player);
        if (player->getMode() == WaveOutputMode::GENERATE && player->isEnabled())
            playAll (player, true);
    };

    player->onRunRequested = [this, player]
    {
        if (player->getMode() == WaveOutputMode::GENERATE)
            playCurrent (player, false);
    };

    player->onEnabledChanged = [this, player] (bool enabled)
    {
        if (player->getMode() != WaveOutputMode::GENERATE)
            return;

        if (enabled)
            playAll (player, true);
        else
            stopPlayback();
    };

    player->onModeChanged = [this, player] (WaveOutputMode mode)
    {
        updateArmedState (player);

        if (mode == WaveOutputMode::LISTEN)
        {
            stopPlayback();
            String message;
            {
                std::lock_guard<std::mutex> lock (impl->mutex);
                message = impl->armedMessage;
            }
            if (message.isNotEmpty())
            {
                const Config config = parseConfig (message);
                if (config.ok && ! config.patterns.isEmpty())
                    applyPreview (player, config.patterns.getReference (0));
            }
        }
        else
        {
            player->updateWaveform();
        }
    };
}

HarpQuacPlayer* HarpQuacNode::getPlayer()
{
    auto* editor = dynamic_cast<HarpQuacEditor*> (getEditor());
    if (editor == nullptr)
        return nullptr;

    HarpQuacPlayer* player = editor->getHarpQuacPlayer();
    wire (player);
    return player;
}

void HarpQuacNode::handleTTLEvent (TTLEventPtr event)
{
    if (event == nullptr || ! event->getState())
        return;

    if (outputMode.load() != (int) WaveOutputMode::GENERATE)
        return;

    const int line = triggerLine.load();
    if (line < 1 || event->getLine() + 1 != line)
        return;

    QuacWave wave;
    {
        std::lock_guard<std::mutex> lock (impl->mutex);
        if (! impl->hasCurrent)
            return;
        wave = impl->current;
    }

    impl->link.play ({ std::move (wave) }, false);
}

void HarpQuacNode::handleBroadcastMessage (const String& message, const int64)
{
    if (! message.equalsIgnoreCase ("enable_output"))
        return;

    if (outputMode.load() == (int) WaveOutputMode::LISTEN)
    {
        replayArmed();
        return;
    }

    if (triggerLine.load() != -2)
        return;

    QuacWave wave;
    {
        std::lock_guard<std::mutex> lock (impl->mutex);
        if (! impl->hasCurrent)
            return;
        wave = impl->current;
    }

    impl->link.play ({ std::move (wave) }, false);
}

String HarpQuacNode::handleConfigMessage (const String& message)
{
    auto* player = getPlayer();
    if (player == nullptr || player->getMode() != WaveOutputMode::LISTEN)
        return "Harp Quac is in GENERATE mode; inbound configs are ignored";

    const Config config = parseConfig (message);
    if (! config.ok)
        return config.error;

    if (config.stop)
    {
        stopPlayback();
        return "OK";
    }

    if (! config.patterns.isEmpty())
        applyPreview (player, config.patterns.getReference (0));

    {
        std::lock_guard<std::mutex> lock (impl->mutex);
        impl->armed = config.waves;
        impl->armedLoop = config.loop;
        impl->armedMessage = message;
    }

    if (config.play)
        impl->link.play (config.waves, config.loop);

    return "OK";
}

bool HarpQuacNode::startAcquisition()
{
    auto* player = getPlayer();
    if (player != nullptr && player->getMode() == WaveOutputMode::GENERATE && player->isEnabled())
        playAll (player, true);
    return true;
}

bool HarpQuacNode::stopAcquisition()
{
    auto* player = getPlayer();
    if (player == nullptr || player->getMode() == WaveOutputMode::GENERATE)
        stopPlayback();
    return true;
}
