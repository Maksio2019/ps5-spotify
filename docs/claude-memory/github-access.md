---
name: github-access
description: "Claude can push to the user's GitHub repo Maksio2019/ps5-spotify (GitHub Pages host of the speaker page) via gh in WSL"
metadata:
  type: reference
---

Set up 2026-10-09. `gh` in WSL is logged in as Maksio2019 with a fine-grained token scoped to ONLY `Maksio2019/ps5-spotify`
(Contents + Pages read/write; expiry chosen by the user). Local clone: `~/ps5-spotify-page`, repo-local git identity
`Maksio2019 <157291633+Maksio2019@users.noreply.github.com>`. Pages serves https://maksio2019.github.io/ps5-spotify/,
which is the PS5 tile's webAppUri (see [[ps5-footprint]]: /data/spkr_url.txt).

The user asked Claude to manage that repo, so pushing page updates there is allowed. Publishing the whole speaker
project needs a new or wider token from the user. If gh says the token expired, ask the user to make a new one
(same scopes) and run `gh auth login --hostname github.com --git-protocol https --with-token`.
