#pragma once

#include "XeSS_UE_Dx12.h"
#include <shaders/depth_scale/DS_Dx12.h>

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace XeSSUnreal::Detail
{
constexpr size_t MaxCandidates = 12;
constexpr size_t CopyListCount = DS_NUM_OF_HEAPS;
constexpr uint64_t ProbePresents = 8;
constexpr uint64_t ConfirmedRequests = 120;
constexpr uint32_t StaleRequests = 60;

struct QueueState
{
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint64_t written = 0;
};

struct Transition
{
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES state;
};

struct CandidateInfo
{
    bool confirmed = false;
    uint64_t confirmedAt = 0;
    uint64_t lastUsed = 0;
};

struct SharedState
{
    std::array<std::atomic<ID3D12Resource*>, MaxCandidates> candidates {};
    std::atomic<uint32_t> candidateCount { 0 };
    std::recursive_mutex mutex;

    std::array<CandidateInfo, MaxCandidates> candidateInfo {};
    std::unordered_map<ID3D12CommandList*, std::vector<Transition>> recorded;
    std::unordered_map<ID3D12Resource*, QueueState> queueStates;
    uint64_t writeOrder = 0;
    uint64_t lastCopyPoint = 0;
    std::unordered_set<ID3D12CommandList*> requests;
    uint64_t requestCount = 0;
    uint32_t requestsSinceCopy = 0;

    std::atomic<void*> sceneDepthRdg { nullptr };
    std::atomic<const wchar_t*> sceneDepthName { nullptr };
    std::atomic<void*> sceneDepthRhi { nullptr };
    std::atomic<uint64_t> flattenedAt { 0 };
    std::atomic<uintptr_t> resourceVtable { 0 };
    std::atomic<uint32_t> minWidth { 0 };
    std::atomic<uint32_t> minHeight { 0 };
    std::atomic<uint32_t> depthFormat { DXGI_FORMAT_UNKNOWN };
    std::atomic<bool> capturedDepth { false };
    size_t resolvedOffsets[2] = {};
};

SharedState& State();

bool SafeReadBytes(uintptr_t address, void* out, size_t size);
template <typename T> bool SafeRead(uintptr_t address, T& out) { return SafeReadBytes(address, &out, sizeof(T)); }

int CandidateSlot(ID3D12Resource* resource);
int AddCandidateLocked(ID3D12Resource* depth);
bool MatchesFilter(ID3D12Resource* resource);
void ProbeSceneDepth();
void RecordSceneDepth(void* sceneDepth);
ID3D12Resource* RequestCopy(ID3D12GraphicsCommandList* commandList, ID3D12Resource* layout);

using PFN_AddVelocityFlatteningXeSSPass = void*(__fastcall*) (void* graphBuilder, void* sceneDepth, void* sceneVelocity,
                                                              void* view, void* arg5, void* arg6, void* arg7,
                                                              void* arg8);
using PFN_FlattenVelocity = void*(__fastcall*) (void* graphBuilder, void* sceneDepth, void* sceneVelocity, void* view,
                                                const wchar_t* name, void* event, bool upsample);
using PFN_GetSecondaryViewRectSize = int32_t*(__fastcall*) (void* view, int32_t* outSize, void* r8, void* r9);

extern PFN_AddVelocityFlatteningXeSSPass o_AddVelocityFlatteningXeSSPass;
extern PFN_FlattenVelocity o_FlattenVelocity;
extern PFN_GetSecondaryViewRectSize o_GetSecondaryViewRectSize;
extern const wchar_t* g_xessVelocityName;
extern int32_t g_viewRectOffset;

void* __fastcall hkAddVelocityFlatteningXeSSPass(void* graphBuilder, void* sceneDepth, void* sceneVelocity, void* view,
                                                 void* arg5, void* arg6, void* arg7, void* arg8);
void* __fastcall hkFlattenVelocity(void* graphBuilder, void* sceneDepth, void* sceneVelocity, void* view,
                                   const wchar_t* name, void* event, bool upsample);
int32_t* __fastcall hkGetSecondaryViewRectSize(void* view, int32_t* outSize, void* r8, void* r9);

bool HookModule(HMODULE module);
} // namespace XeSSUnreal::Detail
