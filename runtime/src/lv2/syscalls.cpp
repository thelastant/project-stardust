// lv2 syscall dispatch. Handlers live in their own files and are referenced from the table.

#include "lv2/syscalls.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>

#include "core/census.h"
#include "core/log.h"
#include "core/threads.h"
#include "sshd/ppu_context.h"
#include "lv2/cell_error.h"

namespace sshd {

// Handlers defined in sys_misc.cpp.
void sysProcessGetPid(SshdPpuContext* ctx, std::uint8_t* base);
void sysProcessIsSpuLockLineReservationAddress(SshdPpuContext* ctx, std::uint8_t* base);
void sysProcessGetSdkVersion(SshdPpuContext* ctx, std::uint8_t* base);
void sysMemoryGetPageAttribute(SshdPpuContext* ctx, std::uint8_t* base);
void sysProcessExit(SshdPpuContext* ctx, std::uint8_t* base);
void sysTtyRead(SshdPpuContext* ctx, std::uint8_t* base);
void sysTtyWrite(SshdPpuContext* ctx, std::uint8_t* base);
void sysTimerUsleep(SshdPpuContext* ctx, std::uint8_t* base);
void sysTimerSleep(SshdPpuContext* ctx, std::uint8_t* base);
void sysTimeGetCurrentTime(SshdPpuContext* ctx, std::uint8_t* base);
void sysTimeGetTimebaseFrequency(SshdPpuContext* ctx, std::uint8_t* base);
void sysMemoryAllocate(SshdPpuContext* ctx, std::uint8_t* base);
void sysMemoryFree(SshdPpuContext* ctx, std::uint8_t* base);
void sysMemoryGetUserMemorySize(SshdPpuContext* ctx, std::uint8_t* base);
void sysMemoryContainerCreate(SshdPpuContext* ctx, std::uint8_t* base);
void sysMemoryContainerDestroy(SshdPpuContext* ctx, std::uint8_t* base);
void sysMemoryContainerGetSize(SshdPpuContext* ctx, std::uint8_t* base);
void sysMemoryAllocateFromContainer(SshdPpuContext* ctx, std::uint8_t* base);
void sysExitNotification(SshdPpuContext* ctx, std::uint8_t* base);

// Handlers defined in sys_mutex_cond.cpp.
void sysMutexCreate(SshdPpuContext* ctx, std::uint8_t* base);
void sysMutexDestroy(SshdPpuContext* ctx, std::uint8_t* base);
void sysMutexLock(SshdPpuContext* ctx, std::uint8_t* base);
void sysMutexTryLock(SshdPpuContext* ctx, std::uint8_t* base);
void sysMutexUnlock(SshdPpuContext* ctx, std::uint8_t* base);
void sysCondCreate(SshdPpuContext* ctx, std::uint8_t* base);
void sysCondDestroy(SshdPpuContext* ctx, std::uint8_t* base);
void sysCondWait(SshdPpuContext* ctx, std::uint8_t* base);
void sysCondSignal(SshdPpuContext* ctx, std::uint8_t* base);
void sysCondSignalAll(SshdPpuContext* ctx, std::uint8_t* base);

// Handlers defined in sys_semaphore.cpp.
void sysSemaphoreCreate(SshdPpuContext* ctx, std::uint8_t* base);
void sysSemaphoreDestroy(SshdPpuContext* ctx, std::uint8_t* base);
void sysSemaphoreWait(SshdPpuContext* ctx, std::uint8_t* base);
void sysSemaphoreTryWait(SshdPpuContext* ctx, std::uint8_t* base);
void sysSemaphorePost(SshdPpuContext* ctx, std::uint8_t* base);
void sysSemaphoreGetValue(SshdPpuContext* ctx, std::uint8_t* base);

// Handlers defined in sys_event.cpp.
void sysEventFlagCreate(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventFlagDestroy(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventFlagWait(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventFlagSet(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventFlagClear(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventFlagCancel(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventFlagGet(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventQueueCreate(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventQueueDestroy(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventQueueReceive(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventQueueTryReceive(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventQueueDrain(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventPortCreate(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventPortDestroy(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventPortConnectLocal(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventPortDisconnect(SshdPpuContext* ctx, std::uint8_t* base);
void sysEventPortSend(SshdPpuContext* ctx, std::uint8_t* base);

// Handlers defined in sys_ppu_thread.cpp.
void sysPpuThreadExit(SshdPpuContext* ctx, std::uint8_t* base);
void sysPpuThreadYield(SshdPpuContext* ctx, std::uint8_t* base);
void sysPpuThreadJoin(SshdPpuContext* ctx, std::uint8_t* base);
void sysPpuThreadDetach(SshdPpuContext* ctx, std::uint8_t* base);
void sysPpuThreadSetPriority(SshdPpuContext* ctx, std::uint8_t* base);
void sysPpuThreadGetPriority(SshdPpuContext* ctx, std::uint8_t* base);

// Handlers defined in sys_fs.cpp.
void sysFsOpen(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsRead(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsWrite(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsClose(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsOpendir(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsReaddir(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsClosedir(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsStat(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsFstat(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsMkdir(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsUnlink(SshdPpuContext* ctx, std::uint8_t* base);
void sysFsLseek(SshdPpuContext* ctx, std::uint8_t* base);

// Handlers defined in sys_spu.cpp.
void sysSpuInitialize(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupCreate(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupDestroy(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadInitialize(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupStart(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupTerminate(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupJoin(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadWriteLs(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadReadLs(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadWriteSpuMb(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupConnectEvent(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupDisconnectEvent(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadConnectEvent(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadDisconnectEvent(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadBindQueue(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadUnbindQueue(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupSetCooperativeVictims(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupConnectEventAllThreads(SshdPpuContext* ctx, std::uint8_t* base);
void sysSpuThreadGroupDisconnectEventAllThreads(SshdPpuContext* ctx, std::uint8_t* base);

namespace {

// The numbers the game's code issues (the census of `tools/ps3tool.py probe`: the r11 value in front
// of each syscall site) plus the implemented ones. Names come from config/lv2_syscalls.json, which
// mirrors RPCS3 `Emu/Cell/lv2/lv2.cpp`; 988 has none. Keep sorted by number.
constexpr SyscallEntry kSyscalls[] = {
        {1, "sys_process_getpid", sysProcessGetPid},
        {14, "sys_process_is_spu_lock_line_reservation_address", sysProcessIsSpuLockLineReservationAddress},
        {22, "_sys_process_exit", sysProcessExit},
        {25, "sys_process_get_sdk_version", sysProcessGetSdkVersion},
        {41, "_sys_ppu_thread_exit", sysPpuThreadExit},
        {43, "sys_ppu_thread_yield", sysPpuThreadYield, true},
        {44, "sys_ppu_thread_join", sysPpuThreadJoin},
        {45, "sys_ppu_thread_detach", sysPpuThreadDetach},
        {47, "sys_ppu_thread_set_priority", sysPpuThreadSetPriority},
        {48, "sys_ppu_thread_get_priority", sysPpuThreadGetPriority},
        {82, "sys_event_flag_create", sysEventFlagCreate},
        {83, "sys_event_flag_destroy", sysEventFlagDestroy},
        {85, "sys_event_flag_wait", sysEventFlagWait},
        {87, "sys_event_flag_set", sysEventFlagSet},
        {90, "sys_semaphore_create", sysSemaphoreCreate},
        {91, "sys_semaphore_destroy", sysSemaphoreDestroy},
        {92, "sys_semaphore_wait", sysSemaphoreWait},
        {93, "sys_semaphore_trywait", sysSemaphoreTryWait},
        {94, "sys_semaphore_post", sysSemaphorePost},
        {100, "sys_mutex_create", sysMutexCreate},
        {101, "sys_mutex_destroy", sysMutexDestroy},
        {102, "sys_mutex_lock", sysMutexLock},
        {103, "sys_mutex_trylock", sysMutexTryLock},
        {104, "sys_mutex_unlock", sysMutexUnlock},
        {105, "sys_cond_create", sysCondCreate},
        {106, "sys_cond_destroy", sysCondDestroy},
        {107, "sys_cond_wait", sysCondWait},
        {108, "sys_cond_signal", sysCondSignal},
        {109, "sys_cond_signal_all", sysCondSignalAll},
        {114, "sys_semaphore_get_value", sysSemaphoreGetValue},
        {118, "sys_event_flag_clear", sysEventFlagClear},
        {128, "sys_event_queue_create", sysEventQueueCreate},
        {129, "sys_event_queue_destroy", sysEventQueueDestroy},
        {130, "sys_event_queue_receive", sysEventQueueReceive},
        {131, "sys_event_queue_tryreceive", sysEventQueueTryReceive},
        {132, "sys_event_flag_cancel", sysEventFlagCancel},
        {133, "sys_event_queue_drain", sysEventQueueDrain},
        {134, "sys_event_port_create", sysEventPortCreate},
        {135, "sys_event_port_destroy", sysEventPortDestroy},
        {136, "sys_event_port_connect_local", sysEventPortConnectLocal},
        {137, "sys_event_port_disconnect", sysEventPortDisconnect},
        {138, "sys_event_port_send", sysEventPortSend},
        {139, "sys_event_flag_get", sysEventFlagGet},
        {141, "sys_timer_usleep", sysTimerUsleep, true},
        {142, "sys_timer_sleep", sysTimerSleep, true},
        {145, "sys_time_get_current_time", sysTimeGetCurrentTime},
        {147, "sys_time_get_timebase_frequency", sysTimeGetTimebaseFrequency, true},
        {169, "sys_spu_initialize", sysSpuInitialize},
        {170, "sys_spu_thread_group_create", sysSpuThreadGroupCreate},
        {171, "sys_spu_thread_group_destroy", sysSpuThreadGroupDestroy},
        {172, "sys_spu_thread_initialize", sysSpuThreadInitialize},
        {173, "sys_spu_thread_group_start", sysSpuThreadGroupStart},
        {177, "sys_spu_thread_group_terminate", sysSpuThreadGroupTerminate},
        {178, "sys_spu_thread_group_join", sysSpuThreadGroupJoin},
        {181, "sys_spu_thread_write_ls", sysSpuThreadWriteLs},
        {182, "sys_spu_thread_read_ls", sysSpuThreadReadLs},
        {185, "sys_spu_thread_group_connect_event", sysSpuThreadGroupConnectEvent},
        {186, "sys_spu_thread_group_disconnect_event", sysSpuThreadGroupDisconnectEvent},
        {190, "sys_spu_thread_write_spu_mb", sysSpuThreadWriteSpuMb},
        {191, "sys_spu_thread_connect_event", sysSpuThreadConnectEvent},
        {192, "sys_spu_thread_disconnect_event", sysSpuThreadDisconnectEvent},
        {193, "sys_spu_thread_bind_queue", sysSpuThreadBindQueue},
        {194, "sys_spu_thread_unbind_queue", sysSpuThreadUnbindQueue},
        {250, "sys_spu_thread_group_set_cooperative_victims", sysSpuThreadGroupSetCooperativeVictims},
        {251, "sys_spu_thread_group_connect_event_all_threads", sysSpuThreadGroupConnectEventAllThreads},
        {252, "sys_spu_thread_group_disconnect_event_all_threads", sysSpuThreadGroupDisconnectEventAllThreads},
        {300, "sys_vm_memory_map", nullptr},
        {301, "sys_vm_unmap", nullptr},
        {324, "sys_memory_container_create", sysMemoryContainerCreate},
        {325, "sys_memory_container_destroy", sysMemoryContainerDestroy},
        {341, "sys_memory_container_create", sysMemoryContainerCreate},
        {342, "sys_memory_container_destroy", sysMemoryContainerDestroy},
        {343, "sys_memory_container_get_size", sysMemoryContainerGetSize},
        {348, "sys_memory_allocate", sysMemoryAllocate},
        {349, "sys_memory_free", sysMemoryFree},
        {350, "sys_memory_allocate_from_container", sysMemoryAllocateFromContainer},
        {351, "sys_memory_get_page_attribute", sysMemoryGetPageAttribute},
        {352, "sys_memory_get_user_memory_size", sysMemoryGetUserMemorySize},
        {402, "sys_tty_read", sysTtyRead},
        {403, "sys_tty_write", sysTtyWrite},
        {801, "sys_fs_open", sysFsOpen},
        {802, "sys_fs_read", sysFsRead},
        {803, "sys_fs_write", sysFsWrite},
        {804, "sys_fs_close", sysFsClose},
        {805, "sys_fs_opendir", sysFsOpendir},
        {806, "sys_fs_readdir", sysFsReaddir},
        {807, "sys_fs_closedir", sysFsClosedir},
        {808, "sys_fs_stat", sysFsStat},
        {809, "sys_fs_fstat", sysFsFstat},
        {810, "sys_fs_link", nullptr},
        {811, "sys_fs_mkdir", sysFsMkdir},
        {812, "sys_fs_rename", nullptr},
        {813, "sys_fs_rmdir", nullptr},
        {814, "sys_fs_unlink", sysFsUnlink},
        {815, "sys_fs_utime", nullptr},
        {818, "sys_fs_lseek", sysFsLseek},
        {820, "sys_fs_fsync", nullptr},
        {831, "sys_fs_truncate", nullptr},
        {832, "sys_fs_ftruncate", nullptr},
        {834, "sys_fs_chmod", nullptr},
        {988, nullptr, sysExitNotification},
};

static_assert(std::ranges::is_sorted(kSyscalls, {}, &SyscallEntry::number), "syscall table must be sorted by number");

// One census slot per table entry, so a syscall issued by several PPU threads counts without the census mutex.
CensusSlot g_census_slots[std::size(kSyscalls)];

}  // namespace

std::span<const SyscallEntry> syscallTable() { return kSyscalls; }

const SyscallEntry* findSyscall(uint32_t number) {
    const auto it = std::ranges::lower_bound(kSyscalls, number, {}, &SyscallEntry::number);
    return it != std::end(kSyscalls) && it->number == number ? &*it : nullptr;
}

}  // namespace sshd

extern "C" {

void sshdSyscall(SshdPpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)site;
    const uint32_t number = static_cast<uint32_t>(ctx->gpr[11]);
    const sshd::SyscallEntry* entry = sshd::findSyscall(number);
    const char* name = entry != nullptr ? entry->name : nullptr;
    const bool implemented = entry != nullptr && entry->handler != nullptr;
    if (entry != nullptr) {
        sshd::censusSyscall(sshd::g_census_slots[entry - sshd::kSyscalls], number, name, implemented);
    } else {
        sshd::censusSyscall(number, name, implemented);
    }

    if (implemented) {
        const sshd::ActivityScope activity(name != nullptr ? name : "syscall ?");
        if (entry->kernel_free) {
            entry->handler(ctx, base);
            return;
        }
        // After the ActivityScope, so a contended kernel lock is attributed to this call.
        const sshd::KernelSection kernel;
        entry->handler(ctx, base);
        return;
    }

    // Unimplemented syscalls are rare; they stay in the kernel section like every other non-exempt entry.
    const sshd::KernelSection kernel;
    // The key is built on the stack: unimplemented syscalls can be hot.
    std::array<char, 24> key;
    const auto end = std::format_to_n(key.data(), key.size(), "default-sc-{}", number);
    sshd::logOnce(std::string_view(key.data(), static_cast<size_t>(end.out - key.data())),
                  "sshd: default sc {} {}({:#x}, {:#x}, {:#x}, {:#x})", number, name != nullptr ? name : "?",
                  ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
    ctx->gpr[3] = sshd::kCellOk;
}

}  // extern "C"
