#include "spotify_engine.hpp"

#include "audio_sink.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <ifaddrs.h>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <variant>

#include "CivetServer.h"
#include "BellLogger.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "civetweb.h"
#include "nlohmann/json.hpp"

namespace spotify_engine
{
namespace
{
constexpr const char *kDeviceName = "PS5 Speaker";
// The sandbox refuses fixed listening ports (bind -> EACCES) but allows a
// system-assigned one. The helper payload (payload/mdns_helper.c) reads the
// port and this name from here and announces them over mDNS, which needs the
// fixed port 5353 that only a payload can open.
constexpr const char *kZeroconfPortPath = "/download0/zeroconf.port";
constexpr const char *kZeroconfPortTemporary = "/download0/zeroconf.tmp";
// Reusable login token from Spotify (not a password), kept so the speaker
// signs in again by itself on the next launch.
constexpr const char *kCredentialsPath = "/download0/credentials.json";
constexpr const char *kCredentialsTemporary = "/download0/credentials.tmp";

std::mutex g_statusMutex;
std::string g_status[kStatusLines];

void setStatus(int index, const std::string &text)
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status[index] = text;
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

// Writes a complete temporary file, then renames it over the stable path.
// fopen, like the log: the app's stdio is known to write /download0.
bool writeFileAtomically(const char *temporary, const char *path, const std::string &content)
{
    FILE *file = std::fopen(temporary, "wb");
    if (file == nullptr)
    {
        std::printf("[file] open %s failed: errno %d\n", temporary, errno);
        return false;
    }
    std::size_t wrote = std::fwrite(content.data(), 1, content.size(), file);
    bool closed = std::fclose(file) == 0;
    if (wrote != content.size() || !closed)
    {
        std::printf("[file] write %s failed: errno %d\n", temporary, errno);
        return false;
    }
    if (rename(temporary, path) != 0)
    {
        std::printf("[file] rename to %s failed: errno %d\n", path, errno);
        return false;
    }
    return true;
}

void saveCredentials(const std::string &json)
{
    if (!writeFileAtomically(kCredentialsTemporary, kCredentialsPath, json))
        std::printf("[engine] cannot save credentials\n");
}

std::string lanAddress()
{
    struct ifaddrs *entries = nullptr;
    if (getifaddrs(&entries) != 0 || entries == nullptr)
        return "UNKNOWN";
    char text[INET_ADDRSTRLEN] = "UNKNOWN";
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(entries->ifa_addr)->sin_addr, text,
              sizeof text);
    freeifaddrs(entries);
    return text;
}

// Credentials arrive from the phone (Zeroconf) or from the saved file.
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

    // Returns the listening port.
    int start()
    {
        server_ = std::make_unique<CivetServer>(
            std::vector<std::string>{"listening_ports", "0", "num_threads", "2"});
        server_->addHandler("/spotify_info", this);
        auto ports = server_->getListeningPorts();
        if (ports.empty())
            throw std::runtime_error("no listening port");
        return ports.front();
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

std::string upper(std::string_view text)
{
    std::string out;
    for (char c : text)
    {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        bool known = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     std::strchr(" .:-%/", c) != nullptr;
        out.push_back(known && c != '\0' ? c : ' ');
    }
    return out;
}

void runSession(Login &login, AudioSink &sink)
{
    std::shared_ptr<cspot::LoginBlob> blob;
    {
        std::lock_guard<std::mutex> lock(login.mutex);
        blob = login.blob;
    }
    auto ctx = cspot::Context::createFromBlob(blob);
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_320;

    setStatus(1, "CONNECTING TO SPOTIFY...");
    ctx->session->connectWithRandomAp();
    ctx->config.authData = ctx->session->authenticate(blob);
    if (ctx->config.authData.empty())
        throw std::invalid_argument("login rejected");
    saveCredentials(ctx->getCredentialsJson());
    setStatus(1, "CONNECTED AS " + upper(ctx->config.username));
    setStatus(2, "READY - PICK PS5 IN THE SPOTIFY APP");

    auto handler = std::make_shared<cspot::SpircHandler>(ctx);
    ctx->session->startTask();

    std::hash<std::string_view> hash;
    handler->getTrackPlayer()->setDataCallback(
        [&sink, hash](std::uint8_t *data, std::size_t bytes, std::string_view trackId) {
            return sink.write(data, bytes, hash(trackId));
        });

    std::atomic<bool> depleted{false};
    handler->setEventHandler([&](std::unique_ptr<cspot::SpircHandler::Event> event) {
        using Type = cspot::SpircHandler::EventType;
        switch (event->eventType)
        {
        case Type::PLAY_PAUSE: {
            bool paused = std::get<bool>(event->data);
            sink.setPaused(paused);
            setStatus(2, paused ? "PAUSED" : "PLAYING");
            break;
        }
        case Type::FLUSH:
        case Type::DISC:
        case Type::SEEK:
            sink.clear();
            if (event->eventType == Type::DISC)
                setStatus(2, "DISCONNECTED - PICK PS5 IN THE SPOTIFY APP");
            break;
        case Type::PLAYBACK_START:
            sink.setPaused(true);
            sink.setDraining(false);
            depleted = false;
            sink.clear();
            break;
        case Type::DEPLETED:
            depleted = true;
            sink.setDraining(true);
            break;
        case Type::VOLUME:
            sink.setVolume(std::get<int>(event->data));
            break;
        case Type::TRACK_INFO: {
            const auto &track = std::get<cspot::TrackInfo>(event->data);
            setStatus(3, upper(track.name));
            setStatus(4, upper(track.artist + " - " + track.album));
            std::printf("[engine] track: %s - %s\n", track.artist.c_str(),
                        track.name.c_str());
            break;
        }
        default:
            break;
        }
    });

    // Tells Spotify when audio actually reaches the speaker or runs out.
    std::atomic<bool> running{true};
    std::thread notifier([&] {
        while (running)
        {
            if (sink.takeTrackStarted())
                handler->notifyAudioReachedPlayback();
            if (depleted && sink.empty())
            {
                depleted = false;
                sink.setDraining(false);
                handler->notifyAudioEnded();
            }
            char line[64];
            std::snprintf(line, sizeof line, "UNDERRUNS %llu",
                          static_cast<unsigned long long>(sink.underruns()));
            setStatus(5, line);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // The Mercury task reconnects by itself; this loop dispatches packets.
    try
    {
        while (true)
            ctx->session->handlePacket();
    }
    catch (...)
    {
        running = false;
        notifier.join();
        throw;
    }
}

void engineMain()
{
    bell::setDefaultLogger();

    AudioSink sink;
    int port = sink.start();
    char line[64];
    std::snprintf(line, sizeof line, "SPEAKER %s  IP %s  AUDIO %X", kDeviceName,
                  lanAddress().c_str(), static_cast<unsigned>(port));
    setStatus(0, line);

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
        int port = zeroconf.start();
        std::printf("[engine] Zeroconf HTTP on port %d\n", port);
        bool written = writeFileAtomically(kZeroconfPortTemporary, kZeroconfPortPath,
                                           std::to_string(port) + "\n" + kDeviceName + "\n");
        std::printf("[engine] port file %s\n", written ? "written" : "NOT written");
    }
    catch (const std::exception &error)
    {
        std::printf("[engine] Zeroconf failed: %s\n", error.what());
        setStatus(1, "ERROR: " + upper(error.what()));
    }

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(login.mutex);
            if (!login.available)
                setStatus(1, "WAITING - PICK PS5 IN THE SPOTIFY APP");
            login.ready.wait(lock, [&] { return login.available; });
        }
        try
        {
            runSession(login, sink);
        }
        catch (const std::invalid_argument &error)
        {
            // Spotify refused the credentials: forget them, wait for the phone.
            std::printf("[engine] login failed: %s\n", error.what());
            unlink(kCredentialsPath);
            std::lock_guard<std::mutex> lock(login.mutex);
            login.available = false;
            login.blob = std::make_shared<cspot::LoginBlob>(kDeviceName);
        }
        catch (const std::exception &error)
        {
            std::printf("[engine] session error: %s\n", error.what());
            setStatus(1, "ERROR: " + upper(error.what()) + " - RETRYING");
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}
} // namespace

void start() noexcept
{
    setStatus(1, "STARTING...");
    std::thread(engineMain).detach();
}

void statusLine(int index, char *out, unsigned size) noexcept
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    std::snprintf(out, size, "%s", g_status[index].c_str());
}
} // namespace spotify_engine
