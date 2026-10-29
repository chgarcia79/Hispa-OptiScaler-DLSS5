#pragma once

// Shared RAII & deferred retirement helpers for modular DLSS-NR DX12 passes (DenoiseFirst & DetailReuse).
// Preserves bindless game root signatures, skips heap capture during internal compute/NGX work,
// safely retires D3D12/NGX resources across in-flight FG frames, and saves/restores NGX 1:1 params.

#include <SysUtils.h>
#include <State.h>
#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <hooks/D3D12_Hooks.h>
#include <proxies/NVNGX_Proxy.h>
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

inline DXGI_FORMAT ResolveSrvFormat(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return fmt;
    }
}

inline bool CreateSafeSrv(ID3D12Device* dev, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                          DXGI_FORMAT overrideFmt = DXGI_FORMAT_UNKNOWN)
{
    if (!dev || !res)
        return false;
    const auto desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        return false;
    const DXGI_FORMAT srvFmt = ResolveSrvFormat(overrideFmt != DXGI_FORMAT_UNKNOWN ? overrideFmt : desc.Format);
    if (srvFmt == DXGI_FORMAT_UNKNOWN)
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = srvFmt;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = desc.DepthOrArraySize > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
    if (desc.DepthOrArraySize > 1)
    {
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = 1;
    }
    else
        srv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(res, &srv, cpuHandle);
    return true;
}

inline void CopySubRegion(ID3D12GraphicsCommandList* cmd, ID3D12Resource* dst, D3D12_RESOURCE_STATES dstState,
                          ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, uint32_t w, uint32_t h)
{
    Transition(cmd, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmd, dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
    const CD3DX12_TEXTURE_COPY_LOCATION dstLoc(dst, 0), srcLoc(src, 0);
    const CD3DX12_BOX box(0, 0, 0, w, h, 1);
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
    Transition(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, dstState);
    Transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
}
} // namespace DlssNrDx12Helpers
