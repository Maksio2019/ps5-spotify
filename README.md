# PS5 Speaker: Spotify Connect for a jailbroken PS5

Turns a jailbroken PS5 into a Spotify Connect speaker. Pick **PS5 Speaker** in the Spotify app on your
phone and the music plays on the console: on the home screen **and while you play games**.

> **Status: experimental, shelved (October 2026).** It works on the author's console (firmware 9.60) with
> one Spotify Premium account. Read [Known issues](#known-issues) before using it.

Not affiliated with Spotify or Sony. Needs **Spotify Premium** (Spotify Connect playback requires it).
It is an independent Spotify Connect client built on [cspot](https://github.com/feelfreelinux/cspot); no DRM
is bypassed and the official PS5 Spotify app is not touched. Audio is only streamed in memory, never written to disk.

## Features

- Shows up as **PS5 Speaker** in Spotify's device list (Spotify Connect pairing on your local network).
- **Keeps playing during games** and on the home screen.
- Play, pause, skip, seek and volume from the phone (about 1.5 s delay at the home screen, about 2 s in a game).
- Remembers the login: after the first pairing it signs in by itself and also appears under "On other networks".
- "PS5 Speaker" tile on the home screen with a now-playing screen (cover, song, artist, progress).
- PS5 notification with the cover, song and artist when a new song starts.

The now-playing screen and the song notification were finished right before the project was shelved and have
**not been seen on a console yet** (see [Known issues](#known-issues)).

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

A payload can't play audio in the background by itself, and a normal app is muted once a game starts. A web app
tile's `<audio>` is played by the PS5's system music player, which keeps going while a game runs. So the payload
runs the Spotify client and serves the music as one endless WAV "file", and the tile's page plays it. The page
itself is hosted on GitHub Pages (`main` branch of this repo, https://maksio2019.github.io/ps5-spotify/), because
the web app launcher only accepts https pages.

All the details and every hardware finding: [howitworks.md](howitworks.md). Developer handoff notes: [CLAUDE.md](CLAUDE.md).

## Requirements

- A jailbroken PS5 (tested on firmware **9.60**) with an ELF loader listening on port **9021** (elfldr; Payload
  Manager or any payload sender works too). An FTP server payload (ftpsrv) helps for logs and uninstalling.
- Spotify **Premium**.
- The phone and the PS5 on the same network for the first pairing.
- Internet on the console: the tile loads its page from `maksio2019.github.io`, and Spotify streams from
  `*.spotify.com` / `*.scdn.co`. If you use a DNS blocker payload, don't block those.

## Install and run

Download the payloads from the [Releases](https://github.com/Maksio2019/ps5-spotify/releases) page:
`spotify_speaker.elf`, `webapp_install.elf` and `webapp_uninstall.elf`.

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
| Optional other page address for the tile (one https URL) | `/data/spkr_url.txt` |
| Ports | 8795 TCP (stream, `/status`, `/quit`), 8796 TCP (Spotify Connect pairing), 5353 UDP (mDNS) |

## Troubleshooting

- **The tile says "The speaker isn't running":** send `spotify_speaker.elf` again (needed after every restart).
- **PS5 Speaker is not in the device list:** check that the phone is on the same network, close and reopen the
  Spotify app. After the first login it is also listed under "On other networks".
- **It hangs on "connecting" on a phone:** see the first known issue below.
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
   and that can't be changed from a web page (see the latency section in howitworks.md).
3. **No PS5 Control Center / Quick Menu music card.** There is no API for it that a web app or payload can use;
   every route tried is documented in howitworks.md.
4. **The payload has to be sent again after every PS5 restart.**
5. **The song notification and the now-playing screen are untested on a console.** They were finished right before
   the project was shelved; they were only checked in a desktop browser against the real payload.
6. **Closing the game or the tile does not stop the music.** Pause from the phone.
7. **The stream is one 6-hour WAV "file".** What happens after 6 hours of non-stop playback (especially during a
   game, when the page is frozen) has not been tested.
8. **Only tested on firmware 9.60**, with one console and one account.

## Build from source

Linux (Ubuntu; WSL2 works). The build uses the
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) tooling and the
PS5 payload SDK.

```bash
sudo apt install curl git make cmake ninja-build ccache pkg-config python3 python3-pip python3-venv tar wget unzip \
  clang-18 lld-18 llvm-18
git clone --recurse-submodules -b spotify-speaker https://github.com/Maksio2019/ps5-spotify.git
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

The page is [webapp/index.html](webapp/index.html). To use your own copy, host it on https (for example GitHub Pages),
write its address into `/data/spkr_url.txt` on the console and send `webapp_install.elf` again.

### Repository layout

| Path | What |
| --- | --- |
| `payload/spotify_daemon.cpp` | The speaker payload: cspot session, Spotify Connect pairing, mDNS, live WAV server, song info, notification |
| `webapp/index.html` | The tile's page (plays the stream, now-playing screen); `webapp/test_server.py` is a PC test server for it |
| `probes/webapp_install.c` | Tile installer / uninstaller payloads |
| `probes/` | Hardware experiments (audio from a payload, live MP3 server, music-core tests) |
| `src/`, `tools/build-cspot.sh` | The earlier native app version (title PPSA99777): plays only while it is in the foreground |
| `third_party/cspot` | cspot (git submodule) |
| `howitworks.md`, `CLAUDE.md`, `docs/claude-memory/` | Research notes, handoff notes |
| `docs/TEMPLATE_README.md` | The original boilerplate README |

This snapshot leaves out the template's `.github/workflows/`; take them from the boilerplate repository if needed.

## License

GPL-3.0-or-later, like the boilerplate it is built on and cspot. mbedTLS is Apache-2.0.
