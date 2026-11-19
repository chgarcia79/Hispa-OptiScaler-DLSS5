#include "pch.h"
#include "XeSS_UE_Dx12_Internal.h"

#include <detours/detours.h>

namespace XeSSUnreal::Detail
{
namespace
{
ID3D12Device* g_device = nullptr;
DS_Dx12* g_shader = nullptr;
std::array<ID3D12CommandAllocator*, CopyListCount> g_allocators {};
std::array<ID3D12GraphicsCommandList*, CopyListCount> g_copyLists {};
std::array<uint64_t, CopyListCount> g_copyFenceValues {};
ID3D12Fence* g_fence = nullptr;
uint64_t g_fenceValue = 0;
uint32_t g_nextCopyList = 0;

using PFN_ResourceBarrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using PFN_CommandListReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PFN_ResourceBarrier o_ResourceBarrier = nullptr;
PFN_CommandListReset o_CommandListReset = nullptr;
PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;

bool IsComputeReadable(D3D12_RESOURCE_STATES state)
{
    return (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0 &&
           (state & D3D12_RESOURCE_STATE_DEPTH_WRITE) == 0;
}

void STDMETHODCALLTYPE hkResourceBarrier(ID3D12GraphicsCommandList* This, UINT NumBarriers,
                                         const D3D12_RESOURCE_BARRIER* pBarriers)
{
    auto& s = State();
    if (s.candidateCount.load(std::memory_order_relaxed) != 0 && pBarriers != nullptr)
    {
        for (UINT i = 0; i < NumBarriers; i++)
        {
            const auto& barrier = pBarriers[i];
            if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                (barrier.Transition.Subresource != 0 && barrier.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
                continue;
            auto resource = barrier.Transition.pResource;
            auto known = CandidateSlot(resource) >= 0;
            if (!known && ((barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_DEPTH_WRITE) == 0 || !MatchesFilter(resource)))
                continue;
            std::scoped_lock lock(s.mutex);
            if (!known && CandidateSlot(resource) < 0)
                AddCandidateLocked(resource);
            s.recorded[This].push_back({ resource, barrier.Transition.StateAfter });
        }
    }
    o_ResourceBarrier(This, NumBarriers, pBarriers);
}

HRESULT STDMETHODCALLTYPE hkCommandListReset(ID3D12GraphicsCommandList* This, ID3D12CommandAllocator* pAllocator,
                                             ID3D12PipelineState* pInitialState)
{
    auto& s = State();
    if (s.candidateCount.load(std::memory_order_relaxed) != 0)
    {
        std::scoped_lock lock(s.mutex);
        s.recorded.erase(This);
        s.requests.erase(This);
    }
    return o_CommandListReset(This, pAllocator, pInitialState);
}

void ApplyTransitions(ID3D12CommandList* commandList)
{
    auto& s = State();
    auto recorded = s.recorded.find(commandList);
    if (recorded == s.recorded.end())
        return;
    for (auto& transition : recorded->second)
    {
        auto slot = CandidateSlot(transition.resource);
        if (slot < 0)
            continue;
        auto& queueState = s.queueStates[transition.resource];
        queueState.state = transition.state;
        if (transition.state & D3D12_RESOURCE_STATE_DEPTH_WRITE)
        {
            queueState.written = ++s.writeOrder;
            s.candidateInfo[slot].lastUsed = s.writeOrder;
        }
    }
    s.recorded.erase(recorded);
}

ID3D12Resource* SelectSource()
{
    auto& s = State();
    ID3D12Resource* best = nullptr;
    bool bestConfirmed = false;
    uint64_t bestWritten = UINT64_MAX;
    for (size_t i = 0; i < s.candidates.size(); i++)
    {
        auto resource = s.candidates[i].load();
        if (resource == nullptr) continue;
        auto qs = s.queueStates.find(resource);
        if (qs == s.queueStates.end() || qs->second.written <= s.lastCopyPoint || !IsComputeReadable(qs->second.state))
            continue;
        auto& info = s.candidateInfo[i];
        bool confirmed = info.confirmed && s.requestCount - info.confirmedAt <= ConfirmedRequests;
        if ((confirmed && !bestConfirmed) || (confirmed == bestConfirmed && qs->second.written < bestWritten))
        {
            best = resource; bestConfirmed = confirmed; bestWritten = qs->second.written;
        }
    }
    s.lastCopyPoint = s.writeOrder;
    return best;
}

ID3D12GraphicsCommandList* RecordCopy(ID3D12CommandQueue* queue, uint64_t& fenceValue)
{
    auto& s = State();
    if (g_shader == nullptr || !g_shader->CanRender() || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return nullptr;
    auto source = SelectSource();
    if (source == nullptr)
        return nullptr;
    auto index = g_nextCopyList;
    if (g_fence->GetCompletedValue() < g_copyFenceValues[index])
        return nullptr;
    if (FAILED(g_allocators[index]->Reset()) || FAILED(g_copyLists[index]->Reset(g_allocators[index], nullptr)))
        return nullptr;
    auto buffer = g_shader->Buffer();
    auto srcDesc = source->GetDesc();
    auto bufDesc = buffer->GetDesc();
    g_shader->SetBufferState(g_copyLists[index], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bool dispatched = g_shader->Dispatch(g_copyLists[index], source, buffer, 1.0f,
                                         (UINT) std::min(srcDesc.Width, bufDesc.Width),
                                         std::min(srcDesc.Height, bufDesc.Height));
    g_shader->SetBufferState(g_copyLists[index], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (FAILED(g_copyLists[index]->Close()) || !dispatched)
        return nullptr;
    s.requestsSinceCopy = 0;
    s.capturedDepth = true;
    fenceValue = ++g_fenceValue;
    g_copyFenceValues[index] = fenceValue;
    g_nextCopyList = (index + 1) % CopyListCount;
    return g_copyLists[index];
}

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists,
                                             ID3D12CommandList* const* ppCommandLists)
{
    ProbeSceneDepth();
    auto& s = State();
    if (s.candidateCount.load(std::memory_order_relaxed) == 0)
        return o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
    struct Submission { UINT start; UINT count; ID3D12GraphicsCommandList* copy; uint64_t fenceValue; };
    std::vector<Submission> submissions;
    {
        std::scoped_lock lock(s.mutex);
        UINT start = 0;
        for (UINT i = 0; i < NumCommandLists; i++)
        {
            if (s.requests.erase(ppCommandLists[i]) != 0)
            {
                uint64_t fv = 0;
                if (auto copy = RecordCopy(This, fv); copy != nullptr)
                {
                    submissions.push_back({ start, i - start, copy, fv });
                    start = i;
                }
            }
            ApplyTransitions(ppCommandLists[i]);
        }
        submissions.push_back({ start, NumCommandLists - start, nullptr, 0 });
    }
    for (auto& sub : submissions)
    {
        if (sub.count != 0)
            o_ExecuteCommandLists(This, sub.count, ppCommandLists + sub.start);
        if (sub.copy != nullptr)
        {
            ID3D12CommandList* copy[] = { sub.copy };
            o_ExecuteCommandLists(This, 1, copy);
            This->Signal(g_fence, sub.fenceValue);
        }
    }
}

bool InitCopy(ID3D12GraphicsCommandList* commandList, ID3D12Device* device)
{
    if (g_shader != nullptr) return g_shader->IsInit();
    g_device = device;
    g_shader = new DS_Dx12("Scene Depth Copy", device);
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    for (size_t i = 0; i < CopyListCount; i++)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[i]))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[i], nullptr, IID_PPV_ARGS(&g_copyLists[i]))))
            return false;
        g_copyLists[i]->SetName(L"Scene Depth Copy");
        g_copyLists[i]->Close();
    }
    D3D12_COMMAND_QUEUE_DESC qd {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
    auto listVt = *(void***) commandList;
    auto queueVt = *(void***) queue;
    o_CommandListReset = (PFN_CommandListReset) listVt[10];
    o_ResourceBarrier = (PFN_ResourceBarrier) listVt[26];
    o_ExecuteCommandLists = (PFN_ExecuteCommandLists) queueVt[10];
    queue->Release();
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_CommandListReset, hkCommandListReset);
    DetourAttach(&(PVOID&) o_ResourceBarrier, hkResourceBarrier);
    DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);
    DetourTransactionCommit();
    return g_shader->IsInit();
}
} // namespace

ID3D12Resource* RequestCopy(ID3D12GraphicsCommandList* commandList, ID3D12Resource* layout)
{
    auto& s = State();
    if (s.candidateCount.load() == 0) return nullptr;
    std::scoped_lock lock(s.mutex);
    if (g_shader == nullptr)
    {
        ID3D12Device* device = nullptr;
        if (FAILED(commandList->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
        InitCopy(commandList, device);
        device->Release();
    }
    if (!g_shader->IsInit() || o_ExecuteCommandLists == nullptr) return nullptr;
    auto buffer = g_shader->Buffer();
    uint32_t w = s.minWidth.load(), h = s.minHeight.load();
    if (buffer != nullptr) { auto d = buffer->GetDesc(); w = std::max(w, (uint32_t) d.Width); h = std::max(h, d.Height); }
    if (buffer == nullptr || buffer->GetDesc().Width != w || buffer->GetDesc().Height != h)
        if (!g_shader->CreateBufferResource(g_device, layout, w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) return nullptr;
    s.requests.insert(commandList);
    s.requestCount++;
    if (++s.requestsSinceCopy == StaleRequests)
        LOG_WARN("Scene depth not copied for {} frames", StaleRequests);
    return s.requestsSinceCopy < StaleRequests ? g_shader->Buffer() : nullptr;
}
} // namespace XeSSUnreal::Detail
