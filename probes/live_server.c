/* Live MP3 server probe (payload).
 *
 * Serves http://<console>:8790/live.mp3: an MP3 "file" with a fixed length (12 hours at
 * 128 kbit/s) whose bytes are encoded with LAME in real time, about one second ahead of
 * playback. This is the shape the system music core plays in the background (see
 * howitworks.md). The audio is a two-tone beep every 2 s; nothing is written to disk.
 * Also serves /status (JSON) to test whether the web page can read data from us.
 *
 * Log: /data/spkr_live.log
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <lame/lame.h>

#define PORT 8790
#define LOG_FILE "/data/spkr_live.log"
#define RATE 44100
#define KBPS 128
#define LIVE_LENGTH (12LL * 3600 * KBPS * 1000 / 8)
#define CHUNK_FRAMES (RATE / 10)
#define AHEAD_SECONDS 1.0

typedef struct {
  char useless1[45];
  char message[3075];
} notify_t;
int sceKernelSendNotificationRequest(int, notify_t *, size_t, int);

static void notify(const char *fmt, ...) {
  notify_t req;
  va_list ap;
  memset(&req, 0, sizeof(req));
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) {
  char line[512];
  va_list ap;
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  int n = (int)strftime(line, sizeof(line), "%H:%M:%S ", &tm);
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
  va_end(ap);
  strcat(line, "\n");
  pthread_mutex_lock(&log_lock);
  int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    write(fd, line, strlen(line));
    close(fd);
  }
  pthread_mutex_unlock(&log_lock);
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int send_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  while (len) {
    ssize_t n = send(fd, p, len, 0);
    if (n <= 0) {
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* 150 ms beep every 2 s: 660 Hz then 990 Hz, so it differs from the PC test server. */
static void tone(int16_t *out, int frames, long long start) {
  for (int i = 0; i < frames; i++) {
    long long s = start + i;
    long long t = s % (RATE * 2);
    double f = t < RATE * 0.075 ? 660.0 : 990.0;
    int16_t v = t < RATE * 0.15 ? (int16_t)(8000 * sin(2 * M_PI * f * s / RATE)) : 0;
    out[2 * i] = out[2 * i + 1] = v;
  }
}

/* Parses "Range: bytes=a-b". Returns 1 if present. */
static int parse_range(const char *req, long long *start, long long *end) {
  const char *r = strcasestr(req, "\nRange: bytes=");
  if (!r) {
    return 0;
  }
  r += strlen("\nRange: bytes=");
  char *dash;
  if (*r == '-') { /* suffix range: last N bytes */
    *start = LIVE_LENGTH - strtoll(r + 1, NULL, 10);
    *end = LIVE_LENGTH - 1;
    return 1;
  }
  *start = strtoll(r, &dash, 10);
  *end = (*dash == '-' && dash[1] >= '0' && dash[1] <= '9') ? strtoll(dash + 1, NULL, 10)
                                                         : LIVE_LENGTH - 1;
  if (*end >= LIVE_LENGTH) {
    *end = LIVE_LENGTH - 1;
  }
  return 1;
}

static void serve_live(int fd, const char *req) {
  long long start = 0, end = LIVE_LENGTH - 1;
  char head[512];
  int ranged = parse_range(req, &start, &end);

  if (ranged) {
    snprintf(head, sizeof(head),
             "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/mpeg\r\n"
             "Accept-Ranges: bytes\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
             "Content-Length: %lld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
             start, end, (long long)LIVE_LENGTH, end - start + 1);
  } else {
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nAccept-Ranges: bytes\r\n"
             "Content-Length: %lld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
             (long long)LIVE_LENGTH);
  }
  if (send_all(fd, head, strlen(head))) {
    return;
  }

  /* A request far into the file (players peek at the end for tags): send filler. */
  if (start > 1000000) {
    static const char zeros[4096];
    long long left = end - start + 1;
    while (left > 0) {
      size_t n = left > (long long)sizeof(zeros) ? sizeof(zeros) : (size_t)left;
      if (send_all(fd, zeros, n)) {
        break;
      }
      left -= (long long)n;
    }
    logf_("live: filler range %lld-%lld", start, end);
    return;
  }

  lame_t lame = lame_init();
  lame_set_in_samplerate(lame, RATE);
  lame_set_num_channels(lame, 2);
  lame_set_brate(lame, KBPS);
  lame_set_quality(lame, 5);
  if (lame_init_params(lame) < 0) {
    logf_("live: lame_init_params failed");
    lame_close(lame);
    return;
  }

  static int16_t pcm[CHUNK_FRAMES * 2];
  unsigned char mp3[CHUNK_FRAMES * 5 / 4 + 7200];
  long long sent = 0, total = end - start + 1, frames = 0;
  double began = now_seconds();
  logf_("live: start (range %s %lld-%lld)", ranged ? "yes" : "no", start, end);

  while (sent < total) {
    tone(pcm, CHUNK_FRAMES, frames);
    int n = lame_encode_buffer_interleaved(lame, pcm, CHUNK_FRAMES, mp3, sizeof(mp3));
    frames += CHUNK_FRAMES;
    if (n > 0) {
      if (n > total - sent) {
        n = (int)(total - sent);
      }
      if (send_all(fd, mp3, (size_t)n)) {
        break;
      }
      sent += n;
    }
    double ahead = (double)frames / RATE - (now_seconds() - began);
    if (ahead > AHEAD_SECONDS) {
      usleep((useconds_t)((ahead - AHEAD_SECONDS) * 1e6));
    }
  }
  logf_("live: closed after %.1f s, %lld bytes", now_seconds() - began, sent);
  lame_close(lame);
}

static void serve_status(int fd) {
  char body[128], head[256];
  snprintf(body, sizeof(body), "{\"server\":\"spkr live probe\",\"uptime\":%.0f}", now_seconds());
  snprintf(head, sizeof(head),
           "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
           "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
           strlen(body));
  send_all(fd, head, strlen(head));
  send_all(fd, body, strlen(body));
}

static void *client_main(void *arg) {
  int fd = (int)(intptr_t)arg;
  char req[4096];
  size_t len = 0;

  /* Read the request head. */
  while (len < sizeof(req) - 1) {
    ssize_t n = recv(fd, req + len, sizeof(req) - 1 - len, 0);
    if (n <= 0) {
      break;
    }
    len += (size_t)n;
    req[len] = 0;
    if (strstr(req, "\r\n\r\n")) {
      break;
    }
  }
  req[len] = 0;

  char method[8] = "", path[256] = "";
  sscanf(req, "%7s %255s", method, path);
  const char *ua = strcasestr(req, "\nUser-Agent: ");
  char agent[96] = "";
  if (ua) {
    sscanf(ua + 13, "%95[^\r\n]", agent);
  }
  logf_("%s %s (%s)", method, path, agent);

  if (!strncmp(path, "/live.mp3", 9)) {
    serve_live(fd, req);
  } else if (!strncmp(path, "/status", 7)) {
    serve_status(fd);
  } else {
    const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(fd, nf, strlen(nf));
  }
  close(fd);
  return NULL;
}

int main(void) {
  struct sockaddr_in addr;
  int one = 1;

  signal(SIGPIPE, SIG_IGN);
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) || listen(srv, 8)) {
    notify("Spotify live probe: port %d busy (errno %d)", PORT, errno);
    return 1;
  }
  logf_("---- listening on port %d", PORT);
  notify("Spotify live probe: serving live MP3 on port %d", PORT);

  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) {
      continue;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    pthread_t t;
    if (pthread_create(&t, NULL, client_main, (void *)(intptr_t)fd)) {
      close(fd);
      continue;
    }
    pthread_detach(t);
  }
}
