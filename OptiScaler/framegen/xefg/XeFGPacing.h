#pragma once
// Spreads Intel's generated frames out in time so multi-frame generation above 2X is evenly paced.
#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#define XE_RETURN_ADDRESS() _ReturnAddress()
#else
#define XE_RETURN_ADDRESS() __builtin_return_address(0)
#endif
#include <algorithm>
#include <format>
#include "Logger.h"
#include "SysUtils.h"

namespace XeFGPacing
{
constexpr uint32_t PresentThunkRva = 0x25C0, NativePresentRva = 0x21F730;
constexpr uint32_t PacedCallerRva = 0x2202ED, LastFrameCallerRva = 0x220467;
constexpr uint32_t LimiterEnabledOffset = 0x340, BurstLimiterField = 0x28;
constexpr uint32_t SchedThunkRva = 0x3100, SchedFnRva = 0x21EE30, BurstGateOffset = 0xC0;
constexpr uint32_t SchedLimiterOffset = 0x340, SchedEnableOffset = 0x341;
constexpr uint32_t RingSnapshotFnRva = 0x224CF0, RingOffset = 0x168;
constexpr uint32_t TimestampThunkRva = 0x3430, TimestampFnRva = 0x224B30, RingMeasuredOffset = 0x1B8;
constexpr uint32_t PresentThunkSize = 16;

inline const uint8_t TimestampThunkExpected[PresentThunkSize] = { 0xE9, 0xFB, 0x16, 0x22, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
inline const uint8_t SchedThunkExpected[PresentThunkSize] = { 0xE9, 0x2B, 0xBD, 0x21, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
inline const uint8_t PresentThunkExpected[PresentThunkSize] = { 0xE9, 0x6B, 0xD1, 0x21, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };

using PresentFn = int64_t (*)(void*, uint32_t, uint32_t, uint64_t, void*, void*, uint64_t);
using SchedFn = bool (*)(void*, void*, uint8_t, void*, uint32_t);
using RingSnapshotFn = void* (*) (void*, void*);
using TimestampFn = void* (*) (void*, int64_t*, void*, void*, uint32_t, uint32_t);

constexpr int32_t SampleCount = 15;
inline uint8_t* g_base = nullptr;
inline PresentFn g_native = nullptr;
inline SchedFn g_schedNative = nullptr;
inline RingSnapshotFn g_ringSnapshot = nullptr;
inline TimestampFn g_tsNative = nullptr;
inline uint8_t* g_ring = nullptr;
inline bool g_enabled = false, g_presentHooked = false, g_schedHooked = false, g_tsHooked = false;
inline uint8_t g_presentThunkOrig[PresentThunkSize] {}, g_schedThunkOrig[PresentThunkSize] {}, g_tsThunkOrig[PresentThunkSize] {};
inline LARGE_INTEGER g_freq {};
inline int64_t g_lastBurstQpc = 0, g_periodNs = 0, g_intervalQpc = 0, g_targetQpc = 0;
inline int64_t g_samples[SampleCount] {};
inline int32_t g_sampleCount = 0, g_samplePos = 0;
inline int64_t g_lastPacedQpc = 0, g_lastMultiplier = 0, g_burstBlockQpc = 0, g_renderTimeNs = 0;
inline double g_fedFrameTimeMs = 0.0;
inline int64_t g_nextDeadlineNs = 0, g_burstStepNs = 0;
inline uint32_t g_lastTsIndex = 0, g_lastTsCountPlus1 = 0;

inline bool SchedulerUsable(void* ctx) {
    const auto* b = static_cast<const uint8_t*>(ctx);
    return b[SchedLimiterOffset] == 0 && b[SchedEnableOffset] != 0;
}
inline int64_t NsFromQpc(int64_t delta) {
    if (g_freq.QuadPart <= 0 || delta <= 0) return 0;
    return (delta / g_freq.QuadPart) * 1000000000LL + ((delta % g_freq.QuadPart) * 1000000000LL) / g_freq.QuadPart;
}
inline int64_t QpcFromNs(int64_t ns) { return g_freq.QuadPart > 0 ? (ns * g_freq.QuadPart) / 1000000000LL : 0; }
inline double RenderTimeMs() { return g_renderTimeNs > 0 ? g_renderTimeNs / 1000000.0 : 0.0; }
inline void NoteFedFrameTime(double ms) { g_fedFrameTimeMs = ms; }

inline void PushPeriod(int64_t ns) {
    if (ns <= 0) return;
    g_samples[g_samplePos] = ns;
    g_samplePos = (g_samplePos + 1) % SampleCount;
    if (g_sampleCount < SampleCount) g_sampleCount++;
    int64_t sorted[SampleCount];
    memcpy(sorted, g_samples, sizeof(int64_t) * g_sampleCount);
    std::sort(sorted, sorted + g_sampleCount);
    g_periodNs = sorted[g_sampleCount / 2];
}

inline void NoteFrame(uint64_t index, uint64_t count, int64_t nowQpc) {
    if (index > 1) return;
    if (g_lastBurstQpc != 0) {
        const int64_t totalDeltaNs = NsFromQpc(nowQpc - g_lastBurstQpc);
        if (totalDeltaNs >= 1000000LL && totalDeltaNs <= 500000000LL) {
            const int64_t blockNs = NsFromQpc(g_burstBlockQpc);
            const int64_t pureNs = (totalDeltaNs > blockNs + 2000000LL) ? (totalDeltaNs - blockNs) : 2000000LL;
            PushPeriod(pureNs);
            g_renderTimeNs = g_periodNs;
        }
    }
    g_burstBlockQpc = 0;
    g_lastBurstQpc = nowQpc;
    if (g_periodNs > 0 && g_freq.QuadPart > 0)
        g_intervalQpc = QpcFromNs(g_periodNs / (static_cast<int64_t>(count) + 1));
}

inline void WaitUntil(int64_t targetQpc) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t yieldBelow = QpcFromNs(200000);
    while (now.QuadPart < targetQpc) {
        if ((targetQpc - now.QuadPart) > yieldBelow) Sleep(0);
        else YieldProcessor();
        QueryPerformanceCounter(&now);
    }
}

inline void PaceFrame(uint64_t index, uint64_t count) {
    const int64_t multiplier = static_cast<int64_t>(count) + 1;
    LARGE_INTEGER now, end;
    QueryPerformanceCounter(&now);
    NoteFrame(index, count, now.QuadPart);
    if (multiplier <= 2) return;
    if (index <= 1) {
        if (g_periodNs <= 0 || g_intervalQpc <= 0) return;
        g_targetQpc = now.QuadPart + g_intervalQpc;
    } else {
        if (g_intervalQpc <= 0) return;
        g_targetQpc += g_intervalQpc;
    }
    const int64_t latest = now.QuadPart + QpcFromNs(g_periodNs);
    if (g_targetQpc > latest) g_targetQpc = latest;
    WaitUntil(g_targetQpc);
    QueryPerformanceCounter(&end);
    g_lastPacedQpc = end.QuadPart;
}

inline bool SchedForwarder(void* ctx, void* burst, uint8_t gate, void* timing, uint32_t index) {
    if (g_schedNative == nullptr) return false;
    LARGE_INTEGER before, after;
    QueryPerformanceCounter(&before);
    const bool scheduled = g_schedNative(ctx, burst, gate, timing, index);
    QueryPerformanceCounter(&after);
    g_burstBlockQpc += (after.QuadPart - before.QuadPart);
    return scheduled;
}

inline void ScheduleFrame(void* ctx, uint8_t* burst, uint64_t index) {
    if (g_schedNative == nullptr || g_ringSnapshot == nullptr) return;
    g_ring = reinterpret_cast<uint8_t*>(ctx) + RingOffset;
    alignas(16) uint8_t timing[0x20] {};
    g_ringSnapshot(reinterpret_cast<uint8_t*>(ctx) + RingOffset, timing);
    const uint8_t gate = burst[BurstGateOffset] & 1;
    const uint64_t count = *reinterpret_cast<uint64_t*>(burst + 8);
    LARGE_INTEGER before;
    QueryPerformanceCounter(&before);
    NoteFrame(index, count, before.QuadPart);
    SchedForwarder(ctx, burst, gate, timing, static_cast<uint32_t>(index));
}

inline bool ProviderPacesLastFrame(void* ctx, const uint8_t* burst, uint64_t count) {
    if (count <= 2) return false;
    return reinterpret_cast<const uint8_t*>(ctx)[LimiterEnabledOffset] != 0 &&
           *reinterpret_cast<const uint32_t*>(burst + BurstLimiterField) == 0;
}

inline void TryPace(void* ctx, void* arg5, void* arg6, uint64_t arg7, bool isLast) {
    if (arg5 == nullptr || ((static_cast<uint8_t>(arg7) == 1) == isLast)) return;
    auto* burst = reinterpret_cast<uint8_t*>(arg5) - 0x38;
    const uint64_t count = *reinterpret_cast<uint64_t*>(burst + 8);
    if (count < 1 || count > 5) return;
    g_lastMultiplier = static_cast<int64_t>(count) + 1;
    uint64_t index = count;
    if (!isLast) {
        auto* frames = *reinterpret_cast<uint8_t**>(burst);
        if (frames == nullptr || reinterpret_cast<uint8_t*>(arg6) < frames) return;
        const uint64_t offset = reinterpret_cast<uint8_t*>(arg6) - frames;
        if ((offset % 8) != 0) return;
        index = offset / 8;
        if (index < 1 || index >= count) return;
    }
    if (g_schedNative != nullptr && SchedulerUsable(ctx)) {
        if (!isLast) ScheduleFrame(ctx, burst, index);
        return;
    }
    if (isLast && ProviderPacesLastFrame(ctx, burst, count)) return;
    PaceFrame(index, count);
}

inline int64_t Detour(void* ctx, uint32_t a2, uint32_t a3, uint64_t a4, void* arg5, void* arg6, uint64_t arg7) {
    if (g_enabled) {
        auto* caller = reinterpret_cast<uint8_t*>(XE_RETURN_ADDRESS());
        if (caller == g_base + PacedCallerRva) TryPace(ctx, arg5, arg6, arg7, false);
        else if (caller == g_base + LastFrameCallerRva) TryPace(ctx, arg5, arg6, arg7, true);
        else if (g_lastMultiplier >= 2 && g_burstStepNs > 0 && g_lastPacedQpc != 0) {
            const int64_t targetQpc = g_lastPacedQpc + QpcFromNs(g_burstStepNs);
            LARGE_INTEGER before, after;
            QueryPerformanceCounter(&before);
            if (before.QuadPart < targetQpc) {
                WaitUntil(targetQpc);
                QueryPerformanceCounter(&after);
                g_burstBlockQpc += (after.QuadPart - before.QuadPart);
            }
        }
    }
    const int64_t res = g_native(ctx, a2, a3, a4, arg5, arg6, arg7);
    if (g_enabled) {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        g_lastPacedQpc = end.QuadPart;
    }
    return res;
}

inline void* TsDetour(void* a1, int64_t* out, void* lookup, void* timing, uint32_t index, uint32_t countPlus1) {
    void* const result = g_tsNative(a1, out, lookup, timing, index, countPlus1);
    if (out == nullptr || timing == nullptr || index == 0 || index > 5 || countPlus1 < 3) return result;
    const int64_t median = *reinterpret_cast<const int64_t*>(reinterpret_cast<const uint8_t*>(timing) + 8);
    const int64_t refNs = (g_renderTimeNs > 0) ? g_renderTimeNs : median;
    const int64_t stepNs = std::clamp(refNs / static_cast<int64_t>(countPlus1), 1000000LL, 20000000LL);
    if (stepNs <= 0) return result;
    LARGE_INTEGER nowQpc {};
    QueryPerformanceCounter(&nowQpc);
    const int64_t nowNs = NsFromQpc(nowQpc.QuadPart);
    if (index == 1 || index <= g_lastTsIndex || countPlus1 != g_lastTsCountPlus1) {
        g_nextDeadlineNs = nowNs;
        g_burstStepNs = stepNs;
    } else {
        g_nextDeadlineNs += g_burstStepNs;
        if (nowNs > 0 && g_nextDeadlineNs < nowNs + (g_burstStepNs / 2))
            g_nextDeadlineNs = nowNs + g_burstStepNs;
    }
    g_lastTsIndex = index;
    g_lastTsCountPlus1 = countPlus1;
    *out = g_nextDeadlineNs;
    return result;
}

inline bool WriteVerified(uint8_t* dst, const uint8_t* bytes, uint32_t size) {
    DWORD oldProtect = 0, ignored = 0;
    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    memcpy(dst, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), dst, size);
    VirtualProtect(dst, size, oldProtect, &ignored);
    return memcmp(dst, bytes, size) == 0;
}

inline bool HookThunk(uint8_t* base, uint32_t rva, uint32_t targetRva, const uint8_t* expected, void* detour,
                      void** nativeOut, uint8_t* origOut, bool* hookedFlag) {
    uint8_t* thunk = base + rva;
    if (memcmp(thunk, expected, PresentThunkSize) != 0) return false;
    if (origOut) memcpy(origOut, thunk, PresentThunkSize);
    uint8_t rep[PresentThunkSize] = { 0xFF, 0x25, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    auto target = reinterpret_cast<uint64_t>(detour);
    memcpy(rep + 6, &target, sizeof(target));
    if (!WriteVerified(thunk, rep, PresentThunkSize)) return false;
    *nativeOut = base + targetRva;
    if (hookedFlag) *hookedFlag = true;
    return true;
}

inline unsigned int InstalledDetourCount() {
    return (g_presentHooked ? 1u : 0u) + (g_schedHooked ? 1u : 0u) + (g_tsHooked ? 1u : 0u);
}

inline bool Install(uint8_t* base) {
    if (g_enabled) return true;
    if (base == nullptr || !QueryPerformanceFrequency(&g_freq) || g_freq.QuadPart <= 0) return false;
    g_base = base;
    g_lastBurstQpc = g_periodNs = g_intervalQpc = g_targetQpc = g_lastPacedQpc = g_lastMultiplier = g_burstBlockQpc = g_renderTimeNs = 0;
    g_sampleCount = g_samplePos = 0;
    g_fedFrameTimeMs = 0.0;
    g_ring = nullptr;
    g_nextDeadlineNs = g_burstStepNs = 0;
    g_lastTsIndex = g_lastTsCountPlus1 = 0;
    void* native = nullptr;
    if (!HookThunk(base, PresentThunkRva, NativePresentRva, PresentThunkExpected, reinterpret_cast<void*>(&Detour), &native, g_presentThunkOrig, &g_presentHooked))
        return false;
    g_native = reinterpret_cast<PresentFn>(native);
    g_ringSnapshot = reinterpret_cast<RingSnapshotFn>(base + RingSnapshotFnRva);
    g_enabled = true;
    g_schedNative = reinterpret_cast<SchedFn>(base + SchedFnRva);
    if (!HookThunk(base, SchedThunkRva, SchedFnRva, SchedThunkExpected, reinterpret_cast<void*>(&SchedForwarder), &native, g_schedThunkOrig, &g_schedHooked))
        g_schedNative = nullptr;
    g_tsNative = reinterpret_cast<TimestampFn>(base + TimestampFnRva);
    if (g_schedNative == nullptr || !HookThunk(base, TimestampThunkRva, TimestampFnRva, TimestampThunkExpected, reinterpret_cast<void*>(&TsDetour), &native, g_tsThunkOrig, &g_tsHooked))
        g_tsNative = nullptr;
    return true;
}

inline void Uninstall() {
    if (!g_base) return;
    if (g_presentHooked) { WriteVerified(g_base + PresentThunkRva, g_presentThunkOrig, PresentThunkSize); g_presentHooked = false; }
    if (g_schedHooked) { WriteVerified(g_base + SchedThunkRva, g_schedThunkOrig, PresentThunkSize); g_schedHooked = false; }
    if (g_tsHooked) { WriteVerified(g_base + TimestampThunkRva, g_tsThunkOrig, PresentThunkSize); g_tsHooked = false; }
    g_ring = nullptr;
    g_nextDeadlineNs = g_burstStepNs = 0;
    g_lastTsIndex = g_lastTsCountPlus1 = 0;
    g_enabled = false;
    g_native = nullptr;
    g_schedNative = nullptr;
    g_tsNative = nullptr;
    g_ringSnapshot = nullptr;
    g_base = nullptr;
}
} // namespace XeFGPacing
