---
name: ps5-access-automatic
description: "User allows Claude to deploy to and read logs from their PS5 over FTP without asking, but every location written must be tracked"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 228b7081-14a5-44ff-a618-e143cc0f5e4e
  modified: 2026-10-08T22:10:54.195Z
---

Claude may push payload ELFs straight to elfldr on 192.168.1.120:9021 (user offered it 2026-10-09), deploy (`make deploy PS5_HOST=192.168.1.120`) and fetch the app's log over FTP (port 2121) on its own, without asking each time. Say so briefly when doing it.

**Why:** the user was surprised to learn Claude was reading the console over the LAN (2026-10-09), then approved it ("do automatic"), on one condition: if the project is ever abandoned, they want to know every leftover file so they can delete them.

**How to apply:** before writing anywhere new on the PS5, add the path to [[ps5-footprint]]. Read only this project's files; don't browse the console.
