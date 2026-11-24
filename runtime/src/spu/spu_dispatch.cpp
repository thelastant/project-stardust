// The image registry, the dispatcher that generated SPU code calls for every computed transfer, and
// the hooks that end the thread: halt and unhandled words.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include "core/fatal.h"
#include "core/log.h"
#include "crypto/host_crypto.h"
#include "spu/spu_thread.h"

namespace sshd {
namespace {

constexpr std::uint16_t kNoEntry = 0xffff;
constexpr std::uint32_t kLookupSize = kSshdSpuLsSize / 4;

struct ImageTable {
    // LS address / 4 -> index into the image's entries.
    std::array<std::uint16_t, kLookupSize> entry_of;
};

const SshdSpuImageInfo* g_images = nullptr;
std::uint32_t g_image_count = 0;
// Built by registerSpuImages, read by every SPU thread without a lock: registration happens before
// any thread exists.
std::array<std::unique_ptr<ImageTable>, kMaxSpuImages> g_tables;
std::array<std::atomic<bool>, kMaxSpuImages> g_seen;
// One bit per entry of each image, set by the first dispatch to it from any thread (census).
std::array<std::unique_ptr<std::atomic<std::uint64_t>[]>, kMaxSpuImages> g_entry_seen;
// Guest span [g_code_lo, g_code_hi) from the lowest code segment start to the highest code segment end of all images;
// empty (lo > hi) without code segments. Same publication rule as g_images.
std::uint64_t g_code_lo = UINT64_MAX;
std::uint64_t g_code_hi = 0;

[[noreturn]] void badTable(const SshdSpuImageInfo& image, std::string_view why) {
    stopProcess(ExitCode::LoadError, std::format("sshd: spu: image {}: {}", image.name, why));
}

std::string sha256Hex(const std::uint8_t* data, std::uint32_t size) {
    crypto::Sha256Digest digest{};
    if (!crypto::sha256({data, size}, digest)) {
        return {};
    }
    std::string hex;
    for (const std::uint8_t byte : digest) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

std::uint32_t loadWord(const std::uint8_t* ls, std::uint32_t addr) {
    std::uint32_t v;
    std::memcpy(&v, ls + (addr & (kSshdSpuLsSize - 4u)), sizeof v);
    return std::byteswap(v);
}

// The stop paths are out of line so the dispatcher keeps no locals that would block its tail call.
[[noreturn]] SSHD_NOINLINE void stopUnknownTarget(const SshdSpuContext* ctx, std::uint32_t target, SpuThread& thread,
                                                  const SshdSpuImageInfo* image) {
    thread.countUnknownTarget();
    spuStop(thread, std::format("sshd: stop: spu: unknown target ls {:#07x} from ls {:#07x} on thread {:#010x} ({})",
                                target, ctx->jump_site, thread.id(), image != nullptr ? image->name : "untracked"));
}

[[noreturn]] SSHD_NOINLINE void stopNonLink(const SshdSpuContext* ctx, std::uint32_t target, const SpuThread& thread,
                                            const SshdSpuImageInfo& image, std::int32_t delta) {
    spuStop(thread,
            std::format("sshd: stop: spu: target ls {:#07x} from ls {:#07x} on thread {:#010x} ({}) loaded at a "
                        "non-link address (delta {})",
                        target, ctx->jump_site, thread.id(), image.name, delta));
}

[[noreturn]] SSHD_NOINLINE void stopMismatch(std::uint32_t target, const SpuThread& thread,
                                             const SshdSpuImageInfo& image, const SshdSpuEntry& entry,
                                             std::uint32_t word_index, std::uint32_t found) {
    spuStop(thread,
            std::format("sshd: stop: spu: code mismatch at ls {:#07x} ({}) on thread {:#010x}", target, image.name,
                        thread.id()),
            std::format("word {} at ls {:#07x}: LS has {:#010x}, the image has {:#010x}", word_index,
                        target + 4 * word_index, found, entry.check[word_index]));
}

}  // namespace

void registerSpuImages(const SshdSpuImageInfo* images, std::uint32_t count) {
    if (count > kMaxSpuImages) {
        stopProcess(ExitCode::LoadError,
                    std::format("sshd: spu: {} images registered, at most {}", count, kMaxSpuImages));
    }
    g_images = nullptr;
    g_image_count = 0;
    g_code_lo = UINT64_MAX;
    g_code_hi = 0;
    for (std::uint32_t i = 0; i < kMaxSpuImages; i++) {
        g_tables[i].reset();
        g_seen[i].store(false);
        g_entry_seen[i].reset();
    }
    for (std::uint32_t i = 0; i < count; i++) {
        const SshdSpuImageInfo& image = images[i];
        if (image.entry_count >= kNoEntry) badTable(image, "too many entries");
        auto table = std::make_unique<ImageTable>();
        table->entry_of.fill(kNoEntry);
        for (std::uint32_t e = 0; e < image.entry_count; e++) {
            const SshdSpuEntry& entry = image.entries[e];
            if (entry.ls % 4 != 0 || entry.ls >= kSshdSpuLsSize) {
                badTable(image, std::format("entry ls {:#x} is not a word in the LS", entry.ls));
            }
            if (entry.check_count < 1 || entry.check_count > 4 || entry.host == nullptr) {
                badTable(image, std::format("entry ls {:#x} has no check words or no function", entry.ls));
            }
            if (e > 0 && image.entries[e - 1].ls >= entry.ls) {
                badTable(image, std::format("entries are not sorted at ls {:#x}", entry.ls));
            }
            table->entry_of[entry.ls / 4] = static_cast<std::uint16_t>(e);
        }
        for (std::uint32_t j = 0; j < image.segment_count; j++) {
            const SshdSpuSegment& s = image.segments[j];
            if (s.code == 0) continue;
            g_code_lo = std::min<std::uint64_t>(g_code_lo, s.ea);
            g_code_hi = std::max<std::uint64_t>(g_code_hi, std::uint64_t{s.ea} + s.size);
        }
        g_tables[i] = std::move(table);
        // Value-initialised: all bits clear.
        g_entry_seen[i] = std::make_unique<std::atomic<std::uint64_t>[]>((image.entry_count + 63) / 64);
    }
    g_images = images;
    g_image_count = count;
}

std::uint32_t spuImageCount() { return g_image_count; }

bool spuCodeRangeOverlaps(std::uint32_t ea, std::uint32_t size) {
    return ea < g_code_hi && std::uint64_t{ea} + size > g_code_lo;
}

const SshdSpuImageInfo& spuImage(std::uint32_t index) { return g_images[index]; }

const SshdSpuEntry* spuFindEntry(std::uint32_t index, std::uint32_t ls) {
    const std::uint16_t e = g_tables[index]->entry_of[(ls & (kSshdSpuLsSize - 1u)) / 4];
    return e == kNoEntry ? nullptr : &g_images[index].entries[e];
}

bool spuMarkImageSeen(std::uint32_t index) { return !g_seen[index].exchange(true); }

void spuMarkEntryDispatched(std::uint32_t index, std::uint32_t entry_index) {
    std::atomic<std::uint64_t>& word = g_entry_seen[index][entry_index / 64];
    const std::uint64_t bit = std::uint64_t{1} << (entry_index % 64);
    // Plain load first: after warm-up the line is only read, never written.
    if ((word.load(std::memory_order_relaxed) & bit) == 0) word.fetch_or(bit, std::memory_order_relaxed);
}

std::uint32_t spuDistinctEntries(std::uint32_t index) {
    if (index >= g_image_count) return 0;
    std::uint32_t n = 0;
    for (std::uint32_t w = 0; w < (g_images[index].entry_count + 63) / 64; w++) {
        n += static_cast<std::uint32_t>(std::popcount(g_entry_seen[index][w].load(std::memory_order_relaxed)));
    }
    return n;
}

bool checkSpuImages(const std::uint8_t* base) {
    bool ok = true;
    for (std::uint32_t i = 0; i < g_image_count; i++) {
        const SshdSpuImageInfo& image = g_images[i];
        const std::string got = sha256Hex(base + image.ea, image.size);
        if (got != image.sha256) {
            logError("sshd: spu: image {} at {:#010x}: hash {} does not match the generated code ({})", image.name,
                     image.ea, got.empty() ? "(unavailable)" : got, image.sha256);
            ok = false;
        }
    }
    if (ok) {
        logInfo("sshd: spu: {} images ok", g_image_count);
    }
    return ok;
}

}  // namespace sshd

using sshd::SpuLoadRecord;
using sshd::SpuThread;

extern "C" {

void sshdSpuDispatch(SshdSpuContext* __restrict ctx, uint8_t* __restrict ls) {
    const uint32_t target = ctx->jump_target & (kSshdSpuLsSize - 4u);
    if (target == ctx->exit_ls || sshdSpuHaltRequested(ctx)) {
        return;
    }
    SpuThread* thread = sshd::spuThreadOf(ctx);
    const SpuLoadRecord* record = thread->recordAt(target);
    // Resolved by the tracked identity of the target, not by the image of the branching code: the
    // kernel returns into policy modules and policy modules start tasks.
    if (__builtin_expect(record == nullptr, 0)) {
        sshd::stopUnknownTarget(ctx, target, *thread, nullptr);
    }
    const SshdSpuImageInfo& image = sshd::spuImage(record->image);
    std::uint32_t link_target = target;
    if (__builtin_expect(record->delta != 0, 0)) {
        const std::int64_t candidate = std::int64_t{target} - record->delta;
        // These FMOD overlays execute from dynamically relocated LS addresses and publish
        // relocated intra-image entry addresses. Only their recovered entries may dispatch
        // from a relocated segment; instruction-word checks below still gate every target.
        const bool fmod_relocated_overlay_entry =
                image.name != nullptr && record->segment == 0 &&
                (std::string_view(image.name) == "eboot_fmod_raw" ||
                 std::string_view(image.name) == "eboot_fmod_adpcm" ||
                 std::string_view(image.name) == "eboot_fmod_mpeg_init" ||
                 std::string_view(image.name) == "eboot_fmod_mpeg_functions");
        if (!fmod_relocated_overlay_entry) {
            sshd::stopNonLink(ctx, target, *thread, image, record->delta);
        }
        link_target = static_cast<std::uint32_t>(candidate);
    }
    const SshdSpuEntry* entry = sshd::spuFindEntry(record->image, link_target);
    if (__builtin_expect(entry == nullptr, 0)) {
        sshd::stopUnknownTarget(ctx, target, *thread, &image);
    }
    for (uint32_t i = 0; i < entry->check_count; i++) {
        const uint32_t word = sshd::loadWord(ls, target + 4 * i);
        if (__builtin_expect(word != entry->check[i], 0)) {
            sshd::stopMismatch(target, *thread, image, *entry, i, word);
        }
    }
    thread->countDispatch(record->image, target);
    sshd::spuMarkEntryDispatched(record->image, static_cast<std::uint32_t>(entry - image.entries));
    ctx->image_delta = record->delta;
    ctx->jump_target = link_target;
    SSHD_MUSTTAIL return entry->host(ctx, ls);
}

void sshdSpuHalt(SshdSpuContext* ctx, uint8_t* ls, uint32_t site) {
    (void)ls;
    const SpuThread& thread = *sshd::spuThreadOf(ctx);
    sshd::spuStop(thread, std::format("sshd: stop: spu: halt at ls {:#07x} on thread {:#010x}", site, thread.id()));
}

void sshdSpuUnhandled(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t word) {
    (void)ls;
    const SpuThread& thread = *sshd::spuThreadOf(ctx);
    sshd::spuStop(thread, std::format("sshd: stop: spu: unhandled word {:#010x} at ls {:#07x} on thread {:#010x}", word,
                                      site, thread.id()));
}

}  // extern "C"
