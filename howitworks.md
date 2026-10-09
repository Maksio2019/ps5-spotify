# How PS5 homebrew works (reference notes)

Findings from reading the source of public PS5 homebrew projects (2026-10-09), so we stop guessing.
Each claim names the project it comes from. "Untested" means nobody has published a result.

## Two kinds of homebrew code

| | Title (our app) | Payload |
| --- | --- | --- |
| Built with | native title template (fself, `eboot.bin`) | ps5-payload-dev/sdk (`.deps/native/ps5-payload-sdk`), plain ELF |
| Started by | the system, from a home-screen tile | an ELF loader (elfldr port 9021, Payload Manager) |
| Rights | sandboxed title (uid 1, no fixed ports, system-only exports resolve to NULL) | jailbroken process: kernel r/w via `<ps5/kernel.h>`, any port, can resolve any export by NID |
| Video / audio | normal (audio routed while in focus) | VideoOut needs tricks (below); our probe: AudioOut opens but is routed nowhere |
| Survives a game launch | no: a title is the one "BigApp"; launching another BigApp replaces it | yes, payloads are separate processes |

Payloads cannot be launched from a tile by themselves; a resident payload must already be running
(sent again after each jailbreak). Tiles stay registered across reboots.

## The four kinds of home-screen tile

### 1. Native folder title (what we use)
`/data/homebrew/<ID>/` with `eboot.bin`, `sce_sys/param.json`, icons. ShadowMountPlus (drakmor) scans
`/data/homebrew`, `/mnt/ext*/homebrew`, `/mnt/usb*/homebrew` and registers each folder. It also mounts
`/data` and `/mnt` into the sandbox at `/mnt/sandbox/<ID>_<n>/{data,mnt}` for `PPSA*`, `CUSA*`, `LAPY*`, `FAKE*`
IDs (five digits). Examples: our app, RetroArch (PPSA99169), ProsperoLight (PPSA99002), Jelly5 (PPSA99505).
Games use `applicationCategoryType` 0, media apps 65536.

### 2. `deeplinkUri` tile: opens the system web browser at a URL
param.json is minimal: `titleId`, `deeplinkUri`, `localizedParameters` (sometimes category/attributes).
Selecting the tile opens the PS5 web browser at that URL, with no splash. This is what "web panel" apps are.
- websrv Homebrew Launcher: `FAKE00000` -> `http://127.0.0.1:8080`
- ftpsrv: `BREW10001` -> `http://127.0.0.1:8080/fs/user/app/BREW10001/websrv.html`
- BFplayer: `PSMC00001`, category 65536, `attribute` 0x20000001, `attribute3` 4 -> `http://127.0.0.1:9040/launch`
- ProsperoPlayer: `PRSP10001` -> `http://127.0.0.1:9055/launch`

The URL usually points at a resident payload on loopback. BFplayer's `/launch` handler then starts the real
player as a BigApp (see "BigApp hosting" below) and answers 204.

Registration (websrv `src/ps5/sys.c`): write `/user/app/<ID>/sce_sys/param.json` and `icon0.png`, then
`sceAppInstUtilInitialize()` and `sceAppInstUtilAppInstallTitleDir(id, "/user/app/", 0)`. That function is resolved
at runtime: `kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &h)` + `kernel_dynlib_resolve(-1, h, "Wudg3Xe3heE")`,
falling back to `sceAppInstUtilAppInstallAll(0)`. The SDK sample `samples/install_app` does the same.
websrv also calls `kernel_set_ucred_authid(-1, 0x4801000000000013)` before installing.

### 3. `webAppUri` tile (category 66048 = 0x10200): the system web app launcher
The system runs its own `/web_app_launcher` and loads `webAppUri` as a 10-foot web app, with a splash.
Working examples, all by ps5-payload-dev: svtplay (`BREW10002`), tv4play, plutotv, launchpad (`BREW10006`).
svtplay's param.json is only: `applicationCategoryType` 66048, `titleId`, `localizedParameters`,
`downloadDataSize` 256, `webAppUri` `https://ps5-payload-dev.github.io/svtplay`.
Pages are hosted on **https** (GitHub Pages). Once installed, the tile works **without a jailbreak**.

Installer (svtplay `main.c`, a payload):
1. `sceAppInstUtilInitialize()`, `sceAppInstUtilAppUnInstall(ID)`.
2. Remount `/system_ex` writable: `nmount` with `from=/dev/ssd0.system_ex`, `fspath=/system_ex`,
   `fstype=exfatfs`, `large=yes`, `timezone=static`, `async`, `ignoreacl`, flag `MNT_UPDATE`.
3. Create `/system_ex/app/<ID>/eboot.bin` (**empty file**) and `/system_ex/app/<ID>/sce_sys/param.json`.
4. Create `/user/app/<ID>/sce_sys/{icon0.png,pic1.png,param.json}`.
5. `sceAppInstUtilAppInstallTitleDir(ID, "/user/app/", 0)`.
Linked with `-lSceIpmi -lSceAppInstUtil`.

Our failed attempts with category 66048, for comparison:
- Our app with 66048 and no `webAppUri`: "Something went wrong" (expected).
- PPSA99779 with music-core fields + `http://192.168.1.90:8765/`, registered by ShadowMountPlus: page never fetched.
- PPSA99778 "Web App Test" (2026-10-09) without music-core fields, same http URL, registered by ShadowMountPlus,
  with our hello-world `eboot.bin`: "Something went wrong", page never fetched.
Differences from the working projects: registration route (ShadowMountPlus vs the installer above), http LAN vs https,
`PPSA` vs non-PPSA title ID, non-empty eboot. Not yet known which one matters.

Hardware results on our 9.60 console (2026-10-09, probes/webapp_install.c):
- Installed with the svtplay recipe, a web app tile with `http://192.168.1.90:8765/` started
  `/web_app_launcher/eboot.bin` and its web engine (NKUIProcess, NKNetworkProcessMediaApp, NKWebProcessMediaApp),
  then closed itself (`KillApp() ... requested from <own appId>`) and showed "Something went wrong" without
  requesting the page.
- The same recipe with `https://ps5-payload-dev.github.io/svtplay` opened SVT Play. **The web app launcher needs https**
  (or rejects plain-http LAN URLs).
- A running web app is a media app to the system (`AvControl ... (MEDIA RUNNING)`, `cat[0x10200] MediaAppTypes[2]`),
  and goes to `MEDIA SUSPEND` when the user presses PS.
- Installer notes: linking `-lSceAppInstUtil` made the payload hang before `main()` (twice); loading
  `libSceUserService.sprx` and `libSceAppInstUtil.sprx` with `dlopen()` after start works. Order that works:
  `sceUserServiceInitialize(0)`, `kernel_set_ucred_authid(-1, 0x4801000000000013)`, `sceAppInstUtilInitialize()`.
  The `/system_ex` remount returns 0.

**BACKGROUND AUDIO WORKS from our own web app (2026-10-09, hardware-verified):**
- Web app tile (category 66048, svtplay recipe, our own ID `SPKR00001`), page on https (a localhost.run tunnel to the PC).
- `<audio>` is played by the system music core (requests come from `MusicCoreHttpAgent libhttp/9.60`).
  A normal MP3 file with `Content-Length` and `Accept-Ranges` plays (about 3-5 s start delay). A live MP3 stream without a
  length, or with a fake 1 GiB length and no range support, is rejected (`MEDIA_ERR_SRC_NOT_SUPPORTED`, code 4) after
  the music core reads ~1.6 s of it.
- On PS button the page gets `blur` and the system pauses the element (`pause` event). Calling `audio.play()` again
  from the `pause` handler succeeds (`resume-ok`) and **playback continues at the home screen**.
- Starting a game: the game becomes the BigApp, our web app shows as `(MEDIA SUSPEND)` in AvControl and its JavaScript
  stops (no more heartbeats), but **the audio keeps playing during the game**: the music core plays the already-fetched
  file on its own. So during a game the page cannot change tracks; the audio must be one continuous source.
- **A live source works too (verified 2026-10-09, 2+ minutes into a game):** an MP3 "file" with a fixed
  `Content-Length` (1 hour at 128 kbit/s = 57,600,000 bytes) and `Accept-Ranges: bytes`, whose bytes are produced in real
  time (~1 s ahead); a request for bytes not produced yet just waits. The music core opened it with one plain GET
  (no Range header), kept the connection open and played it continuously at home and during a game with the page frozen.
  This is the design for Spotify: one long live MP3 fed by a payload, track after track, silence while paused.
- **Mixed content is allowed for `<audio>`:** the https page played the live file from plain
  `http://192.168.1.90:8765/live.mp3`; the music core fetched it directly from the console. Over the LAN it started
  in ~1 s. In a 5-minute run the music core stayed ~1.7 KB behind the live point (it really plays live).
- **From the console itself works:** a payload (probes/live_server.c, LAME 3.100 built with the payload toolchain into
  `.deps/native/lame`) serving the live MP3 at `http://127.0.0.1:8790/live.mp3` plays in the web app.
  Verified 2026-10-09: foreground, home screen and during a game.
- **Plain-http `<img>` is allowed** in the https page (a PNG from `http://192.168.1.90:8765/card.png` loaded), so the
  payload can show song info as an image it renders.
- **`fetch()` from the https page to `http://127.0.0.1:8790/status` is blocked** (`TypeError: Load failed`, the request
  never reaches the payload): mixed content is allowed for `<audio>` but not for data requests. Song info needs another
  route (untested ideas: an `<img>` rendered by the payload, or an https endpoint).
- **Latency (measured 2026-10-09, page reports `audio.currentTime` vs seconds sent by the server):**
  MP3 128 kbit/s: ~4-5 s; MP3 320 kbit/s: ~2.5-3 s; **WAV 44.1 kHz 16-bit: ~1.9 s** (start ~1.2 s). The music core
  only accepts WAV at 44.1 or 48 kHz 16-bit (88.2/96/192 kHz and 24/32-bit: `MEDIA_ERR_SRC_NOT_SUPPORTED`). It keeps
  ~1.8 s buffered at all times: sending 1.5 s of silence at once and then nothing for 1.0-1.3 s did not lower the
  delay; the player waits (`waiting` events) until it has its buffer again. So ~1.8 s plus Spotify's command time is
  the floor for pause/skip during a game on this route. The speaker payload streams WAV for this reason.
- **No Media Session API** in the web app's WebKit (`navigator.mediaSession` / `MediaMetadata` missing, 2026-10-09),
  so a web app cannot fill the Control Center media card through the web standard.
- Stopping: playback continues after the game is closed; reopening the app does not stop it by itself.
- Music core test tile (`SPKR00003`: same page + `musicCoreName` CustomMusicCore + `musicCoreTitleId` NPXS40201 +
  attribute 0x62000000, displayLocation 188, serviceLaunchButtonKeyCode 2): no sound.
- The system browser (`deeplinkUri` tile) freezes the whole page when it loses focus: no background audio there.

The system web browser (`deeplinkUri` tiles), tested with our page:
- Runs as `NKWebProcess` under NPXS40087 (category shell_ui). UA: `Mozilla/5.0 (PlayStation; PlayStation 5/9.60)
  AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15`.
- No Web Audio (`AudioContext` and `webkitAudioContext` are missing).
- `<audio>` media is fetched by the **system music core**, not WebKit (`UA: MusicCoreHttpAgent libhttp/9.60`), but for
  our URL the element fails at once with `MEDIA_ERR_SRC_NOT_SUPPORTED` (code 4), for WAV, an MP3 file and a live MP3
  stream alike. The log shows `[UrlConfigResolver] failed to get app config. url:[...] ret:[80BA8087]`: the browser
  seems to have per-site configs, and media is not allowed without one.
- On PS button the page gets blur, `visibilitychange` hidden and `pause`.
- When the page loads, ShellUI also fetches it with UA `rnps-action-cards/9.6.0` (probably to build a card).

### 4. The official music-core apps (not reproducible)
Spotify `PPSA05688` sets `musicCoreName` "CustomMusicCore" + `musicCoreTitleId` "NPXS40201". The system music core
runs the web app with background-audio rights. Our own IDs with these fields were refused (see CLAUDE.md).

## BigApp hosting: running a payload ELF as a real app (websrv `hbldr.c`, BFplayer)
Used by every websrv homebrew (RetroArch cores, FFplay, SVTplay native, ...) and BFplayer.
1. Create a fake title `/system_ex/app/FAKE00000/` whose `eboot.bin` is a copy of the PS Now eboot
   (`/system_ex/app/NPXS40106/eboot.bin`) and whose param.json has category 0, `attribute` 1, `attribute3` 4.
2. Watch `SceSysCore.elf` with kqueue (`EVFILT_PROC`, `NOTE_FORK|NOTE_EXEC|NOTE_TRACK`).
3. `sceSystemServiceLaunchApp("FAKE00000", argv, &ctx)` with `ctx.user_id` = foreground user.
4. ptrace-attach the new child, follow exec, set a breakpoint just before `main()`, then replace the process
   image with the homebrew ELF (elfldr code), set argv0, cwd, environ, stdio, heap size.
Result: the homebrew is a real BigApp (VideoOut, AudioOut routed in the foreground, pad input) with payload privileges.
Only one BigApp runs at a time: websrv kills the running one (`sceSystemServiceGetAppIdOfRunningBigApp` +
`sceSystemServiceKillApp`) before launching. So this does **not** help with audio during a game.

## Audio
- Titles and BigApps: `sceAudioOutInit()`, `sceAudioOutOpen(0xFF /*SYSTEM user*/, 0 /*MAIN*/, 0, grain, 48000, 1 /*S16 stereo*/)`.
  Grain must be 256/512/768/1024... (pYTM5). Used by ps5-homebrew-ui, our app, SDL's PS5 backend.
- Payload process: opens, but our probe saw port output state 0x0000 (silent). Real user id fails 0x80260011.
- **No public homebrew plays audio in the background or during a game** (searched 2026-10-09: pYTM5, Jelly5,
  SymphonyStation5, ProsperoRadio, ProsperoPlayer, BFplayer, ytm_ps5, jtplay, Nativehbl). Jelly5's "background playback"
  means within its own app.
- System background music is played by system music cores, not apps: "SystemMusicCore" for USB Music folder files,
  "CustomMusicCore" (NPXS40201) for Spotify/Apple Music. Our USB test (2026-10-09): USB MP3 kept playing at home and in a game.
- Audio exports in the SDK stubs that nobody has used publicly (signatures unknown, **untested**):
  `sceAudioOutAttachToApplicationByPid`, `sceAudioOutDetachFromApplicationByPid`, `sceAudioOutGetFocusEnablePid`,
  `sceAudioOutChangeAppModuleState`, `sceAudioOutSysOpen`, `sceAudioOutExOpen`, `sceAudioOutOpenEx`,
  `sceAudioOutDeviceIdOpen`, and the `sceAudioOut2*` family (contexts, ports, `MbusSetPortConnections`).
  The system log already showed the focus model: `AvControl ... AudioOut: shared (pid=...)`.
- SystemService exports in the SDK stubs: `sceSystemServiceAcquireBgmCpuBudget`, `IsBgmCpuBudgetAvailable`,
  `IsBgmPlaying`, `NotifyBgmCoreTermination`, `ReleaseBgmCpuBudget`, `DisableMusicPlayer`/`ReenableMusicPlayer`,
  `DisableMediaPlay`/`ReenableMediaPlay`, `LaunchWebBrowser`. In a title these resolve to NULL (our crash CE-108255-1).
- pYTM5 README has a full inventory of the music widget / PSM symbols (`sceSystemStateMgrEnterMediaPlaybackMode`,
  `sceShellCoreUtilPostPsmEventToShellUI`, `sceAppInstUtilAppInstallMediaPlayer`, ...). The PSM message format is unknown.

## Video from a payload (pYTM5 notes, from ps5-moonlight)
`sceVideoOutRegisterBuffers` fails 0x80290008 outside a BigApp. The v2 path works from a payload:
`sceSystemServiceHideSplashScreen()` (required), `sceVideoOutOpen(0xFF, 0, 0, NULL)`, direct memory
(`sceKernelAllocateMainDirectMemory`, type 3), `sceVideoOutSetBufferAttribute2`, `sceVideoOutRegisterBuffers2`,
then `sceVideoOutSetFlipRate`. Scan-out is tiled (512x128 tiles), see ps5-payload-dev/SDL.

## Other useful facts
- System browser from a payload: `sceSystemServiceLaunchWebBrowser(url, 0)` after `sceUserServiceInitialize(0)`
  (SDK `samples/browser`).
- Launch any installed title: `sceSystemServiceLaunchApp(title_id, argv, &ctx)` (websrv `sys_launch_title`).
- Find a process by name: `sysctl {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}`, `ki_pid` at offset 72, thread name at 447.
- Notifications: SDK `samples/notify`. Kernel log line from anything: syscall 0x259 (`klog`), SDK `samples/install_app/eboot.c`.
- Error codes: etaHEN `Source Code/util/include/error_translator.hpp` maps ~14k SCE error codes to names, e.g.
  `0x8138xxxx` = SCE_CUSTOM_MUSIC_CORE_ERROR_*, `0x80FDxxxx` = SCE_MUSICCORE_SERVER_ERROR_*.
- Standard payloads: elfldr 9021, websrv 8080, shsrv 2323 (shell), ftpsrv 2121, klogsrv 3232, gdbsrv.
- Libraries for payloads: ps5-payload-dev/pacbrew-repo (SDL2, ffmpeg, freetype, zlib, sqlite, ...),
  ps5-payload-dev/SDL (PS5 SDL2 port; audio via sceAudioOut, video via VideoOut v2).
- RetroArch port (mihawk-99): native title PPSA99169 (category 0, `attribute3` 0x80040, `kernel.flexibleMemorySize` 1 GiB,
  `gameIntent` launchActivity) plus a resident payload daemon for its WebUI.
- etaHEN (LightningMods) and onionHEN: daemons that inject into system processes (`libhijacker`, ShellUI toolbox, game overlay).
  We do not go that route.

## Sources
- https://github.com/ps5-payload-dev/websrv (`src/ps5/sys.c`, `src/ps5/hbldr.c`, `param.json`)
- https://github.com/ps5-payload-dev/sdk (`samples/install_app`, `samples/browser`, `sce_stubs/`)
- https://github.com/ps5-payload-dev/svtplay, /launchpad, /plutotv, /tv4play (webAppUri tiles + installer)
- https://github.com/ps5-payload-dev/ftpsrv (`assets/param.json`), /jtplay, /SDL, /pacbrew-repo
- https://github.com/ItsBlurf/BFplayer (`docs/STANDALONE_LAUNCHER.md`, `docs/ARCHITECTURE.md`, `assets/tile/param.json`)
- https://github.com/KINGDKAK/ProsperoPlayer (`prospero_media_standalone/assets/param.json`)
- https://github.com/pLabs5/pYTM5 (README: audio, VideoOut v2, PSM inventory)
- https://github.com/blackbearreloaded/ps5-homebrew-ui (`src/platform/ps5/audio_out.cpp`), /ProsperoRadio
- https://github.com/drakmor/ShadowMountPlus (README: scan paths, sandbox mounts)
- https://github.com/mihawk-99/PS5_RetroArch, https://github.com/cancanoo1103/Jelly5_CEC-Controls
- https://github.com/LightningMods/etaHEN, https://github.com/aydencharles/onionHEN
- https://github.com/TsvetomirGT/SymphonyStation5, https://github.com/allnewryan1/ytm_ps5

## Control Center card: the msdk bridge and music cores (tested 2026-10-09)
Probe pages on GitHub Pages reported back to the PC with plain-http `<img>` requests.
- Spotify's PS5 page (https://api-partner.spotify.com/tvapp?platform=ps5-cmc, JS on tv.scdn.co/ps5/v2/) does not play
  audio or send song info itself. It uses Sony's injected `window.msdk` (event `loadedmsdk`):
  `msdk.music.addSource("dummy","user","xxx",cb)`, `source.load()`, `msdk.music.registerNotifyMessage("web,play",cb)`, and
  sends play/pause as `source.sendMessageSync(playControl, msg)` to a native "container" (Spotify's player). The
  Control Center card is filled from the music core side, not by the page.
- `msdk` is a JS wrapper over PSM (`Sce.PlayStation.Core.PsmException`). `addSource(srcUri, userName, oauth, cb)` sends
  `Music.addSource {srcUri,userName,oauth}`. A source has play/pause/stop/skipNext/skipPrevious/get/setVolume/
  getCapabilities/`getMusicInfo` (`srcUri, musicName, albumName, artistName, coverArtUri, contextUri`, read-only)/
  sendMessageSync/Async. There is no call that sets song info.
- A plain web app tile (0x10200) gets `msdk` with appData, commerce, device, psn, saveData, sound, system: **no `music`**.
- `musicCoreName` "CustomMusicCore" + `musicCoreTitleId` "NPXS40201" (our own title ID): `msdk.music` appears; init OK,
  addSource OK, but every source (`dummy`, an https WAV, `http://127.0.0.1:8795/live.wav`) fails at load/play with
  `ERROR_ABORTED:DETAIL_INTERNAL`, before fetching anything; `registerNotifyMessage` fails 0x81790FFF. The container
  most likely ships in Spotify's licensed package (/user/app/PPSA05688/app.pkg, 40 MB, encrypted): a dead end.
- `musicCoreName` "MseMusicCore" (with musicCoreTitleId NPXS40039 or none): `msdk.music` appears, `addSource` is refused
  0x81797008 (also for a MediaSource blob: URL). `<audio>` plays, also fed through Media Source Extensions
  (`audio/mp4; codecs="mp4a.40.2"` and WebM Opus supported; MP3/WAV/AAC not), but **no card** appears.
- MediaCoreServer NPXS40039 (/system/vsh/app) holds MseMusicCore.elf, SystemMusicCore.elf, becore.elf. ShellUI's PSM
  assemblies (/system_ex/app/NPXS40087/psm/Application/*.dll.sprx) are encrypted; not analysed.
Result: no Control Center card for our speaker by any web-side route tested.
- The official package's plaintext metadata (read 2026-10-09 from /user/app/PPSA05688/app.pkg, "\x7FFIH" image; only the
  sce_sys entry area at ~0x470000 is plaintext, the rest is encrypted) holds param.json, origin-param.json and
  target-param.json: `applicationDrmType` "free", contentId EP4950-PPSA05688_00-SPOTIFYMUSIC0000, no eboot.
  `webAppUri` carries `containerVersion=<contentVersion>`, so the native "container" the page drives is the package's
  own (encrypted) content. Versions: 01.000.000 required FW 5.00, 01.011.000 required 9.00, the installed 01.012.000
  requires 10.20. Without a licence for this console the content cannot be mounted, so the container is out of reach.
