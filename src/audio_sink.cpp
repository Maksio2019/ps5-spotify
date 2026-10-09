#include "audio_sink.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned freq,
                        unsigned param);
    int sceAudioOutOutput(int handle, const void *ptr);
    int sceAudioOutClose(int handle);
    int sceAudioOutGetPortState(int handle, void *state);
}

namespace
{
constexpr int kSystemUser = 0xFF;
constexpr int kPortMain = 0;
constexpr unsigned kFormatS16Stereo = 1;
constexpr unsigned kGrainFrames = 256;
constexpr unsigned kOutputRate = 48000;
constexpr unsigned kInputRate = 44100;
// Two seconds of input: enough to ride out network hiccups.
constexpr std::size_t kRingFrames = kInputRate * 2;
// Input frames one output grain needs, plus interpolation headroom.
constexpr std::size_t kFramesPerGrain = kGrainFrames * kInputRate / kOutputRate + 2;
} // namespace

AudioSink::AudioSink() : ring_(kRingFrames * 2)
{
}

AudioSink::~AudioSink()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    if (handle_ >= 0)
        sceAudioOutClose(handle_);
}

int AudioSink::start()
{
    // 0x8026000e: already initialised, which is fine.
    sceAudioOutInit();
    handle_ = sceAudioOutOpen(kSystemUser, kPortMain, 0, kGrainFrames, kOutputRate,
                              kFormatS16Stereo);
    std::printf("[audio] sceAudioOutOpen -> 0x%08x\n", static_cast<unsigned>(handle_));
    if (handle_ < 0)
        return handle_;
    running_ = true;
    thread_ = std::thread(&AudioSink::run, this);
    return handle_;
}

std::size_t AudioSink::write(const std::uint8_t *data, std::size_t bytes, std::size_t trackHash)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t wanted = bytes / 4;
    std::size_t room = kRingFrames - frames_;
    std::size_t count = std::min(wanted, room);
    if (count == 0)
        return 0;
    if (trackHash != lastWriteHash_)
    {
        markers_.push_back({writeTotal_, trackHash});
        lastWriteHash_ = trackHash;
    }
    std::size_t writeFrame = (readFrame_ + frames_) % kRingFrames;
    const auto *samples = reinterpret_cast<const std::int16_t *>(data);
    for (std::size_t i = 0; i < count; i++)
    {
        ring_[2 * writeFrame] = samples[2 * i];
        ring_[2 * writeFrame + 1] = samples[2 * i + 1];
        writeFrame = (writeFrame + 1) % kRingFrames;
    }
    frames_ += count;
    writeTotal_ += count;
    return count * 4;
}

void AudioSink::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    readFrame_ = 0;
    frames_ = 0;
    readTotal_ = writeTotal_;
    markers_.clear();
    lastWriteHash_ = 0;
    resetResampler_ = true;
}

void AudioSink::setPaused(bool paused)
{
    paused_ = paused;
}

void AudioSink::setVolume(int volume)
{
    // Squared: closer to how loud the slider sounds than a straight line.
    double level = std::clamp(volume, 0, 65535) / 65535.0;
    gainQ15_ = static_cast<int>(level * level * 32768.0);
}

void AudioSink::setDraining(bool draining)
{
    draining_ = draining;
}

bool AudioSink::takeTrackStarted()
{
    return trackStarted_.exchange(false);
}

bool AudioSink::empty()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_ == 0;
}

// Called with mutex_ held.
bool AudioSink::readFrame(std::int16_t frame[2])
{
    if (frames_ == 0)
        return false;
    while (!markers_.empty() && markers_.front().frame <= readTotal_)
    {
        if (markers_.front().hash != lastPlayedHash_)
        {
            lastPlayedHash_ = markers_.front().hash;
            trackStarted_ = true;
        }
        markers_.pop_front();
    }
    frame[0] = ring_[2 * readFrame_];
    frame[1] = ring_[2 * readFrame_ + 1];
    readFrame_ = (readFrame_ + 1) % kRingFrames;
    frames_--;
    readTotal_++;
    return true;
}

void AudioSink::run()
{
    static std::int16_t grain[kGrainFrames * 2];
    // Linear interpolation between previous and current input frames.
    std::int16_t previous[2] = {0, 0};
    std::int16_t current[2] = {0, 0};
    std::uint32_t phase = 0; // position between previous and current, Q16
    constexpr std::uint32_t step = (static_cast<std::uint64_t>(kInputRate) << 16) / kOutputRate;
    const auto started = std::chrono::steady_clock::now();
    auto lastHeartbeat = started;

    while (running_)
    {
        bool silent = true;
        if (!paused_)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (resetResampler_)
            {
                previous[0] = previous[1] = current[0] = current[1] = 0;
                phase = 0;
                resetResampler_ = false;
            }
            if (frames_ >= kFramesPerGrain || (draining_ && frames_ > 0))
            {
                silent = false;
                const int gain = gainQ15_.load();
                for (unsigned i = 0; i < kGrainFrames; i++)
                {
                    while (phase >= 0x10000)
                    {
                        previous[0] = current[0];
                        previous[1] = current[1];
                        if (!readFrame(current))
                            current[0] = current[1] = 0;
                        phase -= 0x10000;
                    }
                    for (int c = 0; c < 2; c++)
                    {
                        int value = previous[c] +
                                    static_cast<int>((static_cast<std::int64_t>(current[c] - previous[c]) *
                                                      static_cast<std::int64_t>(phase)) >> 16);
                        grain[2 * i + c] = static_cast<std::int16_t>((value * gain) >> 15);
                    }
                    phase += step;
                }
            }
            else
            {
                underruns_++;
            }
        }
        if (silent)
            std::memset(grain, 0, sizeof grain);
        // Heartbeat: shows whether the app keeps running in the background and
        // whether the port stays routed to a device (state bytes 0-1).
        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= std::chrono::seconds(10))
        {
            lastHeartbeat = now;
            std::uint8_t state[64] = {};
            sceAudioOutGetPortState(handle_, state);
            std::size_t buffered;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                buffered = frames_;
            }
            std::printf("[audio t=%.1f] output %02x%02x volume %02x%02x paused %d buffered %zu "
                        "underruns %llu\n",
                        std::chrono::duration<double>(now - started).count(), state[1], state[0],
                        state[5], state[4], paused_.load() ? 1 : 0, buffered,
                        static_cast<unsigned long long>(underruns_.load()));
        }
        // Blocks until the port takes the grain: this paces the loop.
        int result = sceAudioOutOutput(handle_, grain);
        if (result < 0)
        {
            std::printf("[audio] sceAudioOutOutput -> 0x%08x\n", static_cast<unsigned>(result));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
