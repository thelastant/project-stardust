// sshd_game: loads the decrypted EBOOT.elf and runs it through the generated PPU code.

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <string_view>
#include <vector>

#include "app/game_options.h"
#include "app/settings_store.h"
#include "audio/audio_engine.h"
#include "audio/audio_output.h"
#if defined(_WIN32)
#include "audio/wasapi_output.h"
#elif defined(__APPLE__)
#include "audio/coreaudio_output.h"
#endif
#include "audio/wav_file.h"
#include "core/boot.h"
#include "core/census.h"
#include "core/dispatch.h"
#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_dump.h"
#include "core/guest_memory.h"
#include "core/host_priority.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "core/perf_log.h"
#include "core/reservation.h"
#include "core/thread_cpu.h"
#include "core/threads.h"
#include "core/timer_resolution.h"
#include "sshd/ppu_context.h"
#include "hle/cell_audio_ports.h"
#include "hle/hle.h"
#include "hle/libsre.h"
#include "hle/save_dialog.h"
#if defined(_WIN32)
#include "input/gameinput_backend.h"
#elif defined(__APPLE__)
#include "input/gamecontroller_backend.h"
#endif
#include "input/gamepad_hub.h"
#include "input/keyboard_pad.h"
#include "input/pad_script.h"
#include "libsre_ppu_tables.h"
#include "lv2/sys_fs.h"
#include "lv2/sys_sync.h"
#include "lv2/syscalls.h"
#include "lv2/vfs.h"
#include "lv2/vfs_config.h"
#include "media/atrac_decoder.h"
#include "media/ffmpeg_codecs.h"
#include "media/movie_decoder.h"
#include "overlay/debug_overlay.h"
#include "overlay/overlay_sources.h"
#include "overlay/overlay_stack.h"
#include "overlay/save_dialog_overlay.h"
#include "overlay/ui_font.h"
#include "render/render_scale.h"
#include "render/shader_compiler.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "render/vk_window.h"
#include "rsx/rsx_thread.h"
#include "spu/spu_capture.h"
#include "spu/spu_census.h"
#include "spu/spu_thread.h"

namespace {

// Guest addresses in the NPEA00014 EBOOT, read only for the per-second perf line.
// Time block: f32 dt at +0, u64 render hand-off job counters at +0x120/+0x128/+0x130.
constexpr std::uint32_t kGameTimeBlock = 0x116abd0;
constexpr std::uint32_t kGameTimeBlockBytes = 0x138;
constexpr std::uint32_t kGameJobCounters = 0x120;
// Frame block: u32 frame interval at +0 (1 = 60 fps, 2 = 30 fps), u64 frame counter at +8.
constexpr std::uint32_t kGameFrameBlock = 0x10cd670;
constexpr std::uint32_t kGameFrameBlockBytes = 0x10;

// Range of the game's dt over the flips of the last perf interval. Written by the RSX thread, drained by the perf
// thread; a flip racing the drain lands in either interval.
class GameDtWindow {
public:
    void add(float dt) {
        float cur = min_.load(std::memory_order_relaxed);
        while (dt < cur && !min_.compare_exchange_weak(cur, dt, std::memory_order_relaxed)) {
        }
        cur = max_.load(std::memory_order_relaxed);
        while (dt > cur && !max_.compare_exchange_weak(cur, dt, std::memory_order_relaxed)) {
        }
    }

    // Returns false when no flip was sampled.
    bool take(float& min, float& max) {
        min = min_.exchange(std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        max = max_.exchange(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        return min <= max;
    }

private:
    std::atomic<float> min_{std::numeric_limits<float>::infinity()};
    std::atomic<float> max_{-std::numeric_limits<float>::infinity()};
};

GameDtWindow& gameDtWindow() {
    static GameDtWindow window;
    return window;
}

// False until the EBOOT is loaded (or when the images differ).
bool gameTimeMapped() {
    const sshd::GuestMemory& memory = sshd::process().memory;
    return memory.isCommitted(kGameTimeBlock, kGameTimeBlockBytes) &&
           memory.isCommitted(kGameFrameBlock, kGameFrameBlockBytes);
}

float readGameDt() { return std::bit_cast<float>(sshdRead32(sshd::process().memory.base(), kGameTimeBlock)); }

// The Vulkan device and the renderer for the RSX thread. Built and destroyed on that thread; the
// counters are published for the census, which reads them from another one.
class VulkanExecutor final : public sshd::RsxExecutor {
public:
    // `presenter`, `overlay` and `save_overlay` may be null (no window).
    VulkanExecutor(std::unique_ptr<sshd::VkContext> context, std::unique_ptr<sshd::Presenter> presenter,
                   std::unique_ptr<sshd::DebugOverlay> overlay, std::unique_ptr<sshd::SaveDialogOverlay> save_overlay,
                   const sshd::RenderOptions& options)
        : context_(std::move(context)),
          presenter_(std::move(presenter)),
          overlay_(std::move(overlay)),
          save_overlay_(std::move(save_overlay)),
          renderer_(std::make_unique<sshd::RsxRenderer>(*context_, options)) {
        renderer_->setPresenter(presenter_.get());
        // The save dialog is drawn first, so the debug overlay stays on top of it.
        overlays_.add(save_overlay_.get());
        overlays_.add(overlay_.get());
        renderer_->setPresentOverlay(overlay_ || save_overlay_ ? &overlays_ : nullptr);
    }

    ~VulkanExecutor() override {
        // The renderer and the swapchain go before the device they were created from. The presenter's
        // destructor joins the window thread, which calls into the overlays, so the overlays outlive it.
        renderer_.reset();
        presenter_.reset();
        save_overlay_.reset();
        overlay_.reset();
        context_.reset();
    }

    bool valid() const { return renderer_->valid(); }
    const std::string& error() const { return renderer_->error(); }
    const sshd::VkContext& context() const { return *context_; }

    void execute(std::span<sshd::RsxCommand> commands) override {
        if (!renderer_->valid()) return;
        renderer_->execute(commands);
        const sshd::RendererStats& stats = renderer_->stats();
        draws_.store(stats.draws, std::memory_order_relaxed);
        skipped_.store(stats.draws_skipped, std::memory_order_relaxed);
        clears_.store(stats.clears, std::memory_order_relaxed);
        if (stats.flips != flips_.load(std::memory_order_relaxed) && gameTimeMapped()) {
            gameDtWindow().add(readGameDt());
        }
        flips_.store(stats.flips, std::memory_order_relaxed);
        if (!renderer_->valid()) {
            failed_.store(true, std::memory_order_relaxed);
            sshd::logError("sshd: render: renderer stopped: {}", renderer_->error());
        }
    }

    sshd::RsxRenderCounts counts() const override {
        sshd::RsxRenderCounts counts;
        counts.draws = draws_.load(std::memory_order_relaxed);
        counts.draws_skipped = skipped_.load(std::memory_order_relaxed);
        counts.clears = clears_.load(std::memory_order_relaxed);
        counts.flips = flips_.load(std::memory_order_relaxed);
        // The dump worker keeps its own atomic counters, so these are current without the RSX thread's help.
        const sshd::DumpCounts dumps = renderer_->dumpCounts();
        counts.frames_dumped = dumps.written;
        counts.dumps_skipped = dumps.skipped;
        counts.dumps_failed = dumps.failed;
        counts.validation = context_->validationEnabled();
        counts.validation_errors = context_->validationErrorCount();
        counts.failed = failed_.load(std::memory_order_relaxed);
        const sshd::PipelineCacheStats pipelines = renderer_->pipelineCacheStats();
        counts.pipelines_created = pipelines.created;
        counts.pipeline_create_us = pipelines.create_us;
        counts.pipeline_cache_loaded = pipelines.loaded_bytes;
        return counts;
    }

    // RSX thread, no FIFO work: keeps an open save dialog on screen while the game does not flip.
    void idle() override { renderer_->presentIdle(); }

    std::uint64_t flushOutput(std::chrono::milliseconds timeout) override {
        if (!renderer_->savePipelineCache()) sshd::logWarn("sshd: render: pipeline cache not saved");
        renderer_->logFrameGenSummary();
        return renderer_->flushDumps(timeout);
    }

private:
    std::unique_ptr<sshd::VkContext> context_;
    std::unique_ptr<sshd::Presenter> presenter_;
    std::unique_ptr<sshd::DebugOverlay> overlay_;
    std::unique_ptr<sshd::SaveDialogOverlay> save_overlay_;
    sshd::OverlayStack overlays_;
    std::unique_ptr<sshd::RsxRenderer> renderer_;
    std::atomic<std::uint64_t> draws_{0}, skipped_{0}, clears_{0}, flips_{0};
    std::atomic<bool> failed_{false};
};

// The process ends without running destructors on the stop paths, so the pending settings are written from a
// stop hook. The store is created only when changes can be saved (not with --no-config). It is never destroyed:
// a stop can arrive after main has returned and static destruction has begun, and the hook must not reach a
// freed object.
sshd::SettingsStore* g_settings_store = nullptr;

// Config keys of the overlay's saveable settings. Only changes the user makes in the overlay get here, so a value
// given on the command line is never written unless the user changes that setting.
void saveOverlaySetting(sshd::OverlaySetting setting, std::string_view value) {
    if (g_settings_store == nullptr) return;
    switch (setting) {
        case sshd::OverlaySetting::FrameLimit: g_settings_store->set("frame_limit", value); break;
        case sshd::OverlaySetting::ResolutionScale: g_settings_store->set("resolution_scale", value); break;
        case sshd::OverlaySetting::AaMode: g_settings_store->set("aa_mode", value); break;
        case sshd::OverlaySetting::Anisotropy: g_settings_store->set("anisotropic_filter", value); break;
        case sshd::OverlaySetting::WindowSize: g_settings_store->set("window_size", value); break;
        case sshd::OverlaySetting::FrameGen: g_settings_store->set("frame_gen", value); break;
        case sshd::OverlaySetting::ObjectMotion: g_settings_store->set("object_motion", value); break;
    }
}

VkPresentModeKHR presentModeFor(sshd::FgPresentMode mode) {
    switch (mode) {
        case sshd::FgPresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
        case sshd::FgPresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
        case sshd::FgPresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_MAILBOX_KHR;
}

// Runs on the RSX thread; a null result makes the consumer continue without rendering.
std::unique_ptr<sshd::RsxExecutor> createVulkanExecutor(const sshd::GameOptions& opt,
                                                        const std::shared_ptr<sshd::KeyboardPadState>& keyboard,
                                                        const std::vector<std::filesystem::path>& ui_font_candidates,
                                                        std::string& error) {
    const auto start = std::chrono::steady_clock::now();
    sshd::VkContextOptions context_options;
    context_options.validation = opt.vk_validation;
    context_options.window = opt.window;
    context_options.driver = opt.vk_driver;
    // Always loaded: availability is fixed at context creation and the overlay can switch to DLAA later.
    context_options.streamline = true;
    // Frame generation is a property of presentation: without a window nothing is presented.
    context_options.frame_generation = opt.window;
    std::unique_ptr<sshd::VkContext> context = sshd::createVkContext(context_options, error);
    if (!context) return nullptr;
    const std::uint32_t api = context->deviceProperties().apiVersion;
    // The driver is named because one GPU can be listed by several drivers (KosmicKrisp and MoltenVK on macOS).
    sshd::logInfo("sshd: render: vulkan device {} (api {}.{}.{}, driver {}, d24s8 {}, triangle fans {})",
                  context->deviceName(), VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                  VK_API_VERSION_PATCH(api), context->driverName(), context->hasD24S8() ? "yes" : "no",
                  context->triangleFans() ? "yes" : "no");
    if (context->streamline() != nullptr) {
        sshd::logInfo("sshd: render: DLAA available (Streamline {})", context->streamline()->version());
    } else {
        sshd::logInfo("sshd: render: DLAA unavailable: {}", context->streamlineStatus());
    }
    if (!opt.window) {
        sshd::logInfo("sshd: render: frame generation unavailable: no window");
    } else if (context->frameGenerationSupported()) {
        sshd::logInfo("sshd: render: frame generation available ({})", context->frameGenerationStatus());
    } else {
        sshd::logInfo("sshd: render: frame generation unavailable: {}", context->frameGenerationStatus());
    }
    if (opt.vk_validation && !context->validationEnabled()) {
        sshd::logWarn("sshd: render: validation layer requested but not installed");
    }

    sshd::RenderOptions render_options;
    render_options.dump_dir = opt.dump_frames;
    render_options.dump_every = opt.dump_frames_every;
    render_options.resolution_scale = opt.resolution_scale;
    render_options.aa_mode = opt.aa_mode;
    render_options.anisotropy = opt.anisotropy;
    render_options.frame_gen = opt.frame_gen;
    render_options.dlaa_debug.motion = opt.object_motion ? sshd::DlaaMotion::Object : sshd::DlaaMotion::Matrices;
    render_options.fg_debug.present_mode = presentModeFor(opt.fg_present_mode);
    render_options.fg_debug.reflex_low_latency = opt.fg_reflex;
    render_options.fg_debug.only_generated = opt.fg_only_generated;
    render_options.fg_debug.cycle_seconds = opt.fg_cycle_seconds;
    render_options.shader_cache =
            opt.shader_cache.empty() ? std::filesystem::path("generated") / "shader_cache" : opt.shader_cache;
    // Requested PNG dumps follow --dump-frames so they land next to the periodic ones.
    if (!opt.dump_frames.empty()) render_options.requested_dump_dir = opt.dump_frames;
    // Declared before the presenter so that on every return path the presenter (and its window thread, which
    // calls into the overlay) is destroyed first. Only created with a window.
    std::unique_ptr<sshd::DebugOverlay> overlay;
    std::unique_ptr<sshd::SaveDialogOverlay> save_overlay;
    std::unique_ptr<sshd::Presenter> presenter;
    if (opt.window) {
        sshd::OverlaySources overlay_sources = sshd::makeRuntimeOverlaySources();
        if (keyboard && sshd::padUsesGamepad(opt.pad)) {
            overlay_sources.capture_chord_down = [keyboard] {
                constexpr sshd::PadButtons kCaptureChord =
                        sshd::padButtonBit(sshd::PadButton::L3) | sshd::padButtonBit(sshd::PadButton::R3);
                return (keyboard->held(sshd::InputSource::Gamepad) & kCaptureChord) == kCaptureChord;
            };
        }
        overlay_sources.setting_changed = saveOverlaySetting;
        overlay = std::make_unique<sshd::DebugOverlay>(*context, std::move(overlay_sources));
        save_overlay = std::make_unique<sshd::SaveDialogOverlay>(
                *context, sshd::saveDialogBoard(),
                sshd::SaveDialogOverlayOptions{.font_candidates = ui_font_candidates});
        sshd::PresenterOptions presenter_options;
        presenter_options.on_message = [overlay = overlay.get()](const sshd::WindowMessage& message) {
            return overlay->onWindowMessage(message);
        };
        presenter_options.frame_slots = sshd::RsxRenderer::kFramesInFlight;
        presenter_options.window_size = opt.window_size;
        presenter_options.fullscreen = opt.fullscreen;
        presenter_options.on_fullscreen_changed = [](bool fullscreen) {
            if (g_settings_store != nullptr) g_settings_store->set("fullscreen", fullscreen ? "on" : "off");
        };
        presenter_options.on_close = [] {
            sshd::stopProcess(sshd::ExitCode::ProcessExit, "sshd: stop: window closed");
        };
        if (keyboard && sshd::padUsesKeyboard(opt.pad)) {
            // These run on the window thread; the state is lock-free, so the guest never waits on it.
            // Focus loss releases the keyboard source only: the gamepad thread is the sole writer of its own.
            presenter_options.on_key = [keyboard](const sshd::KeyMessage& key) {
                keyboard->keyEvent(key.scan_code, key.down, key.repeat, sshd::KeyboardPadState::Clock::now());
                if (key.repeat) return;
                if (const std::optional<sshd::PadButton> button = sshd::padButtonForScanCode(key.scan_code)) {
                    sshd::logDebug("sshd: pad: key {:#x} {} -> {}", key.scan_code, key.down ? "down" : "up",
                                   sshd::padButtonName(*button));
                }
            };
            presenter_options.on_focus_lost = [keyboard] { keyboard->releaseAll(sshd::InputSource::Keyboard); };
        }
        presenter = sshd::Presenter::create(*context, std::move(presenter_options), error);
        if (!presenter) {
            error = "window: " + error;
            return nullptr;
        }
        overlay->attachWindow(presenter->nativeWindowHandle());
    }
    auto executor = std::make_unique<VulkanExecutor>(std::move(context), std::move(presenter), std::move(overlay),
                                                     std::move(save_overlay), render_options);
    if (!executor->valid()) {
        error = executor->error();
        return nullptr;
    }
    const auto init_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    sshd::logInfo("sshd: render: device and renderer ready in {} ms", init_ms);
    if (!render_options.dump_dir.empty()) {
        sshd::logInfo("sshd: render: dumping every {} flip(s) to {}", render_options.dump_every,
                      render_options.dump_dir.generic_string());
    }
    return executor;
}

// The movies decode through FFmpeg DLLs loaded at run time. Without them the factory stays unset
// and every movie plays as black frames and silence for its duration.
void installMovieCodecs() {
    std::string error;
    std::unique_ptr<sshd::MediaCodecFactory> factory = sshd::createFfmpegCodecFactory(error);
    if (!factory) {
        sshd::logWarn("sshd: sail: codecs unavailable: {}", error);
        return;
    }
    sshd::setMediaCodecFactory(std::move(factory));
}

// cellAtrac sounds decode through the same DLLs. Without them every sound plays as silence of its
// own length and the stream protocol is unchanged.
void installAtracDecoder() {
    std::string error;
    std::unique_ptr<sshd::AtracDecoderFactory> factory = sshd::createFfmpegAtracDecoderFactory(error);
    if (!factory) {
        sshd::logWarn("sshd: atrac: no decoder: {}", error);
        return;
    }
    sshd::setAtracDecoderFactory(std::move(factory));
}

// Video and audio out report what the game's PARAM.SFO allows. A missing or broken file keeps the
// defaults of gameParamSfo().
void loadGameParamSfo() {
    bool read_only = false;
    const auto sfo_path = sshd::resolveGuestPath("/dev_bdvd/PS3_GAME/PARAM.SFO", read_only);
    if (!sfo_path) {
        sshd::logWarn("sshd: sfo: /dev_bdvd is not mounted; using the defaults");
        return;
    }
    auto sfo = sshd::readParamSfoFile(*sfo_path);
    if (!sfo) {
        sshd::logWarn("sshd: sfo: {}; using the defaults", sfo.error());
        return;
    }
    const auto title_id = sfo->strings.find("TITLE_ID");
    const auto resolution = sfo->ints.find("RESOLUTION");
    const auto sound_format = sfo->ints.find("SOUND_FORMAT");
    sshd::logInfo("sshd: sfo: TITLE_ID {} RESOLUTION {:#x} SOUND_FORMAT {:#x}",
                  title_id == sfo->strings.end() ? "?" : title_id->second,
                  resolution == sfo->ints.end() ? 0x1u : resolution->second,
                  sound_format == sfo->ints.end() ? 0x1u : sound_format->second);
    sshd::setGameParamSfo(std::move(*sfo));
}

// The census is printed on every stop path and the process then ends without running destructors,
// so this is also where an --audio-dump file gets its final header.
std::vector<std::string> audioCensusLines() {
    const sshd::AudioEngineStats engine = sshd::audioEngineStats();
    sshd::finishAudioDump();

    std::vector<std::string> lines;
    if (!engine.output.empty()) {  // otherwise cellAudioInit never ran
        const sshd::CellAudioStats ports = sshd::cellAudioPorts().stats();
        lines.push_back(std::format(
                "audio {}: {} blocks ({} non-silent), skipped {}, dropped {}, underrun {} frames, notify {} sent {} failed, "
                "max producer {:.2f} ms, clock {:+.0f} ppm",
                engine.output, engine.blocks, engine.non_silent_blocks, engine.skipped_blocks,
                engine.output_stats.blocks_dropped, engine.output_stats.underrun_frames, ports.notifies_sent,
                ports.notifies_failed, static_cast<double>(engine.max_producer_time.count()) / 1000.0,
                engine.clock_ppm));
        std::string channels;
        for (std::size_t c = 0; c < ports.nonzero_samples.size(); ++c) {
            channels += std::format("{}{}:{}", c == 0 ? "" : " ", c, ports.nonzero_samples[c]);
        }
        lines.push_back(std::format(
                "audio input: {} periods, non-zero samples per channel {}, unwritten blocks {} (runs {}, longest {})",
                ports.periods, channels, ports.unwritten_blocks, ports.unwritten_runs, ports.longest_unwritten_run));
    }

    const sshd::CellAtracStats atrac = sshd::cellAtracStats();
    if (atrac.decoders != 0) {
        lines.push_back(
                std::format("atrac {} decoders, {} frames decoded ({} non-silent), {} errors, {} without decoder",
                            atrac.decoders, atrac.frames, atrac.non_silent, atrac.errors, atrac.without_decoder));
    }
    return lines;
}

// Interval deltas for the once-per-second perf line; each source keeps its previous snapshot because the perf thread
// is the only caller.
sshd::PerfSource audioPerfSource() {
    return [prev = sshd::CellAudioStats{}, prev_flips = std::uint64_t{0}]() mutable -> std::string {
        if (sshd::audioEngineStats().output.empty()) return {};  // cellAudioInit has not run
        const sshd::CellAudioStats now = sshd::cellAudioPorts().stats();
        const std::uint64_t flips = sshd::rsxCensusSnapshot().flips;
        std::string body = std::format("audio flips {} periods {} unwritten {} (runs {})", flips - prev_flips,
                                       now.periods - prev.periods, now.unwritten_blocks - prev.unwritten_blocks,
                                       now.unwritten_runs - prev.unwritten_runs);
        prev = now;
        prev_flips = flips;
        return body;
    };
}

// The game's own timing state, for judging whether dt and the render hand-off keep up with the frame interval.
// Snapshot reads of live guest memory without the kernel lock; the values are diagnostic only.
sshd::PerfSource gamePerfSource() {
    return []() -> std::string {
        if (!gameTimeMapped()) return {};
        const std::uint8_t* base = sshd::process().memory.base();
        std::string body = std::format("game dt {:.3f} interval {} frame {} jobs {}/{}/{}", readGameDt(),
                                       sshdRead32(base, kGameFrameBlock), sshdRead64(base, kGameFrameBlock + 8),
                                       sshdRead64(base, kGameTimeBlock + kGameJobCounters),
                                       sshdRead64(base, kGameTimeBlock + kGameJobCounters + 8),
                                       sshdRead64(base, kGameTimeBlock + kGameJobCounters + 0x10));
        float min = 0.0f, max = 0.0f;
        if (gameDtWindow().take(min, max)) body += std::format(" dt-at-flips min {:.3f} max {:.3f}", min, max);
        return body;
    };
}

// Where the RSX thread's time goes, per completed flip. Empty in a second without a flip.
sshd::PerfSource rsxPerfSource() {
    // The first interval starts when the source is created, not at thread start.
    return [prev = sshd::rsxThreadTimes(), prev_flips = sshd::rsxFlipCount()]() mutable -> std::string {
        const sshd::RsxThreadTimes now = sshd::rsxThreadTimes();
        const std::uint64_t flips = sshd::rsxFlipCount();
        const sshd::RsxThreadTimes delta{now.idle_us - prev.idle_us, now.lock_wait_us - prev.lock_wait_us,
                                         now.pump_us - prev.pump_us, now.build_us - prev.build_us,
                                         now.record_us - prev.record_us};
        const std::string body = sshd::formatRsxBusy(delta, flips - prev_flips);
        prev = now;
        prev_flips = flips;
        return body;
    };
}

bool isAudioPathClass(sshd::GuestLockClass cls) {
    return cls == sshd::GuestLockClass::Audio || cls == sshd::GuestLockClass::Spu ||
           cls == sshd::GuestLockClass::PpuHigh;
}

sshd::PerfSource lockPerfSource() {
    return [prev = sshd::GuestLockStats{}, prev_yields = sshd::PpuYieldStats{},
            prev_handoffs = sshd::SyncHandoffStats{}, prev_reservations = sshd::PpuReservationStats{},
            prev_slots = sshd::PpuSlotStats{}, prev_slow = sshd::SyncHandoffStats{}]() mutable -> std::string {
        const sshd::GuestLockStats now = sshd::guestLock().stats();
        std::string body;
        for (std::size_t i = 0; i < sshd::kGuestLockClassCount; i++) {
            const auto cls = static_cast<sshd::GuestLockClass>(i);
            const sshd::GuestLockClassStats& a = now.classes[i];
            const sshd::GuestLockClassStats& b = prev.classes[i];
            const sshd::LatencySnapshot wait = a.wait.since(b.wait);
            if (wait.count == 0) continue;
            const sshd::LatencySnapshot hold = a.hold.since(b.hold);
            body += std::format("{}{} {} hold p99 {:.2f} max {:.2f}", body.empty() ? "lock " : " | ",
                                sshd::guestLockClassName(cls), sshd::formatLatency(wait, sshd::kGuestLockWaitOverUs),
                                static_cast<double>(hold.percentileUs(0.99)) / 1000.0,
                                static_cast<double>(hold.max_us) / 1000.0);
            if (!isAudioPathClass(cls)) continue;
            // Mean queue length over the contended acquisitions only, the same denominator as the census.
            const std::uint64_t contended = a.contended - b.contended;
            const double ahead =
                    contended != 0 ? static_cast<double>(a.ahead_sum - b.ahead_sum) / static_cast<double>(contended)
                                   : 0.0;
            body += std::format(" ahead avg {:.1f}", ahead);
            bool first = true;
            for (std::size_t holder = 0; holder < sshd::kGuestLockClassCount; holder++) {
                const std::uint64_t us = a.waited_behind_us[holder] - b.waited_behind_us[holder];
                if (us == 0) continue;
                body += std::format("{} {} {:.2f}", first ? " behind" : "",
                                    sshd::guestLockClassName(static_cast<sshd::GuestLockClass>(holder)),
                                    static_cast<double>(us) / 1000.0);
                first = false;
            }
            if (!first) body += " ms";
        }
        const sshd::PpuYieldStats yields = sshd::ppuYieldStats();
        if (yields.calls != prev_yields.calls) {
            body += std::format("{}yield calls {} ok {} host {} kept {} backoff {}", body.empty() ? "lock " : " | ",
                                yields.calls - prev_yields.calls, yields.switched - prev_yields.switched,
                                yields.to_host - prev_yields.to_host, yields.kept - prev_yields.kept,
                                yields.backoffs - prev_yields.backoffs);
            prev_yields = yields;
        }
        const sshd::SyncHandoffStats handoffs = sshd::syncHandoffStats();
        if (handoffs.mutex_waits != prev_handoffs.mutex_waits ||
            handoffs.mutex_handoffs != prev_handoffs.mutex_handoffs ||
            handoffs.lwmutex_waits != prev_handoffs.lwmutex_waits ||
            handoffs.lwmutex_handoffs != prev_handoffs.lwmutex_handoffs) {
            body += std::format("{}lwmutex wait {} handoff {} mutex wait {} handoff {}", body.empty() ? "lock " : " | ",
                                handoffs.lwmutex_waits - prev_handoffs.lwmutex_waits,
                                handoffs.lwmutex_handoffs - prev_handoffs.lwmutex_handoffs,
                                handoffs.mutex_waits - prev_handoffs.mutex_waits,
                                handoffs.mutex_handoffs - prev_handoffs.mutex_handoffs);
            prev_handoffs = handoffs;
        }
        // Appended after the fields above, whose text scripts parse.
        if (now.long_holds != prev.long_holds) {
            body += std::format("{}long holds {}", body.empty() ? "lock " : " | ", now.long_holds - prev.long_holds);
        }
        const sshd::FsTransferStats fs = sshd::takeFsTransferStats();
        if (fs.reads != 0 || fs.writes != 0) {
            body += std::format("{}fs read {} {} bytes max {:.2f} ms write {} {} bytes max {:.2f} ms",
                                body.empty() ? "lock " : " | ", fs.reads, fs.read_bytes,
                                static_cast<double>(fs.read_max_us) / 1000.0, fs.writes, fs.write_bytes,
                                static_cast<double>(fs.write_max_us) / 1000.0);
        }
        // In the split modes the hold time is kernel sections, which this name would misreport; the slots field below
        // replaces it.
        if (!sshd::splitPpuMode() && now.ppu_hold_us != prev.ppu_hold_us) {
            body += std::format("{}ppu overlap {:.2f} of {:.2f} ms", body.empty() ? "lock " : " | ",
                                static_cast<double>(now.ppu_hold_overlap_us - prev.ppu_hold_overlap_us) / 1000.0,
                                static_cast<double>(now.ppu_hold_us - prev.ppu_hold_us) / 1000.0);
        }
        const sshd::PpuReservationStats reservations = sshd::ppuReservationStats();
        if (reservations.stwcx != prev_reservations.stwcx) {
            body += std::format("{}stwcx {} fail {}", body.empty() ? "lock " : " | ",
                                reservations.stwcx - prev_reservations.stwcx,
                                reservations.stwcx_failed - prev_reservations.stwcx_failed);
            prev_reservations = reservations;
        }
        if (sshd::splitPpuMode()) {
            const sshd::PpuSlotStats slots = sshd::ppuSlots().stats();
            body += std::format("{}slots par {:.2f} ms full {:.2f} ms", body.empty() ? "lock " : " | ",
                                static_cast<double>(slots.parallel_us - prev_slots.parallel_us) / 1000.0,
                                static_cast<double>(slots.full_waiting_us - prev_slots.full_waiting_us) / 1000.0);
            prev_slots = slots;
        }
        // Last, so the fields above keep their positions for the scripts that parse them. The uncontended calls count
        // nothing; these are the calls that left the compare-and-swap path.
        const std::uint64_t slow_locks = handoffs.lwmutex_slow_locks - prev_slow.lwmutex_slow_locks;
        const std::uint64_t slow_unlocks = handoffs.lwmutex_slow_unlocks - prev_slow.lwmutex_slow_unlocks;
        const std::uint64_t misaligned = handoffs.lwmutex_misaligned - prev_slow.lwmutex_misaligned;
        if (slow_locks != 0 || slow_unlocks != 0 || misaligned != 0) {
            body += std::format("{}lwmutex slow lock {} unlock {}", body.empty() ? "lock " : " | ", slow_locks,
                                slow_unlocks);
            if (misaligned != 0) body += std::format(" misaligned {}", misaligned);
            prev_slow = handoffs;
        }
        prev = now;
        return body;
    };
}

sshd::PerfSource spuPerfSource() {
    return [prev = std::map<std::uint32_t, sshd::SpuReceiveRow>{}, prev_waits = sshd::SpuReservationWaits{},
            prev_fails = sshd::SpuPutllcFails{}]() mutable -> std::string {
        std::string body;
        for (const sshd::SpuReceiveRow& row : sshd::spuReceiveSnapshot()) {
            const sshd::SpuReceiveRow& before = prev[row.id];
            const sshd::LatencySnapshot busy = row.busy.since(before.busy);
            const sshd::LatencySnapshot wait = row.wait.since(before.wait);
            if (busy.count != 0 || wait.count != 0) {
                body += std::format("{}spu {:#010x} {} busy {}; wait p99 {:.2f} max {:.2f}", body.empty() ? "" : " | ",
                                    row.id, row.name, sshd::formatLatency(busy, sshd::kSpuReceiveOverUs),
                                    static_cast<double>(wait.percentileUs(0.99)) / 1000.0,
                                    static_cast<double>(wait.max_us) / 1000.0);
            }
            prev[row.id] = row;
        }
        const sshd::SpuReservationWaits waits = sshd::spuReservationWaitSnapshot();
        const std::string segment = sshd::spuReservationWaitSegment(waits, prev_waits);
        if (!segment.empty()) body += std::format("{}{}", body.empty() ? "" : " | ", segment);
        prev_waits = waits;
        const sshd::SpuPutllcFails fails = sshd::spuPutllcFailSnapshot();
        const std::string fail_segment = sshd::spuPutllcFailSegment(fails, prev_fails);
        if (!fail_segment.empty()) body += std::format("{}{}", body.empty() ? "" : " | ", fail_segment);
        prev_fails = fails;
        return body;
    };
}

// CPU time per registered host thread in the last interval. The process time of the first interval starts when the
// source is created, so the perf thread's first line does not include start-up.
sshd::PerfSource cpuPerfSource() {
    return [prev = sshd::hostThreadCpuSnapshot(), prev_process_us = sshd::processCpuUs()]() mutable -> std::string {
        std::vector<sshd::HostThreadCpu> now = sshd::hostThreadCpuSnapshot();
        const std::uint64_t process_us = sshd::processCpuUs();
        std::string body = sshd::formatHostThreadCpu(prev, now, prev_process_us, process_us);
        prev = std::move(now);
        prev_process_us = process_us;
        return body;
    };
}

void configureAudio(const sshd::GameOptions& opt) {
    sshd::AudioEngineConfig config;
#if defined(_WIN32)
    // Only the wasapi choice builds a device output, so a run without it never touches COM.
    if (opt.audio == sshd::kHostAudioOutput) config.make_output = [] { return sshd::makeWasapiOutput(); };
#elif defined(__APPLE__)
    if (opt.audio == sshd::kHostAudioOutput) config.make_output = [] { return sshd::makeCoreAudioOutput(); };
#endif
    config.dump = opt.audio_dump;
    sshd::configureAudioEngine(std::move(config));
    sshd::setAudioCensusProvider(&audioCensusLines);
}

// sshd_game lives in build/<preset>/bin; a decrypted sdata cache placed next to that directory is
// found without --sdata-cache. The hdd directory defaults to the same place so that game data and
// saves persist between runs without --hdd-dir. Both apply only when neither the command line nor
// the VFS configuration chose one.
void applyBuildDirDefaults(sshd::VfsLayout& layout) {
    const std::filesystem::path exe_dir = sshd::executableDirectory();
    if (exe_dir.empty()) return;
    const std::filesystem::path build_dir = exe_dir.parent_path();
    std::error_code ec;
    if (!layout.sdata_cache) {
        const std::filesystem::path cache = build_dir / "sdata_cache";
        if (std::filesystem::is_directory(cache, ec)) {
            sshd::logInfo("sshd: vfs: sdata cache {} (default)", cache.generic_string());
            layout.sdata_cache = cache;
        }
    }
    if (std::ranges::find(layout.mounts, "dev_hdd0", &sshd::VfsMount::name) == layout.mounts.end()) {
        const std::filesystem::path hdd = build_dir / "hdd";
        sshd::logInfo("sshd: vfs: hdd directory {} (default)", hdd.generic_string());
        for (sshd::VfsMount& mount : sshd::hddDirMounts(hdd)) {
            std::erase_if(layout.mounts, [&](const sshd::VfsMount& m) { return m.name == mount.name; });
            layout.mounts.push_back(std::move(mount));
        }
    }
}

// --save-dialog. The interactive dialog needs the window to draw in and a pad that reads host input to answer it.
// A --pad-script presses buttons by itself, so it only removes the default; an explicit "window" is honoured with one.
void installSaveDialogDriver(const sshd::GameOptions& opt) {
    const bool can_show = opt.window && (sshd::padUsesKeyboard(opt.pad) || sshd::padUsesGamepad(opt.pad));
    sshd::SaveDialogMode mode = sshd::SaveDialogMode::Auto;
    if (opt.save_dialog) {
        mode = *opt.save_dialog;
        if (mode == sshd::SaveDialogMode::Window && !can_show) {
            sshd::logWarn(
                    "sshd: --save-dialog window needs the window and a pad that reads the keyboard or a gamepad "
                    "(--pad {}); using auto",
                    opt.pad);
            mode = sshd::SaveDialogMode::Auto;
        }
    } else if (can_show && opt.pad_script.empty()) {
        mode = sshd::SaveDialogMode::Window;
    }
    switch (mode) {
        case sshd::SaveDialogMode::Auto:
            sshd::setSaveDialogDriver(sshd::makeAutoSaveDialogDriver(sshd::SaveDialogPolicy::Auto));
            sshd::logInfo("sshd: savedata dialog: auto");
            break;
        case sshd::SaveDialogMode::Cancel:
            sshd::setSaveDialogDriver(sshd::makeAutoSaveDialogDriver(sshd::SaveDialogPolicy::Cancel));
            sshd::logInfo("sshd: savedata dialog: cancel");
            break;
        case sshd::SaveDialogMode::Window:
            sshd::setSaveDialogDriver(sshd::makeInteractiveSaveDialogDriver(sshd::saveDialogBoard()));
            sshd::logInfo("sshd: savedata dialog: window");
            break;
    }
}

void flushSettingsStore() {
    if (g_settings_store != nullptr) g_settings_store->flush(std::chrono::milliseconds(250));
}

// Tells whether a run compiled shaders or found them all in the cache, which decides how far its first seconds can be
// compared with another run.
void logShaderCacheStats() {
    const sshd::ShaderCompilerStats stats = sshd::shaderCompilerStats();
    sshd::logInfo("sshd: shader cache: {} hits, {} compiles", stats.cache_hits, stats.compiles);
}

}  // namespace

int main(int argc, char** argv) {
    sshd::installProcessSettings();
    sshd::installCensus();

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    std::error_code cwd_ec;
    const sshd::ConfigSearch search{std::filesystem::current_path(cwd_ec), sshd::executableDirectory()};
    auto parsed = sshd::parseGameOptions(args, search);
    if (!parsed) {
        const sshd::OptionsError& error = parsed.error();
        std::fputs(error.message.c_str(), stderr);
        if (error.print_usage) {
            const std::string_view usage = sshd::gameOptionsUsage();
            std::fwrite(usage.data(), 1, usage.size(), stderr);
        }
        return error.exit_code;
    }
    sshd::GameOptions opt = std::move(*parsed);
    sshd::applyGameOptions(opt);
    if (cwd_ec) {
        // Without a working directory the lookup silently falls back to the executable's directory.
        sshd::logWarn("sshd: config: no working directory ({}); only the executable's directory is searched",
                      cwd_ec.message());
    }

    std::error_code ec;
    sshd::VfsCliOptions cli;
    if (!opt.game_dir.empty()) {
        if (!std::filesystem::is_directory(opt.game_dir, ec)) {
            std::fprintf(stderr, "error: game directory %s does not exist\n", opt.game_dir.string().c_str());
            return static_cast<int>(sshd::ExitCode::Usage);
        }
        cli.game_dir = opt.game_dir;
    }
    if (!opt.hdd_dir.empty()) {
        cli.hdd_dir = opt.hdd_dir;
    }
    if (!opt.sdata_cache.empty()) {
        cli.sdata_cache = opt.sdata_cache;
    }

    if (!opt.log_file.empty() && !sshd::setLogFile(opt.log_file.string())) {
        std::fprintf(stderr, "error: cannot open log file %s\n", opt.log_file.string().c_str());
        return static_cast<int>(sshd::ExitCode::Usage);
    }

    if (!opt.settings_file.empty()) {
        g_settings_store = new sshd::SettingsStore(opt.settings_file, sshd::gameOptionSpecs());
        if (!sshd::addStopHook(&flushSettingsStore)) {
            sshd::logWarn("sshd: config: no free stop hook; settings changed just before the end may be lost");
        }
    }

    sshd::startPerfLog(
            {audioPerfSource(), gamePerfSource(), rsxPerfSource(), lockPerfSource(), spuPerfSource(), cpuPerfSource()});
    if (!sshd::addStopHook(&logShaderCacheStats)) {
        sshd::logWarn("sshd: no free stop hook; the shader cache statistics are not logged");
    }

    // Precedence: command-line options, then config.json, then the --vfs-config file, then the defaults. Keys in
    // config.json act exactly like command-line options, so its game_dir overrides the /dev_bdvd mount of the
    // VFS file.
    std::optional<sshd::VfsConfigFile> vfs_file;
    if (!opt.vfs_config.empty()) {
        auto loaded = sshd::loadVfsConfig(opt.vfs_config);
        if (!loaded) {
            sshd::logError("vfs config: {}", loaded.error());
            return static_cast<int>(sshd::ExitCode::LoadError);
        }
        for (const std::string& warning : loaded->warnings) {
            sshd::logWarn("vfs config: {}", warning);
        }
        vfs_file = std::move(*loaded);
    }
    auto layout = sshd::composeVfsLayout(vfs_file ? &*vfs_file : nullptr, cli);
    if (!layout) {
        sshd::logError("vfs: {}", layout.error());
        return static_cast<int>(sshd::ExitCode::LoadError);
    }
    applyBuildDirDefaults(*layout);
    sshd::configureVfs(std::move(*layout));

    std::filesystem::path elf_path = opt.elf;
    if (opt.elf.empty()) {
        bool read_only = false;
        const auto default_elf = sshd::resolveGuestPath("/dev_bdvd/PS3_GAME/USRDIR/EBOOT.elf", read_only);
        if (!default_elf) {
            sshd::logError("no executable: pass --elf or --game-dir, or mount /dev_bdvd in the VFS config");
            return static_cast<int>(sshd::ExitCode::LoadError);
        }
        elf_path = *default_elf;
    }

    if (!opt.png_dump.empty()) {
        std::filesystem::create_directories(opt.png_dump, ec);
        sshd::setPngDumpDirectory(opt.png_dump);
    }

    loadGameParamSfo();
    sshd::setRsxFlipLimit(opt.frame_limit);
    sshd::setHostPrioritiesEnabled(opt.host_priorities);
    sshd::setYieldBackoffEnabled(opt.yield_backoff);
    sshd::setLwmutexFastPathEnabled(opt.lwmutex_fast_path);
    // Before boot creates the process lwmutexes, which the watch has to register.
    sshd::setLwmutexWatchEnabled(opt.lwmutex_watch);
    // Before any guest thread exists: the mode fixes the slot capacity and the lock rules those threads start with.
    sshd::setPpuThreadsMode(opt.ppu_threads);
    sshd::logInfo("sshd: ppu threads: {}", opt.ppu_threads == sshd::PpuThreadsMode::Legacy   ? "legacy"
                                           : opt.ppu_threads == sshd::PpuThreadsMode::Split1 ? "1"
                                                                                             : "2");
    if (!opt.rsx_census.empty()) sshd::setRsxArgCensusFile(opt.rsx_census.string());
    if (!opt.rsx_capture_frames.empty()) {
        sshd::setRsxCaptureFrames(opt.rsx_capture_frames, std::filesystem::path("generated") / "rsx" / "captures");
    }

    for (sshd::GuestDumpSpec& dump : opt.dumps) {
        if (!sshd::addGuestDump(std::move(dump))) {
            std::fputs("error: too many --dump-guest options\n", stderr);
            return static_cast<int>(sshd::ExitCode::Usage);
        }
    }

    sshd::installFaultHandler();
    sshd::Process& process = sshd::process();
    process.strict = opt.strict;
    if (!process.memory.reserve()) {
        sshd::logError("cannot reserve the guest address space");
        return static_cast<int>(sshd::ExitCode::Fatal);
    }

    const auto loaded = sshd::loadExecutable(elf_path);
    if (!loaded) {
        sshd::logError("load failed: {}", loaded.error());
        return static_cast<int>(sshd::ExitCode::LoadError);
    }
    const sshd::LoadedExecutable& exe = *loaded;

    const std::string_view expected_sha256 = kSshdPpuExecutableSha256;
    if (exe.sha256 != expected_sha256) {
        sshd::logError("executable hash {} does not match the generated code ({})", exe.sha256, expected_sha256);
        return static_cast<int>(sshd::ExitCode::LoadError);
    }

    // libsre sits directly above the executable, before anything else allocates from the main region.
    // By default it is the file the build wrote next to the executable.
    const std::filesystem::path libsre_path =
            opt.libsre.empty() ? elf_path.parent_path() / "libsre.reloc.elf" : opt.libsre;
    if (!std::filesystem::is_regular_file(libsre_path, ec)) {
        sshd::logError(
                "libsre: cannot open {} (build with -Pup to produce generated/inputs/libsre.reloc.elf, or pass --libsre)",
                libsre_path.generic_string());
        return static_cast<int>(sshd::ExitCode::LoadError);
    }
    const auto loaded_module = sshd::loadModule(libsre_path, sshd::libsre::kBase, exe.image_end);
    if (!loaded_module) {
        sshd::logError("libsre: load failed: {}", loaded_module.error());
        return static_cast<int>(sshd::ExitCode::LoadError);
    }
    const sshd::LoadedModule& module = *loaded_module;
    const std::string_view libsre_expected_sha256 = kSshdLibsrePpuExecutableSha256;
    if (module.sha256 != libsre_expected_sha256) {
        sshd::logError("libsre hash {} does not match the generated code ({})", module.sha256, libsre_expected_sha256);
        return static_cast<int>(sshd::ExitCode::LoadError);
    }
    // The whole file is pinned by its hash; these checks catch an offset in the layout header that
    // drifted from the image. A long list has one root cause, so only the first few are logged.
    const std::vector<std::string> layout_errors = sshd::libsre::checkLibsreLayout(
            process.memory.base(), std::span(kSshdLibsrePpuImports, kSshdLibsrePpuImportCount),
            std::span(kSshdLibsrePpuExports, kSshdLibsrePpuExportCount));
    constexpr std::size_t kMaxLayoutMessages = 5;
    for (std::size_t i = 0; i < layout_errors.size() && i < kMaxLayoutMessages; ++i) {
        sshd::logError("libsre: layout: {}", layout_errors[i]);
    }
    if (layout_errors.size() > kMaxLayoutMessages) {
        sshd::logError("libsre: layout: {} more failed checks", layout_errors.size() - kMaxLayoutMessages);
    }
    bool libsre_ok = layout_errors.empty();
    if (module.base != sshd::libsre::kBase || module.toc != sshd::libsre::kToc ||
        module.image_end != sshd::libsre::kImageEnd) {
        sshd::logError("libsre: module base {:#010x} toc {:#010x} image end {:#010x} differ from the layout header",
                       module.base, module.toc, module.image_end);
        libsre_ok = false;
    }
    if (!libsre_ok) return static_cast<int>(sshd::ExitCode::LoadError);

    // The SPU tables hold hashes of the blobs inside the two images; a mismatch means they were emitted from
    // other inputs, so this stops before an SPU thread can run code the tables do not describe.
    // With a capture selector the SPU runtime gets a patched copy of the tables; without one it gets the generated
    // tables themselves. The capture's start time counts from here.
    const SshdSpuImageInfo* spu_images = kSshdSpuImages;
    if (!opt.spu_captures.empty()) {
        std::string capture_error;
        spu_images = sshd::installSpuCapture(kSshdSpuImages, kSshdSpuImageCount, opt.spu_captures, opt.spu_capture,
                                             capture_error);
        if (!spu_images) {
            std::fprintf(stderr, "error: --spu-capture: %s\n", capture_error.c_str());
            return static_cast<int>(sshd::ExitCode::Usage);
        }
    }
    sshd::registerSpuImages(spu_images, kSshdSpuImageCount);
    if (!sshd::checkSpuImages(process.memory.base())) return static_cast<int>(sshd::ExitCode::LoadError);

    sshdRegisterFuncs(kSshdPpuFuncMappings, kSshdPpuFuncMappingCount);
    sshd::appendFuncs(kSshdLibsrePpuFuncMappings, kSshdLibsrePpuFuncMappingCount);
    sshd::setQuietHle(opt.quiet_hle);
    const sshd::GuestExports guest_exports[] = {{"libsre", kSshdLibsrePpuExports, kSshdLibsrePpuExportCount}};
    sshd::installImports(kSshdPpuImports, kSshdPpuImportCount, guest_exports);
    sshd::installModuleImports("libsre", kSshdLibsrePpuImports, kSshdLibsrePpuImportCount);
    sshd::setProcessSdkVersion(exe.sdk_version);

    sshd::logInfo(
            "sshd: loaded {}: entry {:#010x} toc {:#010x} opd {:#010x} tls {:#010x}/{}/{:#x} sdk {:#010x} "
            "prio {} stacksize {:#x} malloc {:#x} ppc_seg {:#x} image end {:#010x}",
            elf_path.filename().string(), exe.entry_func, exe.entry_toc, exe.entry_opd, exe.tls_vaddr, exe.tls_filesz,
            exe.tls_memsz, exe.sdk_version, exe.primary_prio, exe.primary_stacksize, exe.malloc_pagesize, exe.ppc_seg,
            exe.image_end);
    sshd::logInfo("sshd: sha256 {}", exe.sha256);
    sshd::logInfo("sshd: loaded {}: module {} {}.{} base {:#010x} toc {:#010x} image end {:#010x}",
                  libsre_path.filename().string(), module.name, unsigned{module.version_major},
                  unsigned{module.version_minor}, module.base, module.toc, module.image_end);
    sshd::logInfo("sshd: libsre: sha256 {}", module.sha256);
    sshd::logInfo("sshd: libsre: layout ok");

    if (opt.load_only) {
        return static_cast<int>(sshd::ExitCode::ProcessExit);
    }

    installMovieCodecs();
    installAtracDecoder();

    // Held until the process ends (runSupervisor does not return); the decode loops of the game poll
    // with 1 ms sleeps, which the default tick would stretch to 15 ms.
    const sshd::TimerResolution timer_resolution;
    if (timer_resolution.active()) {
        const auto period_us = sshd::currentTimerPeriodUs();
        sshd::logDebug("sshd: timer: requested {} ms, system period {}", timer_resolution.periodMs(),
                       period_us ? std::format("{:.3f} ms", *period_us / 1000.0) : std::string("unknown"));
    } else {
        sshd::logWarn("sshd: timer: the 1 ms resolution request was refused; timed waits use the default tick");
    }

    // The host pad state, set for every --pad that reads host input (keyboard, gamepad, both). The window
    // thread, the script thread and the gamepad thread each write their own source of it. The executor
    // factory below copies it, so it is created first.
    std::shared_ptr<sshd::KeyboardPadState> keyboard;
    if (sshd::padUsesKeyboard(opt.pad) || sshd::padUsesGamepad(opt.pad)) {
        keyboard = std::make_shared<sshd::KeyboardPadState>();
        // With the keyboard in the pad, port 0 is always there; a gamepad-only pad follows the hardware.
        sshd::setPadProvider(sshd::makeKeyboardPadProvider(keyboard, {}, {.always_connected = opt.pad != "gamepad"}));
        std::string where;
        if (opt.window) where += " (window)";
        if (!opt.pad_script.empty()) {
            where += std::format(" (script {}, {} events)", opt.pad_script.filename().string(),
                                 opt.pad_script_data.events.size());
        }
        sshd::logInfo("sshd: pad: {}{}", opt.pad, where);
    } else {
        sshd::setPadBackend(opt.pad);
    }

    configureAudio(opt);
    installSaveDialogDriver(opt);

    // The firmware font sits next to libsre.reloc.elf (generated/inputs/ after a build with -Pup).
    std::vector<std::filesystem::path> ui_font_candidates = sshd::uiFontCandidates(
            opt.ui_font, libsre_path.parent_path(), elf_path.parent_path(), sshd::executableDirectory());

    // The device is created on the RSX thread when it starts, so it belongs to that thread.
    if (opt.render_vulkan) {
        sshd::setRsxExecutorFactory(
                [opt, keyboard, ui_font_candidates = std::move(ui_font_candidates)](std::string& error) {
                    return createVulkanExecutor(opt, keyboard, ui_font_candidates, error);
                });
    }

    // RPCS3 passes this VFS path as argv[0] (`Emu/System.cpp` `Emulator::Load`, "Fake arg").
    // A seconds script starts here so that it matches --seconds within a few milliseconds; a flip script
    // follows the process-wide flip count.
    if (keyboard && !opt.pad_script.empty()) sshd::startPadScript(opt.pad_script_data, keyboard);
    // GameInput takes about a second to initialise, so the gamepad thread creates the backend itself.
    // The probe alone (no gamepad in the pad) runs the thread without a state to write.
#if defined(_WIN32)
    if (sshd::padUsesGamepad(opt.pad) || opt.pad_probe) {
        sshd::startGamepadHub(
                []() -> std::expected<std::shared_ptr<sshd::GamepadBackend>, std::string> {
                    return sshd::makeGameInputBackend({});
                },
                sshd::padUsesGamepad(opt.pad) ? keyboard : nullptr,
                {.is_foreground = sshd::processOwnsForegroundWindow,
                 .detect_steam = sshd::detectSteamContext,
                 .probe = opt.pad_probe});
    }
#elif defined(__APPLE__)
    // The framework reports controllers on the main queue, which runWithHostEventLoop below serves.
    if (sshd::padUsesGamepad(opt.pad) || opt.pad_probe) {
        sshd::startGamepadHub(
                []() -> std::expected<std::shared_ptr<sshd::GamepadBackend>, std::string> {
                    return sshd::makeGameControllerBackend();
                },
                sshd::padUsesGamepad(opt.pad) ? keyboard : nullptr,
                {.is_foreground = sshd::processOwnsForegroundWindow,
                 .detect_steam = sshd::detectSteamContext,
                 .probe = opt.pad_probe});
    }
#else
    if (sshd::padUsesGamepad(opt.pad) || opt.pad_probe) sshd::logWarn("sshd: pad: no gamepad backend on this platform");
#endif
    // Hang diagnostics: a running game flips every frame. If flips stop for 10 s, log the stall and the
    // census once (thread states, SPU jump sites), so a freeze leaves evidence in the log instead of
    // looking like a silent dead window.
    if (opt.window) {
        std::thread([] {
            std::uint64_t last = sshd::rsxFlipCount();
            int still = 0;
            bool reported = false;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const std::uint64_t now = sshd::rsxFlipCount();
                if (now != last) {
                    last = now;
                    still = 0;
                    reported = false;
                    continue;
                }
                if (++still == 10 && !reported) {
                    reported = true;
                    sshd::logWarn("sshd: hang: no flips for {} s (last flip {})", still, now);
                    sshd::printCensus();
                }
            }
        }).detach();
    }

    // The window belongs to the main thread on macOS, so the boot and the supervisor run on a thread of
    // their own while the main thread serves the user interface; on Windows this is a plain call.
    sshd::runWithHostEventLoop(
            [&] {
                sshd::bootMainThread(exe, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN", module.image_end - module.base);
                sshd::runSupervisor(opt.seconds);
            },
            opt.window);
}
