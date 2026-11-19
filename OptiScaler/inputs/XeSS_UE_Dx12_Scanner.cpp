#include "pch.h"
#include "XeSS_UE_Dx12_Internal.h"

#include <State.h>

namespace XeSSUnreal::Detail
{
SharedState& State()
{
    static SharedState s;
    return s;
}

bool SafeReadBytes(uintptr_t address, void* out, size_t size)
{
    __try
    {
        memcpy(out, (const void*) address, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

namespace
{
struct RdgTexture
{
    void* vtable;
    const wchar_t* name;
    void* rhiTexture;
};

bool IsUserPointer(uintptr_t value) { return value >= 0x10000 && value < 0x7FFFFFFF0000 && (value & 7) == 0; }

bool SafeGetDesc(ID3D12Resource* resource, D3D12_RESOURCE_DESC& desc)
{
    __try
    {
        desc = resource->GetDesc();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadResourceName(const wchar_t* name, std::wstring& text)
{
    text.clear();
    for (size_t i = 0; i < 64; i++)
    {
        wchar_t c = 0;
        if (!SafeRead((uintptr_t) (name + i), c))
            return false;
        if (c == L'\0')
            return !text.empty();
        if (c < 0x20 || c > 0x7E)
            return false;
        text.push_back(c);
    }
    return false;
}

bool IsDepthResource(uintptr_t candidate, uintptr_t resourceVtable)
{
    uintptr_t vtable = 0;
    D3D12_RESOURCE_DESC desc {};
    return IsUserPointer(candidate) && SafeRead(candidate, vtable) && vtable == resourceVtable &&
           SafeGetDesc((ID3D12Resource*) candidate, desc) && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
}

ID3D12Resource* FindD3D12Resource(void* rhiTexture, uintptr_t resourceVtable)
{
    auto& s = State();
    auto texture = (uintptr_t) rhiTexture;
    uintptr_t l1 = 0, l2 = 0;
    if (s.resolvedOffsets[0] != 0)
    {
        if (SafeRead(texture + s.resolvedOffsets[0], l1) && IsUserPointer(l1) &&
            SafeRead(l1 + s.resolvedOffsets[1], l2) && IsDepthResource(l2, resourceVtable))
            return (ID3D12Resource*) l2;
        return nullptr;
    }
    for (size_t o1 = 8; o1 < 0x400; o1 += 8)
    {
        if (!SafeRead(texture + o1, l1) || !IsUserPointer(l1))
            continue;
        for (size_t o2 = 0; o2 < 0x80; o2 += 8)
        {
            if (!SafeRead(l1 + o2, l2) || !IsDepthResource(l2, resourceVtable))
                continue;
            s.resolvedOffsets[0] = o1;
            s.resolvedOffsets[1] = o2;
            LOG_INFO("Scene depth ID3D12Resource found at FRHITexture +{:X} -> +{:X}", o1, o2);
            return (ID3D12Resource*) l2;
        }
    }
    return nullptr;
}

bool CoversInput(const D3D12_RESOURCE_DESC& desc)
{
    auto& s = State();
    return desc.Width >= s.minWidth.load() && desc.Height >= s.minHeight.load();
}
} // namespace

bool MatchesFilter(ID3D12Resource* resource)
{
    auto format = State().depthFormat.load(std::memory_order_relaxed);
    if (format == DXGI_FORMAT_UNKNOWN)
        return false;
    auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == format &&
           (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 && CoversInput(desc);
}

int CandidateSlot(ID3D12Resource* resource)
{
    auto& s = State();
    for (size_t i = 0; i < s.candidates.size(); i++)
        if (s.candidates[i].load(std::memory_order_relaxed) == resource)
            return (int) i;
    return -1;
}

int AddCandidateLocked(ID3D12Resource* depth)
{
    auto& s = State();
    size_t slot = 0;
    for (size_t i = 0; i < s.candidates.size(); i++)
    {
        if (s.candidates[i].load() == nullptr) { slot = i; break; }
        if (s.candidateInfo[i].lastUsed < s.candidateInfo[slot].lastUsed) slot = i;
    }
    depth->AddRef();
    auto previous = s.candidates[slot].exchange(depth);
    s.candidateInfo[slot] = { false, 0, s.writeOrder };
    if (previous != nullptr) { s.queueStates.erase(previous); previous->Release(); }
    else { s.candidateCount++; }
    auto desc = depth->GetDesc();
    LOG_INFO("Scene depth candidate: {:X}, {}x{}, format: {}", (size_t) depth, desc.Width, desc.Height, (UINT) desc.Format);
    return (int) slot;
}

void ProbeSceneDepth()
{
    static std::mutex mutex;
    std::scoped_lock lock(mutex);
    auto& s = State();
    auto vtable = s.resourceVtable.load();
    if (vtable == 0 || ::State::Instance().frameCount - s.flattenedAt.load() > ProbePresents)
        return;
    void* rhiTexture = nullptr;
    if (auto rdgTexture = s.sceneDepthRdg.load(); rdgTexture != nullptr)
    {
        RdgTexture rdg {};
        if (SafeRead((uintptr_t) rdgTexture, rdg) && rdg.name == s.sceneDepthName.load())
            rhiTexture = rdg.rhiTexture;
    }
    auto setupRhi = s.sceneDepthRhi.load();
    if (rhiTexture == nullptr)
        rhiTexture = setupRhi;
    if (!IsUserPointer((uintptr_t) rhiTexture))
        return;
    auto depth = FindD3D12Resource(rhiTexture, vtable);
    if (depth == nullptr || !CoversInput(depth->GetDesc()))
    {
        if (rhiTexture == setupRhi)
            s.sceneDepthRhi.compare_exchange_strong(setupRhi, nullptr);
        return;
    }
    s.sceneDepthRhi = rhiTexture;
    std::scoped_lock lockState(s.mutex);
    auto slot = CandidateSlot(depth);
    if (slot < 0)
        slot = AddCandidateLocked(depth);
    auto& info = s.candidateInfo[slot];
    info.confirmed = true;
    info.confirmedAt = s.requestCount;
    info.lastUsed = std::max(info.lastUsed, s.writeOrder);
    s.depthFormat = depth->GetDesc().Format;
}

void RecordSceneDepth(void* sceneDepth)
{
    auto& s = State();
    RdgTexture rdg {};
    std::wstring name;
    if (sceneDepth == nullptr || !SafeRead((uintptr_t) sceneDepth, rdg) || rdg.name == nullptr ||
        ((uintptr_t) rdg.name & 1) != 0 || !ReadResourceName(rdg.name, name))
        return;
    static std::atomic<uint32_t> nameChanges { 0 };
    if (s.sceneDepthName.exchange(rdg.name) != rdg.name && nameChanges++ < 16)
        LOG_INFO("Velocity flattening depth input: {}", wstring_to_string(name));
    s.sceneDepthRdg = sceneDepth;
    if (rdg.rhiTexture != nullptr)
        s.sceneDepthRhi = rdg.rhiTexture;
    s.flattenedAt = ::State::Instance().frameCount;
}
} // namespace XeSSUnreal::Detail
