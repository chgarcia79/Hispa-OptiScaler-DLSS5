#pragma once

#include <Config.h>
#include <dlssnr/NrPassBorrow.h>
#include <dlssnr/PassProfiles.h>
#include <shaders/dlssnr/DlssNr_Retire.h>
#include <shaders/dlssnr/precompile/dlssnr_reapply_Shader.h>
#include <shaders/Shader_Dx12Utils.h>

#include <array>
#include <d3d12.h>

namespace DlssNr::PassBorrowBridge
{
struct RetiredBorrowObj
{
    ID3D12Object* obj = nullptr;
};

inline Retire::FenceQueue<RetiredBorrowObj>& RetiredQueue()
{
    static Retire::FenceQueue<RetiredBorrowObj> q;
    return q;
}

inline void FreeObjNow(const RetiredBorrowObj& item)
{
    if (item.obj != nullptr)
        item.obj->Release();
}

struct ReapplyConstants
{
    uint32_t gMode = 0;
    float gGain = 0.81f;
    uint32_t gWidth = 0;
    uint32_t gHeight = 0;
};

inline void CollectRetired(ID3D12Device* device, ID3D12CommandQueue* queue, uint64_t epoch, bool gpuSafeEnabled)
{
    if (device == nullptr)
        return;
    ID3D12Object* batch[32] {};
    for (;;)
    {
        const unsigned count = NrPassBorrow::TakeRetired(batch, 32);
        for (unsigned i = 0; i < count; ++i)
        {
            if (batch[i] != nullptr)
                RetiredQueue().Park({ batch[i] });
        }
        if (count < 32)
            break;
    }
    if (queue != nullptr)
        RetiredQueue().SignalEpoch(device, queue, epoch);
    RetiredQueue().Tick(epoch, gpuSafeEnabled, FreeObjNow);
}

inline void ReleaseAll()
{
    NrPassBorrow::Reset();
    ID3D12Object* batch[32] {};
    for (;;)
    {
        const unsigned count = NrPassBorrow::TakeRetired(batch, 32);
        for (unsigned i = 0; i < count; ++i)
        {
            if (batch[i] != nullptr)
                batch[i]->Release();
        }
        if (count < 32)
            break;
    }
    RetiredQueue().FreeAll(FreeObjNow);
}

inline void BuildPassCuts(const Config& cfg, unsigned int effectivePasses, std::array<NrPassBorrow::Cut, 10>& cuts,
                          unsigned int& netEvaluations)
{
    netEvaluations = 0;
    for (unsigned int pass = 0; pass < effectivePasses && pass < 10; ++pass)
    {
        if (pass > 0 && DlssNr::Profiles::PassReapplies(cfg, pass))
            continue;
        if (netEvaluations == 0)
        {
            cuts[netEvaluations++] = NrPassBorrow::Cut::None;
        }
        else
        {
            const auto& ov = cfg.DlssNrPassOverrides[pass - 1];
            cuts[netEvaluations++] = NrPassBorrow::ChooseCut(
                ov.borrowTransformer.value_or(false), ov.borrowBelow.value_or(false), ov.borrowLevel.value_or(32u));
        }
    }
}
} // namespace DlssNr::PassBorrowBridge
