/*
 * ps5-native-app-boilerplate - Minimal native application example.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Draws a small CPU-rendered scene and keeps the process alive for normal
 * shell-mediated closure.
 */

#include "demo_renderer.hpp"
#include "spotify_engine.hpp"

#include <array>
#include <cstdio>
#include <span>

namespace
{
std::array<char, 48> banner{};
std::array<char, 48> build_label{};

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;

    canvas.clear(Color::background);
    canvas.text(120, 90, "SPOTIFY CONNECT", 10, Color::white);
    canvas.rectangle(120, 230, 1680, 8, Color::white);
    char line[96];
    for (int index = 0; index < spotify_engine::kStatusLines; index++)
    {
        spotify_engine::statusLine(index, line, sizeof line);
        canvas.text(120, 300 + 100 * index, line, 4, index == 3 ? Color::yellow : Color::cyan);
    }

    // A build that is not a release says which one it is (docs/PULL_REQUEST_BUILDS.md).
    if (build_label[0] != '\0')
        canvas.text(120, 960, build_label.data(), 4, Color::yellow);
}

// cspot logs to stdout: keep it in the title's storage, readable over FTP at
// /mnt/sandbox/<TITLE_ID>_000/download0/spotify.log while the app runs.
void open_log() noexcept
{
    rename("/download0/spotify.log", "/download0/spotify.prev.log");
    if (std::freopen("/download0/spotify.log", "w", stdout) != nullptr)
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (std::freopen("/download0/spotify.log", "a", stderr) != nullptr)
        std::setvbuf(stderr, nullptr, _IONBF, 0);
}
} // namespace

int main()
{
    open_log();
    ps5::demo::read_asset_text("/app0/assets/banner.txt", std::span{banner}, "APP0 ASSET FAILED");
    // Written by the build when BUILD_LABEL is set; the demo font has capitals only.
    ps5::demo::read_asset_text("/app0/build-label.txt", std::span{build_label}, "");
    for (char &character : build_label)
    {
        if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 'a' + 'A');
    }
    spotify_engine::start();
    ps5::demo::run_frames(draw_scene, banner.data());
}
