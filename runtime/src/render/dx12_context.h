#pragma once

// Native Direct3D 12 device foundation for the optional Windows renderer.

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wrl/client.h>

#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace sshd {

// Owns the DXGI factory, selected adapter, D3D12 device and direct queue. It deliberately does not
// interact with the Vulkan context: the two backends have independent device lifetimes.
class Dx12Context final {
public:
    Dx12Context() = default;
    Dx12Context(const Dx12Context&) = delete;
    Dx12Context& operator=(const Dx12Context&) = delete;

    bool initialize(std::string& error);
    bool valid() const { return device_ != nullptr && queue_ != nullptr; }
    const std::string& adapterName() const { return adapter_name_; }

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    IDXGIFactory6* factory() const { return factory_.Get(); }

private:
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    std::string adapter_name_;
};

}  // namespace sshd

#endif  // defined(_WIN32)
