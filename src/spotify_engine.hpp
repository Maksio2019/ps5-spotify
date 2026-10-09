#pragma once

// Runs the Spotify Connect speaker (cspot) on background threads and exposes
// a few lines of status for the screen.

namespace spotify_engine
{
constexpr int kStatusLines = 6;
void start() noexcept;
// Copies status line `index` (upper case, screen font safe) into `out`.
void statusLine(int index, char *out, unsigned size) noexcept;
} // namespace spotify_engine
