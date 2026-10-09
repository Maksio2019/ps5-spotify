# CLAUDE.md - PS5 Spotify Connect speaker (handoff notes)

## SHELVED 2026-10-09 evening: read this first when resuming
- The user paused the project. Everything was removed from the PS5 (verified over FTP, nothing left; details in
  docs/claude-memory/ps5-footprint.md). The tile, the saved Spotify login and the logs are gone.
- All work is on GitHub: `main` branch of https://github.com/Maksio2019/ps5-spotify (a PUBLIC repo). GitHub Pages
  serves `main`'s root, so the tile's page is `index.html` at the repo root (`.nojekyll` keeps Pages from running
  Jekyll over the repo). Clone it with `--recurse-submodules` (cspot).
- docs/claude-memory/ holds Claude's memory notes (PS5 access rules, footprint list, GitHub access). Copy them back
  into Claude's memory folder (~/.claude/projects/<project>/memory/) when resuming on a new machine.
- Not in git: .deps/ (`make deps`), build/ (rebuild), the Python venv in ~/pyenv (see "What exists"), ~/cspot.
- README.md (user-facing, says everything: install, run, uninstall, build, how it works, endpoints, known issues,
  release notes) was written at shelving time. The built payloads are attached to the GitHub release `v1.0.0`
  (with SHA256SUMS).
- To get it running again: jailbreak, send webapp_install.elf once (it now defaults to the GitHub Pages address;
  /data/spkr_url.txt only overrides it), then spotify_speaker.elf (port 9021). Pick "PS5 Speaker" on the phone again
  (the saved login was deleted).
- Open bug reported by the user: connecting from a different account/device hangs on "connecting"; only one account
  works. Analysis and fix idea in README.md, "Known issues" 1.
- Unfinished at shelving time, built but NEVER SEEN ON THE CONSOLE:
  - Song toast: payload sends a sceNotificationSend toast (InteractiveToastTemplateB, icon = the cover JPEG downloaded
    to /data/spkr/covers/) ~1.5 s after a new song enters the stream. The cover was downloaded on the console once
    (so the code path ran); whether the toast showed, with or without the cover, is unknown.
  - "Now playing" page (index.html, live on GitHub Pages): song info comes from /meta.png (JSON bytes as grey
    pixels, read through a canvas; fallback: one image per byte, width = byte + 1). Both routes verified in headless
    Chromium against the real payload over the LAN, not yet in the PS5 web engine.
- The test server (build/webapp/server.py) is now tracked as webapp/test_server.py; it serves ./www next to itself.

## Goal
A Spotify Connect speaker for a jailbroken PS5, Premium account only (no piracy).
The PS5 shows up as a speaker in the phone's Spotify app; audio plays on the console.
- MUST HAVE: playback continues while a game is running (background play).
- NICE TO HAVE: PS5 Control Center / Quick Menu "Music" card integration.
- Never write decoded Spotify audio to any file or disk image. Streaming only, in memory.

## User and environment
- Windows 11 (build 26100), WSL 3.0.1 app (WSL2 mechanism), Ubuntu 26.04.1, 24 cores, 15 GB RAM.
- WSL mirrored networking enabled (~/.wslconfig on Windows) and Hyper-V firewall inbound set to Allow
  (Set-NetFirewallHyperVVMSetting -Name '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -DefaultInboundAction Allow).
  Without these the phone cannot discover the speaker running in WSL.
- PC LAN IP 192.168.1.90. PS5 IP 192.168.1.120, firmware 9.60.
- PS5 side: jailbroken, ShadowMountPlus + ftpsrv (port 2121) + klogsrv (3232) + nanodns + an ELF loader on port 9021.
  Claude may push payloads straight to 192.168.1.120:9021 (raw TCP send of the ELF); the user can also use Payload Manager.
  After a PS5 reboot, payloads are gone: the user re-jailbreaks, then spotify_speaker.elf must be sent again.
- Claude may deploy and read the app log over FTP without asking; every path written on the PS5 is tracked
  in Claude's memory (ps5-footprint) so the user can clean up if the project is ever dropped.
- The user wants shell commands given ONE AT A TIME, waiting for output, except trivial short ones.
- Keep projects in the Linux filesystem (~), not /mnt/c.

## What exists
- ~/spotify-ps5: clone of blackbearreloaded/ps5-native-app-boilerplate (GPL-3.0-or-later).
  Initialised with: make init TITLE_ID=PPSA99777 APP_NAME="Spotify" APP_CATEGORY=media
  `make` works, `make deploy PS5_HOST=<ps5 ip>` works, tile appears in Media and shows Hello World.
  Gotcha: needed `sudo apt install llvm-18` (provides llvm-config-18). The README prerequisites omit it.
- ~/cspot: recursive clone of github.com/feelfreelinux/cspot (Spotify Connect player in C++, last commit 2024).
  CLI built at ~/cspot/targets/cli/build/cspotcli (cmake .. -DUSE_ALSA=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5).
  Needs PATH=$HOME/pyenv/bin:$PATH (uv-made Python 3.12 venv with protobuf>=4.25,<5, grpcio-tools 1.62.3,
  setuptools<81) because Python 3.14 is too new for the nanopb generator.
  ~/.asoundrc routes ALSA to PulseAudio (WSLg). VERIFIED: it appears as "CSpot player" in the phone app,
  Spotify hands over credentials (Zeroconf), and Premium audio streamed and played on the PC.
  One earlier run died with "Can't connect to spotify servers" (PlainConnection::connect throws on a failed
  connect; ApResolve only uses the first access point). Ports 4070/443/80 were reachable, so it looked transient.
- Reference apps worth reading: TsvetomirGT/SymphonyStation5 (streaming music client: libcurl, decoder thread,
  ring buffer to mixer, controller UI; same build template), blackbearreloaded/ProsperoRadio,
  blackbearreloaded/ps5-homebrew-ui, pLabs5/pYTM5 (research notes on the Control Center media API).
- Catalog of existing PS5 homebrew: https://homebrew.page (title IDs 99000-99011 etc. are taken; ours is PPSA99777).

## Status (2026-10-09 evening): WORKING, background play included
Spotify plays on the PS5 at the home screen and DURING GAMES, controlled from the phone (pause/skip ~1.5 s in the
app or at home, ~2 s in games). Volume from the phone works. The Control Center card is NOT done (see Decided).

## How the current version works
- `spotify_speaker.elf` (payload/spotify_daemon.cpp, `make -C payload`; needs tools/build-cspot-payload.sh first):
  cspot + Zeroconf (port 8796) + in-process mDNS + a live WAV stream at http://127.0.0.1:8795/live.wav
  (44.1 kHz 16-bit, fixed 6 h length, produced in real time, silence while paused, only the newest listener is served),
  /state.png (width = count of user actions: pause/play/skip/seek), /status (JSON), /quit (a new copy sends it to
  replace the old one). Log + saved login in /data/spkr/. Reports the real volume to Spotify (cspot's default is 0).
- Web app tile "PS5 Speaker" (SPKR00001, category 0x10200, webAppUri = an https page) installed by
  `webapp_install.elf` (probes/webapp_install.c; reads the page address from /data/spkr_url.txt; checks it before
  uninstalling anything). Built by `make -C payload` too (also webapp_uninstall.elf).
- The page (index.html at the repo root; hosted on GitHub Pages at https://maksio2019.github.io/ps5-spotify/,
  the user's repo maksio2019/ps5-spotify, uploaded by the user through the web UI) plays the WAV through the system
  music core, calls play() again when the system pauses it (that is what keeps it playing in the background and in
  games), polls /state.png 4x/s and reconnects on user actions to drop the music core's ~1.8 s buffer. During a game
  the page's JS is frozen, but the music core keeps playing the stream. No PC is needed at runtime.
- Changing the page: edit index.html and push `main` (commits as Maksio2019
  <157291633+Maksio2019@users.noreply.github.com>); Claude may do this itself. ~/ps5-spotify-page is the old
  page-only clone of the repo, superseded since the project moved to `main`. `gh` is
  logged in with a fine-grained token limited to that one repo (Contents + Pages read/write). GitHub Pages caches ~10 min.
  The tile address only changes if /data/spkr_url.txt changes and webapp_install.elf is pushed again.
- For debugging, webapp/test_server.py (port 8765, needs a venv with `lameenc`) serves a copy of the page from
  webapp/www/ (create it) and logs the page's POST /log events (logging is off on *.github.io). Reaching it from the PS5
  needs https: a localhost.run tunnel (ssh -R 80:127.0.0.1:8765 nokey@localhost.run), whose address changes often;
  Cloudflare quick tunnels do not work (port 7844 blocked).
- Every measured fact is in howitworks.md ("BACKGROUND AUDIO WORKS" and the latency section).

## Decided / do not retry
- No DRM workarounds: the official Spotify app (PPSA05688) needs a licence on 9.60; not backporting/decrypting it.
- Control Center card: the PS5 web engine has no Media Session API; integrating with Sony's system media player
  (SystemMusicCore/MseMusicCore, fake USB drives, system services) was stopped by the safety classifier several times.
  Do not go there again. open.spotify.com in a web app tile shows "Something went wrong".
  Retried on the user's request 2026-10-09 via Sony's web bridge `window.msdk.music` (CustomMusicCore and MseMusicCore
  test tiles): no card; the sources fail or are refused. Details in howitworks.md ("Control Center card").
- MP3 (LAME) was replaced by WAV: lower delay. Higher WAV rates (88.2/96/192 kHz, 24/32-bit) are rejected by the
  music core. Sending prefill silence does not lower its ~1.8 s buffer.

## Next steps (user's choice)
1. DONE 2026-10-09: page on GitHub Pages, tile SPKR00001 points at it.
2. "Now playing" screen in the page: song/artist/cover rendered by the payload as images (plain-http <img> works,
   fetch()/XHR to 127.0.0.1 is blocked as mixed content).
3. Commit the work (git status shows many untracked files); the test server lives in build/webapp, which is not tracked.

## Old title app (PPSA99777, superseded and deleted from the console)
How it was built:
- cspot is a git submodule at third_party/cspot (pinned 1b07a9c, GPL-3.0-or-later, same as the template).
  tools/build-cspot.sh builds mbedTLS 3.6.5 (sha256-pinned download) and cspot+bell as static archives with
  tooling/cmake/ps5-title.cmake; `make` runs it first. Needs the Python venv in ~/pyenv (PYTHON_VENV) for nanopb.
  Never pass CMAKE_C_FLAGS/CMAKE_CXX_FLAGS to those cmake calls: they replace the toolchain's PS5 flags
  (this once built mbedTLS against the PC's glibc headers).
- C++ exceptions WORK in a title (hardware-verified): tools/build.sh has APP_CXX_EXCEPTIONS=1 (Makefile default)
  and APP_CXX_RUNTIME, which link the SDK's libc++/libc++abi/libunwind + clang builtins; ps5-pie.ld provides
  __eh_frame_* bounds. So cspot is used unmodified.
- src/spotify_engine.cpp: Zeroconf (civetweb on a system-assigned port), session loop, saved credentials in
  /download0/credentials.json. src/audio_sink.cpp: ring buffer, 44.1->48 kHz linear resampler, volume, AudioOut.
- src/runtime/: shims for what the title's libc lacks or binds to libScePosixForWebKit (a NULL import in a
  title): getaddrinfo/gethostbyname/getnameinfo (sceNetResolver), getifaddrs, C-locale *_l functions for
  libc++, getpwnam/setgid/__xuname, __assert, mbedTLS entropy (/dev/urandom, sceRandom fallback),
  and a --wrap=pthread_create that gives attribute-less threads a 512 KiB stack.
  After any new dependency, check that nothing binds to libScePosixForWebKit (llvm-nm against the SDK stubs).
- App log: /download0/spotify.log, read over FTP while the app runs at
  /mnt/sandbox/PPSA99777_000/download0/spotify.log.


Hardware findings:
- The title sandbox (uid 1) refuses bind() to any fixed port (EACCES, also via sceNetBind); port 0 works.
  So mDNS (UDP 5353) cannot run in the app. payload/mdns_helper.c (`make payload`,
  build/payload/spotify_mdns_helper.elf) runs as a payload, reads the app's /download0/zeroconf.port through
  the sandbox mount and announces the speaker. The user sends it with Payload Manager.
- Audio from a payload process opens fine but is routed nowhere (port state output 0x0000, silent);
  from the title it routes (0x0081) and is audible. sceAudioOutOpen with the real user id fails 0x80260011 in
  a payload. Payload probe: probes/audio_probe.c.
- ShadowMountPlus 1.7beta4 mounts /data and /mnt into the sandbox (files only, no network rights).
- "PS5-967" in the phone's device list is Sony's own Spotify integration, unrelated to this app.
- Saved credentials work: after a restart the app logs in alone and appears in the phone's list under
  "On other networks" (via Spotify's cloud), no helper payload needed. The helper is only for pairing.
- Background: the app keeps running when not in the foreground (heartbeat continues, stream continues), but
  the system unroutes its audio port (output 0x0081 -> 0x0000), MAIN and BGM port alike. The libSceSystemService
  BGM/media calls (sceSystemServiceAcquireBgmCpuBudget, sceSystemStateMgrEnterMediaPlaybackMode, ...) resolve to
  NULL in a title (system-only exports); calling one crashed the app (CE-108255-1). sceKernelLoadStartModule on
  /system/common/lib/<name>.sprx returned 0x80020002, so a dlsym route was never actually tested.
- The official PS5 Spotify app (PPSA05688) has NO executable: its param.json sets musicCoreName
  "CustomMusicCore", musicCoreTitleId "NPXS40201", webAppUri https://api-partner.spotify.com/tvapp?platform=ps5-cmc,
  applicationCategoryType 66048, attribute 0x62000000, requiredSystemSoftwareVersion 10.20. The system music
  core runs Spotify's web app with background-audio rights. Test titles copying these fields (own title IDs)
  failed: a custom http page was never even fetched (CE-117690-4); Spotify's URL showed the Spotify splash, then
  "Install Spotify to listen" (CE-105773-3). The music core looks tied to the official title ID.
- applicationCategoryType 0x10200 (66048) = "web_based_media": the system does NOT run the title's eboot but its own
  /web_app_launcher/eboot.bin, which loads webAppUri; without one the launch fails ("Something went wrong").
  So a native app must stay 0x10000. The installed official Spotify package fails to mount on 9.60
  (PFS EICV, ekey/skey 0xffffffff: no licence keys); working around that would bypass DRM, so it is off the table.
- klogsrv (port 3232) sends recent history on connect, so a failed launch can be read right after it happens.
- App data lives in /user/download/PPSA99777/download0.dat (disk image). The FTP server ignores the path in MLSD:
  cwd into a folder before listing it.


## Reference
howitworks.md: how public PS5 homebrew works (tile types, deeplinkUri vs webAppUri, web app installer,
BigApp hosting, payload audio/video, error-code table) plus all hardware results of this project.
Read it before guessing at system behaviour.
