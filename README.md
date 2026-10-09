# PS5 Speaker: Spotify Connect for a jailbroken PS5

Turns a jailbroken PS5 into a Spotify Connect speaker. Pick **PS5 Speaker** in the Spotify app on your
phone and the music plays on the console: on the home screen **and while you play games**.

**Version 1.0.0** · [Download](https://github.com/Maksio2019/ps5-spotify/releases/tag/v1.0.0) · tested on firmware 9.60 ·
GPL-3.0-or-later · development is paused (October 2026)

Not affiliated with Spotify or Sony. Needs **Spotify Premium** (Spotify Connect playback requires it).
It is an independent Spotify Connect client built on [cspot](https://github.com/feelfreelinux/cspot); no DRM
is bypassed and the official PS5 Spotify app is not touched. Audio is only streamed in memory, never written to disk.

## Contents

- [Features](#features)
- [Release 1.0.0](#release-100)
- [Requirements](#requirements)
- [Install and run](#install-and-run)
- [Uninstall](#uninstall)
- [Files and ports on the console](#files-and-ports-on-the-console)
- [Troubleshooting](#troubleshooting)
- [Known issues](#known-issues)
- [How it works](#how-it-works)
- [HTTP endpoints of the payload](#http-endpoints-of-the-payload)
- [What was tried and does not work](#what-was-tried-and-does-not-work)
- [Build from source](#build-from-source)
- [Repository layout](#repository-layout)
- [Project history](#project-history)
- [Credits and license](#credits-and-license)

## Features

- Shows up as **PS5 Speaker** in Spotify's device list (Spotify Connect pairing on your local network).
- **Keeps playing during games** and on the home screen.
- Play, pause, skip, seek and volume from the phone (about 1.5 s delay at the home screen, about 2 s in a game).
- Remembers the login: after the first pairing it signs in by itself and also appears under "On other networks".
- **PS5 Speaker** tile on the home screen with a now-playing screen: cover, song, artist, album, progress bar,
  playing/paused state, a colour taken from the cover, and clear screens for "ready", "signing in" and
  "speaker not running".
- PS5 notification with the cover, song and artist when a new song starts (only the last one when you skip quickly).
- Audio quality: Spotify's 320 kbit/s Ogg Vorbis, played as 44.1 kHz 16-bit stereo.

## Release 1.0.0

Download: [v1.0.0 on the Releases page](https://github.com/Maksio2019/ps5-spotify/releases/tag/v1.0.0)

| File | What it does | When to send it |
| --- | --- | --- |
| `webapp_install.elf` | Installs the **PS5 Speaker** tile | Once |
| `spotify_speaker.elf` | The speaker itself (Spotify client + audio stream) | After every jailbreak |
| `webapp_uninstall.elf` | Removes the tile | To uninstall |
| `SHA256SUMS` | Checksums of the three files | Check with `sha256sum -c SHA256SUMS` |

Send the payloads to your ELF loader (port 9021), open the PS5 Speaker tile, then choose **PS5 Speaker** in the
Spotify app on your phone. Music keeps playing during games. Spotify Premium required.

Known issues in this release (details in [Known issues](#known-issues)):

- Only one Spotify account works: when a phone with a different account picks the speaker, it hangs on
  "connecting". Workaround: delete `/data/spkr/credentials.json` over FTP, send `spotify_speaker.elf` again and
  pair again.
- Pause and skip take about 1.5–2 s. There is no Control Center card. The payload must be sent again after every
  restart.
- The song notification and the now-playing screen have not been tested on a console yet.

The files are built from this repository at the commit the `v1.0.0` tag points to.

## Requirements

- A jailbroken PS5 (tested on firmware **9.60**) with an ELF loader listening on port **9021** (elfldr; Payload
  Manager or any payload sender works too). An FTP server payload (ftpsrv) helps for logs and uninstalling.
- Spotify **Premium**.
- The phone and the PS5 on the same network for the first pairing.
- Internet on the console: the tile loads its page from `maksio2019.github.io`, and Spotify streams from
  `*.spotify.com` / `*.scdn.co`. If you use a DNS blocker payload, don't block those.

## Install and run

Download the payloads from the [release](https://github.com/Maksio2019/ps5-spotify/releases/tag/v1.0.0).

Send a payload with your payload sender, or from a PC (replace `192.168.1.120` with your PS5's IP):

```bash
nc -q1 192.168.1.120 9021 < spotify_speaker.elf
```

**Once:**

1. Send `webapp_install.elf`. A **PS5 Speaker** tile appears (in your library / Media). The tile stays after
   reboots and opens without a jailbreak, but it only plays when the speaker payload runs.

**After every jailbreak** (payloads are gone after a PS5 restart):

2. Send `spotify_speaker.elf`. A notification says "Spotify speaker running".
3. Open the **PS5 Speaker** tile. You can go back to the home screen or start a game afterwards; it keeps running.
4. On your phone, open Spotify, tap the devices icon and choose **PS5 Speaker**. The first time, the phone hands
   the login over to the console; after that the speaker signs in by itself.
5. Play. Control everything from the phone.

Sending a new `spotify_speaker.elf` replaces the one that is running. To stop it without a reboot, open
`http://<ps5-ip>:8795/quit` in a browser.

## Uninstall

1. Send `webapp_uninstall.elf`. It removes the tile and its files.
2. Over FTP, delete what is left:
   - `/data/spkr/` (log, saved Spotify login, covers)
   - `/data/webapp_probe.log` (installer log)
   - `/data/spkr_url.txt` (only if you created it)
   - `/user/download/SPKR00001/` and `/system_ex/app/SPKR00001/`, if they still exist
3. Restart the PS5 to stop the payload.

## Files and ports on the console

| What | Where |
| --- | --- |
| Tile (installed by `webapp_install.elf`) | `/system_ex/app/SPKR00001/`, `/user/app/SPKR00001/`; the system adds `/user/appmeta/SPKR00001/` and `/user/download/SPKR00001/` (about 320 MB of web app storage) |
| Log | `/data/spkr/spotify.log` (previous run: `spotify.prev.log`) |
| Saved login (a reusable Spotify token, not your password) | `/data/spkr/credentials.json` |
| Covers for the notification (last 8, cleared at start) | `/data/spkr/covers/` |
| Installer log | `/data/webapp_probe.log` |
| Optional other page address for the tile (one https URL) | `/data/spkr_url.txt` |
| Ports | 8795 TCP (stream and the endpoints below), 8796 TCP (Spotify Connect pairing), 5353 UDP (mDNS) |

The tile's title ID is `SPKR00001`.

## Troubleshooting

- **The tile says "The speaker isn't running":** send `spotify_speaker.elf` again (needed after every restart).
- **PS5 Speaker is not in the device list:** check that the phone is on the same network, close and reopen the
  Spotify app. After the first login it is also listed under "On other networks".
- **It hangs on "connecting" on a phone:** see the first known issue below.
- **The tile shows "Something went wrong":** the console could not load the page; check its internet connection and
  that `maksio2019.github.io` is not blocked.
- **Log:** read `/data/spkr/spotify.log` over FTP (port 2121 with ftpsrv). `http://<ps5-ip>:8795/status` shows the
  current state. The system log (klogsrv, port 3232) shows launch errors.
- **"Can't connect to spotify servers" in the log:** usually temporary; send the payload again.

## Known issues

1. **Only one Spotify account works, and connecting from a different account breaks it.** The speaker stays
   signed in to the first account that paired: the login is saved in `/data/spkr/credentials.json` and reused at
   every start. When a phone with **another account** picks PS5 Speaker, it hangs on "connecting" and the speaker
   keeps playing for the first account.
   - Likely cause, from reading the code (`payload/spotify_daemon.cpp`, not confirmed on the console): the pairing
     endpoint (`Zeroconf::handlePost`) accepts the new login, but the running Spotify session is never stopped,
     because `runSession()` loops until the payload quits. The new login data is also written into the same
     `LoginBlob` object the running session uses, so later reconnects can fail too.
   - Workaround: delete `/data/spkr/credentials.json` over FTP, send `spotify_speaker.elf` again, then pair from
     the account you want to use.
   - Fix idea: when a new login arrives while a session runs, end that session (disconnect, clear the audio
     buffer), give the new login its own `LoginBlob`, start a new session with it and save its credentials.
2. **Pause, skip and seek take about 1.5–2 s.** The system music player always keeps about 1.8 s of audio buffered,
   and that can't be changed from a web page (see [Latency](#latency)).
3. **No PS5 Control Center / Quick Menu music card.** There is no API for it that a web app or payload can use (see
   [What was tried](#what-was-tried-and-does-not-work)).
4. **The payload has to be sent again after every PS5 restart.**
5. **The song notification and the now-playing screen are untested on a console.** They were finished right before
   development paused; they were only checked in a desktop browser against the real payload. The notification uses
   an undocumented system toast format (copied from the payload SDK's sample) and may show without the cover or
   not at all.
6. **Closing the game or the tile does not stop the music.** Pause from the phone.
7. **The stream is one 6-hour WAV "file".** What happens after 6 hours of non-stop playback (especially during a
   game, when the page is frozen) has not been tested.
8. **Only tested on firmware 9.60**, with one console and one account.

## How it works

```
phone (Spotify app) ──Spotify Connect──▶ spotify_speaker.elf (payload on the PS5, cspot)
                                              │ live WAV stream, http://127.0.0.1:8795/live.wav
                                              ▼
                         "PS5 Speaker" tile: a web app page with an <audio> element
                                              │
                                              ▼
                       PS5 system music player: keeps playing at home and in games
```

### Why a payload plus a web app tile

- A **payload** (an ELF sent to the loader) has full rights and keeps running during games, but audio it opens is
  routed nowhere: the console stays silent.
- A **normal app** (a native title) plays sound only while it is in front: once you go to the home screen or start
  a game, the system mutes it.
- A **web app tile** (category `0x10200`, a `webAppUri` page) is different: its `<audio>` element is played by the
  PS5's **system music player** (the same one that plays music from a USB stick), and that keeps playing at the home
  screen and during games. The page only has to call `play()` again when the system pauses it as the tile loses
  focus. During a game the page's JavaScript is frozen, but the music player keeps playing the stream on its own.

So the payload runs the Spotify client and serves the music as **one endless WAV "file"** on `127.0.0.1:8795`, and
the tile's page plays it. The page must be on **https** (the web app launcher rejects plain-http pages), so it is
hosted on GitHub Pages from this repository's `main` branch: https://maksio2019.github.io/ps5-spotify/. It may
load audio and images from the console over plain http, but not data (`fetch()` is blocked as mixed content).

### The live WAV stream

- The music player only plays something that looks like a normal file: a fixed `Content-Length` and
  `Accept-Ranges: bytes`. The payload announces a 6-hour WAV (44.1 kHz, 16-bit stereo) and produces the bytes in real
  time; a read of bytes that don't exist yet just waits. Silence is sent while paused, so the "file" never stops.
- WAV instead of MP3 because the player buffers a fixed amount of data before it plays: about 1.8 s of WAV, but
  2.5–5 s of MP3. The player only accepts 44.1 or 48 kHz 16-bit WAV.
- Only the newest listener is served: when the page reconnects, the old connection is closed at once.

### Fast pause and skip

The player's ~1.8 s buffer would delay every pause or skip. The payload counts user actions (pause, play, skip,
seek); the page polls `/state.png` 4 times a second, an image whose **width** is that count, and when it changes
the page reconnects to the stream, which throws the old buffer away. Images because the https page may load
images from the console but not data.

### Song info for the now-playing screen

`/meta.png` is a one-row grey PNG whose pixels are the bytes of a JSON text (song, artist, album, cover URL,
duration, position, paused). The page reads the pixels through a canvas (allowed thanks to
`Access-Control-Allow-Origin: *`). If a browser refuses to read pixels, the page falls back to one image per byte
(`/meta.png?stable&b=<k>`, width = byte + 1) plus `/ver.png` and `/pos.png`. Covers load directly from Spotify's
image server (`i.scdn.co`, https).

### The song notification

When the first audio of a new song enters the stream, the payload waits ~1.5 s (until it is audible), downloads the
cover to `/data/spkr/covers/` and sends a system toast with `sceNotificationSend` (`InteractiveToastTemplateB`,
the format of the payload SDK's notify sample). If that function is missing it falls back to a plain text
notification.

### Pairing and login

The payload announces `_spotify-connect._tcp` over mDNS (UDP 5353) and runs Spotify's Zeroconf HTTP endpoint on
port 8796. The phone posts its encrypted login there; the payload signs in with cspot and saves the resulting
reusable token in `/data/spkr/credentials.json`, so later starts sign in without the phone. The volume is reported
to Spotify correctly (cspot's default would report 0).

### Latency

Measured with the page reporting `audio.currentTime` against the seconds the payload sent:

| Stream format | Delay |
| --- | --- |
| MP3 128 kbit/s | ~4–5 s |
| MP3 320 kbit/s | ~2.5–3 s |
| **WAV 44.1 kHz 16-bit (used)** | **~1.9 s** (start ~1.2 s) |

Sending silence ahead of time does not shrink the player's buffer; it always waits until it has ~1.8 s again.
With the reconnect trick, pause/skip lands in about 1.5 s at the home screen and about 2 s in a game.

All measurements and system details: [howitworks.md](howitworks.md).

## HTTP endpoints of the payload

All on port 8795 (`http://127.0.0.1:8795` from the console, `http://<ps5-ip>:8795` from the LAN):

| Endpoint | What |
| --- | --- |
| `/live.wav` | The live stream (6-hour WAV, real time). `?backlog=<ms>` sets how far behind live a new listener starts (default 250) |
| `/status` | JSON: state, user, track, artist, album, cover, duration, position, paused, listeners, underruns, version |
| `/state.png` | Image whose width is 1 + the count of user actions |
| `/meta.png` | The status JSON as grey pixels; `?stable` without the changing fields; `&b=<k>` one byte as width |
| `/ver.png`, `/pos.png` | Width = 1 + status version, 1 + position in seconds |
| `/quit` | Stops the payload (a newly sent copy uses this to replace the old one) |

Port 8796: `/spotify_info` (Spotify Connect pairing, GET info / POST login).

## What was tried and does not work

- **The official PS5 Spotify app** needs a licence the console doesn't have on 9.60 (its package can't be mounted).
  Getting around that would mean bypassing DRM, so it was not done.
- **Sony's system music player integration** (the Control Center card): the official Spotify app is a web app run
  by a special "CustomMusicCore". Copying its settings into our own tile gives access to Sony's `window.msdk.music`
  bridge, but every source fails to load, and the "MseMusicCore" variant plays audio without showing a card. The
  web engine has no Media Session API. Result: no card by any route tested.
- **open.spotify.com in a tile** shows "Something went wrong".
- **Playing audio from a payload**: the audio port opens but is routed nowhere (silent).
- **A native app (title)**: works and plays Spotify, but only while it is in the foreground; the system-only
  background-music calls crash a normal app.
- **Higher WAV rates** (88.2/96/192 kHz, 24/32-bit) are rejected by the music player.
- **The system web browser** (a `deeplinkUri` tile) freezes the page when it loses focus: no background audio.

Details and error codes: [howitworks.md](howitworks.md).

## Build from source

Linux (Ubuntu; WSL2 works). The build uses the
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) tooling and the
PS5 payload SDK.

```bash
sudo apt install curl git make cmake ninja-build ccache pkg-config python3 python3-pip python3-venv tar wget unzip \
  clang-18 lld-18 llvm-18
git clone --recurse-submodules https://github.com/Maksio2019/ps5-spotify.git
cd ps5-spotify
make deps
```

cspot generates protobuf code with nanopb, which needs **Python 3.12** with an older protobuf (newer Pythons fail).
With [uv](https://docs.astral.sh/uv/):

```bash
uv venv --python 3.12 ~/pyenv
uv pip install --python ~/pyenv/bin/python "protobuf>=4.25,<5" grpcio-tools==1.62.3 "setuptools<81"
```

Then build cspot for the payload toolchain and the payloads (set `PYTHON_VENV` if the venv is not in `~/pyenv`):

```bash
bash tools/build-cspot-payload.sh
make -C payload
```

The payloads land in `build/payload/`: `spotify_speaker.elf`, `webapp_install.elf`, `webapp_uninstall.elf`
(and `spotify_mdns_helper.elf`, only used by the old native app version).

Gotchas: `llvm-18` is needed (for `llvm-config-18`) although the boilerplate's own README doesn't list it. Never
pass `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS` to the cspot/mbedTLS cmake calls; they replace the PS5 toolchain flags.

**The page** is [index.html](index.html) at the repository root, because GitHub Pages serves the root of `main`
(`.nojekyll` makes Pages serve the files as they are). Pushing a change to `main` updates the tile (GitHub Pages
caches for about 10 minutes). To use your own copy, host it on https, write its address into `/data/spkr_url.txt`
on the console and send `webapp_install.elf` again. `webapp/test_server.py` is a PC test server for the page.

## Repository layout

| Path | What |
| --- | --- |
| `index.html` | The tile's page (plays the stream, now-playing screen), served by GitHub Pages |
| `payload/spotify_daemon.cpp` | The speaker payload: cspot session, pairing, mDNS, live WAV server, song info, notification |
| `payload/Makefile` | Builds all payloads |
| `probes/webapp_install.c` | Tile installer / uninstaller payloads |
| `probes/` | Hardware experiments (audio from a payload, live MP3 server, music-core tests) |
| `webapp/test_server.py` | PC test server for the page (logs the page's events) |
| `src/`, `tools/build-cspot.sh` | The earlier native app version (title PPSA99777), which plays only in the foreground |
| `third_party/cspot` | cspot (git submodule) |
| `howitworks.md` | How PS5 homebrew works and every hardware finding of this project |
| `CLAUDE.md`, `docs/claude-memory/` | Developer handoff notes |
| `docs/TEMPLATE_README.md`, `docs/*.md` | The original boilerplate's documentation |

The boilerplate's `.github/workflows/` are not included; take them from the boilerplate repository if needed.

## Project history

1. **Native app:** cspot ported into a PS5 title (with C++ exceptions, libc shims and a resampler to the PS5's
   48 kHz audio). It played Spotify, but the system mutes a title as soon as it leaves the foreground.
2. **Research:** tile types, payload audio, the system music player, the official Spotify app's structure
   ([howitworks.md](howitworks.md)).
3. **Breakthrough:** a web app tile's `<audio>` keeps playing during games. A live MP3 stream from a payload,
   then WAV for lower delay, then the reconnect trick for fast pause/skip.
4. **Speaker payload + tile (1.0.0):** everything runs on the console, no PC needed. Added the now-playing screen and
   the song notification.

## Credits and license

- [cspot](https://github.com/feelfreelinux/cspot) by feelfreelinux and contributors (Spotify Connect client), GPL-3.0.
- [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (build tooling).
- [ps5-payload-dev](https://github.com/ps5-payload-dev) SDK and tools; the tile installer follows
  [svtplay](https://github.com/ps5-payload-dev/svtplay)'s installer.
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0).
- Research references: SymphonyStation5, ProsperoRadio, ps5-homebrew-ui, pYTM5, websrv, BFplayer, ShadowMountPlus
  (links in [howitworks.md](howitworks.md)).

This project is licensed under **GPL-3.0-or-later** (see [LICENSE](LICENSE)). Spotify is a trademark of Spotify AB;
PlayStation and PS5 are trademarks of Sony Interactive Entertainment. This project is not affiliated with either.
