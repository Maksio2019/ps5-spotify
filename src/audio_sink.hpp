#pragma once

// PS5 audio output for cspot: 44.1 kHz S16 stereo PCM in, resampled to the
// console's 48 kHz main port. Audio stays in memory (a bounded ring buffer).

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class AudioSink final
{
  public:
    AudioSink();
    ~AudioSink();
    AudioSink(const AudioSink &) = delete;
    AudioSink &operator=(const AudioSink &) = delete;

    // Opens the audio port and starts the output thread. Returns the
    // sceAudioOutOpen handle, negative on failure.
    int start();

    // Non-blocking: stores as many whole frames as fit and returns the bytes
    // taken. trackHash marks where a new track begins.
    std::size_t write(const std::uint8_t *data, std::size_t bytes, std::size_t trackHash);
    // Drops everything queued (seek, skip, new playlist).
    void clear();
    void setPaused(bool paused);
    // Spotify volume, 0 to 65535.
    void setVolume(int volume);
    // When cspot reports the queue is finished, play out what is left.
    void setDraining(bool draining);

    // True once each time the output reaches the start of a new track.
    bool takeTrackStarted();
    bool empty();
    std::uint64_t underruns() const
    {
        return underruns_.load();
    }

  private:
    void run();
    bool readFrame(std::int16_t frame[2]);

    std::mutex mutex_;
    std::vector<std::int16_t> ring_; // interleaved stereo frames
    std::size_t readFrame_ = 0;
    std::size_t frames_ = 0;
    std::uint64_t readTotal_ = 0;
    std::uint64_t writeTotal_ = 0;
    std::size_t lastWriteHash_ = 0;
    std::size_t lastPlayedHash_ = 0;
    struct Marker
    {
        std::uint64_t frame;
        std::size_t hash;
    };
    std::deque<Marker> markers_;
    bool resetResampler_ = true;

    std::atomic<bool> paused_{true};
    std::atomic<bool> draining_{false};
    std::atomic<bool> trackStarted_{false};
    std::atomic<int> gainQ15_{32768};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> underruns_{0};
    int handle_ = -1;
    std::thread thread_;
};
