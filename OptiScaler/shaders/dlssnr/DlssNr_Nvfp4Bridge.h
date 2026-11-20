#pragma once

#include <Config.h>
#include <dlssnr/NrNvfp4.h>
#include <dlssnr/DlssNr_Weights.h>
#include <shaders/dlssnr/DlssNr_Retire.h>
#include <Util.h>

#include <d3d12.h>
#include <filesystem>

namespace DlssNr::Nvfp4Bridge
{
struct RetiredNvfp4Ticket
{
    ID3D12Device* device = nullptr;
    unsigned long long ticket = 0;
};

inline Retire::FenceQueue<RetiredNvfp4Ticket>& RetiredQueue()
{
    static Retire::FenceQueue<RetiredNvfp4Ticket> q;
    return q;
}

inline void FreeTicketNow(const RetiredNvfp4Ticket& item)
{
    if (item.device != nullptr && item.ticket != 0)
        DlssNr::Nvfp4::FreeRetired(item.device, item.ticket);
}

inline std::filesystem::path AssetsPath()
{
    return Util::DllPath().parent_path() / L"OptiScaler" / L"nvfp4";
}

inline void ApplyWeightsFolder(HMODULE forwarder, const Config& cfg)
{
    if (forwarder == nullptr)
        return;
    using PFN_Select = void (*)(const wchar_t*);
    static auto fn = reinterpret_cast<PFN_Select>(GetProcAddress(forwarder, "dlssnr_weights_select"));
    if (fn != nullptr)
    {
        const auto folder = DlssNr::Weights::Folder(cfg.DlssNrWeightsSet.value_or_default());
        fn(folder.empty() ? L"" : folder.c_str());
    }
}

inline void ConfigureForPresets(const unsigned int* passPresets, unsigned int passCount)
{
    DlssNr::Nvfp4::SetFp8Enabled(Config::Instance()->DlssNrFp8Enabled.value_or_default());
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
        RetiredQueue().SignalEpoch(device, queue, epoch);
    RetiredQueue().Tick(epoch, gpuSafeEnabled, [](const RetiredNvfp4Ticket& item, bool) { FreeTicketNow(item); });
}

inline void ReleaseAll(ID3D12Device* device = nullptr)
{
    RetiredQueue().FreeAll(FreeTicketNow);
    ID3D12Device* target = device != nullptr ? device : LastDevice();
    if (target != nullptr)
        DlssNr::Nvfp4::Release(target);
    LastDevice() = nullptr;
}
} // namespace DlssNr::Nvfp4Bridge
