#pragma once

#include "runtime/fps_overlay_layout.h"
#include "platform/system_usage.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

struct ImFont;

namespace mscharged::runtime {

// The performance overlay (display.fps_overlay): FPS | GPU % | CPU %, then in
// network sessions LAT (round trip, ms) and in network matches with rollback
// DLY (input delay, ms) | RB (rollback frames), drawn by Aurora over the presented picture in windowed and fullscreen mode.
// Host presentation only; the game never sees it. Use it on the presentation
// owner thread, between game frames.
class FpsOverlay {
public:
    // font: a TrueType file (the launcher's Roboto); without it the counter
    // falls back to ImGui's built-in font.
    FpsOverlay(const std::filesystem::path& font, OverlayCorner corner);
    ~FpsOverlay();
    FpsOverlay(const FpsOverlay&) = delete;
    FpsOverlay& operator=(const FpsOverlay&) = delete;

    // A new measurement, in frames per second.
    void SetRate(double rate);
    // The network numbers: none outside network sessions (LAT, DLY and RB
    // hidden), a latency alone in one, all three in a match with rollback.
    struct Network {
        bool session = false;
        std::optional<double> latency_ms;
        bool rollback = false;
        std::optional<double> delay_ms;
        std::optional<double> rollback_frames;
    };
    void SetNetwork(const Network& network);
    // Rebuild only when a displayed value or the window's pixel size changes.
    // Unknown measurements show N/A, including during the first sample window.
    void Update(std::uint32_t surface_width, std::uint32_t surface_height);

private:
    void BuildFonts(float size);

    platform::SystemUsageMonitor usage_;
    std::vector<unsigned char> fontData_;
    OverlayCorner corner_;
    std::optional<int> value_;
    Network network_;
    std::size_t shownCount_ = 0;
    std::array<std::optional<int>, kMaxPerformanceMetrics> shownValues_{};
    std::uint32_t shownWidth_ = 0;
    std::uint32_t shownHeight_ = 0;
    float builtSize_ = 0.0f;
    ImFont* number_ = nullptr;
    ImFont* label_ = nullptr;
};

} // namespace mscharged::runtime
