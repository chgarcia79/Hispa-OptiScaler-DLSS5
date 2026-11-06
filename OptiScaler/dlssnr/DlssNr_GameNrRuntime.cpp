#include "pch.h"
#include "DlssNr_GameNrRuntime.h"
#include "DlssNr_GameNrLog.h"

#include <State.h>
#include <Util.h>
#include <proxies/NVNGX_Proxy.h>

#include <d3d12.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>

namespace DlssNr::GameNr
{
namespace
{
struct DirectBridge
{
    HMODULE forwarder = nullptr;
    std::wstring snippetPath;
    int(__cdecl* preload)(const wchar_t*, const wchar_t*, ID3D12Device*, const void*) = nullptr;
    int(__cdecl* create)(const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, const void*, void**) = nullptr;
    int(__cdecl* evaluate)(ID3D12GraphicsCommandList*, const void*, const void*) = nullptr;
    int(__cdecl* release)(void*) = nullptr;
};

std::mutex g_mutex;
DirectBridge g_bridge {};
std::unordered_set<const NVSDK_NGX_Handle*> g_liveDirect;
std::unordered_set<const NVSDK_NGX_Handle*> g_liveDriver;
std::atomic<size_t> g_liveCount { 0 };

std::optional<std::filesystem::path> FindBesideDllOrExe(const char* fileName)
{
    auto found = Util::FindFilePath(Util::DllPath().remove_filename(), fileName);
    return found ? found : Util::FindFilePath(Util::ExePath().remove_filename(), fileName);
}

bool EnsureBridgeLocked()
{
    if (g_bridge.forwarder)
        return g_bridge.create && !g_bridge.snippetPath.empty();
    const auto fwd = FindBesideDllOrExe("nvngx.dll_dlssnr.dll");
    const auto snip = FindBesideDllOrExe("nvngx_dlssnr.dll");
    if (!fwd || !snip || !(g_bridge.forwarder = LoadLibraryW(fwd->wstring().c_str())))
        return false;
    g_bridge.snippetPath = snip->wstring();
    g_bridge.preload = reinterpret_cast<decltype(g_bridge.preload)>(GetProcAddress(g_bridge.forwarder, "dlssnr_direct_preload"));
    g_bridge.create = reinterpret_cast<decltype(g_bridge.create)>(GetProcAddress(g_bridge.forwarder, "dlssnr_direct_create"));
    g_bridge.evaluate = reinterpret_cast<decltype(g_bridge.evaluate)>(GetProcAddress(g_bridge.forwarder, "dlssnr_direct_evaluate"));
    g_bridge.release = reinterpret_cast<decltype(g_bridge.release)>(GetProcAddress(g_bridge.forwarder, "dlssnr_direct_release"));
    return g_bridge.preload && g_bridge.create && g_bridge.evaluate && g_bridge.release;
}

NVSDK_NGX_Parameter* QueryCapabilityBlock(ID3D12Device* device)
{
    NVSDK_NGX_Parameter* cap = nullptr;
    if ((!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device)) || !NVNGXProxy::D3D12_GetCapabilityParameters())
        return nullptr;
    return NVNGXProxy::D3D12_GetCapabilityParameters()(&cap) == NVSDK_NGX_Result_Success ? cap : nullptr;
}

void SyncCountLocked() { g_liveCount.store(g_liveDirect.size() + g_liveDriver.size(), std::memory_order_release); }
} // namespace

bool HasCandidates() { return FindBesideDllOrExe("nvngx.dll_dlssnr.dll") && FindBesideDllOrExe("nvngx_dlssnr.dll"); }
bool IsGameNativeNrRunning() { return g_liveCount.load(std::memory_order_acquire) != 0; }

NVSDK_NGX_Result CreateDirect(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle)
{
    ID3D12Device* device = nullptr;
    if (!commands || !params || !handle || FAILED(commands->GetDevice(IID_PPV_ARGS(&device))) || !device)
        return NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Parameter* cap = QueryCapabilityBlock(device);
    std::unique_lock lock(g_mutex);
    if (!EnsureBridgeLocked())
    {
        device->Release();
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    const auto bridge = g_bridge;
    lock.unlock();

    if (cap)
        bridge.preload(bridge.snippetPath.c_str(), ::State::Instance().NVNGX_ApplicationDataPath.c_str(), device, cap);
    void* raw = nullptr;
    const auto res = (NVSDK_NGX_Result) bridge.create(
        bridge.snippetPath.c_str(), ::State::Instance().NVNGX_ApplicationDataPath.c_str(), device, commands, params, &raw);
    device->Release();
    *handle = static_cast<NVSDK_NGX_Handle*>(raw);
    LOG_INFO("DLSS-NR (game): direct runtime CreateFeature(18) result=0x{:08X} handle={}", (unsigned int) res, raw);
    if (res != NVSDK_NGX_Result_Success || !*handle)
        return res == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : res;

    LogGameSettingsAndYieldOwnNr(params);
    lock.lock();
    g_liveDirect.insert(*handle);
    SyncCountLocked();
    return res;
}

void RecordDriverCreate(const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params)
{
    if (!handle)
        return;
    LogGameSettingsAndYieldOwnNr(params);
    std::lock_guard lock(g_mutex);
    g_liveDriver.insert(handle);
    SyncCountLocked();
}

bool Owns(const NVSDK_NGX_Handle* handle)
{
    std::lock_guard lock(g_mutex);
    return handle && g_liveDirect.contains(handle);
}

bool ForgetDriverHandle(const NVSDK_NGX_Handle* handle)
{
    std::lock_guard lock(g_mutex);
    if (!handle || g_liveDriver.erase(handle) == 0)
        return false;
    SyncCountLocked();
    return true;
}

NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params)
{
    decltype(g_bridge.evaluate) fn = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (g_liveDirect.contains(handle))
            fn = g_bridge.evaluate;
    }
    return fn ? (NVSDK_NGX_Result) fn(commands, handle, params) : NVSDK_NGX_Result_FAIL_FeatureNotFound;
}
NVSDK_NGX_Result Release(NVSDK_NGX_Handle* handle)
{
    decltype(g_bridge.release) fn = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (g_liveDirect.erase(handle) > 0)
        {
            SyncCountLocked();
            fn = g_bridge.release;
        }
    }
    return fn ? (NVSDK_NGX_Result) fn(handle) : NVSDK_NGX_Result_FAIL_FeatureNotFound;
}
void Preload(ID3D12Device* device)
{
    if (!device)
        return;
    const auto start = std::chrono::steady_clock::now();
    NVSDK_NGX_Parameter* cap = QueryCapabilityBlock(device);
    std::unique_lock lock(g_mutex);
    if (!cap || !EnsureBridgeLocked())
        return;
    const auto bridge = g_bridge;
    lock.unlock();
    const int res = bridge.preload(bridge.snippetPath.c_str(), ::State::Instance().NVNGX_ApplicationDataPath.c_str(), device, cap);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    LOG_INFO("DLSS-NR: Preload NR finished in {} ms (result=0x{:X})", ms, (unsigned int) res);
}
void PreloadRuntimeIfEnabled(ID3D12Device* device)
{
    if (ShouldPreload(Config::Instance()->DlssNrPreload.value_or_default(), HasCandidates(), DriverGate::Blocked()))
        Preload(device);
}
void Shutdown()
{
    std::unordered_set<const NVSDK_NGX_Handle*> direct;
    decltype(g_bridge.release) fn = nullptr;
    {
        std::lock_guard lock(g_mutex);
        direct.swap(g_liveDirect);
        g_liveDriver.clear();
        g_liveCount.store(0, std::memory_order_release);
        fn = g_bridge.release;
    }
    for (const auto* h : direct)
        if (fn)
            fn(const_cast<NVSDK_NGX_Handle*>(h));
}
} // namespace DlssNr::GameNr
