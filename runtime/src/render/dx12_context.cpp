#include "render/dx12_context.h"

#if defined(_WIN32)

#include <cstdint>
#include <format>
#include <string>

#include "core/log.h"

namespace sshd {
namespace {

std::string hresultMessage(const char* operation, HRESULT result) {
    return std::format("{} failed (HRESULT {:#010x})", operation, static_cast<std::uint32_t>(result));
}

std::string wideToUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}

}  // namespace

bool Dx12Context::initialize(std::string& error) {
    if (valid()) return true;
    error.clear();

    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(result)) {
        error = hresultMessage("CreateDXGIFactory2", result);
        return false;
    }

    // Prefer a real hardware adapter. The factory order is driver-selected; skipping software
    // adapters avoids silently making the optional backend unusably slow on machines with a GPU.
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        result = factory_->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                      IID_PPV_ARGS(&candidate));
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
            error = hresultMessage("IDXGIFactory6::EnumAdapterByGpuPreference", result);
            return false;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(candidate->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
            continue;
        }
        adapter_ = std::move(candidate);
        adapter_name_ = wideToUtf8(description.Description);
        break;
    }
    if (!adapter_) {
        error = "no hardware adapter supports Direct3D 12 feature level 12_0";
        return false;
    }

    result = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
    if (FAILED(result)) {
        error = hresultMessage("D3D12CreateDevice", result);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    result = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_));
    if (FAILED(result)) {
        error = hresultMessage("ID3D12Device::CreateCommandQueue", result);
        device_.Reset();
        return false;
    }

    logInfo("sshd: render: Direct3D 12 device initialized ({})", adapter_name_);
    return true;
}

}  // namespace sshd

#endif  // defined(_WIN32)
