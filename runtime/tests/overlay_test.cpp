// Self-checking tests for the debug overlay library. The model and panel tests need no GPU or window; the
// overlay tests draw into an offscreen image on the first Vulkan device. On Windows they also create a
// hidden window, to give the Win32 backend a handle.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <mach-o/dyld.h>

#include <cstring>
#endif

#include "imgui.h"
#include "imgui_internal.h"
#include "hle/save_dialog_text.h"
#include "overlay/debug_overlay.h"
#include "overlay/imgui_context.h"
#include "overlay/overlay_model.h"
#include "overlay/overlay_panels.h"
#include "overlay/overlay_sources.h"
#include "overlay/overlay_stack.h"
#include "overlay/save_dialog_overlay.h"
#include "overlay/ui_font.h"
#include "render/frame_dump.h"
#include "render/vk_renderer.h"
#include "rsx/rsx_unimplemented.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using Clock = sshd::FrameStatsHistory::Clock;

Clock::time_point at(double seconds) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)) +
                             std::chrono::seconds(100));
}

void testSteadyFlips() {
    sshd::FrameStatsHistory history;
    sshd::RendererStats stats;
    for (int i = 0; i <= 60; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        history.onFlip(stats, at(i / 60.0));
    }
    const Clock::time_point last = at(1.0);
    CHECK(history.flip() == 61);
    CHECK(std::fabs(history.flipsPerSecond(last) - 60.0) < 0.01);
    CHECK(history.flipsPerSecond(last + std::chrono::seconds(2)) == 0.0);
    CHECK(history.frameTimeCount() == 60);
    CHECK(history.frameTimeOffset() == 0);
    bool steady = true;
    for (std::size_t i = 0; i < history.frameTimeCount(); ++i) {
        steady = steady && std::fabs(history.frameTimesMs()[i] - 16.667f) < 0.01f;
    }
    CHECK(steady);
    CHECK(std::fabs(history.maxFrameTimeMs() - 16.667f) < 0.01f);
}

void testRingWraps() {
    sshd::FrameStatsHistory history;
    sshd::RendererStats stats;
    for (int i = 0; i < 300; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        // The last interval is the longest, so the maximum has to come from the newest slot.
        history.onFlip(stats, at(i * 0.01 + (i == 299 ? 0.05 : 0.0)));
    }
    CHECK(history.frameTimeCount() == sshd::FrameStatsHistory::kSamples);
    // 299 intervals were written; the next slot to overwrite (the oldest) is 299 % 240.
    CHECK(history.frameTimeOffset() == 299 % sshd::FrameStatsHistory::kSamples);
    CHECK(std::fabs(history.frameTimesMs()[(history.frameTimeOffset() + 239) % 240] - 60.0f) < 0.01f);
    CHECK(std::fabs(history.maxFrameTimeMs() - 60.0f) < 0.01f);
    CHECK(history.flip() == 300);
}

void testDrawDeltas() {
    sshd::FrameStatsHistory history;
    sshd::RendererStats stats;
    stats.flips = 5;
    stats.draws = 40;
    stats.draws_skipped = 3;
    history.onFlip(stats, at(0.0));
    // The first call reports everything so far.
    CHECK(history.drawsLastFlip() == 40);
    CHECK(history.skippedLastFlip() == 3);
    CHECK(history.skippedTotal() == 3);

    stats.flips = 6;
    stats.draws = 52;
    stats.draws_skipped = 4;
    history.onFlip(stats, at(0.016));
    CHECK(history.drawsLastFlip() == 12);
    CHECK(history.skippedLastFlip() == 1);
    CHECK(history.skippedTotal() == 4);
    CHECK(history.flip() == 6);
}

void testRefresh() {
    int audio_calls = 0;
    int unimplemented_calls = 0;
    int limit_calls = 0;
    std::uint64_t skipped = 0;
    std::uint64_t underruns = 0;

    sshd::OverlaySources sources;
    sources.audio = [&] {
        ++audio_calls;
        sshd::OverlayAudioStats stats;
        stats.output = "fake";
        stats.skipped_blocks = skipped;
        stats.underrun_frames = underruns;
        return stats;
    };
    sources.unimplemented = [&] {
        ++unimplemented_calls;
        return std::vector<std::pair<std::string, std::uint64_t>>{{"state: x", 2}};
    };
    sources.unimplemented_dropped = [] { return std::uint64_t{7}; };
    sources.flip_limit = [&] {
        ++limit_calls;
        return sshd::RsxFlipLimit::Off;
    };
    sources.log_level = [] { return sshd::LogLevel::Warn; };
    sources.capture_status = [] {
        sshd::RsxCaptureStatus status;
        status.available = true;
        status.last_frame = 9;
        return status;
    };

    sshd::OverlayState state;
    CHECK(state.flip_limit == sshd::RsxFlipLimit::Vsync);
    CHECK(state.log_level == sshd::LogLevel::Info);

    state.refresh(sources, at(0.0));
    CHECK(audio_calls == 1);
    CHECK(unimplemented_calls == 1);
    CHECK(state.audio.output == "fake");
    CHECK(state.skipped_per_s == 0.0);
    CHECK(state.unimplemented.size() == 1 && state.unimplemented[0].second == 2);
    CHECK(state.unimplemented_dropped == 7);
    CHECK(state.flip_limit == sshd::RsxFlipLimit::Off);
    CHECK(state.log_level == sshd::LogLevel::Warn);
    CHECK(state.capture.available && state.capture.last_frame == 9);

    // Inside the audio interval: the cheap providers still run, the audio one does not.
    skipped = 10;
    state.refresh(sources, at(0.1));
    CHECK(audio_calls == 1);
    CHECK(unimplemented_calls == 1);
    CHECK(limit_calls == 2);

    skipped = 5;
    underruns = 480;
    state.refresh(sources, at(0.25));
    CHECK(audio_calls == 2);
    CHECK(unimplemented_calls == 1);
    CHECK(std::fabs(state.skipped_per_s - 20.0) < 1e-6);
    CHECK(std::fabs(state.underrun_frames_per_s - 1920.0) < 1e-6);

    state.refresh(sources, at(0.5));
    CHECK(audio_calls == 3);
    CHECK(unimplemented_calls == 2);
    CHECK(state.skipped_per_s == 0.0);

    // Empty sources leave the cached values alone.
    state.refresh(sshd::OverlaySources{}, at(5.0));
    CHECK(state.audio.output == "fake");
    CHECK(state.unimplemented_dropped == 7);
}

sshd::OverlaySources makeFakeSources() {
    sshd::OverlaySources sources;
    sources.audio = [] {
        sshd::OverlayAudioStats stats;
        stats.output = "fake";
        stats.max_producer_ms = 1.5;
        stats.mean_fill_frames = 1500.0;
        stats.unwritten_blocks = 7;
        stats.unwritten_runs = 3;
        sshd::OverlaySpuRow spu;
        spu.name = "bnusCoreMain";
        spu.receives = 100;
        spu.busy_mean_ms = 2.0;
        spu.busy_p99_ms = 5.1;
        spu.busy_max_ms = 6.6;
        spu.busy_over = 4;
        stats.spu.push_back(spu);
        sshd::OverlayLockRow lock;
        lock.name = "audio";
        lock.acquisitions = 182;
        lock.wait_p99_ms = 0.12;
        lock.wait_max_ms = 1.9;
        lock.wait_over = 2;
        stats.locks.push_back(lock);
        lock.name = "ppu";
        stats.locks.push_back(lock);
        return stats;
    };
    sources.unimplemented = [] { return std::vector<std::pair<std::string, std::uint64_t>>{{"state: x", 2}}; };
    sources.unimplemented_dropped = [] { return std::uint64_t{1}; };
    sources.flip_limit = [] { return sshd::RsxFlipLimit::Vsync; };
    sources.set_flip_limit = [](sshd::RsxFlipLimit) {};
    sources.log_level = [] { return sshd::LogLevel::Info; };
    sources.set_log_level = [](sshd::LogLevel) {};
    sources.request_capture = [] { return true; };
    sources.capture_status = [] {
        sshd::RsxCaptureStatus status;
        status.available = true;
        status.last_frame = 4;
        status.last_path = "captures/frame_4.sshdcap";
        return status;
    };
    return sources;
}

// Runs three frames of the overlay window in a private ImGui context and returns whether the last one
// produced geometry. The Vulkan backend is not involved.
bool drawFrames(sshd::OverlayState& state, const sshd::OverlaySources& sources) {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->Build();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);

    bool drew = false;
    for (int i = 0; i < 3; ++i) {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        sshd::drawOverlayWindow(state, sources);
        ImGui::Render();
        drew = ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->TotalVtxCount > 0;
    }
    CHECK(ImGui::FindWindowByName("sshd") != nullptr);
    ImGui::DestroyContext(context);
    return drew;
}

void testPanels() {
    sshd::OverlayState state;
    sshd::RendererStats stats;
    for (int i = 0; i < 5; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        stats.draws = static_cast<std::uint64_t>(10 * (i + 1));
        stats.draws_skipped = static_cast<std::uint64_t>(i);
        state.frames.onFlip(stats, at(i / 60.0));
    }
    const sshd::OverlaySources sources = makeFakeSources();
    state.refresh(sources, at(1.0));
    state.dumps.written = 2;
    state.last_dump = "frames/frame_000002.png";
    CHECK(drawFrames(state, sources));

    // The last frame ran through every section with data; clicks are not simulated, so no request is set.
    CHECK(!state.dump_requested);

    // No providers at all: every section falls back to its placeholder.
    sshd::OverlayState empty_state;
    CHECK(drawFrames(empty_state, sshd::OverlaySources{}));

    // The engine runs but no SPU thread has received and no lock was taken: the new rows stay out.
    sshd::OverlaySources bare_sources = makeFakeSources();
    bare_sources.audio = [] {
        sshd::OverlayAudioStats stats;
        stats.output = "fake";
        return stats;
    };
    sshd::OverlayState bare_state;
    bare_state.refresh(bare_sources, at(1.0));
    CHECK(bare_state.audio.spu.empty() && bare_state.audio.locks.empty() && bare_state.lock_rates.empty());
    CHECK(drawFrames(bare_state, bare_sources));

    // Rates arrive one refresh late; the rows must draw before and after that without a mismatch.
    sshd::OverlayState rate_state;
    rate_state.refresh(sources, at(1.0));
    CHECK(rate_state.lock_rates.size() == rate_state.audio.locks.size());
    rate_state.refresh(sources, at(1.5));
    CHECK(drawFrames(rate_state, sources));

    // The Anti-aliasing row with DLAA listed or not, and with DLAA as the reported mode and as a pending request.
    for (const bool dlaa_available : {false, true}) {
        sshd::OverlayState aa_state;
        aa_state.refresh(sources, at(1.0));
        aa_state.aa_mode = sshd::AaMode::Dlaa;
        aa_state.msaa_available = true;
        aa_state.dlaa_available = dlaa_available;
        CHECK(drawFrames(aa_state, sources));
        aa_state.aa_mode_request = sshd::AaMode::Dlaa;
        CHECK(drawFrames(aa_state, sources));
    }

    // The Frame generation row: unsupported (not listed), supported but inactive, active and suspended by the overlay,
    // each with DLAA and without it.
    const sshd::FrameGenStatus inactive{.game_fps = 60, .inactive_reason = "target is less than twice the game rate"};
    const sshd::FrameGenStatus active{.active = true,
                                      .multiplier = 4,
                                      .game_fps = 30,
                                      .presented_fps = 119.6f,
                                      .inactive_reason = {},
                                      .max_multiplier = 6};
    const sshd::FrameGenStatus suspended{.inactive_reason = std::string(sshd::kFrameGenOverlayOpenReason),
                                         .max_multiplier = 6};
    struct FrameGenCase {
        bool supported;
        sshd::FrameGenStatus status;
    };
    for (const FrameGenCase& test : {FrameGenCase{false, {}}, FrameGenCase{true, inactive}, FrameGenCase{true, active},
                                     FrameGenCase{true, suspended}}) {
        for (const sshd::AaMode mode : {sshd::AaMode::Dlaa, sshd::AaMode::Msaa}) {
            sshd::OverlayState fg_state;
            fg_state.refresh(sources, at(1.0));
            fg_state.aa_mode = mode;
            fg_state.msaa_available = true;
            fg_state.dlaa_available = true;
            fg_state.setFrameGen(test.supported, sshd::FrameGenTarget::Fps120, test.status);
            CHECK(drawFrames(fg_state, sources));
            fg_state.frame_gen_request = sshd::FrameGenTarget::Unlimited;
            CHECK(drawFrames(fg_state, sources));
        }
    }
}

void testUnwrittenRates() {
    std::uint64_t unwritten = 10;
    std::uint64_t acquisitions = 1000;
    std::uint64_t over = 4;
    bool with_ppu = false;

    sshd::OverlaySources sources;
    sources.audio = [&] {
        sshd::OverlayAudioStats stats;
        stats.output = "fake";
        stats.unwritten_blocks = unwritten;
        sshd::OverlayLockRow audio_row;
        audio_row.name = "audio";
        audio_row.acquisitions = acquisitions;
        audio_row.wait_over = over;
        if (with_ppu) {
            // A class that appears later shifts the rows; the rates still follow the names.
            sshd::OverlayLockRow ppu;
            ppu.name = "ppu";
            ppu.acquisitions = 500;
            stats.locks.push_back(ppu);
        }
        stats.locks.push_back(audio_row);
        return stats;
    };

    sshd::OverlayState state;
    state.refresh(sources, at(0.0));
    CHECK(state.unwritten_per_s == 0.0);
    CHECK(state.lock_rates.size() == 1 && state.lock_rates[0].acquisitions_per_s == 0.0);

    unwritten = 13;
    acquisitions = 1500;
    over = 5;
    with_ppu = true;
    state.refresh(sources, at(0.5));
    CHECK(std::fabs(state.unwritten_per_s - 6.0) < 1e-6);
    CHECK(state.lock_rates.size() == 2);
    if (state.lock_rates.size() == 2) {
        CHECK(state.lock_rates[0].acquisitions_per_s == 0.0);  // ppu: no earlier row to diff against
        CHECK(std::fabs(state.lock_rates[1].acquisitions_per_s - 1000.0) < 1e-6);
        CHECK(std::fabs(state.lock_rates[1].wait_over_per_s - 2.0) < 1e-6);
    }

    // A counter that went backwards (source restarted) reads as no change.
    unwritten = 2;
    state.refresh(sources, at(1.0));
    CHECK(state.unwritten_per_s == 0.0);
}

// Drives the real Settings panel in a private ImGui context with simulated mouse input. An item is found by
// moving the pointer down the window until ImGui reports it hovered and enabled.
class SettingsProbe {
public:
    SettingsProbe(sshd::OverlayState& state, const sshd::OverlaySources& sources) : state_(state), sources_(sources) {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->Build();
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        frame();
        frame();
        window_ = ImGui::FindWindowByName("sshd");
        CHECK(window_ != nullptr);
        if (window_ == nullptr) return;
        const std::optional<ImVec2> header = findHovered(window_->GetID("Settings"), 60.0f, 10.0f, 720.0f);
        CHECK(header.has_value());
        closed_height_ = window_->Size.y;
        if (header) click(*header);
        frame();
        frame();
    }

    ~SettingsProbe() { ImGui::DestroyContext(context_); }

    SettingsProbe(const SettingsProbe&) = delete;
    SettingsProbe& operator=(const SettingsProbe&) = delete;

    void frame() {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        sshd::drawOverlayWindow(state_, sources_);
        ImGui::Render();
    }

    // The pointer position over the item with this id, or nothing.
    std::optional<ImVec2> findHovered(ImGuiID id, float x, float y_begin, float y_end,
                                      ImGuiWindow* expected_window = nullptr) {
        if (expected_window != nullptr) {
            const float item_height = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
            const int item_count = static_cast<int>(std::ceil(expected_window->Size.y / item_height));
            for (int i = 0; i < item_count; ++i) {
                const float y = expected_window->Pos.y + ImGui::GetStyle().WindowPadding.y +
                                item_height * (static_cast<float>(i) + 0.5f);
                ImGui::GetIO().AddMousePosEvent(x, y);
                frame();
                const ImGuiContext& g = *ImGui::GetCurrentContext();
                if (g.LastItemData.ID == id && !g.HoveredIdIsDisabled) return ImVec2(x, y);
            }
        }
        const float item_height = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
        for (float y = y_begin; y < y_end; y += item_height) {
            ImGui::GetIO().AddMousePosEvent(x, y + item_height * 0.5f);
            frame();
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.HoveredId == id && !g.HoveredIdIsDisabled &&
                (expected_window == nullptr || expected_window->RootWindow == g.HoveredWindow))
                return ImVec2(x, y + item_height * 0.5f);
        }
        for (float y = y_begin; y < y_end; y += 1.0f) {
            ImGui::GetIO().AddMousePosEvent(x, y);
            frame();
            frame();
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.HoveredId == id && !g.HoveredIdIsDisabled &&
                (expected_window == nullptr || g.HoveredWindow == expected_window ||
                 expected_window->RootWindow == g.HoveredWindow))
                return ImVec2(x, y);
        }
        return std::nullopt;
    }

    void click(ImVec2 at) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        frame();
    }

    ImGuiWindow* window() const { return window_; }
    float closedHeight() const { return closed_height_; }

private:
    sshd::OverlayState& state_;
    const sshd::OverlaySources& sources_;
    ImGuiContext* context_ = nullptr;
    ImGuiWindow* window_ = nullptr;
    float closed_height_ = 0.0f;
};

// Collects what the panels report through OverlaySources::setting_changed.
struct SettingCalls {
    std::vector<std::pair<sshd::OverlaySetting, std::string>> calls;

    void attach(sshd::OverlaySources& sources) {
        sources.setting_changed = [this](sshd::OverlaySetting setting, std::string_view value) {
            calls.emplace_back(setting, std::string(value));
        };
    }
};

void testResolutionScalePanel() {
    SettingCalls recorded;
    sshd::OverlaySources sources = makeFakeSources();
    recorded.attach(sources);
    constexpr float kProbeX = 60.0f;
    constexpr float kWindowBottom = 720.0f;

    {
        // No scale reported: the combo is disabled and nothing can be requested.
        sshd::OverlayState state;
        SettingsProbe probe(state, sources);
        if (probe.window() != nullptr) {
            const ImGuiID id = probe.window()->GetID("Internal resolution scale");
            CHECK(!probe.findHovered(id, kProbeX, 10.0f, kWindowBottom));
            // The window followed the section that was opened after its first frames, and nothing is clipped.
            CHECK(probe.window()->Size.y > probe.closedHeight());
            CHECK(!probe.window()->ScrollbarY && !probe.window()->ScrollbarX);
        }
        CHECK(state.resolution_scale_request == 0);
    }

    {
        // The combo is enabled, so it sits outside the group of unimplemented settings.
        sshd::OverlayState state;
        state.resolution_scale = 2;
        SettingsProbe probe(state, sources);
        if (probe.window() == nullptr) return;
        const ImGuiID id = probe.window()->GetID("Internal resolution scale");
        const std::optional<ImVec2> combo = probe.findHovered(id, kProbeX, 10.0f, kWindowBottom);
        CHECK(combo.has_value());
        if (!combo) return;
        CHECK(state.resolution_scale_request == 0);
        CHECK(recorded.calls.empty());

        probe.click(*combo);
        ImGuiContext& g = *ImGui::GetCurrentContext();
        CHECK(g.OpenPopupStack.Size == 1);
        if (g.OpenPopupStack.Size != 1 || g.OpenPopupStack[0].Window == nullptr) return;
        ImGuiWindow* popup = g.OpenPopupStack[0].Window;
        CHECK(recorded.calls.empty());  // opening the list changes nothing
        const ImGuiID entry = popup->GetID("3x");
        CHECK(entry != 0);
        CHECK(state.resolution_scale_request == 0);
        CHECK(state.resolution_scale_request == 0);
    }
}

void testFlipLimiterPanel() {
    SettingCalls recorded;
    sshd::OverlaySources sources = makeFakeSources();
    recorded.attach(sources);
    sshd::RsxFlipLimit applied = sshd::RsxFlipLimit::Vsync;
    sources.set_flip_limit = [&](sshd::RsxFlipLimit limit) { applied = limit; };

    sshd::OverlayState state;
    SettingsProbe probe(state, sources);
    if (probe.window() == nullptr) return;
    const std::optional<ImVec2> combo = probe.findHovered(probe.window()->GetID("Flip limiter"), 60.0f, 10.0f, 720.0f);
    CHECK(combo.has_value());
    if (!combo) return;
    CHECK(recorded.calls.empty());

    probe.click(*combo);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    CHECK(g.OpenPopupStack.Size == 1);
    if (g.OpenPopupStack.Size != 1 || g.OpenPopupStack[0].Window == nullptr) return;
    ImGuiWindow* popup = g.OpenPopupStack[0].Window;
    const ImGuiID off_id = popup->GetID("Unlimited");
    CHECK(off_id != 0);
    CHECK(recorded.calls.empty());
    CHECK(applied == sshd::RsxFlipLimit::Vsync);
}

// Draws the open Settings panel with every combination of reported mode and MSAA support; ImGui asserts on a
// Begin/End mismatch, so reaching the end is the check.
void testAntiAliasingPanel() {
    SettingCalls recorded;
    sshd::OverlaySources sources = makeFakeSources();
    recorded.attach(sources);
    constexpr float kProbeX = 60.0f;
    constexpr float kWindowBottom = 720.0f;

    const std::optional<sshd::AaMode> modes[] = {std::nullopt, sshd::AaMode::Supersample, sshd::AaMode::Msaa,
                                                 sshd::AaMode::Dlaa};
    for (const std::optional<sshd::AaMode>& mode : modes) {
        for (const bool msaa_available : {false, true}) {
            for (const bool dlaa_available : {false, true}) {
                recorded.calls.clear();
                sshd::OverlayState state;
                state.aa_mode = mode;
                state.msaa_available = msaa_available;
                state.dlaa_available = dlaa_available;
                SettingsProbe probe(state, sources);
                if (probe.window() == nullptr) continue;
                const ImGuiID id = probe.window()->GetID("Anti-aliasing");
                const std::optional<ImVec2> combo = probe.findHovered(id, kProbeX, 10.0f, kWindowBottom);
                // Unknown mode: the combo is disabled.
                CHECK(combo.has_value() == mode.has_value());
                CHECK(!probe.window()->ScrollbarY && !probe.window()->ScrollbarX);
                CHECK(!state.aa_mode_request);
                CHECK(recorded.calls.empty());
                if (!combo) continue;

                probe.click(*combo);
                ImGuiContext& g = *ImGui::GetCurrentContext();
                CHECK(g.OpenPopupStack.Size == 1);
                if (g.OpenPopupStack.Size != 1 || g.OpenPopupStack[0].Window == nullptr) continue;
                ImGuiWindow* popup = g.OpenPopupStack[0].Window;
                const auto find = [&](const char* label) {
                    return probe.findHovered(popup->GetID(label), popup->Pos.x + 10.0f, popup->Pos.y,
                                             popup->Pos.y + popup->Size.y);
                };

                // The current mode is not a change; MSAA cannot be picked when the device lacks it.
                const std::optional<ImVec2> supersample = find("Supersample");
                const std::optional<ImVec2> msaa = find("MSAA 2x");
                const std::optional<ImVec2> dlaa = find("DLAA");
                CHECK(supersample.has_value());
                CHECK(msaa.has_value() == msaa_available);
                // DLAA is not listed at all when unavailable, even while it is the reported mode.
                CHECK(dlaa.has_value() == dlaa_available);
                // A click closes the popup, so each state makes one.
                if (mode == sshd::AaMode::Msaa && supersample) {
                    probe.click(*supersample);
                    CHECK(state.aa_mode_request == sshd::AaMode::Supersample);
                    CHECK(recorded.calls.size() == 1);
                    if (recorded.calls.size() == 1) {
                        CHECK(recorded.calls[0].first == sshd::OverlaySetting::AaMode);
                        CHECK(recorded.calls[0].second == "supersample");
                    }
                } else if (mode == sshd::AaMode::Supersample && msaa) {
                    probe.click(*msaa);
                    CHECK(state.aa_mode_request == sshd::AaMode::Msaa);
                    CHECK(recorded.calls.size() == 1 && recorded.calls[0].second == "msaa");
                } else if (mode == sshd::AaMode::Supersample && dlaa) {
                    probe.click(*dlaa);
                    CHECK(state.aa_mode_request == sshd::AaMode::Dlaa);
                    CHECK(recorded.calls.size() == 1 && recorded.calls[0].first == sshd::OverlaySetting::AaMode &&
                          recorded.calls[0].second == "dlaa");
                } else if (mode == sshd::AaMode::Dlaa && supersample) {
                    probe.click(*supersample);
                    CHECK(state.aa_mode_request == sshd::AaMode::Supersample);
                    CHECK(recorded.calls.size() == 1 && recorded.calls[0].second == "supersample");
                } else if (mode == sshd::AaMode::Supersample && supersample) {
                    probe.click(*supersample);
                    CHECK(!state.aa_mode_request);
                    CHECK(recorded.calls.empty());
                }
            }
        }
    }
}

void testAaModeChoices() {
    using sshd::AaMode;
    sshd::OverlayState state;
    CHECK((sshd::aaModeChoices(state) == std::vector<AaMode>{AaMode::Supersample, AaMode::Msaa}));
    // MSAA availability is shown as a disabled entry, so it does not change the list.
    state.msaa_available = true;
    CHECK((sshd::aaModeChoices(state) == std::vector<AaMode>{AaMode::Supersample, AaMode::Msaa}));
    state.dlaa_available = true;
    CHECK((sshd::aaModeChoices(state) == std::vector<AaMode>{AaMode::Supersample, AaMode::Msaa, AaMode::Dlaa}));
    // The reported mode does not decide what is listed.
    state.dlaa_available = false;
    state.aa_mode = AaMode::Dlaa;
    CHECK((sshd::aaModeChoices(state) == std::vector<AaMode>{AaMode::Supersample, AaMode::Msaa}));
}

void testFrameGenChoices() {
    using sshd::FrameGenTarget;
    sshd::OverlayState state;
    CHECK((sshd::frameGenChoices(state) ==
           std::vector<FrameGenTarget>{FrameGenTarget::Off, FrameGenTarget::Fps60, FrameGenTarget::Fps90,
                                       FrameGenTarget::Fps120, FrameGenTarget::Fps180, FrameGenTarget::Unlimited}));
    // The list is the same whatever the renderer reported.
    state.setFrameGen(true, FrameGenTarget::Fps60, {});
    CHECK(sshd::frameGenChoices(state).size() == 6);

    CHECK(sshd::frameGenItemLabel(FrameGenTarget::Off) == "Off");
    CHECK(sshd::frameGenItemLabel(FrameGenTarget::Fps120) == "120 fps");
    CHECK(sshd::frameGenItemLabel(FrameGenTarget::Fps180) == "180 fps (unverified)");
    CHECK(sshd::frameGenItemLabel(FrameGenTarget::Unlimited) == "Unlimited");
}

void testFrameGenStatusLine() {
    using sshd::FrameGenTarget;
    sshd::OverlayState state;
    state.aa_mode = sshd::AaMode::Dlaa;
    // Nothing to say while the target is off.
    state.setFrameGen(true, FrameGenTarget::Off, {});
    CHECK(sshd::frameGenStatusLine(state).empty());

    const sshd::FrameGenStatus active{.active = true,
                                      .multiplier = 4,
                                      .game_fps = 30,
                                      .presented_fps = 119.6f,
                                      .inactive_reason = {},
                                      .max_multiplier = 6};
    state.setFrameGen(true, FrameGenTarget::Fps120, active);
    CHECK(sshd::frameGenStatusLine(state) == "4x: 30 -> 120 fps, measured 119.6");
    // No measurement yet.
    sshd::FrameGenStatus fresh = active;
    fresh.presented_fps = 0.0f;
    state.setFrameGen(true, FrameGenTarget::Fps120, fresh);
    CHECK(sshd::frameGenStatusLine(state) == "4x: 30 -> 120 fps");

    // The overlay opening suspends generation; the line keeps the last active figures.
    state.setFrameGen(true, FrameGenTarget::Fps120, active);
    const sshd::FrameGenStatus suspended{.game_fps = 30,
                                         .inactive_reason = std::string(sshd::kFrameGenOverlayOpenReason)};
    state.setFrameGen(true, FrameGenTarget::Fps120, suspended);
    CHECK(sshd::frameGenStatusLine(state) ==
          "suspended while this panel is open; last: 4x: 30 -> 120 fps, measured 119.6");
    // A new target forgets them.
    state.setFrameGen(true, FrameGenTarget::Fps90, suspended);
    CHECK(sshd::frameGenStatusLine(state) == "suspended while this panel is open");

    state.setFrameGen(true, FrameGenTarget::Fps120,
                      {.game_fps = 60, .inactive_reason = "target is less than twice the game rate"});
    CHECK(sshd::frameGenStatusLine(state) ==
          "inactive: target is less than twice the game rate (presenting game 60 fps)");
    state.setFrameGen(true, FrameGenTarget::Fps120, {.inactive_reason = "measuring the game rate"});
    CHECK(sshd::frameGenStatusLine(state) == "inactive: measuring the game rate");
    state.setFrameGen(true, FrameGenTarget::Fps120, {});
    CHECK(sshd::frameGenStatusLine(state) == "inactive");

    // Without DLAA the note says why and the line stays out of its way.
    state.aa_mode = sshd::AaMode::Msaa;
    CHECK(sshd::frameGenStatusLine(state).empty());
    CHECK(sshd::frameGenDisabledNote(state) == "needs DLAA anti-aliasing");
    state.aa_mode = sshd::AaMode::Dlaa;
    CHECK(sshd::frameGenDisabledNote(state).empty());
}

// The combo needs DLAA: a pending anti-aliasing request counts, an unknown mode does not.
void testFrameGenSelectable() {
    sshd::OverlayState state;
    CHECK(!sshd::frameGenSelectable(state));
    state.aa_mode = sshd::AaMode::Dlaa;
    CHECK(sshd::frameGenSelectable(state));
    state.aa_mode_request = sshd::AaMode::Msaa;
    CHECK(!sshd::frameGenSelectable(state));
    state.aa_mode = sshd::AaMode::Msaa;
    state.aa_mode_request = sshd::AaMode::Dlaa;
    CHECK(sshd::frameGenSelectable(state));
    state.aa_mode_request.reset();
    CHECK(!sshd::frameGenSelectable(state));
    state.aa_mode = sshd::AaMode::Supersample;
    CHECK(!sshd::frameGenSelectable(state));
}

void testObjectMotionPanel() {
    SettingCalls recorded;
    sshd::OverlaySources sources = makeFakeSources();
    recorded.attach(sources);
    constexpr float kProbeX = 60.0f;
    constexpr float kWindowBottom = 720.0f;

    {
        // Not supported by the device: the row is not drawn.
        sshd::OverlayState unsupported;
        unsupported.aa_mode = sshd::AaMode::Dlaa;
        SettingsProbe probe(unsupported, sources);
        if (probe.window() == nullptr) return;
        CHECK(!probe.findHovered(probe.window()->GetID("Object motion vectors"), kProbeX, 10.0f, kWindowBottom));
    }

    // Without DLAA the checkbox is disabled and keeps its value: no click reaches a request or a saved setting.
    for (const sshd::AaMode mode : {sshd::AaMode::Msaa, sshd::AaMode::Supersample}) {
        sshd::OverlayState state;
        state.aa_mode = mode;
        state.object_motion_supported = true;
        state.object_motion = true;
        SettingsProbe probe(state, sources);
        if (probe.window() == nullptr) continue;
        CHECK(!probe.findHovered(probe.window()->GetID("Object motion vectors"), kProbeX, 10.0f, kWindowBottom));
        CHECK(!state.object_motion_request && state.object_motion && recorded.calls.empty());
        CHECK(!sshd::objectMotionSelectable(state));
        CHECK(sshd::frameGenDisabledNote(state) == "needs DLAA anti-aliasing");
    }

    // With DLAA a click toggles it, requests the new value and reports it for saving.
    for (const bool before : {true, false}) {
        recorded.calls.clear();
        sshd::OverlayState state;
        state.aa_mode = sshd::AaMode::Dlaa;
        state.object_motion_supported = true;
        state.object_motion = before;
        SettingsProbe probe(state, sources);
        if (probe.window() == nullptr) return;
        const std::optional<ImVec2> box =
                probe.findHovered(probe.window()->GetID("Object motion vectors"), kProbeX, 10.0f, kWindowBottom);
        CHECK(box.has_value());
        if (!box) return;
        probe.click(*box);
        CHECK(state.object_motion_request == !before);
        CHECK(recorded.calls.size() == 1);
        if (recorded.calls.size() == 1) {
            CHECK(recorded.calls[0].first == sshd::OverlaySetting::ObjectMotion);
            CHECK(recorded.calls[0].second == (before ? "off" : "on"));
        }
    }
}

void testAnisotropyPanel() {
    SettingCalls recorded;
    sshd::OverlaySources sources = makeFakeSources();
    recorded.attach(sources);
    constexpr float kProbeX = 60.0f;
    constexpr float kWindowBottom = 720.0f;

    {
        // Not reported, or reported as unsupported: the combo is disabled and nothing can be requested.
        struct Case {
            std::uint32_t level;
            float max;
        };
        for (const Case disabled : {Case{0, 0.0f}, Case{0, 16.0f}, Case{1, 0.0f}}) {
            sshd::OverlayState state;
            state.anisotropy = disabled.level;
            state.max_anisotropy = disabled.max;
            SettingsProbe probe(state, sources);
            if (probe.window() == nullptr) continue;
            CHECK(!probe.findHovered(probe.window()->GetID("Anisotropic filtering"), kProbeX, 10.0f, kWindowBottom));
            CHECK(!probe.window()->ScrollbarY && !probe.window()->ScrollbarX);
            CHECK(state.anisotropy_request == 0);
            CHECK(recorded.calls.empty());
        }
    }

    for (const float max_anisotropy : {16.0f, 8.0f}) {
        sshd::OverlayState state;
        state.anisotropy = 1;
        state.max_anisotropy = max_anisotropy;
        SettingsProbe probe(state, sources);
        if (probe.window() == nullptr) return;
        const std::optional<ImVec2> combo =
                probe.findHovered(probe.window()->GetID("Anisotropic filtering"), kProbeX, 10.0f, kWindowBottom);
        CHECK(combo.has_value());
        if (!combo) return;
        // The combo sits outside the group of unimplemented settings, which only keeps Volume.
        CHECK(!probe.window()->ScrollbarY && !probe.window()->ScrollbarX);

        probe.click(*combo);
        ImGuiContext& g = *ImGui::GetCurrentContext();
        CHECK(g.OpenPopupStack.Size == 1);
        if (g.OpenPopupStack.Size != 1 || g.OpenPopupStack[0].Window == nullptr) return;
        ImGuiWindow* popup = g.OpenPopupStack[0].Window;
        CHECK(recorded.calls.empty());  // opening the list changes nothing
        const auto find = [&](const char* label) {
            return probe.findHovered(popup->GetID(label), popup->Pos.x + 10.0f, popup->Pos.y,
                                     popup->Pos.y + popup->Size.y);
        };

        // Levels above the device maximum cannot be picked; the current level is not a change.
        const std::optional<ImVec2> sixteen = find("16x");
        CHECK(sixteen.has_value() == (max_anisotropy >= 16.0f));
        CHECK(find("8x").has_value());
        const std::optional<ImVec2> off = find("Off");
        CHECK(off.has_value());
        if (off) {
            probe.click(*off);
            CHECK(state.anisotropy_request == 0 && recorded.calls.empty());
            probe.click(*combo);
        }
        if (g.OpenPopupStack.Size != 1 || g.OpenPopupStack[0].Window == nullptr) return;
        popup = g.OpenPopupStack[0].Window;

        const std::optional<ImVec2> pick = max_anisotropy >= 16.0f ? find("16x") : find("8x");
        CHECK(pick.has_value());
        if (!pick) return;
        probe.click(*pick);
        const std::uint32_t expected = max_anisotropy >= 16.0f ? 16 : 8;
        CHECK(state.anisotropy_request == expected);
        CHECK(recorded.calls.size() == 1);
        if (recorded.calls.size() == 1) {
            CHECK(recorded.calls[0].first == sshd::OverlaySetting::Anisotropy);
            CHECK(recorded.calls[0].second == std::to_string(expected));
        }
        // The request is taken once; the renderer reports the level it applied on a later flip.
        const sshd::PresentOverlayRequests requests = sshd::takePresentRequests(state);
        CHECK(requests.anisotropy == expected);
        CHECK(state.anisotropy_request == 0);
        recorded.calls.clear();
    }
}

void testWindowSizeLabels() {
    constexpr sshd::ClientSize kBig{3840, 2160};
    CHECK(sshd::windowSizeItemLabel(sshd::WindowSizePreset::Size720p, kBig) == "720p (1280x720)");
    CHECK(sshd::windowSizeItemLabel(sshd::WindowSizePreset::Size2160p, kBig) == "2160p (3840x2160)");
    // A preset that exactly fills the available area still fits.
    CHECK(sshd::windowSizeItemLabel(sshd::WindowSizePreset::Size1080p, {1920, 1080}) == "1080p (1920x1080)");
    // Only the presets that do not fit read as clamped.
    constexpr sshd::ClientSize kSmall{2496, 1404};
    CHECK(sshd::windowSizeItemLabel(sshd::WindowSizePreset::Size1440p, kSmall) ==
          "1440p (2560x1440, fits as 2496x1404)");
    CHECK(sshd::windowSizeItemLabel(sshd::WindowSizePreset::Size2160p, kSmall) ==
          "2160p (3840x2160, fits as 2496x1404)");
    CHECK(sshd::windowSizeItemLabel(sshd::WindowSizePreset::Size1080p, kSmall) == "1080p (1920x1080)");
    // Nothing measured yet: no clamp is claimed.
    CHECK(sshd::windowSizeItemLabel(sshd::WindowSizePreset::Size2160p, {}) == "2160p (3840x2160)");

    sshd::WindowStatus status;
    status.preset = sshd::WindowSizePreset::Size1080p;
    status.available = kBig;
    status.client = {1920, 1080};
    CHECK(sshd::windowSizePreviewLabel(status) == "1080p (1920x1080)");

    // The window was dragged to another size.
    status.client = {1600, 900};
    CHECK(sshd::windowSizePreviewLabel(status) == "1080p (window 1600x900)");

    // A clamped preset whose window has the clamped size is not a deviation.
    status.preset = sshd::WindowSizePreset::Size2160p;
    status.available = kSmall;
    status.client = {2496, 1404};
    CHECK(sshd::windowSizePreviewLabel(status) == "2160p (3840x2160, fits as 2496x1404)");
    status.client = {2000, 1125};
    CHECK(sshd::windowSizePreviewLabel(status) == "2160p (window 2000x1125)");

    // Minimised: the empty client area is not a size to show.
    status.client = {};
    CHECK(sshd::windowSizePreviewLabel(status) == "2160p (3840x2160, fits as 2496x1404)");

    // Fullscreen: the preset applies when windowed, whatever the client area is.
    status.fullscreen = true;
    status.preset = sshd::WindowSizePreset::Size1440p;
    status.client = {3840, 2160};
    CHECK(sshd::windowSizePreviewLabel(status) == "1440p (when windowed)");
}

void testWindowSizePanel() {
    SettingCalls recorded;
    sshd::OverlaySources sources = makeFakeSources();
    recorded.attach(sources);
    constexpr float kProbeX = 60.0f;
    constexpr float kWindowBottom = 720.0f;

    {
        // No status reported (no window): the combo is disabled and nothing can be requested.
        sshd::OverlayState state;
        SettingsProbe probe(state, sources);
        if (probe.window() == nullptr) return;
        CHECK(!probe.findHovered(probe.window()->GetID("Window size"), kProbeX, 10.0f, kWindowBottom));
        CHECK(!probe.window()->ScrollbarY && !probe.window()->ScrollbarX);
        CHECK(!state.window_size_request);
        CHECK(recorded.calls.empty());
    }

    // The first state picks another preset, the second the one that is already set.
    for (const bool pick_current : {false, true}) {
        recorded.calls.clear();
        sshd::OverlayState state;
        state.window = sshd::WindowStatus{.preset = sshd::WindowSizePreset::Size720p,
                                          .client = {1280, 720},
                                          .available = {3840, 2160},
                                          .fullscreen = false};
        SettingsProbe probe(state, sources);
        if (probe.window() == nullptr) return;
        const std::optional<ImVec2> combo =
                probe.findHovered(probe.window()->GetID("Window size"), kProbeX, 10.0f, kWindowBottom);
        CHECK(combo.has_value());
        if (!combo) return;
        CHECK(!probe.window()->ScrollbarY && !probe.window()->ScrollbarX);

        probe.click(*combo);
        ImGuiContext& g = *ImGui::GetCurrentContext();
        CHECK(g.OpenPopupStack.Size == 1);
        if (g.OpenPopupStack.Size != 1 || g.OpenPopupStack[0].Window == nullptr) return;
        ImGuiWindow* popup = g.OpenPopupStack[0].Window;
        CHECK(recorded.calls.empty());  // opening the list changes nothing
        CHECK(!state.window_size_request);
        const char* label = pick_current ? "720p (1280x720)" : "1080p (1920x1080)";
        const std::optional<ImVec2> pick = probe.findHovered(popup->GetID(label), popup->Pos.x + 10.0f, popup->Pos.y,
                                                             popup->Pos.y + popup->Size.y);
        CHECK(pick.has_value());
        if (!pick) return;
        probe.click(*pick);
        CHECK(state.window_size_request ==
              (pick_current ? sshd::WindowSizePreset::Size720p : sshd::WindowSizePreset::Size1080p));
        if (pick_current) {
            // Re-applies the size without anything to save.
            CHECK(recorded.calls.empty());
        } else {
            CHECK(recorded.calls.size() == 1);
            if (recorded.calls.size() == 1) {
                CHECK(recorded.calls[0].first == sshd::OverlaySetting::WindowSize);
                CHECK(recorded.calls[0].second == "1080p");
            }
        }
        const sshd::PresentOverlayRequests requests = sshd::takePresentRequests(state);
        CHECK(requests.window_size ==
              (pick_current ? sshd::WindowSizePreset::Size720p : sshd::WindowSizePreset::Size1080p));
        CHECK(!state.window_size_request);
    }
}

void testPresentRequests() {
    sshd::OverlayState state;
    state.dump_requested = true;
    state.resolution_scale_request = 3;
    state.aa_mode_request = sshd::AaMode::Supersample;
    state.anisotropy_request = 4;
    state.window_size_request = sshd::WindowSizePreset::Size1440p;
    state.frame_gen_request = sshd::FrameGenTarget::Fps180;
    state.object_motion_request = false;
    sshd::PresentOverlayRequests requests = sshd::takePresentRequests(state);
    CHECK(requests.object_motion == false);
    CHECK(!state.object_motion_request);
    CHECK(requests.dump_display);
    CHECK(requests.resolution_scale == 3);
    CHECK(requests.aa_mode == sshd::AaMode::Supersample);
    CHECK(requests.anisotropy == 4);
    CHECK(requests.window_size == sshd::WindowSizePreset::Size1440p);
    CHECK(requests.frame_gen == sshd::FrameGenTarget::Fps180);
    CHECK(!state.dump_requested);
    CHECK(state.resolution_scale_request == 0);
    CHECK(!state.aa_mode_request);
    CHECK(state.anisotropy_request == 0);
    CHECK(!state.window_size_request);
    CHECK(!state.frame_gen_request);

    requests = sshd::takePresentRequests(state);
    CHECK(!requests.dump_display);
    CHECK(requests.resolution_scale == 0);
    CHECK(!requests.aa_mode);
    CHECK(requests.anisotropy == 0);
    CHECK(!requests.window_size);
    CHECK(!requests.frame_gen);
    CHECK(!requests.object_motion);

    // A DLAA request is delivered once.
    state.aa_mode_request = sshd::AaMode::Dlaa;
    requests = sshd::takePresentRequests(state);
    CHECK(requests.aa_mode == sshd::AaMode::Dlaa);
    CHECK(!state.aa_mode_request);
    requests = sshd::takePresentRequests(state);
    CHECK(!requests.aa_mode);

    // The reported window status is not a request.
    state.window = sshd::WindowStatus{};
    requests = sshd::takePresentRequests(state);
    CHECK(!requests.window_size);
    CHECK(state.window.has_value());

    // The reported level is a status, not a request.
    state.anisotropy = 8;
    requests = sshd::takePresentRequests(state);
    CHECK(requests.anisotropy == 0);
    CHECK(state.anisotropy == 8);

    // The reported mode is a status, not a request.
    state.aa_mode = sshd::AaMode::Msaa;
    requests = sshd::takePresentRequests(state);
    CHECK(!requests.aa_mode);
    CHECK(state.aa_mode == sshd::AaMode::Msaa);

    // The reported target and status are not requests; Off is a request like any other target.
    state.setFrameGen(
            true, sshd::FrameGenTarget::Fps120,
            sshd::FrameGenStatus{.active = true, .multiplier = 2, .inactive_reason = {}, .max_multiplier = 6});
    requests = sshd::takePresentRequests(state);
    CHECK(!requests.frame_gen);
    CHECK(state.frame_gen_target == sshd::FrameGenTarget::Fps120);
    state.frame_gen_request = sshd::FrameGenTarget::Off;
    requests = sshd::takePresentRequests(state);
    CHECK(requests.frame_gen == sshd::FrameGenTarget::Off);
    requests = sshd::takePresentRequests(state);
    CHECK(!requests.frame_gen);

    // The reported scale is a status, not a request, so taking requests leaves it alone.
    state.resolution_scale = 2;
    requests = sshd::takePresentRequests(state);
    CHECK(requests.resolution_scale == 0);
    CHECK(state.resolution_scale == 2);
}

void testRuntimeSources() {
    const sshd::OverlaySources sources = sshd::makeRuntimeOverlaySources();
    CHECK(sources.audio && sources.unimplemented && sources.unimplemented_dropped && sources.flip_limit &&
          sources.set_flip_limit && sources.log_level && sources.set_log_level && sources.request_capture &&
          sources.capture_chord_down == nullptr && sources.capture_status);
    if (!(sources.audio && sources.unimplemented && sources.unimplemented_dropped && sources.flip_limit &&
          sources.set_flip_limit && sources.log_level && sources.set_log_level && sources.request_capture &&
          sources.capture_status)) {
        return;
    }

    const sshd::RsxFlipLimit limit = sources.flip_limit();
    for (const sshd::RsxFlipLimit other :
         {sshd::RsxFlipLimit::Vsync, sshd::RsxFlipLimit::Hz60, sshd::RsxFlipLimit::Off}) {
        sources.set_flip_limit(other);
        CHECK(sources.flip_limit() == other);
    }
    sources.set_flip_limit(limit);
    CHECK(sources.flip_limit() == limit);

    sshd::rsxUnimplementedReset();
    sshd::rsxNoteUnimplemented("state", "overlay test");
    const auto counts = sources.unimplemented();
    CHECK(counts.size() == 1 && counts[0].first == "state overlay test" && counts[0].second == 1);
    CHECK(sources.unimplemented_dropped() == 0);
    sshd::rsxUnimplementedReset();

    // No audio engine and no RSX thread run in this process.
    CHECK(sources.audio().output.empty());
    CHECK(!sources.capture_status().available);
    CHECK(!sources.request_capture());
    CHECK(!sources.capture_chord_down);

    const sshd::LogLevel level = sources.log_level();
    sources.set_log_level(sshd::LogLevel::Warn);
    CHECK(sources.log_level() == sshd::LogLevel::Warn);
    sources.set_log_level(sshd::LogLevel::Debug);
    CHECK(sources.log_level() == sshd::LogLevel::Debug);
    sources.set_log_level(level);
    CHECK(sources.log_level() == level);
}

// A PresentOverlay that logs its calls into a shared list, to check the stack's order and merging.
class FakeOverlay final : public sshd::PresentOverlay {
public:
    FakeOverlay(int id, std::vector<std::string>& log, bool wants, sshd::PresentOverlayRequests requests = {})
        : id_(id), log_(log), wants_(wants), requests_(std::move(requests)) {}

    sshd::PresentOverlayRequests onFlip(const sshd::PresentFrameInfo&) override {
        log_.push_back("flip" + std::to_string(id_));
        return requests_;
    }
    bool wantsDraw() const override { return wants_; }
    bool wantsIdlePresent() const override { return wants_idle_; }
    void setWantsIdlePresent(bool wants) { wants_idle_ = wants; }
    void record(VkCommandBuffer, const sshd::Presenter::Target&) override {
        log_.push_back("record" + std::to_string(id_));
    }

private:
    int id_;
    std::vector<std::string>& log_;
    bool wants_;
    bool wants_idle_ = false;
    sshd::PresentOverlayRequests requests_;
};

void testOverlayStack() {
    std::vector<std::string> log;
    sshd::OverlayStack empty;
    CHECK(!empty.wantsDraw());
    CHECK(!empty.onFlip({}).dump_display);
    empty.record(VK_NULL_HANDLE, {});
    empty.add(nullptr);
    CHECK(!empty.wantsDraw());

    sshd::PresentOverlayRequests low;
    low.resolution_scale = 2;
    low.anisotropy = 4;
    low.aa_mode = sshd::AaMode::Msaa;
    low.window_size = sshd::WindowSizePreset::Size720p;
    sshd::PresentOverlayRequests high;
    high.dump_display = true;
    high.resolution_scale = 3;
    high.frame_gen = sshd::FrameGenTarget::Fps120;
    high.object_motion = false;
    FakeOverlay first(1, log, false, low);
    FakeOverlay second(2, log, true, high);
    FakeOverlay third(3, log, false);
    sshd::OverlayStack stack;
    stack.add(&first);
    stack.add(&second);
    stack.add(&third);

    // Every child sees the flip, in insertion order. A later child's field wins; unset fields keep the earlier one.
    const sshd::PresentOverlayRequests merged = stack.onFlip({});
    CHECK((log == std::vector<std::string>{"flip1", "flip2", "flip3"}));
    CHECK(merged.dump_display);
    CHECK(merged.resolution_scale == 3);
    CHECK(merged.anisotropy == 4);
    CHECK(merged.aa_mode == sshd::AaMode::Msaa);
    CHECK(merged.window_size == sshd::WindowSizePreset::Size720p);
    CHECK(merged.frame_gen == sshd::FrameGenTarget::Fps120);
    CHECK(merged.object_motion == false);

    // wantsDraw is true when any child wants; record runs only the children that do.
    CHECK(stack.wantsDraw());
    log.clear();
    stack.record(VK_NULL_HANDLE, {});
    CHECK((log == std::vector<std::string>{"record2"}));

    std::vector<std::string> log_all;
    FakeOverlay a(1, log_all, true);
    FakeOverlay b(2, log_all, false);
    FakeOverlay c(3, log_all, true);
    sshd::OverlayStack both;
    both.add(&a);
    both.add(&b);
    both.add(&c);
    both.record(VK_NULL_HANDLE, {});
    CHECK((log_all == std::vector<std::string>{"record1", "record3"}));

    std::vector<std::string> log_none;
    FakeOverlay idle(1, log_none, false);
    sshd::OverlayStack none;
    none.add(&idle);
    CHECK(!none.wantsDraw());
    none.record(VK_NULL_HANDLE, {});
    CHECK(log_none.empty());

    // wantsIdlePresent is independent of wantsDraw and true when any child wants it.
    CHECK(!empty.wantsIdlePresent());
    CHECK(!none.wantsIdlePresent());
    CHECK(!stack.wantsIdlePresent());
    third.setWantsIdlePresent(true);
    CHECK(stack.wantsIdlePresent());
    third.setWantsIdlePresent(false);
    first.setWantsIdlePresent(true);
    CHECK(stack.wantsIdlePresent());
    first.setWantsIdlePresent(false);
    CHECK(!stack.wantsIdlePresent());
}

void testImGuiContextScope() {
    ImGuiContext* a = ImGui::CreateContext();
    ImGuiContext* b = ImGui::CreateContext();
    ImGui::SetCurrentContext(nullptr);

    {
        sshd::ImGuiContextScope outer(a);
        CHECK(ImGui::GetCurrentContext() == a);
        {
            // Nesting on one thread must not deadlock (recursive lock) and unwinds one level at a time.
            sshd::ImGuiContextScope inner(b);
            CHECK(ImGui::GetCurrentContext() == b);
            {
                sshd::ImGuiContextScope none(nullptr);
                CHECK(ImGui::GetCurrentContext() == nullptr);
            }
            CHECK(ImGui::GetCurrentContext() == b);
        }
        CHECK(ImGui::GetCurrentContext() == a);
    }
    CHECK(ImGui::GetCurrentContext() == nullptr);

    // A non-null previous context comes back as well.
    ImGui::SetCurrentContext(b);
    {
        sshd::ImGuiContextScope scope(a);
        CHECK(ImGui::GetCurrentContext() == a);
    }
    CHECK(ImGui::GetCurrentContext() == b);
    ImGui::SetCurrentContext(nullptr);

    // While one thread holds a scope, another thread's scope blocks until it ends.
    // The thread does not use CHECK, whose counters are not atomic.
    std::atomic<bool> second_entered{false};
    std::atomic<ImGuiContext*> second_context{nullptr};
    std::thread other;
    {
        sshd::ImGuiContextScope held(a);
        other = std::thread([&] {
            sshd::ImGuiContextScope scope(b);
            second_context.store(ImGui::GetCurrentContext());
            second_entered.store(true);
        });
        // Several short waits (50 ms in total) are enough for a missing lock to show as an early entry.
        for (int i = 0; i < 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(!second_entered.load());
        CHECK(ImGui::GetCurrentContext() == a);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!second_entered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!second_entered.load()) {
        // Joining a thread that is still blocked would hang the test run.
        std::fprintf(stderr, "check failed (line %d): second thread never entered its scope\n", __LINE__);
        std::_Exit(1);
    }
    other.join();
    CHECK(second_context.load() == b);
    CHECK(ImGui::GetCurrentContext() == nullptr);

    ImGui::DestroyContext(a);
    ImGui::DestroyContext(b);
}

std::filesystem::path exePath() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n));
#else
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // fails and reports the needed size
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::strlen(path.c_str()));
    return path;
#endif
}

// A system font with Latin glyphs only, or empty when the system has none at the usual place.
std::filesystem::path latinOnlySystemFont() {
#if defined(_WIN32)
    wchar_t windir[MAX_PATH];
    const UINT windir_len = GetWindowsDirectoryW(windir, MAX_PATH);
    return windir_len > 0 ? std::filesystem::path(windir) / "Fonts" / "arial.ttf" : std::filesystem::path();
#else
    return "/System/Library/Fonts/Supplemental/Arial.ttf";
#endif
}

void testUiFontCandidates() {
    namespace fs = std::filesystem;
    const auto c = sshd::uiFontCandidates("custom.ttf", "libsre", "elf", "exe");
    CHECK(c.size() == 4);
    if (c.size() == 4) {
        CHECK(c[0] == fs::path("custom.ttf"));
        CHECK(c[1] == fs::path("libsre") / "SCE-PS3-SR-R-JPN.TTF");
        CHECK(c[2] == fs::path("elf") / "SCE-PS3-SR-R-JPN.TTF");
        CHECK(c[3] == fs::path("exe") / "fonts" / "BIZUDPGothic-Regular.ttf");
    }
    const auto no_explicit = sshd::uiFontCandidates({}, "libsre", "elf", "exe");
    CHECK(no_explicit.size() == 3);
    if (no_explicit.size() == 3) CHECK(no_explicit[0] == fs::path("libsre") / "SCE-PS3-SR-R-JPN.TTF");
}

void testLoadUiFont() {
    namespace fs = std::filesystem;
    const fs::path missing = exePath().parent_path() / "no-such-font.ttf";
    const fs::path not_a_font = exePath();  // an executable is readable but not a font
    const fs::path bundled = exePath().parent_path() / "fonts" / "BIZUDPGothic-Regular.ttf";

    const std::vector<fs::path> junk = {missing, not_a_font};
    CHECK(!sshd::loadUiFont(junk).has_value());

    const fs::path arial = latinOnlySystemFont();
    std::error_code ec;
    if (!arial.empty() && fs::exists(arial, ec)) {
        const std::vector<fs::path> latin_only = {arial};
        CHECK(!sshd::loadUiFont(latin_only).has_value());
    } else {
        std::printf("overlay tests: ui font: arial.ttf check skipped (not found)\n");
    }

    if (fs::exists(bundled, ec)) {
        const std::vector<fs::path> list = {missing, not_a_font, arial, bundled};
        const auto font = sshd::loadUiFont(list);
        CHECK(font.has_value());
        if (font) {
            CHECK(font->path == bundled);
            CHECK(font->bytes.size() > 1000);
        }
    } else {
        std::printf("overlay tests: ui font: BIZ UDPGothic check skipped (%s not found)\n", bundled.string().c_str());
    }
}

void testDialogCodePoints() {
    sshd::SaveDialogContent content;
    content.kind = sshd::DialogKind::List;
    sshd::SaveDialogRow row;
    row.title = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 \xE9\x9B\xA3";  // "テスト 難"
    row.info = "2026/10/18  12:34   5 KB";
    content.rows.push_back(row);
    content.message = "line1\nline2";
    const auto points = sshd::dialogCodePoints(content);

    bool ascii = true;
    for (ImWchar c = 0x20; c <= 0x7E; ++c) ascii = ascii && points.count(c) == 1;
    CHECK(ascii);
    CHECK(points.count(0x30BB) == 1);  // セ
    CHECK(points.count(0x96E3) == 1);  // 難, only in the row title
    CHECK(points.count(0x30C6) == 1);  // テ
    CHECK(points.count('\n') == 0);

    // Every code point of a constant, including one that is not in the content.
    ImFontGlyphRangesBuilder probe;
    probe.AddText(sshd::kConfirmOverwrite.data(), sshd::kConfirmOverwrite.data() + sshd::kConfirmOverwrite.size());
    ImVector<ImWchar> expected;
    probe.BuildRanges(&expected);
    bool all = expected.Size > 1;
    for (int i = 0; i + 1 < expected.Size && expected[i] != 0; i += 2) {
        for (ImWchar c = expected[i]; c <= expected[i + 1]; ++c) all = all && points.count(c) == 1;
    }
    CHECK(all);
}

void testGlyphRanges() {
    const ImVector<ImWchar> ranges = sshd::glyphRanges({'A', 'B', 'C', 0x30BB});
    CHECK(ranges.Size == 5);
    if (ranges.Size == 5) {
        CHECK(ranges[0] == 'A');
        CHECK(ranges[1] == 'C');
        CHECK(ranges[2] == 0x30BB);
        CHECK(ranges[3] == 0x30BB);
        CHECK(ranges[4] == 0);
    }
}

constexpr bool kKeyDown = true;
constexpr bool kKeyUp = false;

#if defined(_WIN32)
// Raw Win32 key message values, so these helpers need no windows.h.
constexpr std::uint32_t kWmKeyDown = 0x0100;
constexpr std::uint32_t kWmKeyUp = 0x0101;
constexpr std::int64_t kRepeatBit = std::int64_t{1} << 30;

sshd::WindowMessage keyMessage(bool down, std::uint32_t scan_code, bool repeat = false) {
    sshd::WindowMessage result;
    result.message = down ? kWmKeyDown : kWmKeyUp;
    result.wparam = 0x70;  // the virtual key is only a fallback when the scan code is zero
    result.lparam = static_cast<std::int64_t>(scan_code) << 16 | (repeat ? kRepeatBit : 0);
    return result;
}
#else
sshd::WindowMessage keyMessage(bool down, std::uint32_t scan_code, bool repeat = false) {
    sshd::WindowMessage result;
    result.kind = sshd::WindowEventKind::Key;
    result.key = sshd::KeyMessage{scan_code, down, repeat};
    return result;
}
#endif

// The overlay needs a VkContext only to be constructed; the key path touches neither Vulkan nor ImGui.
void testToggleKey(sshd::VkContext& ctx) {
    sshd::DebugOverlay overlay(ctx, sshd::OverlaySources{});
    const std::uint32_t f1 = sshd::kOverlayToggleScanCode;
    CHECK(!overlay.visible() && !overlay.wantsDraw());

    sshd::MessageRoute route = overlay.onWindowMessage(keyMessage(kKeyDown, f1));
    CHECK(overlay.visible() && overlay.wantsDraw() && route.withhold_key && !route.handled);

    // Auto-repeat neither toggles nor reaches the pad.
    route = overlay.onWindowMessage(keyMessage(kKeyDown, f1, true));
    CHECK(overlay.visible() && route.withhold_key);
    route = overlay.onWindowMessage(keyMessage(kKeyUp, f1));
    CHECK(overlay.visible() && route.withhold_key);

    route = overlay.onWindowMessage(keyMessage(kKeyDown, f1));
    CHECK(!overlay.visible() && route.withhold_key);
    overlay.onWindowMessage(keyMessage(kKeyUp, f1));

    // Other keys are never withheld here: the overlay is hidden or has no window input yet.
    constexpr std::uint32_t kScanZ = 0x2C;
    CHECK(!overlay.onWindowMessage(keyMessage(kKeyDown, kScanZ)).withhold_key);
    CHECK(!overlay.onWindowMessage(keyMessage(kKeyUp, kScanZ)).withhold_key);
    overlay.setVisible(true);
    CHECK(!overlay.onWindowMessage(keyMessage(kKeyDown, kScanZ)).withhold_key);
    CHECK(!overlay.onWindowMessage(keyMessage(kKeyUp, kScanZ)).withhold_key);
}

constexpr std::uint32_t kTargetWidth = 1280;
constexpr std::uint32_t kTargetHeight = 720;

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// An offscreen stand-in for a swapchain image, with a host buffer to read it back.
class OffscreenTarget {
public:
    explicit OffscreenTarget(sshd::VkContext& ctx) : ctx_(ctx) {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_info.extent = {kTargetWidth, kTargetHeight, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo image_alloc_info{};
        image_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ok_ = vmaCreateImage(ctx.allocator(), &image_info, &image_alloc_info, &image_, &image_alloc_, nullptr) ==
              VK_SUCCESS;

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok_ = ok_ && vkCreateImageView(ctx.device(), &view_info, nullptr, &view_) == VK_SUCCESS;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = static_cast<VkDeviceSize>(kTargetWidth) * kTargetHeight * 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo buffer_alloc_info{};
        buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        buffer_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        ok_ = ok_ && vmaCreateBuffer(ctx.allocator(), &buffer_info, &buffer_alloc_info, &buffer_, &buffer_alloc_,
                                     &mapped) == VK_SUCCESS;
        pixels_ = static_cast<const std::uint8_t*>(mapped.pMappedData);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = ctx.graphicsQueueFamily();
        ok_ = ok_ && vkCreateCommandPool(ctx.device(), &pool_info, nullptr, &pool_) == VK_SUCCESS;
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.commandPool = pool_;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        ok_ = ok_ && vkAllocateCommandBuffers(ctx.device(), &cmd_info, &cmd_) == VK_SUCCESS;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        ok_ = ok_ && vkCreateFence(ctx.device(), &fence_info, nullptr, &fence_) == VK_SUCCESS;
        ok_ = ok_ && pixels_ != nullptr;
    }

    ~OffscreenTarget() {
        vkDeviceWaitIdle(ctx_.device());
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(ctx_.device(), fence_, nullptr);
        if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(ctx_.device(), pool_, nullptr);
        if (buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(ctx_.allocator(), buffer_, buffer_alloc_);
        if (view_ != VK_NULL_HANDLE) vkDestroyImageView(ctx_.device(), view_, nullptr);
        if (image_ != VK_NULL_HANDLE) vmaDestroyImage(ctx_.allocator(), image_, image_alloc_);
    }

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    bool ok() const { return ok_; }

    // One submit that follows the renderer's present path: clear to `clear` (the blit), the barriers around
    // the overlay, then a copy into the host buffer instead of the present.
    bool drawOnce(sshd::PresentOverlay& overlay, const sshd::RendererStats& stats, VkClearColorValue clear = {}) {
        sshd::Presenter::Target target;
        target.image = image_;
        target.view = view_;
        target.format = VK_FORMAT_B8G8R8A8_UNORM;
        target.width = kTargetWidth;
        target.height = kTargetHeight;

        vkResetCommandBuffer(cmd_, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &begin);

        imageBarrier(cmd_, image_, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        sshd::PresentFrameInfo info;
        info.stats = &stats;
        overlay.onFlip(info);
        imageBarrier(cmd_, image_, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        overlay.record(cmd_, target);
        imageBarrier(cmd_, image_, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kTargetWidth, kTargetHeight, 1};
        vkCmdCopyImageToBuffer(cmd_, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &region);
        VkMemoryBarrier2 host_barrier{};
        host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &host_barrier;
        vkCmdPipelineBarrier2(cmd_, &dependency);
        if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;

        VkCommandBufferSubmitInfo cmd_submit{};
        cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit.commandBuffer = cmd_;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_submit;
        if (vkQueueSubmit2(ctx_.graphicsQueue(), 1, &submit, fence_) != VK_SUCCESS) return false;
        const bool done = vkWaitForFences(ctx_.device(), 1, &fence_, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS;
        vkResetFences(ctx_.device(), 1, &fence_);
        vmaInvalidateAllocation(ctx_.allocator(), buffer_alloc_, 0, VK_WHOLE_SIZE);
        return done;
    }

    // The pixel's channels in memory order (B, G, R, A); the target is B8G8R8A8.
    struct Pixel {
        std::uint8_t b, g, r, a;
        int minChannel() const { return std::min({b, g, r}); }
        int maxChannel() const { return std::max({b, g, r}); }
    };
    Pixel pixel(std::uint32_t x, std::uint32_t y) const {
        const std::uint8_t* p = pixels_ + (static_cast<std::size_t>(y) * kTargetWidth + x) * 4;
        return {p[0], p[1], p[2], p[3]};
    }

    // Writes the readback as an RGB PNG.
    bool savePng(const std::filesystem::path& path, std::string& error) const {
        const std::span<const std::uint8_t> bgra(pixels_, static_cast<std::size_t>(kTargetWidth) * kTargetHeight * 4);
        return sshd::writePngRgb(path, sshd::rgbFromBgra(bgra, kTargetWidth, kTargetHeight), error);
    }

    // True when any pixel of the rectangle (inclusive) has a non-zero colour channel.
    bool anyLit(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) const {
        for (std::uint32_t y = y0; y <= y1; ++y) {
            for (std::uint32_t x = x0; x <= x1; ++x) {
                const std::uint8_t* p = pixels_ + (static_cast<std::size_t>(y) * kTargetWidth + x) * 4;
                if (p[0] != 0 || p[1] != 0 || p[2] != 0) return true;
            }
        }
        return false;
    }

private:
    sshd::VkContext& ctx_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation image_alloc_ = nullptr;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation buffer_alloc_ = nullptr;
    const std::uint8_t* pixels_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool ok_ = false;
};

// Records the overlay over two consecutive submits, then checks that the window's title bar is lit and
// the opposite corner untouched. The probes follow the panel's window position (10, 10) and width (440).
void drawTwice(sshd::VkContext& ctx, OffscreenTarget& target) {
    sshd::DebugOverlay overlay(ctx, makeFakeSources());
    overlay.attachWindow(nullptr);
    overlay.setVisible(true);
    CHECK(overlay.wantsDraw());

    sshd::RendererStats stats;
    stats.flips = 1;
    stats.draws = 10;
    CHECK(target.drawOnce(overlay, stats));
    CHECK(!overlay.failed());
    // ImGui keeps a new window hidden for its first frame, so the title bar shows from the second one.
    CHECK(!target.anyLit(1270, 710, 1270, 710));

    stats.flips = 2;
    CHECK(target.drawOnce(overlay, stats));
    CHECK(!overlay.failed());
    CHECK(target.anyLit(10, 10, 200, 40));
    CHECK(!target.anyLit(1270, 710, 1270, 710));
}

#if defined(_WIN32)
sshd::DebugOverlay* g_forward_overlay = nullptr;
WNDPROC g_forward_previous = nullptr;
int g_forwarded_capture_changes = 0;

// Routes messages to the overlay first, as the game window's procedure does.
LRESULT CALLBACK forwardingProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_forward_overlay != nullptr) {
        if (message == WM_CAPTURECHANGED) g_forwarded_capture_changes++;
        const sshd::MessageRoute route = g_forward_overlay->onWindowMessage(
                {hwnd, message, static_cast<std::uint64_t>(wparam), static_cast<std::int64_t>(lparam)});
        if (route.handled) return static_cast<LRESULT>(route.result);
    }
    return CallWindowProcW(g_forward_previous, hwnd, message, wparam, lparam);
}

// Feeds the overlay Win32 input through a hidden window: the message hook has to reach the ImGui context
// that record() created, with the real backend initialised for that window.
void testWin32Input(sshd::VkContext& ctx, OffscreenTarget& target) {
    HWND hwnd =
            CreateWindowExW(0, L"STATIC", L"overlay test", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    CHECK(hwnd != nullptr);
    if (hwnd == nullptr) return;
    {
        sshd::DebugOverlay overlay(ctx, makeFakeSources());
        overlay.attachWindow(hwnd);
        overlay.setVisible(true);
        sshd::RendererStats stats;
        stats.flips = 1;
        CHECK(target.drawOnce(overlay, stats));
        CHECK(!overlay.failed());

        sshd::WindowMessage move;
        move.hwnd = hwnd;
        move.message = WM_MOUSEMOVE;
        move.lparam = (15 << 16) | 20;
        overlay.onWindowMessage(move);
        // WantCaptureMouse comes from the window hovered in the previous frame, so it lags by one frame.
        stats.flips = 2;
        CHECK(target.drawOnce(overlay, stats));
        stats.flips = 3;
        CHECK(target.drawOnce(overlay, stats));
        {
            sshd::ImGuiContextScope scope(overlay.imguiContext());
            CHECK(ImGui::GetIO().WantCaptureMouse);
        }

        // Hiding and showing again must leave ImGui focused, or it would drop all input.
        overlay.onWindowMessage(keyMessage(kKeyDown, sshd::kOverlayToggleScanCode));
        overlay.onWindowMessage(keyMessage(kKeyUp, sshd::kOverlayToggleScanCode));
        CHECK(!overlay.visible());
        overlay.onWindowMessage(keyMessage(kKeyDown, sshd::kOverlayToggleScanCode));
        overlay.onWindowMessage(keyMessage(kKeyUp, sshd::kOverlayToggleScanCode));
        CHECK(overlay.visible());
        overlay.onWindowMessage(move);
        stats.flips = 4;
        CHECK(target.drawOnce(overlay, stats));
        {
            sshd::ImGuiContextScope scope(overlay.imguiContext());
            CHECK(ImGui::GetIO().WantCaptureMouse);
        }

        // A click goes through the window procedure like in the game. The button-up makes the handler call
        // ReleaseCapture, which sends WM_CAPTURECHANGED to the same window procedure before the handler returns.
        g_forward_overlay = &overlay;
        const LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&forwardingProc));
        g_forward_previous = reinterpret_cast<WNDPROC>(previous);
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, (15 << 16) | 20);
        CHECK(GetCapture() == hwnd);
        SendMessageW(hwnd, WM_LBUTTONUP, 0, (15 << 16) | 20);
        CHECK(GetCapture() == nullptr);
        CHECK(g_forwarded_capture_changes > 0);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, previous);
        g_forward_overlay = nullptr;
        stats.flips = 5;
        CHECK(target.drawOnce(overlay, stats));
    }
    DestroyWindow(hwnd);
}
#endif

// Destroying an overlay while its own context is current must leave no freed context current afterwards.
void testShutdownInsideScope(sshd::VkContext& ctx, OffscreenTarget& target) {
    auto overlay = std::make_unique<sshd::DebugOverlay>(ctx, makeFakeSources());
    overlay->attachWindow(nullptr);
    overlay->setVisible(true);
    sshd::RendererStats stats;
    stats.flips = 1;
    CHECK(target.drawOnce(*overlay, stats));
    CHECK(overlay->imguiContext() != nullptr);
    {
        sshd::ImGuiContextScope scope(overlay->imguiContext());
        overlay.reset();
        CHECK(ImGui::GetCurrentContext() == nullptr);
    }
    CHECK(ImGui::GetCurrentContext() == nullptr);
}

std::shared_ptr<const sshd::SaveDialogContent> messageContent(std::uint64_t id, sshd::DialogKind kind,
                                                              std::string message) {
    auto content = std::make_shared<sshd::SaveDialogContent>();
    content->id = id;
    content->kind = kind;
    content->message = std::move(message);
    return content;
}

void publishDialog(sshd::SaveDialogBoard& board, std::shared_ptr<const sshd::SaveDialogContent> content,
                   float opacity) {
    sshd::SaveDialogSnapshot snapshot;
    snapshot.content = std::move(content);
    snapshot.opacity = opacity;
    board.publish(std::move(snapshot));
}

// True when a pixel of the rectangle (inclusive) is lighter than the dimmed white background (at most 38).
bool anyBright(const OffscreenTarget& target, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    for (std::uint32_t y = y0; y <= y1; ++y) {
        for (std::uint32_t x = x0; x <= x1; ++x) {
            if (target.pixel(x, y).minChannel() > 120) return true;
        }
    }
    return false;
}

// True when a pixel of the rectangle (inclusive) is lighter than the dim in any channel; unlike anyBright this also
// finds the saturated colours of the button symbols.
bool anyAboveDim(const OffscreenTarget& target, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                 std::uint32_t y1) {
    for (std::uint32_t y = y0; y <= y1; ++y) {
        for (std::uint32_t x = x0; x <= x1; ++x) {
            if (target.pixel(x, y).maxChannel() > 130) return true;
        }
    }
    return false;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < t.size(); ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void appendBigEndian(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendPngChunk(std::vector<std::uint8_t>& out, const char (&type)[5], const std::vector<std::uint8_t>& data) {
    appendBigEndian(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    appendBigEndian(out, crc32(body.data(), body.size()));
}

using PngPixelFn = std::array<std::uint8_t, 3> (*)(std::uint32_t x, std::uint32_t y);

// An 8-bit RGB PNG whose zlib stream holds stored blocks, so the test needs no compressor.
std::shared_ptr<const std::vector<std::uint8_t>> makePng(std::uint32_t width, std::uint32_t height, PngPixelFn pixel) {
    std::vector<std::uint8_t> raw;
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::array<std::uint8_t, 3> rgb = pixel(x, y);
            raw.insert(raw.end(), rgb.begin(), rgb.end());
        }
    }
    std::vector<std::uint8_t> zlib = {0x78, 0x01};
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;) {
        const std::size_t length = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + length >= raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<std::uint8_t>(length));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8));
        zlib.push_back(static_cast<std::uint8_t>(~length));
        zlib.push_back(static_cast<std::uint8_t>((~length) >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                    raw.begin() + static_cast<std::ptrdiff_t>(pos + length));
        pos += length;
        if (last) break;
    }
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    appendBigEndian(zlib, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> header;
    appendBigEndian(header, width);
    appendBigEndian(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bit, RGB, deflate, no filter variants, no interlace
    appendPngChunk(png, "IHDR", header);
    appendPngChunk(png, "IDAT", zlib);
    appendPngChunk(png, "IEND", {});
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(png));
}

constexpr std::uint32_t kIconPngWidth = 320;
constexpr std::uint32_t kIconPngHeight = 176;

std::shared_ptr<const std::vector<std::uint8_t>> redIconPng() {
    return makePng(kIconPngWidth, kIconPngHeight,
                   [](std::uint32_t, std::uint32_t) { return std::array<std::uint8_t, 3>{255, 0, 0}; });
}

// A diagonal colour ramp with a lighter frame, for looking at the render.
std::shared_ptr<const std::vector<std::uint8_t>> gradientIconPng() {
    return makePng(kIconPngWidth, kIconPngHeight, [](std::uint32_t x, std::uint32_t y) {
        if (x < 4 || y < 4 || x >= kIconPngWidth - 4 || y >= kIconPngHeight - 4) {
            return std::array<std::uint8_t, 3>{235, 235, 235};
        }
        return std::array<std::uint8_t, 3>{
                static_cast<std::uint8_t>(x * 255 / kIconPngWidth), static_cast<std::uint8_t>(y * 255 / kIconPngHeight),
                static_cast<std::uint8_t>(255 - (x + y) * 255 / (kIconPngWidth + kIconPngHeight))};
    });
}

// `icon` goes to every row except each third one, which keeps its placeholder.
std::shared_ptr<sshd::SaveDialogContent> listContent(std::uint64_t id, int row_count,
                                                     std::shared_ptr<const std::vector<std::uint8_t>> icon = nullptr) {
    auto content = std::make_shared<sshd::SaveDialogContent>();
    content->id = id;
    content->kind = sshd::DialogKind::List;
    content->list_kind = sshd::SaveListKind::Save;
    for (int i = 0; i < row_count; ++i) {
        sshd::SaveDialogRow row;
        row.new_data = i == 0;
        if (i % 3 != 2) row.icon_png = icon;
        row.title =
                row.new_data
                        ? "\xE6\x96\xB0\xE3\x81\x97\xE3\x81\x84\xE3\x83\x87\xE3\x83\xBC\xE3\x82\xBF"  // 新しいデータ
                        : "Stage " + std::to_string(i) + " \xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96";     // セーブ
        row.info = row.new_data
                           ? "\xE6\x96\xB0\xE3\x81\x97\xE3\x81\x84\xE3\x83\x87\xE3\x83\xBC\xE3\x82\xBF\xE3\x82\x92"
                             "\xE4\xBD\x9C\xE6\x88\x90\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x99"  // 新しいデータを作成します
                           : "2026/10/" + std::to_string(10 + i) + " 21:" + std::to_string(30 + i) + "   " +
                                     std::to_string(120 + i * 7) + " KB";
        // Three detail lines like the game's, and one row with a fourth that the row cuts with "...".
        if (!row.new_data && i % 2 == 1) {
            row.detail =
                    "走り出す運命\nルドガー　Ｌｖ　" + std::to_string(i) + "\nプレイ時間　0：0" + std::to_string(i);
        } else if (i == 2) {
            row.detail = "Chapter 2\nAfter the harbour\nThe long road to the capital\nA fourth line is cut";
        }
        content->rows.push_back(std::move(row));
    }
    return content;
}

// Set SSHD_OVERLAY_TEST_LIST_PNG to a file path to keep the render of a 7-row list for looking at.
void saveListRenderIfRequested(const OffscreenTarget& target) {
#if defined(_WIN32)
    char path[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableA("SSHD_OVERLAY_TEST_LIST_PNG", path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) return;
#else
    const char* path = std::getenv("SSHD_OVERLAY_TEST_LIST_PNG");
    if (path == nullptr || *path == '\0') return;
#endif
    std::string error;
    const bool saved = target.savePng(path, error);
    std::printf("overlay tests: list render %s %s\n", saved ? "saved to" : "not saved:", saved ? path : error.c_str());
}

void testSaveListLayout(sshd::SaveDialogBoard& board, sshd::SaveDialogOverlay& overlay, OffscreenTarget& target,
                        const sshd::RendererStats& stats) {
    constexpr VkClearColorValue kWhite = {{1.0f, 1.0f, 1.0f, 1.0f}};

    // Seven rows of which four show, the second one selected: the dim, the heading, the focus border and the scrollbar
    // thumb.
    sshd::SaveDialogSnapshot snapshot;
    snapshot.content = listContent(10, 7, gradientIconPng());
    snapshot.selected_row = 1;
    snapshot.first_visible_row = 0;
    snapshot.opacity = 1.0f;
    board.publish(snapshot);
    CHECK(target.drawOnce(overlay, stats, kWhite));
    CHECK(!overlay.failed());
    CHECK(target.pixel(4, 700).maxChannel() <= 115);
    CHECK(target.pixel(4, 700).minChannel() > 50);
    const OffscreenTarget::Pixel focus = target.pixel(210, 112 + 130 + 65);
    CHECK(focus.b >= focus.r + 60);
    CHECK(anyBright(target, 64, 36, 200, 72));
    const OffscreenTarget::Pixel thumb = target.pixel(1085, 114);
    const OffscreenTarget::Pixel track = target.pixel(1085, 620);
    CHECK(thumb.minChannel() > track.minChannel());
    CHECK(track.minChannel() > 102);                 // the track is lighter than the plain dim
    CHECK(anyAboveDim(target, 184, 662, 200, 678));  // the enter symbol
    CHECK(anyAboveDim(target, 64, 662, 80, 678));    // the back symbol
    saveListRenderIfRequested(target);

    // Scrolled to the end: the thumb is at the bottom of the track.
    snapshot.selected_row = 6;
    snapshot.first_visible_row = 3;
    board.publish(snapshot);
    CHECK(target.drawOnce(overlay, stats, kWhite));
    CHECK(target.pixel(1085, 114).minChannel() == track.minChannel());
    CHECK(target.pixel(1085, 628).minChannel() > track.minChannel());

    // Three rows fit: no scrollbar.
    snapshot.content = listContent(11, 3);
    snapshot.selected_row = 0;
    snapshot.first_visible_row = 0;
    board.publish(snapshot);
    CHECK(target.drawOnce(overlay, stats, kWhite));
    CHECK(target.pixel(1085, 114).maxChannel() <= 115);
    CHECK(target.pixel(1085, 300).maxChannel() <= 115);

    // An empty list shows only the back item.
    snapshot.content = listContent(12, 0);
    board.publish(snapshot);
    CHECK(target.drawOnce(overlay, stats, kWhite));
    CHECK(target.pixel(4, 700).maxChannel() <= 115);
    CHECK(!anyAboveDim(target, 184, 662, 200, 678));
    CHECK(anyAboveDim(target, 64, 662, 80, 678));
    CHECK(anyBright(target, 400, 350, 880, 390));  // the "no data" line in the middle of the list box
}

bool isRed(const OffscreenTarget::Pixel& pixel) { return pixel.r > 200 && pixel.g < 60 && pixel.b < 60; }

// The icon cells of rows 0 and 1 of a list that shows its first page (canvas == target pixels).
constexpr std::uint32_t kIconCellX = 220;
constexpr std::uint32_t kIconCellWidth = 160;
constexpr std::uint32_t kIconCellHeight = 88;
std::uint32_t iconCellY(std::uint32_t row) { return 112 + 130 * row + 8; }

void testSaveDialogIcons(sshd::VkContext& ctx, OffscreenTarget& target) {
    constexpr VkClearColorValue kWhite = {{1.0f, 1.0f, 1.0f, 1.0f}};
    constexpr std::size_t kFrames = sshd::RsxRenderer::kFramesInFlight;
    sshd::RendererStats stats;
    stats.flips = 1;

    sshd::SaveDialogBoard board;
    sshd::SaveDialogOverlayOptions options;
    options.font_candidates.push_back(exePath().parent_path() / "fonts" / "BIZUDPGothic-Regular.ttf");
    sshd::SaveDialogOverlay overlay(ctx, board, options);

    const auto publish = [&](std::shared_ptr<sshd::SaveDialogContent> content, int selected, int first) {
        sshd::SaveDialogSnapshot snapshot;
        snapshot.content = std::move(content);
        snapshot.selected_row = selected;
        snapshot.first_visible_row = first;
        snapshot.opacity = 1.0f;
        board.publish(std::move(snapshot));
    };
    const auto draw = [&] {
        const bool drawn = target.drawOnce(overlay, stats, kWhite);
        CHECK(drawn);
        CHECK(!overlay.failed());
    };

    // A red icon shows within two records, a bad PNG leaves the placeholder, and a row without an icon too.
    auto content = std::make_shared<sshd::SaveDialogContent>();
    content->id = 30;
    content->kind = sshd::DialogKind::List;
    content->rows.resize(3);
    content->rows[0].icon_png = redIconPng();
    content->rows[1].icon_png =
            std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{1, 2, 3, 4});
    publish(content, 2, 0);
    draw();
    draw();
    const std::uint32_t red_x = kIconCellX + kIconCellWidth / 2;
    CHECK(isRed(target.pixel(red_x, iconCellY(0) + kIconCellHeight / 2)));
    CHECK(isRed(target.pixel(kIconCellX + 2, iconCellY(0) + 2)));  // the 160x88 cell is filled by the 320x176 image
    CHECK(isRed(target.pixel(kIconCellX + kIconCellWidth - 3, iconCellY(0) + kIconCellHeight - 3)));
    CHECK(overlay.iconTextureCount() == 1);
    // The placeholder: its fill lifts the dim (102 over white) a little and its border more.
    for (const std::uint32_t row : {1u, 2u}) {
        const OffscreenTarget::Pixel inside = target.pixel(red_x, iconCellY(row) + kIconCellHeight / 2);
        CHECK(!isRed(inside) && inside.minChannel() >= 105 && inside.maxChannel() <= 125);
        CHECK(target.pixel(kIconCellX, iconCellY(row) + kIconCellHeight / 2).minChannel() > 135);
    }
    // A failed row is not decoded again, whatever the number of records.
    for (int i = 0; i < 3; ++i) draw();
    CHECK(overlay.iconTextureCount() == 1);

    // 40 rows scrolled through, in single steps and in jumps over the whole window: never more than 32 textures,
    // counting the ones that wait for their frames.
    content = std::make_shared<sshd::SaveDialogContent>();
    content->id = 31;
    content->kind = sshd::DialogKind::List;
    const auto red = redIconPng();
    content->rows.resize(40);
    for (sshd::SaveDialogRow& row : content->rows) row.icon_png = red;
    std::size_t most = 0;
    const auto scroll_to = [&](int first) {
        publish(content, first, first);
        draw();
        most = std::max(most, overlay.iconTextureCount());
    };
    for (int first = 0; first <= 36; ++first) scroll_to(first);
    for (const int first : {0, 15, 30, 36, 21, 6, 0, 18, 36, 0}) scroll_to(first);
    CHECK(most > 0 && most <= 32);

    // At rest the window [first - 5, first + 10) holds its icons; the visible page draws red.
    for (int i = 0; i < 10; ++i) scroll_to(20);
    CHECK(overlay.iconTextureCount() == 15);
    for (std::uint32_t row = 0; row < 4; ++row) {
        CHECK(isRed(target.pixel(red_x, iconCellY(row) + kIconCellHeight / 2)));
    }

    // New content: the old textures go once the frames that may use them are done, and not before.
    auto next = std::make_shared<sshd::SaveDialogContent>();
    next->id = 32;
    next->kind = sshd::DialogKind::List;
    next->rows.resize(2);
    publish(next, 0, 0);
    draw();
    CHECK(overlay.iconTextureCount() == 15);
    CHECK(!isRed(target.pixel(red_x, iconCellY(0) + kIconCellHeight / 2)));
    for (std::size_t i = 0; i < kFrames; ++i) draw();
    CHECK(overlay.iconTextureCount() == 15);
    draw();
    CHECK(overlay.iconTextureCount() == 0);

    // The same row numbers again after a release: the new dialog decodes its own icons.
    next->id = 33;
    next->rows[0].icon_png = red;
    publish(next, 0, 0);
    draw();
    CHECK(isRed(target.pixel(red_x, iconCellY(0) + kIconCellHeight / 2)));
    CHECK(overlay.iconTextureCount() == 1);
}

void testSaveDialogOverlay(sshd::VkContext& ctx, OffscreenTarget& target) {
    constexpr VkClearColorValue kWhite = {{1.0f, 1.0f, 1.0f, 1.0f}};
    sshd::RendererStats stats;
    stats.flips = 1;

    sshd::SaveDialogBoard board;
    CHECK(board.drawer() == sshd::SaveDialogDrawer::None);
    // The bundled font when the build copied it, otherwise ImGui's default font; the checks do not depend on which.
    sshd::SaveDialogOverlayOptions options;
    options.font_candidates.push_back(exePath().parent_path() / "fonts" / "BIZUDPGothic-Regular.ttf");
    {
        sshd::SaveDialogOverlay overlay(ctx, board, options);
        CHECK(board.drawer() == sshd::SaveDialogDrawer::Ready);
        CHECK(!overlay.wantsDraw() && !overlay.wantsIdlePresent());
        publishDialog(board, messageContent(1, sshd::DialogKind::YesNo, "line one\n\nline two"), 1.0f);
        CHECK(overlay.wantsDraw() && overlay.wantsIdlePresent());
        board.clear();
        CHECK(!overlay.wantsDraw() && !overlay.wantsIdlePresent());
        CHECK(overlay.atlasBuildCount() == 0);

        publishDialog(board, messageContent(1, sshd::DialogKind::YesNo, "line one\n\nline two"), 1.0f);
        CHECK(target.drawOnce(overlay, stats, kWhite));
        CHECK(!overlay.failed());
        CHECK(board.drawer() == sshd::SaveDialogDrawer::Ready);
        CHECK(overlay.atlasBuildCount() == 1);
        // The dim is alpha 0.85 over white, and the rule below the text is white at alpha 0.6 over that.
        CHECK(target.pixel(4, 700).maxChannel() <= 50);
        CHECK(target.pixel(640, 400).minChannel() > 140);
        CHECK(anyBright(target, 560, 290, 720, 330));  // the first message line

        // The same content again builds nothing.
        CHECK(target.drawOnce(overlay, stats, kWhite));
        CHECK(overlay.atlasBuildCount() == 1);

        // A Japanese message that the dialog constants do not cover needs one more build, once.
        const std::string rare = "\xE9\xAC\xB1\xE9\xAC\xB1";  // "鬱鬱"
        publishDialog(board, messageContent(2, sshd::DialogKind::Ok, rare), 1.0f);
        CHECK(target.drawOnce(overlay, stats, kWhite));
        CHECK(overlay.atlasBuildCount() == 2);
        CHECK(target.drawOnce(overlay, stats, kWhite));
        CHECK(overlay.atlasBuildCount() == 2);
        CHECK(target.pixel(4, 700).maxChannel() <= 50);
        CHECK(target.pixel(640, 400).minChannel() > 140);

        // New content with glyphs the atlas already has builds nothing.
        publishDialog(board, messageContent(3, sshd::DialogKind::YesNo, "line"), 1.0f);
        CHECK(target.drawOnce(overlay, stats, kWhite));
        CHECK(overlay.atlasBuildCount() == 2);

        testSaveListLayout(board, overlay, target, stats);

        // Fully faded out: nothing is drawn.
        publishDialog(board, messageContent(5, sshd::DialogKind::YesNo, "line"), 0.0f);
        CHECK(target.drawOnce(overlay, stats, kWhite));
        CHECK(target.pixel(4, 700).minChannel() >= 250);
        CHECK(target.pixel(640, 400).minChannel() >= 250);

        // Half way: the dim is half as strong.
        publishDialog(board, messageContent(5, sshd::DialogKind::YesNo, "line"), 0.5f);
        CHECK(target.drawOnce(overlay, stats, kWhite));
        CHECK(target.pixel(4, 700).maxChannel() > 100 && target.pixel(4, 700).maxChannel() < 160);
        CHECK(!overlay.failed());
    }
    // The overlay is gone: a waiting dialog must not expect a drawer.
    CHECK(board.drawer() == sshd::SaveDialogDrawer::None);

    // No usable font: the default font stands in and the dialog still draws.
    {
        sshd::SaveDialogOverlay overlay(ctx, board, {});
        publishDialog(board, messageContent(1, sshd::DialogKind::Ok, "line one"), 1.0f);
        CHECK(target.drawOnce(overlay, stats, kWhite));
        CHECK(!overlay.failed());
        CHECK(overlay.atlasBuildCount() == 1);
        CHECK(target.pixel(4, 700).maxChannel() <= 50);
        CHECK(target.pixel(640, 400).minChannel() > 140);
        CHECK(anyBright(target, 560, 340, 720, 380));
    }
}

void testOverlayRecording(sshd::VkContext& ctx) {
    OffscreenTarget target(ctx);
    CHECK(target.ok());
    if (!target.ok()) return;
    const std::uint32_t errors_before = ctx.validationErrorCount();

    drawTwice(ctx, target);
    // The first overlay is gone; a second one starts from scratch on the same device.
    drawTwice(ctx, target);
#if defined(_WIN32)
    testWin32Input(ctx, target);
#endif
    testShutdownInsideScope(ctx, target);
    testSaveDialogOverlay(ctx, target);
    testSaveDialogIcons(ctx, target);

    const std::uint32_t errors = ctx.validationErrorCount() - errors_before;
    std::printf("overlay tests: validation %s, %u error(s)\n", ctx.validationEnabled() ? "active" : "inactive", errors);
    CHECK(errors == 0);
}

}  // namespace

int main() {
    testSteadyFlips();
    testRingWraps();
    testDrawDeltas();
    testRefresh();
    testPanels();
    testUnwrittenRates();
    testPresentRequests();
    testResolutionScalePanel();
    testFlipLimiterPanel();
    testAaModeChoices();
    testAntiAliasingPanel();
    testFrameGenChoices();
    testFrameGenStatusLine();
    testFrameGenSelectable();
    testObjectMotionPanel();
    testAnisotropyPanel();
    testWindowSizeLabels();
    testWindowSizePanel();
    testRuntimeSources();
    testOverlayStack();
    testImGuiContextScope();
    testUiFontCandidates();
    testLoadUiFont();
    testDialogCodePoints();
    testGlyphRanges();

    sshd::VkContextOptions options;
    options.validation = true;
    std::string error;
    std::unique_ptr<sshd::VkContext> ctx = sshd::createVkContext(options, error);
    if (ctx) {
        testToggleKey(*ctx);
        testOverlayRecording(*ctx);
    } else {
        std::printf("overlay tests: vulkan: skipped (%s)\n", error.c_str());
    }
    std::printf("overlay tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
