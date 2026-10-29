#pragma once

// Shared RAII & deferred retirement helpers for modular DLSS-NR DX12 passes (DenoiseFirst & DetailReuse).
// Preserves bindless game root signatures, skips heap capture during internal compute/NGX work,
// safely retires D3D12/NGX resources across in-flight FG frames, and saves/restores NGX 1:1 params.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <hooks/D3D12_Hooks.h>
#include <proxies/NVNGX_Proxy.h>
#include <resource_tracking/ResTrack_dx12.h>
#include <vector>

namespace DlssNrDx12Helpers
{
struct ScopedStateEnvelope
{
    ID3D12GraphicsCommandList* cmd;
    ScopedSkipHeapCapture skipHeap {};
    explicit ScopedStateEnvelope(ID3D12GraphicsCommandList* c) : cmd(c) { D3D12Hooks::SetRootSignatureTracking(false); }
    ~ScopedStateEnvelope()
    {
        D3D12Hooks::RestoreRoot(cmd);
        D3D12Hooks::SetRootSignatureTracking(true);
    }
};

struct DeferredRetireQueue
{
    struct Entry
    {
        uint64_t epoch = 0;
        ID3D12Resource* r0 = nullptr;
        ID3D12Resource* r1 = nullptr;
        ID3D12Resource* r2 = nullptr;
        NVSDK_NGX_Handle* handle = nullptr;
    };
    std::vector<Entry> items;

    void Push(uint64_t epoch, ID3D12Resource* r0, ID3D12Resource* r1, ID3D12Resource* r2,
              NVSDK_NGX_Handle* handle = nullptr)
    {
        if (r0 || r1 || r2 || handle)
            items.push_back({ epoch, r0, r1, r2, handle });
    }

    void Collect(uint64_t currentEpoch, bool forceAll = false)
    {
        for (auto it = items.begin(); it != items.end();)
        {
            if (forceAll || currentEpoch >= it->epoch + 12)
            {
                if (it->handle && NVNGXProxy::D3D12_ReleaseFeature())
                    NVNGXProxy::D3D12_ReleaseFeature()(it->handle);
                SAFE_RELEASE(it->r0);
                SAFE_RELEASE(it->r1);
                SAFE_RELEASE(it->r2);
                it = items.erase(it);
            }
            else
                ++it;
        }
    }
};

struct ScopedOneToOneNgxParams
{
    NVSDK_NGX_Parameter* p;
    uint32_t w = 0, h = 0, ow = 0, oh = 0, sw = 0, sh = 0, rst = 0;
    int pq = 0;
    void *outRes = nullptr, *rrOutRes = nullptr;
    bool hasW, hasH, hasOw, hasOh, hasSw, hasSh, hasPq, hasRst, hasOut, hasRrOut;

    ScopedOneToOneNgxParams(NVSDK_NGX_Parameter* params, uint32_t rw, uint32_t rh, ID3D12Resource* target, bool reset)
        : p(params)
    {
        hasW = p->Get(NVSDK_NGX_Parameter_Width, &w) == NVSDK_NGX_Result_Success;
        hasH = p->Get(NVSDK_NGX_Parameter_Height, &h) == NVSDK_NGX_Result_Success;
        hasOw = p->Get(NVSDK_NGX_Parameter_OutWidth, &ow) == NVSDK_NGX_Result_Success;
        hasOh = p->Get(NVSDK_NGX_Parameter_OutHeight, &oh) == NVSDK_NGX_Result_Success;
        hasSw = p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &sw) == NVSDK_NGX_Result_Success;
        hasSh = p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &sh) == NVSDK_NGX_Result_Success;
        hasPq = p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &pq) == NVSDK_NGX_Result_Success;
        hasRst = p->Get(NVSDK_NGX_Parameter_Reset, &rst) == NVSDK_NGX_Result_Success;
        hasOut = p->Get(NVSDK_NGX_Parameter_Output, &outRes) == NVSDK_NGX_Result_Success;
        hasRrOut = p->Get("DLSSD.Output", &rrOutRes) == NVSDK_NGX_Result_Success;

        p->Set(NVSDK_NGX_Parameter_Width, rw);
        p->Set(NVSDK_NGX_Parameter_Height, rh);
        p->Set(NVSDK_NGX_Parameter_OutWidth, rw);
        p->Set(NVSDK_NGX_Parameter_OutHeight, rh);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, rw);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, rh);
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_DLAA);
        p->Set(NVSDK_NGX_Parameter_Reset, reset ? 1u : 0u);
        p->Set(NVSDK_NGX_Parameter_Output, (void*) target);
        if (hasRrOut)
            p->Set("DLSSD.Output", (void*) target);
    }

    ~ScopedOneToOneNgxParams()
    {
        if (hasW) p->Set(NVSDK_NGX_Parameter_Width, w);
        if (hasH) p->Set(NVSDK_NGX_Parameter_Height, h);
        if (hasOw) p->Set(NVSDK_NGX_Parameter_OutWidth, ow);
        if (hasOh) p->Set(NVSDK_NGX_Parameter_OutHeight, oh);
        if (hasSw) p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, sw);
        if (hasSh) p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, sh);
        if (hasPq) p->Set(NVSDK_NGX_Parameter_PerfQualityValue, pq);
        if (hasRst) p->Set(NVSDK_NGX_Parameter_Reset, rst);
        if (hasOut) p->Set(NVSDK_NGX_Parameter_Output, outRes);
        if (hasRrOut) p->Set("DLSSD.Output", rrOutRes);
    }
};

inline void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                       D3D12_RESOURCE_STATES to)
{
    if (res != nullptr && from != to)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
        cmd->ResourceBarrier(1, &b);
    }
}
} // namespace DlssNrDx12Helpers
