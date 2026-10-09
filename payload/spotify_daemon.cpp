// Spotify Connect speaker as a payload (the background-capable design).
//
// cspot plays Spotify; its 44.1 kHz PCM is served in real time as one long WAV "file"
// at http://127.0.0.1:8795/live.wav. The web app tile's page plays that URL through
// the system music core, which keeps playing at the home screen and during games
// (howitworks.md, "BACKGROUND AUDIO WORKS"). WAV rather than MP3: the music core
// buffers a fixed amount of data before it plays, about 1 s of WAV but 3-5 s of MP3,
// and that buffer is the delay of every pause or skip. Audio exists only in memory:
// nothing decoded is written to disk.
//
// Ports: 8795 live WAV + /status + /meta.png + /quit, 8796 Spotify Zeroconf (announced
// over mDNS).
// Files: /data/spkr/spotify.log (+ .prev), /data/spkr/credentials.json, the last few
// album covers in /data/spkr/covers/ (for the "now playing" toast).
// SPDX-License-Identifier: GPL-3.0-or-later

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "BellLogger.h"
#include "CSpotContext.h"
#include "CivetServer.h"
#include "HTTPClient.h"
#include "LoginBlob.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "civetweb.h"
#include "nlohmann/json.hpp"

extern "C"
{
#include "mdnssvc.h"
}

namespace
{
constexpr const char *kDeviceName = "PS5 Speaker";
constexpr int kLivePort = 8795;
constexpr int kZeroconfPort = 8796;
constexpr const char *kDataDir = "/data/spkr";
constexpr const char *kLogPath = "/data/spkr/spotify.log";
constexpr const char *kLogPrevious = "/data/spkr/spotify.prev.log";
// Reusable login token from Spotify (not a password).
constexpr const char *kCredentialsPath = "/data/spkr/credentials.json";
constexpr const char *kCredentialsTemporary = "/data/spkr/credentials.tmp";
constexpr const char *kCoverDir = "/data/spkr/covers";
// The tile's icon, used by the toast when a song has no cover.
constexpr const char *kTileIcon = "/user/appmeta/SPKR00001/icon0.png";
// How long after audio enters the stream it is heard (the music core's buffer, see
// howitworks.md): the toast waits this long and the reported position lags by it.
constexpr int kHeardDelayMs = 1800;

constexpr int kRate = 44100;
constexpr int kBytesPerFrame = 4; // 16-bit stereo
constexpr long long kBytesPerSecond = static_cast<long long>(kRate) * kBytesPerFrame;
constexpr int kWavHeaderSize = 44;
// The "file" the music core sees: a WAV header and 6 hours of PCM (the data size
// field is 32 bits).
constexpr long long kWavDataSize = 6LL * 3600 * kBytesPerSecond;
constexpr long long kLiveLength = kWavHeaderSize + kWavDataSize;
// 50 ms per chunk.
constexpr int kChunkFrames = kRate / 20;
// New listeners start this far behind the live point, so they can buffer at once
// (default; /live.wav?backlog=<ms> overrides it for tuning).
constexpr int kDefaultBacklogMs = 250;
constexpr std::size_t backlogBytes(int ms)
{
    return static_cast<std::size_t>(kBytesPerSecond * ms / 1000 / kBytesPerFrame * kBytesPerFrame);
}

void wavHeader(unsigned char out[kWavHeaderSize])
{
    auto u32 = [&](int at, std::uint32_t v) {
        for (int i = 0; i < 4; i++)
            out[at + i] = static_cast<unsigned char>(v >> (8 * i));
    };
    auto u16 = [&](int at, std::uint16_t v) {
        out[at] = static_cast<unsigned char>(v);
        out[at + 1] = static_cast<unsigned char>(v >> 8);
    };
    std::memcpy(out, "RIFF", 4);
    u32(4, static_cast<std::uint32_t>(36 + kWavDataSize));
    std::memcpy(out + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1); // PCM
    u16(22, 2);
    u32(24, kRate);
    u32(28, static_cast<std::uint32_t>(kBytesPerSecond));
    u16(32, kBytesPerFrame);
    u16(34, 16);
    std::memcpy(out + 36, "data", 4);
    u32(40, static_cast<std::uint32_t>(kWavDataSize));
}

// ---------------------------------------------------------------- utilities

struct NotifyRequest
{
    char useless1[45];
    char message[3075];
};
extern "C" int sceKernelSendNotificationRequest(int, NotifyRequest *, std::size_t, int);

void notify(const char *format, ...)
{
    NotifyRequest request;
    std::memset(&request, 0, sizeof request);
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(request.message, sizeof request.message, format, arguments);
    va_end(arguments);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
    std::printf("[notify] %s\n", request.message);
}

// The rich toast (cover, title, artist) of the SDK's notify sample. Loaded with
// dlopen() after start: linking a system library can hang a payload before main().
using NotificationSend = int (*)(int userId, bool isLogged, const char *payload);
NotificationSend g_notificationSend = nullptr;

void loadNotifications()
{
    void *library = dlopen("libSceNotification.sprx", RTLD_LAZY);
    if (library != nullptr)
        g_notificationSend = reinterpret_cast<NotificationSend>(dlsym(library, "sceNotificationSend"));
    std::printf("[notify] sceNotificationSend %s\n", g_notificationSend != nullptr ? "found" : "missing");
}

bool richToast(const std::string &icon, const std::string &message, const std::string &subMessage)
{
    if (g_notificationSend == nullptr)
        return false;
    static std::atomic<unsigned> counter{0};
    std::time_t now = std::time(nullptr);
    char created[32];
    std::strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%S.000Z", std::gmtime(&now));
    using nlohmann::json;
    json iconJson = {{"type", "Url"}, {"parameters", {{"url", icon}}}};
    json toast = {
        {"rawData",
         {{"viewTemplateType", "InteractiveToastTemplateB"},
          {"channelType", "Downloads"},
          {"useCaseId", "IDC"},
          {"toastOverwriteType", "No"},
          {"isImmediate", true},
          {"priority", 100},
          {"viewData",
           {{"icon", iconJson}, {"message", {{"body", message}}}, {"subMessage", {{"body", subMessage}}}}},
          {"platformViews",
           {{"previewDisabled", {{"viewData", {{"icon", iconJson}, {"message", {{"body", message}}}}}}}}}}},
        {"createdDateTime", created},
        {"localNotificationId", std::to_string(static_cast<unsigned>(now) * 16 + (counter++ % 16))}};
    std::string text = toast.dump(-1, ' ', false, json::error_handler_t::replace);
    int result = g_notificationSend(0xFE /* SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM */, true, text.c_str());
    std::printf("[notify] toast \"%s\" / \"%s\" icon %s: 0x%x\n", message.c_str(), subMessage.c_str(),
                icon.c_str(), static_cast<unsigned>(result));
    return result >= 0;
}

std::string readFile(const char *path)
{
    std::string content;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return content;
    char buffer[1024];
    ssize_t got;
    while ((got = read(fd, buffer, sizeof buffer)) > 0)
        content.append(buffer, static_cast<std::size_t>(got));
    close(fd);
    return content;
}

bool writeFileAtomically(const char *temporary, const char *path, const std::string &content)
{
    FILE *file = std::fopen(temporary, "wb");
    if (file == nullptr)
        return false;
    std::size_t wrote = std::fwrite(content.data(), 1, content.size(), file);
    bool closed = std::fclose(file) == 0;
    return wrote == content.size() && closed && rename(temporary, path) == 0;
}

bool sendAll(int fd, const void *data, std::size_t size)
{
    const char *p = static_cast<const char *>(data);
    while (size > 0)
    {
        ssize_t sent = send(fd, p, size, 0);
        if (sent <= 0)
            return false;
        p += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

// The LAN address, found by routing towards a public address (no packet is sent).
bool lanAddress(in_addr *address)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    sockaddr_in remote{};
    remote.sin_len = sizeof remote;
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x08080808);
    sockaddr_in local{};
    socklen_t length = sizeof local;
    bool ok = connect(fd, reinterpret_cast<sockaddr *>(&remote), sizeof remote) == 0 &&
              getsockname(fd, reinterpret_cast<sockaddr *>(&local), &length) == 0 &&
              local.sin_addr.s_addr != htonl(INADDR_ANY);
    close(fd);
    if (ok)
        *address = local.sin_addr;
    return ok;
}

// ---------------------------------------------------------------- status

struct Status
{
    std::mutex mutex;
    std::string state = "starting";
    std::string user;
    std::string track;
    std::string artist;
    std::string album;
    std::string cover; // https URL of the album cover (i.scdn.co)
    std::uint32_t duration = 0; // ms
    bool paused = true;
    // Counts changes, so the page knows when to read the song info again.
    unsigned version = 0;
} g_status;

template <typename F> void updateStatus(F change)
{
    std::lock_guard<std::mutex> lock(g_status.mutex);
    change(g_status);
    g_status.version++;
}

// ---------------------------------------------------------------- live sink

// Takes cspot's PCM like the title's AudioSink (same interface), but a clock
// thread moves it at real-time speed into a byte ring that HTTP listeners read
// from. While paused or starved it sends silence, so the live "file" never stops.
class LiveSink final
{
  public:
    LiveSink() : pcm_(kRingFrames * 2), out_(kOutRing)
    {
    }

    void start()
    {
        running_ = true;
        thread_ = std::thread(&LiveSink::run, this);
    }

    std::size_t write(const std::uint8_t *data, std::size_t bytes, std::size_t trackHash)
    {
        std::lock_guard<std::mutex> lock(pcmMutex_);
        std::size_t count = std::min(bytes / 4, kRingFrames - frames_);
        if (count == 0)
            return 0;
        if (trackHash != lastWriteHash_)
        {
            markers_.push_back({writeTotal_, trackHash});
            lastWriteHash_ = trackHash;
        }
        std::size_t at = (readFrame_ + frames_) % kRingFrames;
        const auto *samples = reinterpret_cast<const std::int16_t *>(data);
        for (std::size_t i = 0; i < count; i++)
        {
            pcm_[2 * at] = samples[2 * i];
            pcm_[2 * at + 1] = samples[2 * i + 1];
            at = (at + 1) % kRingFrames;
        }
        frames_ += count;
        writeTotal_ += count;
        return count * 4;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(pcmMutex_);
        readFrame_ = 0;
        frames_ = 0;
        readTotal_ = writeTotal_;
        markers_.clear();
        lastWriteHash_ = 0;
    }

    void setPaused(bool paused)
    {
        paused_ = paused;
    }

    void setVolume(int volume)
    {
        double level = std::clamp(volume, 0, 65535) / 65535.0;
        gainQ15_ = static_cast<int>(level * level * 32768.0);
    }

    void setDraining(bool draining)
    {
        draining_ = draining;
    }

    bool takeTrackStarted()
    {
        return trackStarted_.exchange(false);
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lock(pcmMutex_);
        return frames_ == 0;
    }

    std::uint64_t underruns() const
    {
        return underruns_.load();
    }

    int listeners() const
    {
        return listeners_.load();
    }

    // Frames of real audio (not silence) that went into the stream so far.
    std::uint64_t playedFrames() const
    {
        return played_.load();
    }

    // Streams the live PCM to one HTTP listener until it disconnects or `limit`
    // bytes are sent. Starts on a chunk boundary `backlogMs` behind the live point.
    void serve(int fd, long long limit, int backlogMs)
    {
        const std::size_t backlog = backlogBytes(backlogMs);
        // Only the newest listener is served. The music core keeps reading an old
        // connection for ~10 s after the page switches to a new one, and only then
        // starts the new one; closing the old one at once avoids that wait.
        const std::uint64_t mine = ++listenerGeneration_;
        produced_cv_.notify_all();
        listeners_++;
        std::uint64_t position;
        {
            std::lock_guard<std::mutex> lock(outMutex_);
            position = produced_;
            for (auto it = chunkStarts_.rbegin(); it != chunkStarts_.rend(); ++it)
            {
                position = *it;
                if (produced_ - *it >= backlog)
                    break;
            }
        }
        long long sent = 0;
        std::vector<std::uint8_t> out(64 * 1024);
        while (sent < limit && running_ && mine == listenerGeneration_)
        {
            std::size_t count;
            {
                std::unique_lock<std::mutex> lock(outMutex_);
                produced_cv_.wait_for(lock, std::chrono::seconds(2), [&] {
                    return produced_ > position || mine != listenerGeneration_;
                });
                if (produced_ <= position || mine != listenerGeneration_)
                    continue;
                if (produced_ - position > kOutRing) // fell too far behind: skip ahead
                    position = produced_ - backlog;
                count = static_cast<std::size_t>(
                    std::min<std::uint64_t>({produced_ - position, out.size(),
                                             static_cast<std::uint64_t>(limit - sent)}));
                for (std::size_t i = 0; i < count; i++)
                    out[i] = out_[(position + i) % kOutRing];
            }
            if (!sendAll(fd, out.data(), count))
                break;
            position += count;
            sent += static_cast<long long>(count);
        }
        listeners_--;
        std::printf("[live] listener %llu left after %lld bytes%s\n",
                    static_cast<unsigned long long>(mine), sent,
                    mine != listenerGeneration_ ? " (replaced by a newer one)" : "");
    }

  private:
    static constexpr std::size_t kRingFrames = kRate * 2;
    static constexpr std::size_t kOutRing = 4 * 1024 * 1024;

    struct Marker
    {
        std::uint64_t frame;
        std::size_t hash;
    };

    // pcmMutex_ held.
    bool readFrame(std::int16_t frame[2])
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
        frame[0] = pcm_[2 * readFrame_];
        frame[1] = pcm_[2 * readFrame_ + 1];
        readFrame_ = (readFrame_ + 1) % kRingFrames;
        frames_--;
        readTotal_++;
        played_++;
        return true;
    }

    void append(const unsigned char *data, int size)
    {
        if (size <= 0)
            return;
        {
            std::lock_guard<std::mutex> lock(outMutex_);
            chunkStarts_.push_back(produced_);
            while (chunkStarts_.size() > 200)
                chunkStarts_.pop_front();
            for (int i = 0; i < size; i++)
                out_[(produced_ + i) % kOutRing] = data[i];
            produced_ += static_cast<std::uint64_t>(size);
        }
        produced_cv_.notify_all();
    }

    void run()
    {
        std::vector<std::int16_t> chunk(kChunkFrames * 2);
        auto next = std::chrono::steady_clock::now();
        auto lastHeartbeat = next;

        while (running_)
        {
            bool silent = true;
            if (!paused_)
            {
                std::lock_guard<std::mutex> lock(pcmMutex_);
                if (frames_ >= static_cast<std::size_t>(kChunkFrames) || (draining_ && frames_ > 0))
                {
                    silent = false;
                    const int gain = gainQ15_.load();
                    for (int i = 0; i < kChunkFrames; i++)
                    {
                        std::int16_t frame[2] = {0, 0};
                        readFrame(frame);
                        chunk[2 * i] = static_cast<std::int16_t>((frame[0] * gain) >> 15);
                        chunk[2 * i + 1] = static_cast<std::int16_t>((frame[1] * gain) >> 15);
                    }
                }
                else
                {
                    underruns_++;
                }
            }
            if (silent)
                std::fill(chunk.begin(), chunk.end(), 0);
            append(reinterpret_cast<const unsigned char *>(chunk.data()), kChunkFrames * kBytesPerFrame);

            auto now = std::chrono::steady_clock::now();
            if (now - lastHeartbeat >= std::chrono::seconds(30))
            {
                lastHeartbeat = now;
                std::size_t buffered;
                {
                    std::lock_guard<std::mutex> lock(pcmMutex_);
                    buffered = frames_;
                }
                std::printf("[live] paused %d buffered %zu underruns %llu listeners %d produced %llu\n",
                            paused_.load() ? 1 : 0, buffered,
                            static_cast<unsigned long long>(underruns_.load()), listeners_.load(),
                            static_cast<unsigned long long>(produced_));
            }
            // Real-time pace: one chunk per chunk duration.
            next += std::chrono::milliseconds(1000 * kChunkFrames / kRate);
            if (next < now - std::chrono::seconds(1))
                next = now; // fell behind (e.g. a stall): do not burst to catch up
            std::this_thread::sleep_until(next);
        }
    }

    std::mutex pcmMutex_;
    std::vector<std::int16_t> pcm_;
    std::size_t readFrame_ = 0;
    std::size_t frames_ = 0;
    std::uint64_t readTotal_ = 0;
    std::uint64_t writeTotal_ = 0;
    std::size_t lastWriteHash_ = 0;
    std::size_t lastPlayedHash_ = 0;
    std::deque<Marker> markers_;

    std::mutex outMutex_;
    std::condition_variable produced_cv_;
    std::vector<std::uint8_t> out_;
    std::uint64_t produced_ = 0;
    std::deque<std::uint64_t> chunkStarts_;

    std::atomic<bool> paused_{true};
    std::atomic<bool> draining_{false};
    std::atomic<bool> trackStarted_{false};
    std::atomic<int> gainQ15_{32768};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> played_{0};
    std::atomic<int> listeners_{0};
    std::atomic<std::uint64_t> listenerGeneration_{0};
    std::thread thread_;
};

LiveSink g_sink;
std::atomic<bool> g_quit{false};
// Counts user actions (pause, play, skip, seek). The page polls /state.png, whose
// width is this count, and reconnects to the stream when it changes: that drops
// the music core's ~1.8 s buffer, so the action is heard sooner. An image because
// the https page may load images (but not data) over plain http from the console.
std::atomic<int> g_actions{0};
// The volume we play at (0-65535), reported to Spotify on connect. cspot's default
// report is 0 while the speaker plays at full volume, so the phone's first volume
// press used to jump to silence.
std::atomic<int> g_volume{65535};

// Playback position: a known position (track start, seek) plus the audio streamed
// since then, minus the time the music core holds it before it is heard.
std::atomic<long long> g_positionBaseMs{0};
std::atomic<std::uint64_t> g_positionBaseFrame{0};
// Where the next track starts (cspot's PLAYBACK_START carries it when the phone
// starts playback in the middle of a song; later tracks start at 0).
std::atomic<int> g_nextStartMs{0};

void setPosition(long long ms)
{
    g_positionBaseFrame = g_sink.playedFrames();
    g_positionBaseMs = ms;
}

long long positionMs(std::uint32_t duration)
{
    long long ms = g_positionBaseMs.load() +
                   static_cast<long long>((g_sink.playedFrames() - g_positionBaseFrame.load()) * 1000 / kRate) -
                   kHeardDelayMs;
    if (duration > 0)
        ms = std::min<long long>(ms, duration);
    return std::max<long long>(ms, 0);
}

// ---------------------------------------------------------------- song toast

// Shows a system toast with the cover, title and artist when a new song is heard.
// Waits until the song is audible; when songs are skipped quickly only the last one
// gets a toast. Covers are downloaded to kCoverDir (the toast takes a file path).
class Toaster final
{
  public:
    void start()
    {
        // Covers of an earlier run are not needed any more.
        if (DIR *dir = opendir(kCoverDir))
        {
            while (dirent *entry = readdir(dir))
                if (entry->d_name[0] != '.')
                    unlink((std::string(kCoverDir) + "/" + entry->d_name).c_str());
            closedir(dir);
        }
        mkdir(kCoverDir, 0755);
        std::thread(&Toaster::run, this).detach();
    }

    void songStarted(const cspot::TrackInfo &track)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = {track.trackId, track.name, track.artist, track.imageUrl};
            due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeardDelayMs - 300);
            waiting_ = true;
        }
        cv_.notify_all();
    }

  private:
    struct Song
    {
        std::string id, name, artist, cover;
    };

    void run()
    {
        std::string lastId;
        while (!g_quit)
        {
            Song song;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::seconds(1), [&] { return waiting_; });
                if (!waiting_)
                    continue;
                if (std::chrono::steady_clock::now() < due_)
                {
                    cv_.wait_until(lock, due_); // a newer song may replace this one meanwhile
                    continue;
                }
                song = pending_;
                waiting_ = false;
            }
            if (song.id == lastId || song.name.empty())
                continue;
            lastId = song.id;
            std::string icon = coverFile(song.cover);
            if (!richToast(icon.empty() ? kTileIcon : icon, song.name, song.artist))
                notify("%s - %s", song.name.c_str(), song.artist.c_str());
        }
    }

    // Downloads the cover once; keeps the last few.
    std::string coverFile(const std::string &url)
    {
        std::size_t slash = url.rfind('/');
        if (url.empty() || slash == std::string::npos)
            return "";
        std::string path = std::string(kCoverDir) + "/" + url.substr(slash + 1) + ".jpg";
        struct stat info;
        if (stat(path.c_str(), &info) == 0 && info.st_size > 0)
            return path;
        try
        {
            auto response = bell::HTTPClient::get(url);
            std::vector<std::uint8_t> bytes = response->bytes();
            std::string temporary = path + ".tmp";
            if (bytes.size() < 100 ||
                !writeFileAtomically(temporary.c_str(), path.c_str(), std::string(bytes.begin(), bytes.end())))
            {
                std::printf("[notify] cover download failed (%zu bytes)\n", bytes.size());
                return "";
            }
        }
        catch (const std::exception &error)
        {
            std::printf("[notify] cover download failed: %s\n", error.what());
            return "";
        }
        covers_.push_back(path);
        while (covers_.size() > 8)
        {
            unlink(covers_.front().c_str());
            covers_.pop_front();
        }
        return path;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    Song pending_;
    std::chrono::steady_clock::time_point due_;
    bool waiting_ = false;
    std::deque<std::string> covers_;
};

Toaster g_toaster;

std::uint32_t crc32(const unsigned char *data, std::size_t size, std::uint32_t crc = 0)
{
    crc = ~crc;
    for (std::size_t i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// A one-row grey PNG whose pixels are the given bytes (zlib "stored" blocks, no
// compression library needed). The page reads the bytes back through a canvas, or
// just the width.
std::string greyPng(const std::string &pixels)
{
    std::string raw = std::string(1, '\0') + pixels; // filter byte: none
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : raw)
    {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    std::string z = "\x78\x01";
    for (std::size_t at = 0; at < raw.size(); at += 65535)
    {
        std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z += static_cast<char>(at + n == raw.size() ? 1 : 0); // final block?
        z += static_cast<char>(n & 0xff);
        z += static_cast<char>(n >> 8);
        z += static_cast<char>(~n & 0xff);
        z += static_cast<char>((~n >> 8) & 0xff);
        z.append(raw, at, n);
    }
    for (int shift = 24; shift >= 0; shift -= 8)
        z += static_cast<char>((((b << 16) | a) >> shift) & 0xff);

    auto be32 = [](std::uint32_t v) {
        std::string out(4, '\0');
        for (int i = 0; i < 4; i++)
            out[i] = static_cast<char>(v >> (24 - 8 * i));
        return out;
    };
    auto chunk = [&](const char *type, const std::string &data) {
        std::string body = std::string(type, 4) + data;
        return be32(static_cast<std::uint32_t>(data.size())) + body +
               be32(crc32(reinterpret_cast<const unsigned char *>(body.data()), body.size()));
    };
    std::string ihdr = be32(static_cast<std::uint32_t>(pixels.size())) + be32(1);
    ihdr += std::string("\x08\x00\x00\x00\x00", 5); // 8-bit grey, no interlace
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", ihdr) + chunk("IDAT", z) +
           chunk("IEND", "");
}

std::string pngOfWidth(int width)
{
    return greyPng(std::string(static_cast<std::size_t>(width), '\x80'));
}

// ---------------------------------------------------------------- live HTTP server

// `live` adds the fields that change all the time (position, stream counters);
// without them the text only changes when `v` does.
std::string statusJson(bool live = true)
{
    nlohmann::json j;
    std::uint32_t duration;
    {
        std::lock_guard<std::mutex> lock(g_status.mutex);
        j["v"] = g_status.version;
        j["state"] = g_status.state;
        j["user"] = g_status.user;
        j["track"] = g_status.track;
        j["artist"] = g_status.artist;
        j["album"] = g_status.album;
        j["cover"] = g_status.cover;
        j["duration"] = duration = g_status.duration;
        j["paused"] = g_status.paused;
    }
    if (live)
    {
        j["position"] = positionMs(duration);
        j["listeners"] = g_sink.listeners();
        j["underruns"] = g_sink.underruns();
    }
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Song info for the https page, which may load images (not data) from the console:
//  /meta.png          all of statusJson() as pixels (read through a canvas)
//  /meta.png?stable   the same without the live fields; with &b=<k>, an image whose
//                     width is 1 + byte k (for a page that cannot read pixels)
//  /ver.png, /pos.png width 1 + version, 1 + position in seconds
std::string metaPng(const char *path)
{
    if (std::strstr(path, "stable") == nullptr)
        return greyPng(statusJson());
    std::string text = statusJson(false);
    if (const char *b = std::strstr(path, "b="))
    {
        std::size_t k = static_cast<std::size_t>(std::max(0, std::atoi(b + 2)));
        return pngOfWidth(1 + (k < text.size() ? static_cast<unsigned char>(text[k]) : 0));
    }
    return greyPng(text);
}

void handleLiveClient(int fd)
{
    char request[4096];
    std::size_t length = 0;
    while (length < sizeof request - 1)
    {
        ssize_t got = recv(fd, request + length, sizeof request - 1 - length, 0);
        if (got <= 0)
            break;
        length += static_cast<std::size_t>(got);
        request[length] = '\0';
        if (std::strstr(request, "\r\n\r\n") != nullptr)
            break;
    }
    request[length] = '\0';
    char method[8] = "", path[256] = "";
    std::sscanf(request, "%7s %255s", method, path);

    if (std::strncmp(path, "/live.wav", 9) == 0)
    {
        long long start = 0, end = kLiveLength - 1;
        bool ranged = false;
        if (const char *range = strcasestr(request, "\nRange: bytes="))
        {
            ranged = true;
            range += std::strlen("\nRange: bytes=");
            char *dash = nullptr;
            if (*range == '-')
                start = kLiveLength - std::strtoll(range + 1, nullptr, 10);
            else
            {
                start = std::strtoll(range, &dash, 10);
                if (dash != nullptr && *dash == '-' && dash[1] >= '0' && dash[1] <= '9')
                    end = std::min(std::strtoll(dash + 1, nullptr, 10), kLiveLength - 1);
            }
        }
        char head[512];
        if (ranged)
            std::snprintf(head, sizeof head,
                          "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/wav\r\n"
                          "Accept-Ranges: bytes\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
                          "Content-Length: %lld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                          start, end, kLiveLength, end - start + 1);
        else
            std::snprintf(head, sizeof head,
                          "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\n"
                          "Content-Length: %lld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                          kLiveLength);
        if (!sendAll(fd, head, std::strlen(head)))
            return;
        std::printf("[live] %s %s range %lld-%lld\n", method, path, start, end);
        if (start > 1000000)
        {
            // A peek far into the file (players look for tags at the end): filler.
            static const char zeros[4096] = {};
            for (long long left = end - start + 1; left > 0;)
            {
                std::size_t n = static_cast<std::size_t>(std::min<long long>(left, sizeof zeros));
                if (!sendAll(fd, zeros, n))
                    break;
                left -= static_cast<long long>(n);
            }
            return;
        }
        int backlogMs = kDefaultBacklogMs;
        if (const char *b = std::strstr(path, "backlog="))
            backlogMs = std::clamp(std::atoi(b + 8), 0, 15000);
        std::printf("[live] backlog %d ms\n", backlogMs);
        long long limit = end - start + 1;
        if (start < kWavHeaderSize)
        {
            unsigned char header[kWavHeaderSize];
            wavHeader(header);
            long long n = std::min<long long>(kWavHeaderSize - start, limit);
            if (!sendAll(fd, header + start, static_cast<std::size_t>(n)))
                return;
            limit -= n;
        }
        g_sink.serve(fd, limit, backlogMs);
        return;
    }

    std::string body;
    const char *type = "application/json";
    if (std::strncmp(path, "/state.png", 10) == 0)
    {
        body = pngOfWidth(1 + g_actions.load() % 4000);
        type = "image/png";
    }
    else if (std::strncmp(path, "/meta.png", 9) == 0)
    {
        body = metaPng(path);
        type = "image/png";
    }
    else if (std::strncmp(path, "/ver.png", 8) == 0 || std::strncmp(path, "/pos.png", 8) == 0)
    {
        unsigned version;
        std::uint32_t duration;
        {
            std::lock_guard<std::mutex> lock(g_status.mutex);
            version = g_status.version;
            duration = g_status.duration;
        }
        body = pngOfWidth(1 + static_cast<int>(path[1] == 'v' ? version % 30000
                                                               : std::min<long long>(positionMs(duration) / 1000, 30000)));
        type = "image/png";
    }
    else if (std::strncmp(path, "/status", 7) == 0)
        body = statusJson();
    else if (std::strncmp(path, "/quit", 5) == 0)
    {
        body = "{\"quit\":true}";
        g_quit = true;
    }
    else
    {
        const char *missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(fd, missing, std::strlen(missing));
        return;
    }
    char head[256];
    // Allow-Origin lets the page read /meta.png's pixels (crossOrigin image).
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nAccess-Control-Allow-Origin: *\r\n"
                  "Cache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  type, body.size());
    sendAll(fd, head, std::strlen(head));
    sendAll(fd, body.data(), body.size());
}

int openListener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 || listen(fd, 16) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

void liveServer(int listener)
{
    while (!g_quit)
    {
        int fd = accept(listener, nullptr, nullptr);
        if (fd < 0)
            continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        std::thread([fd] {
            handleLiveClient(fd);
            close(fd);
        }).detach();
    }
}

// Asks an older copy of this payload to exit, so a new one can take the ports.
void stopPreviousInstance()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kLivePort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0)
    {
        const char *request = "GET /quit HTTP/1.0\r\n\r\n";
        sendAll(fd, request, std::strlen(request));
        char buffer[256];
        while (recv(fd, buffer, sizeof buffer, 0) > 0)
        {
        }
        std::printf("[main] asked the previous instance to quit\n");
        close(fd);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return;
    }
    close(fd);
}

// ---------------------------------------------------------------- Spotify

struct Login
{
    std::mutex mutex;
    std::condition_variable ready;
    bool available = false;
    std::shared_ptr<cspot::LoginBlob> blob;
};

void sendJson(struct mg_connection *conn, const std::string &body)
{
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
              "Content-Length: %zu\r\nConnection: close\r\n\r\n",
              body.size());
    mg_write(conn, body.data(), body.size());
}

// The HTTP half of Spotify's Zeroconf handoff: the phone reads the speaker's
// info, then posts the user's encrypted credentials.
class Zeroconf final : public CivetHandler
{
  public:
    explicit Zeroconf(Login &login) : login_(login)
    {
    }

    void start()
    {
        server_ = std::make_unique<CivetServer>(std::vector<std::string>{
            "listening_ports", std::to_string(kZeroconfPort), "num_threads", "2"});
        server_->addHandler("/spotify_info", this);
    }

    bool handleGet(CivetServer *, struct mg_connection *conn) override
    {
        std::string info;
        {
            std::lock_guard<std::mutex> lock(login_.mutex);
            info = login_.blob->buildZeroconfInfo();
        }
        sendJson(conn, info);
        return true;
    }

    bool handlePost(CivetServer *, struct mg_connection *conn) override
    {
        nlohmann::json reply;
        reply["status"] = 101;
        reply["spotifyError"] = 0;
        reply["statusString"] = "ERROR-OK";
        auto request = mg_get_request_info(conn);
        if (request->content_length > 0)
        {
            std::string body(static_cast<std::size_t>(request->content_length), '\0');
            mg_read(conn, body.data(), body.size());
            mg_header fields[10];
            int count = mg_split_form_urlencoded(body.data(), fields, 10);
            std::map<std::string, std::string> query;
            for (int i = 0; i < count; i++)
                query[fields[i].name] = fields[i].value;
            std::printf("[engine] Zeroconf login from the Spotify app\n");
            {
                std::lock_guard<std::mutex> lock(login_.mutex);
                login_.blob->loadZeroconfQuery(query);
                login_.available = true;
            }
            login_.ready.notify_all();
        }
        sendJson(conn, reply.dump());
        return true;
    }

  private:
    Login &login_;
    std::unique_ptr<CivetServer> server_;
};

void runSession(Login &login)
{
    std::shared_ptr<cspot::LoginBlob> blob;
    {
        std::lock_guard<std::mutex> lock(login.mutex);
        blob = login.blob;
    }
    auto ctx = cspot::Context::createFromBlob(blob);
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_320;
    ctx->config.volume = g_volume.load();
    g_sink.setVolume(g_volume.load());

    updateStatus([](Status &s) { s.state = "connecting"; });
    ctx->session->connectWithRandomAp();
    ctx->config.authData = ctx->session->authenticate(blob);
    if (ctx->config.authData.empty())
        throw std::invalid_argument("login rejected");
    if (!writeFileAtomically(kCredentialsTemporary, kCredentialsPath, ctx->getCredentialsJson()))
        std::printf("[engine] cannot save credentials\n");
    updateStatus([&](Status &s) {
        s.state = "ready";
        s.user = ctx->config.username;
    });
    notify("Spotify: \"%s\" ready for %s", kDeviceName, ctx->config.username.c_str());

    auto handler = std::make_shared<cspot::SpircHandler>(ctx);
    ctx->session->startTask();

    std::hash<std::string_view> hash;
    handler->getTrackPlayer()->setDataCallback(
        [hash](std::uint8_t *data, std::size_t bytes, std::string_view trackId) {
            return g_sink.write(data, bytes, hash(trackId));
        });

    std::atomic<bool> depleted{false};
    handler->setEventHandler([&](std::unique_ptr<cspot::SpircHandler::Event> event) {
        using Type = cspot::SpircHandler::EventType;
        static const char *const names[] = {"PLAY_PAUSE", "VOLUME", "TRACK_INFO", "DISC",
                                            "NEXT", "PREV", "SEEK", "DEPLETED", "FLUSH",
                                            "PLAYBACK_START"};
        int index = static_cast<int>(event->eventType);
        std::string detail;
        if (const bool *b = std::get_if<bool>(&event->data))
            detail = *b ? " true" : " false";
        else if (const int *i = std::get_if<int>(&event->data))
            detail = " " + std::to_string(*i);
        std::printf("[event] %s%s (actions %d)\n", index >= 0 && index < 10 ? names[index] : "?",
                    detail.c_str(), g_actions.load());
        switch (event->eventType)
        {
        case Type::PLAY_PAUSE: {
            bool paused = std::get<bool>(event->data);
            g_sink.setPaused(paused);
            bool changed = false;
            updateStatus([&](Status &s) {
                changed = s.paused != paused;
                s.paused = paused;
                s.state = paused ? "paused" : "playing";
            });
            // Only a real change counts: cspot also sends "playing" after every
            // track load, which must not interrupt a seamless track change.
            if (changed)
                g_actions++;
            break;
        }
        case Type::NEXT:
        case Type::PREV:
            // Drop the old track's buffered audio at once instead of playing it out.
            g_sink.clear();
            g_actions++;
            break;
        case Type::FLUSH:
        case Type::DISC:
        case Type::SEEK:
            g_sink.clear();
            g_actions++;
            if (event->eventType == Type::SEEK)
            {
                setPosition(std::get<int>(event->data) + kHeardDelayMs);
                updateStatus([](Status &) {});
            }
            if (event->eventType == Type::DISC)
                updateStatus([](Status &s) { s.state = "disconnected"; });
            break;
        case Type::PLAYBACK_START:
            g_nextStartMs = std::get<int>(event->data);
            g_sink.setPaused(true);
            g_sink.setDraining(false);
            depleted = false;
            g_sink.clear();
            break;
        case Type::DEPLETED:
            depleted = true;
            g_sink.setDraining(true);
            break;
        case Type::VOLUME:
            g_volume = std::get<int>(event->data);
            g_sink.setVolume(g_volume.load());
            std::printf("[engine] volume %d\n", g_volume.load());
            break;
        case Type::TRACK_INFO: {
            const auto &track = std::get<cspot::TrackInfo>(event->data);
            // Sent when the song's first audio enters the stream.
            setPosition(g_nextStartMs.exchange(0));
            updateStatus([&](Status &s) {
                s.track = track.name;
                s.artist = track.artist;
                s.album = track.album;
                s.cover = track.imageUrl;
                s.duration = track.duration;
            });
            g_toaster.songStarted(track);
            std::printf("[engine] track: %s - %s (%u ms)\n", track.artist.c_str(), track.name.c_str(),
                        track.duration);
            break;
        }
        default:
            break;
        }
    });

    // Tells Spotify when audio actually reaches the stream or runs out.
    std::atomic<bool> running{true};
    std::thread notifier([&] {
        while (running)
        {
            if (g_sink.takeTrackStarted())
                handler->notifyAudioReachedPlayback();
            if (depleted && g_sink.empty())
            {
                depleted = false;
                g_sink.setDraining(false);
                handler->notifyAudioEnded();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    try
    {
        while (!g_quit)
            ctx->session->handlePacket();
    }
    catch (...)
    {
        running = false;
        notifier.join();
        throw;
    }
    running = false;
    notifier.join();
}

void spotifyMain()
{
    bell::setDefaultLogger();

    Login login;
    login.blob = std::make_shared<cspot::LoginBlob>(kDeviceName);
    std::string saved = readFile(kCredentialsPath);
    if (!saved.empty())
    {
        try
        {
            login.blob->loadJson(saved);
            login.available = true;
            std::printf("[engine] using saved credentials\n");
        }
        catch (const std::exception &error)
        {
            std::printf("[engine] saved credentials unreadable: %s\n", error.what());
        }
    }

    Zeroconf zeroconf(login);
    try
    {
        zeroconf.start();
        std::printf("[engine] Zeroconf HTTP on port %d\n", kZeroconfPort);
    }
    catch (const std::exception &error)
    {
        notify("Spotify speaker: Zeroconf failed (%s)", error.what());
    }

    while (!g_quit)
    {
        {
            std::unique_lock<std::mutex> lock(login.mutex);
            if (!login.available)
                updateStatus([](Status &s) { s.state = "waiting for the Spotify app"; });
            login.ready.wait_for(lock, std::chrono::seconds(1), [&] { return login.available; });
            if (!login.available)
                continue;
        }
        try
        {
            runSession(login);
        }
        catch (const std::invalid_argument &error)
        {
            std::printf("[engine] login failed: %s\n", error.what());
            unlink(kCredentialsPath);
            std::lock_guard<std::mutex> lock(login.mutex);
            login.available = false;
            login.blob = std::make_shared<cspot::LoginBlob>(kDeviceName);
        }
        catch (const std::exception &error)
        {
            std::printf("[engine] session error: %s\n", error.what());
            updateStatus([&](Status &s) { s.state = std::string("error: ") + error.what(); });
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

void mdnsMain()
{
    in_addr host{};
    while (!lanAddress(&host) && !g_quit)
        std::this_thread::sleep_for(std::chrono::seconds(2));
    struct mdnsd *server = mdnsd_start(host, false);
    if (server == nullptr)
    {
        notify("Spotify speaker: cannot open mDNS (UDP 5353)");
        return;
    }
    mdnsd_set_hostname(server, "ps5-speaker.local", host);
    const char *txt[] = {"VERSION=1.0", "CPath=/spotify_info", "Stack=SP", nullptr};
    mdnsd_register_svc(server, kDeviceName, "_spotify-connect._tcp.local",
                       static_cast<unsigned short>(kZeroconfPort), nullptr, txt);
    std::printf("[mdns] announcing \"%s\" on %s:%d\n", kDeviceName, inet_ntoa(host), kZeroconfPort);
    while (!g_quit)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    mdnsd_stop(server);
}
} // namespace

int main()
{
    signal(SIGPIPE, SIG_IGN);
    mkdir(kDataDir, 0755);
    rename(kLogPath, kLogPrevious);
    if (std::freopen(kLogPath, "w", stdout) != nullptr)
    {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        dup2(fileno(stdout), fileno(stderr));
    }
    std::printf("[main] Spotify speaker payload starting\n");

    stopPreviousInstance();
    int listener = openListener(kLivePort);
    if (listener < 0)
    {
        notify("Spotify speaker: port %d busy (errno %d)", kLivePort, errno);
        return 1;
    }

    g_sink.start();
    loadNotifications();
    g_toaster.start();
    std::thread(liveServer, listener).detach();
    std::thread(mdnsMain).detach();
    std::thread(spotifyMain).detach();
    notify("Spotify speaker running: pick \"%s\" in the Spotify app", kDeviceName);

    while (!g_quit)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("[main] quit requested\n");
    close(listener);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    _exit(0);
}
