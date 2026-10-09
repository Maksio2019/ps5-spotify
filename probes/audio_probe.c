/* Audio probe v2: which sceAudioOut port types are audible from a payload ELF,
 * and does the device actually consume samples (blocking Output) or not.
 *
 * Runs a fixed list of tests, ~6 s each. Each test plays a 480 Hz beep pattern.
 * A notification announces each test; full results go to /data/audio_probe.log.
 *
 * Build:  bin/prospero-clang -O2 -Wall -o audio_probe.elf audio_probe.c \
 *           -lSceAudioOut -lSceUserService
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned len,
                    unsigned freq, unsigned param);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutSetVolume(int handle, int flag, int *vol);
int sceAudioOutGetPortState(int handle, void *state);
int sceAudioOutGetLastOutputTime(int handle, uint64_t *outputTime);

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *userId);

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

#define GRAIN 256
#define RATE 48000
#define FMT_S16_STEREO 1
#define USER_SYSTEM 0xFF
#define VOL_0DB 32768
#define TEST_SECONDS 6

static FILE *g_log;

static void notify(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;

  memset(&req, 0, sizeof req);
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static void logf_(const char *fmt, ...) {
  va_list ap;

  if (!g_log)
    return;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fflush(g_log);
}

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

/* 480 Hz at 48 kHz is exactly 100 samples per period: square-ish wave. */
static void fill_grain(int16_t *buf, unsigned *phase, int on) {
  for (int i = 0; i < GRAIN; i++) {
    int16_t s = 0;
    if (on)
      s = (*phase % 100) < 50 ? 6000 : -6000;
    buf[2 * i] = s;
    buf[2 * i + 1] = s;
    (*phase)++;
  }
}

static void run_test(int n, int total, int type, int user, const char *label) {
  static int16_t buf[GRAIN * 2];
  uint8_t state[64];
  int vol[8];
  unsigned phase = 0;

  notify("AudioProbe %d/%d: %s (port %d, user 0x%x) - LISTEN", n, total, label,
         type, user);
  logf_("\n== test %d/%d: %s type=%d user=0x%x\n", n, total, label, type, user);

  int h = sceAudioOutOpen(user, type, 0, GRAIN, RATE, FMT_S16_STEREO);
  logf_("open -> 0x%08x\n", h);
  if (h < 0) {
    notify("AudioProbe %d/%d: open FAILED 0x%08x", n, total, h);
    sleep(2);
    return;
  }

  for (int i = 0; i < 8; i++)
    vol[i] = VOL_0DB;
  int r = sceAudioOutSetVolume(h, 0xFF, vol);
  logf_("setvolume(all) -> 0x%08x\n", r);
  if (r < 0) {
    r = sceAudioOutSetVolume(h, 0x3, vol);
    logf_("setvolume(L|R) -> 0x%08x\n", r);
  }

  memset(state, 0, sizeof state);
  r = sceAudioOutGetPortState(h, state);
  logf_("portstate -> 0x%08x bytes:", r);
  for (int i = 0; i < 32; i++)
    logf_(" %02x", state[i]);
  logf_("\n");

  uint64_t t_last0 = 0, t_last1 = 0;
  sceAudioOutGetLastOutputTime(h, &t_last0);

  int grains = TEST_SECONDS * RATE / GRAIN;
  int ok = 0, fail = 0, first_err = 0;
  uint64_t t0 = now_us();
  for (int g = 0; g < grains; g++) {
    /* 0.5 s on, 0.25 s off */
    int on = (g % 141) < 94;
    fill_grain(buf, &phase, on);
    r = sceAudioOutOutput(h, buf);
    if (r < 0) {
      if (!first_err)
        first_err = r;
      fail++;
      usleep(5000);
    } else {
      ok++;
    }
  }
  sceAudioOutOutput(h, NULL); /* drain */
  uint64_t elapsed_ms = (now_us() - t0) / 1000;
  r = sceAudioOutGetLastOutputTime(h, &t_last1);

  logf_("output ok=%d fail=%d first_err=0x%08x elapsed=%llums (expect ~%dms if "
        "blocking)\n",
        ok, fail, first_err, (unsigned long long)elapsed_ms,
        TEST_SECONDS * 1000);
  logf_("lastOutputTime %llu -> %llu (r=0x%08x)\n",
        (unsigned long long)t_last0, (unsigned long long)t_last1, r);

  memset(state, 0, sizeof state);
  sceAudioOutGetPortState(h, state);
  logf_("portstate after:");
  for (int i = 0; i < 32; i++)
    logf_(" %02x", state[i]);
  logf_("\n");

  notify("AudioProbe %d/%d done: ok=%d fail=%d err=0x%08x %llums", n, total, ok,
         fail, first_err, (unsigned long long)elapsed_ms);
  sceAudioOutClose(h);
  sleep(2);
}

int main(void) {
  int fg = -1;

  g_log = fopen("/data/audio_probe.log", "w");
  logf_("audio probe v2, pid %d\n", getpid());

  int r = sceAudioOutInit();
  logf_("sceAudioOutInit -> 0x%08x\n", r);
  r = sceUserServiceInitialize(NULL);
  logf_("sceUserServiceInitialize -> 0x%08x\n", r);
  r = sceUserServiceGetForegroundUser(&fg);
  logf_("sceUserServiceGetForegroundUser -> 0x%08x user=0x%x\n", r, fg);
  if (r < 0)
    fg = USER_SYSTEM;

  notify("AudioProbe v2: 7 tests x %ds start in 5 s. Count which ones you hear.",
         TEST_SECONDS);
  sleep(5);

  run_test(1, 7, 0, USER_SYSTEM, "MAIN sys");
  run_test(2, 7, 0, fg, "MAIN fg");
  run_test(3, 7, 1, USER_SYSTEM, "BGM sys");
  run_test(4, 7, 1, fg, "BGM fg");
  run_test(5, 7, 2, fg, "VOICE fg");
  run_test(6, 7, 3, fg, "PERSONAL fg");
  run_test(7, 7, 4, fg, "PADSPK fg (controller speaker)");

  notify("AudioProbe v2 finished. Log: /data/audio_probe.log");
  logf_("\nfinished\n");
  if (g_log)
    fclose(g_log);
  return 0;
}
