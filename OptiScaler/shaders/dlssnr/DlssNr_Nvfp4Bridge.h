#pragma once

#include <Config.h>
#include <dlssnr/NrNvfp4.h>
#include <dlssnr/DlssNr_Weights.h>
#include <shaders/dlssnr/DlssNr_Retire.h>
#include <Util.h>

#include <array>
#include <d3d12.h>
#include <filesystem>

namespace DlssNr::Nvfp4Bridge
{
struct RetiredNvfp4Ticket
{
    ID3D12Device* device = nullptr;
    unsigned long long ticket = 0;
};

struct RetiredNativeHost
{
    std::shared_ptr<void> host;
};

inline Retire::FenceQueue<RetiredNvfp4Ticket>& RetiredQueue()
{
    static Retire::FenceQueue<RetiredNvfp4Ticket> q;
    return q;
}

inline Retire::FenceQueue<RetiredNativeHost>& RetiredHostQueue()
{
    static Retire::FenceQueue<RetiredNativeHost> q;
    return q;
}

inline void FreeTicketNow(const RetiredNvfp4Ticket& item)
{
    if (item.device != nullptr && item.ticket != 0)
        DlssNr::Nvfp4::FreeRetired(item.device, item.ticket);
}

inline void FreeHostNow(const RetiredNativeHost& item)
{
    (void) item;
}

struct CachedNativeHost
{
    std::shared_ptr<void> host;
    ID3D12Device* device = nullptr;
    unsigned preset = 0;
    unsigned width = 0;
    unsigned height = 0;
};

inline std::array<CachedNativeHost, 10>& PassHosts()
{
    static std::array<CachedNativeHost, 10> hosts {};
    return hosts;
}

inline std::filesystem::path AssetsPath()
{
    return Util::DllPath().parent_path() / L"OptiScaler" / L"nvfp4";
}

inline void ApplyWeightsFolder(HMODULE forwarder, const Config& cfg)
{
    if (forwarder == nullptr)
        return;
    DlssNr::Weights::Exports exports {};
    DlssNr::Weights::Load(exports, forwarder);
    const std::string setA = cfg.DlssNrWeightsSetA.value_or(cfg.DlssNrWeightsSet.value_or_default());
    const std::string setB = cfg.DlssNrWeightsSetB.value_or(cfg.DlssNrWeightsSet.value_or_default());
    DlssNr::Weights::Select(exports, setA, setB, " DX12");
}

inline void ConfigureForPresets(const unsigned int* passPresets, unsigned int passCount)
{
    auto* cfg = Config::Instance();
    DlssNr::Nvfp4::SetFp8Enabled(cfg->DlssNrFp8Enabled.value_or_default());
    DlssNr::Nvfp4::SetInt8Enabled(cfg->DlssNrInt8Enabled.value_or_default());
    bool anyNvfp4 = false;
    for (unsigned int i = 0; i < passCount; ++i)
    {
        if (DlssNr::Nvfp4::IsNvfp4(passPresets[i]))
        {
            anyNvfp4 = true;
            break;
        }
    }
    DlssNr::Nvfp4::Configure(anyNvfp4, AssetsPath());
}

inline unsigned int RuntimePresetFor(unsigned int requestedPreset)
{
    return DlssNr::Nvfp4::IsNvfp4(requestedPreset) ? 1u : requestedPreset;
}

inline bool BeginPass(unsigned int requestedPreset, ID3D12GraphicsCommandList* cmdList)
{
    return DlssNr::Nvfp4::BeginEvaluation(requestedPreset, cmdList);
}

inline bool EvaluateNativeHostPass(unsigned int passIndex, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                                   unsigned int requestedPreset, unsigned int width, unsigned int height,
                                   const DlssNr::Nvfp4::HostPass& pass)
{
    if (passIndex >= PassHosts().size() || device == nullptr || cmdList == nullptr)
        return false;
    auto& slot = PassHosts()[passIndex];
    const unsigned resolvedPreset = DlssNr::Nvfp4::Current(requestedPreset);
    if (!slot.host || slot.device != device || slot.preset != resolvedPreset || slot.width != width ||
        slot.height != height)
    {
        if (slot.host)
            RetiredHostQueue().Park({ std::move(slot.host) });
        slot.host = DlssNr::Nvfp4::CreateHost(device, resolvedPreset, width, height, AssetsPath());
        slot.device = device;
        slot.preset = resolvedPreset;
        slot.width = width;
        slot.height = height;
    }
    if (!slot.host)
        return false;
    return DlssNr::Nvfp4::RunHost(slot.host, cmdList, pass);
}

inline bool EvaluatePass(unsigned int requestedPreset, ID3D12GraphicsCommandList* cmdList, bool borrowed)
{
    DlssNr::Nvfp4::EndEvaluation(cmdList);
    if (!DlssNr::Nvfp4::IsNvfp4(requestedPreset))
        return true;
    return DlssNr::Nvfp4::OursRan(!borrowed);
}

inline ID3D12Device*& LastDevice()
{
    static ID3D12Device* dev = nullptr;
    return dev;
}

inline void TickRetire(ID3D12Device* device, ID3D12CommandQueue* queue, uint64_t epoch, bool gpuSafeEnabled,
                       unsigned int evaluations)
{
    if (device == nullptr)
        return;
    LastDevice() = device;
    const unsigned long long ticket = DlssNr::Nvfp4::RetireIdle(device, evaluations);
    if (ticket != 0)
        RetiredQueue().Park({ device, ticket });
    if (queue != nullptr)
    {
        RetiredQueue().SignalEpoch(device, queue, epoch);
        RetiredHostQueue().SignalEpoch(device, queue, epoch);
    }
    RetiredQueue().Tick(epoch, gpuSafeEnabled, [](const RetiredNvfp4Ticket& item, bool) { FreeTicketNow(item); });
    RetiredHostQueue().Tick(epoch, gpuSafeEnabled, [](const RetiredNativeHost& item, bool) { FreeHostNow(item); });
}

inline void ReleaseAll(ID3D12Device* device = nullptr)
{
    for (auto& slot : PassHosts())
    {
        slot.host.reset();
        slot.device = nullptr;
        slot.preset = 0;
        slot.width = 0;
        slot.height = 0;
    }
    RetiredHostQueue().FreeAll(FreeHostNow);
    RetiredQueue().FreeAll(FreeTicketNow);
    ID3D12Device* target = device != nullptr ? device : LastDevice();
    if (target != nullptr)
        DlssNr::Nvfp4::Release(target);
    LastDevice() = nullptr;
}
} // namespace DlssNr::Nvfp4Bridge
