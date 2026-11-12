#pragma once

// Subresource-0 barrier, multi-mip/array guide copy, typed Depth-Stencil SRV mapping,
// and lazy neutral R32_FLOAT depth fallback for DLSS 5 Neural Rendering (DX12).

#include <Logger.h>
#include <algorithm>
#include <cstdint>
#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace DlssNr::MipSubresource
{
inline bool NeedsSubresource0Barrier(const D3D12_RESOURCE_DESC& d) { return d.MipLevels > 1 || d.DepthOrArraySize > 1; }
inline bool NeedsSubresource0Barrier(ID3D12Resource* r) { return r != nullptr && NeedsSubresource0Barrier(r->GetDesc()); }
inline UINT SubresourceIndexFor(ID3D12Resource* r)
{
    return NeedsSubresource0Barrier(r) ? 0u : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
}

// Transitions subresource 0 only when mip/array > 1; symmetric restore keeps ResTrack valid.
inline void BarrierSubresource0(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                D3D12_RESOURCE_STATES after)
{
    if (!cmd || !res || before == after)
        return;
    D3D12_RESOURCE_BARRIER b { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE };
    b.Transition = { res, SubresourceIndexFor(res), before, after };
    cmd->ResourceBarrier(1, &b);
}

inline bool CopyTopLevelSubresourceDx12(ID3D12GraphicsCommandList* cmd, ID3D12Resource* dst, ID3D12Resource* src,
                                        UINT width = 0, UINT height = 0)
{
    if (!cmd || !dst || !src)
        return false;
    const D3D12_RESOURCE_DESC s = src->GetDesc(), d = dst->GetDesc();
    if (s.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        s.SampleDesc.Count != 1 || d.SampleDesc.Count != 1)
        return false;

    const UINT maxW = (std::min)(static_cast<UINT>(s.Width), static_cast<UINT>(d.Width));
    const UINT maxH = (std::min)(s.Height, d.Height);
    const UINT copyW = (width > 0) ? (std::min)(width, maxW) : maxW;
    const UINT copyH = (height > 0) ? (std::min)(height, maxH) : maxH;
    if (copyW == 0 || copyH == 0)
        return false;

    const CD3DX12_TEXTURE_COPY_LOCATION dstLoc(dst, 0), srcLoc(src, 0);
    const CD3DX12_BOX box(0, 0, 0, copyW, copyH, 1);
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
    return true;
}

inline bool IsDepthStencilTypedFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

inline DXGI_FORMAT TypedCloneFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

inline DXGI_FORMAT ResolveGuideSrvFormat(DXGI_FORMAT f)
{
    if (f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_X24_TYPELESS_G8_UINT)
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    if (f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS ||
        f == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT)
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    return TypedCloneFormat(f);
}

inline bool NeedsGuideClone(const D3D12_RESOURCE_DESC& d)
{
    if (NeedsSubresource0Barrier(d))
        return true;
    if (d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        return false;
    return TypedCloneFormat(d.Format) != d.Format;
}

inline void CreateTopMipSrv(ID3D12Device* dev, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    if (!dev)
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const bool usable = res && res->GetDesc().SampleDesc.Count <= 1;
    const D3D12_RESOURCE_DESC desc = usable ? res->GetDesc() : D3D12_RESOURCE_DESC {};
    srv.Format = usable ? ResolveGuideSrvFormat(desc.Format) : DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv.ViewDimension = (usable && desc.DepthOrArraySize > 1) ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                                              : D3D12_SRV_DIMENSION_TEXTURE2D;
    if (usable && desc.DepthOrArraySize > 1)
        srv.Texture2DArray = { 0, 1, 0, 1, 0, 0.0f };
    else
        srv.Texture2D = { 0, 1, 0, 0.0f };
    dev->CreateShaderResourceView(usable ? res : nullptr, &srv, cpu);
}

inline void CreateTopMipUav(ID3D12Device* dev, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    if (!dev)
        return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    const bool usable = res && res->GetDesc().SampleDesc.Count <= 1;
    const D3D12_RESOURCE_DESC desc = usable ? res->GetDesc() : D3D12_RESOURCE_DESC {};
    uav.Format = usable ? TypedCloneFormat(desc.Format) : DXGI_FORMAT_R32G32B32A32_FLOAT;
    uav.ViewDimension = (usable && desc.DepthOrArraySize > 1) ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                                              : D3D12_UAV_DIMENSION_TEXTURE2D;
    if (usable && desc.DepthOrArraySize > 1)
        uav.Texture2DArray.ArraySize = 1;
    dev->CreateUnorderedAccessView(usable ? res : nullptr, nullptr, &uav, cpu);
}

inline ID3D12Resource* EnsureNeutralDepthTexture(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, UINT width,
                                                 UINT height, ID3D12Resource** tex, ID3D12Resource** uploadOut,
                                                 float fillValue = 1.0f)
{
    if (!dev || !cmd || !tex || !uploadOut || width == 0 || height == 0)
        return nullptr;
    if (*tex != nullptr)
        return ((*tex)->GetDesc().Width == width && (*tex)->GetDesc().Height == height) ? *tex : nullptr;

    const auto heapDef = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, width, height, 1, 1);
    ID3D12Resource* created = nullptr;
    if (FAILED(dev->CreateCommittedResource(&heapDef, D3D12_HEAP_FLAG_NONE, &texDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
        return nullptr;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, nullptr, nullptr, &totalBytes);
    const auto heapUp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
    ID3D12Resource* upload = nullptr;
    if (FAILED(dev->CreateCommittedResource(&heapUp, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        return created->Release(), nullptr;

    uint8_t* mapped = nullptr;
    if (SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped != nullptr)
    {
        std::vector<float> row(width, fillValue);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, row.data(), size_t(width) * 4);
        upload->Unmap(0, nullptr);
    }
    const CD3DX12_TEXTURE_COPY_LOCATION dstLoc(created, 0), srcLoc(upload, fp);
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    BarrierSubresource0(cmd, created, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return (*uploadOut = upload), (*tex = created);
}

inline void LogDepthFallbackOnce(const char* reason, const D3D12_RESOURCE_DESC* desc)
{
    static std::mutex s_mtx;
    static std::unordered_set<uint64_t> s_seen;
    const uint64_t key = desc ? ((uint64_t(desc->Format) & 0xFF) | ((uint64_t(desc->MipLevels) & 0xFF) << 8) |
                                 ((uint64_t(desc->SampleDesc.Count) & 0xFF) << 16) |
                                 ((uint64_t(desc->Width) & 0xFFFF) << 24) | ((uint64_t(desc->Height) & 0xFFFF) << 40))
                              : 0;
    std::lock_guard<std::mutex> lock(s_mtx);
    if (s_seen.insert(key).second)
        LOG_WARN("DLSS-NR optional depth fallback ({}) -> w={} fmt={}", reason ? reason : "invalid",
                 desc ? desc->Width : 0, desc ? static_cast<int>(desc->Format) : -1);
}
} // namespace DlssNr::MipSubresource
