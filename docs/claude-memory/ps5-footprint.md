---
name: ps5-footprint
description: "Every location on the user's PS5 that this project has written to, for cleanup if the project is abandoned"
metadata:
  node_type: memory
  type: project
  originSessionId: 228b7081-14a5-44ff-a618-e143cc0f5e4e
  modified: 2026-10-09T15:23:31.848Z
---

Keep this list current ([[ps5-access-automatic]]). If the user abandons the project, give them this list so they can delete the leftovers.

**2026-10-09 ~17:30: PROJECT SHELVED, CONSOLE CLEANED (verified over FTP: nothing left).** The user paused the project and asked
for everything to be removed from the PS5. Done: speaker stopped (/quit), `webapp_uninstall.elf` unregistered SPKR00001-4,
then deleted over FTP: `/data/spkr/` (log, prev log, credentials.json = Spotify login token, covers/), `/data/spkr_url.txt`,
`/data/webapp_probe.log`, the empty system-made `/system_ex/app/SPKR00001/`. `/user/download/SPKR00001/`, `/user/app/SPKR00001/`
and `/user/appmeta/SPKR00001/` were already gone after the uninstall. Nothing of this project is on the console now.

**If the project is resumed, these are the places it writes (re-add as they come back):**
- `/system_ex/app/SPKR00001/` (`eboot.bin` empty, `sce_sys/param.json`) and `/user/app/SPKR00001/sce_sys/{param.json,icon0.png}`: the "PS5 Speaker" web app tile, installed by `webapp_install.elf`. The system adds `/user/appmeta/SPKR00001/` and `/user/download/SPKR00001/download0.dat` (~320 MB). Remove with `webapp_uninstall.elf`, then delete what is left over FTP.
- `/data/spkr/`: speaker payload folder (`spotify.log`, `spotify.prev.log`, `credentials.json`, `credentials.tmp`, `covers/`).
- `/data/spkr_url.txt` (tile page address, read by the installer), `/data/webapp_probe.log` (recreated each installer run).
- Payloads run from memory only; gone after a reboot.
- FTP notes: this ftpsrv ignores the path in MLSD (cwd first), and answers DELE with 226, which Python's ftplib treats as an error although the file is deleted.

Not ours, left alone: the user's own PS4 package CUSA01780 (official Spotify PS4 app, installed by the user 2026-10-09).

Copies of payloads live in the user's Windows Downloads folder (not on the PS5): spotify_speaker.elf, webapp_install.elf, webapp_uninstall.elf, spkr_live_probe.elf, spotify_mdns_helper.elf, audio_probe_*.elf.
