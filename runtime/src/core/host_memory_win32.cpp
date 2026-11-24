#include "core/host_memory.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

namespace sshd::host {
namespace {

constexpr ULONG kFaultStackBytes = 0x20000;

std::atomic<FaultHandler> g_fault_handler{nullptr};
thread_local std::string g_instruction_location;

std::string instructionLocation(std::uintptr_t address) {
    if (address == 0) {
        return {};
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return std::format(" (host exception address {:#018x})",
                           static_cast<std::uint64_t>(address));
    }
    MODULEINFO info{};
    if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, path.data(),
                                                static_cast<DWORD>(path.size()));
        if (length > 0 && length < path.size()) {
            path.resize(length);
            const auto offset = address - base;
            return std::format(" (host module {} + {:#x})",
                               std::filesystem::path(path).filename().string(), offset);
        }
    }
    return std::format(" (host exception address {:#018x})",
                       static_cast<std::uint64_t>(address));
}

LONG CALLBACK vectoredHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& rec = *info->ExceptionRecord;
    FaultInfo fault{};
    fault.instruction = reinterpret_cast<std::uintptr_t>(rec.ExceptionAddress);
    if (rec.ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        fault.stack_overflow = true;
    } else if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2) {
        const ULONG_PTR access = rec.ExceptionInformation[0];
        fault.address = rec.ExceptionInformation[1];
        fault.access = access == 1 ? FaultAccess::Write : access == 8 ? FaultAccess::Execute : FaultAccess::Read;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (FaultHandler handler = g_fault_handler.load()) {
        if (fault.instruction != 0 && !fault.stack_overflow &&
            rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            // The guest fault handler formats the architectural address. Keep the stable module
            // identity/RVA in thread-local storage so fatal() can append it without doing OS
            // lookups while handling a declined exception.
            g_instruction_location = instructionLocation(fault.instruction);
        }
        handler(fault);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

std::string faultInstructionLocation() { return g_instruction_location; }

std::size_t pageSize() {
    static const std::size_t size = [] {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwPageSize);
    }();
    return size;
}

void* reserve(std::size_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS); }

bool commit(void* address, std::size_t bytes) {
    return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

#if SSHD_RSX_WRITE_WATCH
void* reserveWriteWatched(std::size_t bytes) {
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
}

bool collectWrittenPages(void* address, std::size_t bytes, std::span<void*> pages, std::size_t& count,
                         std::size_t& granularity) {
    ULONG_PTR found = pages.size();
    ULONG os_granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, address, bytes, pages.data(), &found, &os_granularity) != 0) {
        return false;
    }
    count = found;
    granularity = os_granularity;
    return true;
}
#endif

void decommit(void* address, std::size_t bytes) { VirtualFree(address, bytes, MEM_DECOMMIT); }

void release(void* address, std::size_t) { VirtualFree(address, 0, MEM_RELEASE); }

PageState queryPage(const void* address) {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(address, &info, sizeof info) != sizeof info) {
        return PageState::Free;
    }
    return info.State == MEM_COMMIT    ? PageState::Committed
           : info.State == MEM_RESERVE ? PageState::Reserved
                                       : PageState::Free;
}

bool readable(const void* address, std::size_t bytes) {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t end = begin + std::max<std::size_t>(bytes, 1);
    if (end < begin) {
        return false;
    }
    // One query covers a whole run of pages with the same state and protection.
    for (std::uintptr_t at = begin; at < end;) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof info) != sizeof info ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

std::string lastErrorText() { return std::format("error {}", GetLastError()); }

void installFaultHandler(FaultHandler handler) {
    static std::atomic<bool> installed{false};
    if (!installed.exchange(true)) {
        g_fault_handler.store(handler);
        AddVectoredExceptionHandler(1, &vectoredHandler);
    }
}

void reserveFaultStack() {
    ULONG bytes = kFaultStackBytes;
    SetThreadStackGuarantee(&bytes);
}

}  // namespace sshd::host
