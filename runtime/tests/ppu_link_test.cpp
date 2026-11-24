// Links the generated tree against the stub runtime and checks its tables. No game code runs.

#include <cstdint>
#include <cstdio>

#include "libsre_ppu_tables.h"
#include "sshd/ppu_context.h"
#include "sshd/spu_context.h"

namespace {

int g_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

// Every mapping is sorted and resolves to its own host function, and every import stub is a registered
// function (the trampoline).
void checkImage(const SshdFuncMapping* mappings, uint32_t count, const SshdImportInfo* imports, uint32_t import_count) {
    for (uint32_t i = 0; i < count; i++) {
        const SshdFuncMapping& m = mappings[i];
        CHECK(m.host != nullptr);
        CHECK(i == 0 || mappings[i - 1].guest < m.guest);
        CHECK(sshdLookup(m.guest) == m.host);
    }

    for (uint32_t i = 0; i < import_count; i++) {
        const SshdImportInfo& info = imports[i];
        CHECK(info.module != nullptr);
        SshdPpuFunc* host = sshdLookup(info.stub);
        CHECK(host != nullptr);
    }
}

}  // namespace

int main() {
    const uint32_t count = kSshdPpuFuncMappingCount;
    sshdRegisterFuncs(kSshdPpuFuncMappings, count);
    checkImage(kSshdPpuFuncMappings, count, kSshdPpuImports, kSshdPpuImportCount);

    // The stub runtime keeps one merged table, so a second call adds the libsre mappings to it.
    sshdRegisterFuncs(kSshdLibsrePpuFuncMappings, kSshdLibsrePpuFuncMappingCount);
    checkImage(kSshdLibsrePpuFuncMappings, kSshdLibsrePpuFuncMappingCount, kSshdLibsrePpuImports,
               kSshdLibsrePpuImportCount);
    // The first image must stay reachable after the second span was added.
    checkImage(kSshdPpuFuncMappings, count, kSshdPpuImports, kSshdPpuImportCount);

    CHECK(kSshdLibsrePpuExportCount > 0);
    for (uint32_t i = 0; i < kSshdLibsrePpuExportCount; i++) {
        CHECK(kSshdLibsrePpuExports[i].module != nullptr);
    }

    // The game registers these tables next to the PPU ones; here they only have to link and be well formed.
    CHECK(kSshdSpuImageCount > 0);
    for (uint32_t i = 0; i < kSshdSpuImageCount; i++) {
        CHECK(kSshdSpuImages[i].name != nullptr);
        CHECK(kSshdSpuImages[i].entry_count > 0 && kSshdSpuImages[i].entries != nullptr);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "ppu link: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf(
            "ppu link ok: %u mappings, %u imports, executable %.16s; libsre: %u mappings, %u imports, %u exports, "
            "image %.16s; spu: %u images\n",
            count, kSshdPpuImportCount, kSshdPpuExecutableSha256, kSshdLibsrePpuFuncMappingCount,
            kSshdLibsrePpuImportCount, kSshdLibsrePpuExportCount, kSshdLibsrePpuExecutableSha256, kSshdSpuImageCount);
    return 0;
}
