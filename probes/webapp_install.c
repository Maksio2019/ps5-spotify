/* Test tile installer probe (payload).
 *
 * Installs only SPKR00001 (now named "PS5 Speaker"); the other two test tiles are
 * uninstalled and their files removed. The three tiles tried:
 *  - SPKR00001 "Web App Test 2": category 0x10200 web app (webAppUri), installed the
 *    way ps5-payload-dev/svtplay does (main.c, GPL-3.0-or-later, John Törnblom).
 *  - SPKR00002 "Web Panel Test": deeplinkUri tile that opens the system browser, the
 *    way ps5-payload-dev/websrv installs its launcher (sys.c).
 *  - SPKR00003 "Music Core Test": web app that asks the system custom music core
 *    (musicCoreName/musicCoreTitleId) to host our page.
 * The https address of our test page is read from URL_FILE when the payload runs, so a
 * new tunnel address only needs a new file, not a new build.
 * Built with -DUNINSTALL it removes all three tiles and their files instead.
 *
 * Log:    /data/webapp_probe.log
 * Writes: /system_ex/app/SPKR0000{1,3}/{eboot.bin,sce_sys/param.json}
 *         /user/app/SPKR0000{1,2,3}/sce_sys/{param.json,icon0.png}
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define LOG_FILE "/data/webapp_probe.log"
#define URL_FILE "/data/spkr_url.txt"
#define PANEL_URI "http://192.168.1.90:8765/"

#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))
#define IOVEC_ENTRY(x) {x ? x : 0, x ? strlen(x) + 1 : 0}

#define INCASSET(name, file)                                                   \
  __asm__(".section .rodata\n"                                                 \
          ".global " #name "\n"                                                \
          ".global " #name "_end\n"                                            \
          ".global " #name "_size\n"                                           \
          ".align 16\n" #name ":\n"                                            \
          ".incbin \"" file "\"\n" #name "_end:\n" #name "_size:\n"            \
          ".quad " #name "_end - " #name "\n"                                  \
          ".previous\n");                                                      \
  extern const uint8_t name[];                                                 \
  extern const size_t name##_size;

INCASSET(icon0, ICON0_PATH);

enum tile_kind { WEB_APP, WEB_PANEL, MUSIC_CORE, WEB_APP_FIXED };

static const struct tile {
  const char *id;
  const char *name;
  enum tile_kind kind;
  const char *fixed_url; /* WEB_APP_FIXED only */
} tiles[] = {
    {"SPKR00001", "PS5 Speaker", WEB_APP, 0},
    {"SPKR00002", "Web Panel Test", WEB_PANEL, 0},
    {"SPKR00003", "Music Core Test", MUSIC_CORE, 0},
    /* Spotify's public web player: tried 2026-10-09, it shows "Something went wrong";
     * kept here only so the installer removes the tile. Built with
     * -DMUSIC_CORE_TEST_URL it is a second music core test (see param_json). */
    {"SPKR00004", "Music Core Test 2", MUSIC_CORE, 0},
};
#define TILE_COUNT (sizeof(tiles) / sizeof(tiles[0]))

/* System libraries are loaded with dlopen() only after the first notification, so a
 * hang while loading one shows up in the log instead of before main(). */
static int (*sceUserServiceInitialize)(void *);
static int (*sceAppInstUtilInitialize)(void);
static int (*sceAppInstUtilAppInstallAll)(void *);
static int (*sceAppInstUtilAppUnInstall)(const char *);

/* Same layout as payload/mdns_helper.c, which is hardware-verified. */
typedef struct {
  char useless1[45];
  char message[3075];
} notify_t;
int sceKernelSendNotificationRequest(int, notify_t *, size_t, int);

static void notify(const char *msg) {
  notify_t req;
  memset(&req, 0, sizeof(req));
  strncpy(req.message, msg, sizeof(req.message) - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static const char *volatile current_step = "before main";

/* Each step goes to klog and to LOG_FILE (readable over FTP afterwards). */
static void step(const char *fmt, ...) {
  char line[513];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line) - 1, fmt, ap);
  va_end(ap);
  klog_printf("webapp probe: %s\n", line);
  int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    size_t n = strlen(line);
    line[n] = '\n';
    write(fd, line, n + 1);
    fsync(fd);
    close(fd);
  }
}

/* A hung system call must not leave the process behind: log where, then exit. */
static void on_alarm(int sig) {
  (void)sig;
  step("TIMEOUT: stuck in '%s', exiting", (const char *)current_step);
  notify("webapp probe: stuck, see /data/webapp_probe.log");
  _exit(2);
}

static int remount_system_ex(void) {
  struct iovec iov[] = {
      IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
      IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
      IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
      IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
      IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
      IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
  };
  return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}

static int write_file(const char *path, const void *data, size_t size) {
  FILE *f = fopen(path, "w");
  if (!f) {
    return -1;
  }
  if (data && size && fwrite(data, size, 1, f) != 1) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int make_dir(const char *path) {
  return (mkdir(path, 0755) && errno != EEXIST) ? -1 : 0;
}

static int install_app(const char *title_id, const char *dir) {
  int (*install_title_dir)(const char *, const char *, void *) = 0;
  uint32_t handle;

  if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    install_title_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
  }
  if (install_title_dir) {
    return install_title_dir(title_id, dir, 0);
  }
  return sceAppInstUtilAppInstallAll(0);
}

static int read_url(char *url, size_t size) {
  FILE *f = fopen(URL_FILE, "r");
  if (!f) {
    return -1;
  }
  if (!fgets(url, (int)size, f)) {
    fclose(f);
    return -1;
  }
  fclose(f);
  url[strcspn(url, "\r\n ")] = 0;
  return strncmp(url, "https://", 8) ? -1 : 0;
}

static int param_json(const struct tile *t, const char *url, char *out, size_t size) {
  const char *head = "{\n  \"titleId\": \"%s\",\n"
                     "  \"localizedParameters\": {\n"
                     "    \"defaultLanguage\": \"en-US\",\n"
                     "    \"en-US\": { \"titleName\": \"%s\" }\n  },\n";
  int n = snprintf(out, size, head, t->id, t->name);
  switch (t->kind) {
  case WEB_APP_FIXED:
    url = t->fixed_url;
    /* fall through */
  case WEB_APP:
    n += snprintf(out + n, size - n,
                  "  \"applicationCategoryType\": 66048,\n"
                  "  \"downloadDataSize\": 256,\n"
                  "  \"webAppUri\": \"%s\"\n}\n",
                  url);
    break;
  case WEB_PANEL:
    n += snprintf(out + n, size - n, "  \"deeplinkUri\": \"" PANEL_URI "\"\n}\n");
    break;
  case MUSIC_CORE:
#ifdef MUSIC_CORE_TEST_URL
    /* Which music core a test tile asks for: MC3_* for SPKR00003, MC4_* for SPKR00004
     * (an empty title ID leaves the field out). */
    {
      int four = !strcmp(t->id, "SPKR00004");
      const char *core = four ? MC4_NAME : MC3_NAME, *title = four ? MC4_TITLE : MC3_TITLE;
      n += snprintf(out + n, size - n,
                    "  \"applicationCategoryType\": 66048,\n"
                    "  \"attribute\": 1644167168,\n"
                    "  \"downloadDataSize\": 256,\n"
                    "  \"displayLocation\": 188,\n"
                    "  \"serviceLaunchButtonKeyCode\": 2,\n"
                    "  \"musicCoreName\": \"%s\",\n", core);
      if (*title) {
        n += snprintf(out + n, size - n, "  \"musicCoreTitleId\": \"%s\",\n", title);
      }
      n += snprintf(out + n, size - n, "  \"webAppUri\": \"%s\"\n}\n", url);
      break;
    }
#endif
    /* The official Spotify app's param.json uses these fields (and attribute
     * 0x62000000, displayLocation 188, serviceLaunchButtonKeyCode 2). */
    n += snprintf(out + n, size - n,
                  "  \"applicationCategoryType\": 66048,\n"
                  "  \"attribute\": 1644167168,\n"
                  "  \"downloadDataSize\": 256,\n"
                  "  \"displayLocation\": 188,\n"
                  "  \"serviceLaunchButtonKeyCode\": 2,\n"
                  "  \"musicCoreName\": \"CustomMusicCore\",\n"
                  "  \"musicCoreTitleId\": \"NPXS40201\",\n"
                  "  \"webAppUri\": \"%s\"\n}\n",
                  url);
    break;
  }
  return n;
}

static void remove_tile_files(const struct tile *t) {
  char path[256];
  static const char *const roots[] = {"/system_ex/app/", "/user/app/"};
  static const char *const files[] = {"sce_sys/param.json", "sce_sys/icon0.png", "eboot.bin"};

  for (size_t r = 0; r < 2; r++) {
    for (size_t f = 0; f < 3; f++) {
      snprintf(path, sizeof(path), "%s%s/%s", roots[r], t->id, files[f]);
      unlink(path);
    }
    snprintf(path, sizeof(path), "%s%s/sce_sys", roots[r], t->id);
    rmdir(path);
    snprintf(path, sizeof(path), "%s%s", roots[r], t->id);
    rmdir(path);
  }
}

static int write_tile_files(const struct tile *t, const char *param, size_t param_len) {
  char path[256];

  /* Web apps (like svtplay) also need /system_ex/app/<ID> with an empty eboot.bin. */
  if (t->kind != WEB_PANEL) {
    snprintf(path, sizeof(path), "/system_ex/app/%s", t->id);
    if (make_dir(path)) {
      return -1;
    }
    snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys", t->id);
    if (make_dir(path)) {
      return -1;
    }
    snprintf(path, sizeof(path), "/system_ex/app/%s/eboot.bin", t->id);
    if (write_file(path, 0, 0)) {
      return -1;
    }
    snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys/param.json", t->id);
    if (write_file(path, param, param_len)) {
      return -1;
    }
  }
  snprintf(path, sizeof(path), "/user/app/%s", t->id);
  if (make_dir(path)) {
    return -1;
  }
  snprintf(path, sizeof(path), "/user/app/%s/sce_sys", t->id);
  if (make_dir(path)) {
    return -1;
  }
  snprintf(path, sizeof(path), "/user/app/%s/sce_sys/param.json", t->id);
  if (write_file(path, param, param_len)) {
    return -1;
  }
  snprintf(path, sizeof(path), "/user/app/%s/sce_sys/icon0.png", t->id);
  return write_file(path, icon0, icon0_size);
}

int main(void) {
  char msg[512];
  int err;

  signal(SIGALRM, on_alarm);
  alarm(20);
  notify("webapp probe: started");
  step("---- start (pid %d), authid 0x%016lx", getpid(), kernel_get_ucred_authid(-1));

  void *lib;
  current_step = "dlopen libSceUserService.sprx";
  lib = dlopen("libSceUserService.sprx", RTLD_LAZY);
  if (!lib || !(sceUserServiceInitialize = dlsym(lib, "sceUserServiceInitialize"))) {
    step("UserService unavailable: %s", dlerror());
    notify("webapp probe: UserService unavailable");
    return -1;
  }
  current_step = "dlopen libSceAppInstUtil.sprx";
  lib = dlopen("libSceAppInstUtil.sprx", RTLD_LAZY);
  if (!lib || !(sceAppInstUtilInitialize = dlsym(lib, "sceAppInstUtilInitialize")) ||
      !(sceAppInstUtilAppInstallAll = dlsym(lib, "sceAppInstUtilAppInstallAll")) ||
      !(sceAppInstUtilAppUnInstall = dlsym(lib, "sceAppInstUtilAppUnInstall"))) {
    step("AppInstUtil unavailable: %s", dlerror());
    notify("webapp probe: AppInstUtil unavailable");
    return -1;
  }

  current_step = "sceUserServiceInitialize";
  step("UserService init -> 0x%08x", sceUserServiceInitialize(0));

  /* websrv (sys.c) and BFplayer set this authid before using AppInstUtil. */
  current_step = "kernel_set_ucred_authid";
  kernel_set_ucred_authid(-1, 0x4801000000000013L);

  current_step = "sceAppInstUtilInitialize";
  if ((err = sceAppInstUtilInitialize())) {
    step("AppInstUtil init failed 0x%08x", err);
    snprintf(msg, sizeof(msg), "webapp probe: AppInstUtil init 0x%08X", err);
    notify(msg);
    return -1;
  }

#ifndef UNINSTALL
  /* Check the address first: a missing one must not leave the user without a tile. */
  char url[256];
  if (read_url(url, sizeof(url))) {
    step("no https address in " URL_FILE ", nothing changed");
    notify("webapp probe: no https address in " URL_FILE ", nothing changed");
    return -1;
  }
#endif

  current_step = "sceAppInstUtilAppUnInstall";
  for (size_t i = 0; i < TILE_COUNT; i++) {
    step("uninstall old %s -> 0x%08x", tiles[i].id, sceAppInstUtilAppUnInstall(tiles[i].id));
  }

  current_step = "remount /system_ex";
  step("remount /system_ex -> %d (errno %d)", remount_system_ex(), errno);
  current_step = "file writes";

#ifdef UNINSTALL
  for (size_t i = 0; i < TILE_COUNT; i++) {
    remove_tile_files(&tiles[i]);
  }
  step("uninstalled and files removed");
  notify("webapp probe: all test tiles removed");
  return 0;
#else
  step("page address %s", url);

  int n = snprintf(msg, sizeof(msg), "webapp probe:");
  for (size_t i = 0; i < TILE_COUNT; i++) {
    char param[2048];
    /* Only the web app is still needed; the other two tests were removed above.
     * Built with -DMUSIC_CORE_TEST_URL, SPKR00003 is installed too, with that page. */
    const char *page = url;
#ifdef MUSIC_CORE_TEST_URL
    if (!strcmp(tiles[i].id, "SPKR00003")) {
      page = MUSIC_CORE_TEST_URL "?t=3";
    } else if (!strcmp(tiles[i].id, "SPKR00004")) {
      page = MUSIC_CORE_TEST_URL "?t=4";
    } else
#endif
    if (tiles[i].kind != WEB_APP && tiles[i].kind != WEB_APP_FIXED) {
      remove_tile_files(&tiles[i]);
      continue;
    }
    int len = param_json(&tiles[i], page, param, sizeof(param));
    remove_tile_files(&tiles[i]);
    if (write_tile_files(&tiles[i], param, (size_t)len)) {
      step("%s: file write failed (errno %d)", tiles[i].id, errno);
      n += snprintf(msg + n, sizeof(msg) - n, " %s write failed", tiles[i].name);
      continue;
    }
    current_step = tiles[i].id;
    err = install_app(tiles[i].id, "/user/app/");
    step("install %s -> 0x%08x", tiles[i].id, err);
    n += snprintf(msg + n, sizeof(msg) - n, " %s 0x%08X,", tiles[i].name, err);
  }
  notify(msg);
  return 0;
#endif
}
