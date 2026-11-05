#pragma once

// Deferred GPU-safe retirement queue backed by ID3D12Fence (adapted from janblade PR #277 for HispaOptiScaler v1.0.4-dev9).
// Ensures parked nvngx_dlssnr features and scratch ID3D12Resources are only released once fence->GetCompletedValue() >= retireFenceValue
// AND at least kFrames submission epochs have elapsed, preventing UAF device hangs under Frame Generation / UE5 / RE Engine.

#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace DlssNr::Retire
{
constexpr int kFrames = 32;
constexpr int kStuckFrames = 600;

template <typename Item> class FenceQueue
{
  public:
    void RecordCommandList(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
    {
        if (!device || !cmdList)
            return;
        std::lock_guard<std::mutex> lock(_fenceMutex);
        EnsureFenceLocked(device);
        _openLists.insert(static_cast<const void*>(cmdList));
    }

    void OnQueueSubmission(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        if (!queue || count == 0 || !lists)
            return;
        std::lock_guard<std::mutex> lock(_fenceMutex);
        bool matched = false;
        for (UINT i = 0; i < count; ++i)
            matched |= (_openLists.erase(static_cast<const void*>(lists[i])) > 0);
        if (_fence != nullptr && (matched || _pendingSignal))
            SignalQueueLocked(queue);
    }

    void SignalEpoch(ID3D12Device* device, ID3D12CommandQueue* queue, uint64_t epoch)
    {
        if (!device || !queue || epoch == _lastSignaledEpoch)
            return;
        std::lock_guard<std::mutex> lock(_fenceMutex);
        _lastSignaledEpoch = epoch;
        EnsureFenceLocked(device);
        if (_fence != nullptr)
            SignalQueueLocked(queue);
    }

    void Park(const Item& item)
    {
        uint64_t targetFence = 0;
        {
            std::lock_guard<std::mutex> lock(_fenceMutex);
            targetFence = _nextFenceValue + ((!_openLists.empty() || _lastSignaledValue == 0) ? 1ull : 0ull);
            _pendingSignal = true;
        }
        _parked.push_back({ item, targetFence, kFrames });
    }

    template <typename FreeFn> void Tick(uint64_t epoch, bool gpuSafeEnabled, FreeFn&& freeFn)
    {
        if (epoch == _lastTickEpoch)
            return;
        _lastTickEpoch = epoch;
        uint64_t completed = 0;
        bool hasFence = false;
        {
            std::lock_guard<std::mutex> lock(_fenceMutex);
            if (_fence != nullptr)
                hasFence = true, completed = _fence->GetCompletedValue();
        }
        for (size_t i = 0; i < _parked.size();)
        {
            auto& entry = _parked[i];
            --entry.framesLeft;
            const bool gpuDone = !gpuSafeEnabled || (hasFence && entry.fenceValue > 0 && completed >= entry.fenceValue);
            if ((entry.framesLeft <= 0 && gpuDone) || entry.framesLeft <= -kStuckFrames)
            {
                freeFn(entry.item, gpuDone);
                _parked.erase(_parked.begin() + i);
                continue;
            }
            ++i;
        }
    }

    template <typename FreeNowFn> void FreeAll(FreeNowFn&& freeNow)
    {
        for (auto& entry : _parked)
            freeNow(entry.item);
        _parked.clear();
        std::lock_guard<std::mutex> lock(_fenceMutex);
        _openLists.clear();
        _pendingSignal = false;
        if (_fence != nullptr)
            _fence->Release(), _fence = nullptr;
        _device = nullptr;
        _nextFenceValue = _lastSignaledValue = 0;
        _lastSignaledEpoch = _lastTickEpoch = UINT64_MAX;
    }

  private:
    struct ParkedEntry { Item item; uint64_t fenceValue = 0; int framesLeft = kFrames; };

    void SignalQueueLocked(ID3D12CommandQueue* queue)
    {
        const uint64_t nextVal = ++_nextFenceValue;
        if (SUCCEEDED(queue->Signal(_fence, nextVal)))
        {
            _lastSignaledValue = nextVal;
            if (_openLists.empty())
                _pendingSignal = false;
        }
    }

    void EnsureFenceLocked(ID3D12Device* device)
    {
        if (_device == device && _fence != nullptr)
            return;
        if (_fence != nullptr)
            _fence->Release(), _fence = nullptr;
        _device = device;
        _nextFenceValue = _lastSignaledValue = 0;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
            _fence = nullptr;
    }

    std::mutex _fenceMutex;
    ID3D12Device* _device = nullptr;
    ID3D12Fence* _fence = nullptr;
    uint64_t _nextFenceValue = 0, _lastSignaledValue = 0, _lastSignaledEpoch = UINT64_MAX, _lastTickEpoch = UINT64_MAX;
    bool _pendingSignal = false;
    std::unordered_set<const void*> _openLists;
    std::vector<ParkedEntry> _parked;
};
} // namespace DlssNr::Retire
