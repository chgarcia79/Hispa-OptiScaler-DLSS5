#pragma once

//HDR float-flip equivalence (DXGI_FORMAT_R16G16B16A16_FLOAT <-> DXGI_FORMAT_R11G11B10_FLOAT, plus TYPELESS)
//and alternating-rebuild guard (adapted from janblade PR #284 for HispaOptiScaler v1.0.4-dev9).

#include <cstdint>

namespace DlssNr
{
// Numeric DXGI_FORMAT constants (verified by static_assert in DlssNr_Dx12.cpp)
constexpr int kFormatR16G16B16A16Typeless = 9;
constexpr int kFormatR16G16B16A16Float = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
constexpr int kFormatR11G11B10Float = 26;    // DXGI_FORMAT_R11G11B10_FLOAT
constexpr int kFormatR8G8B8A8Typeless = 27;
constexpr int kFormatR8G8B8A8Unorm = 28;
constexpr int kFormatR8G8B8A8UnormSrgb = 29;
constexpr int kFormatB8G8R8A8Unorm = 87;
constexpr int kFormatB8G8R8A8Typeless = 90;
constexpr int kFormatB8G8R8A8UnormSrgb = 91;

constexpr int FloatFormatFamily(int format)
{
    if (format == kFormatR16G16B16A16Float || format == kFormatR16G16B16A16Typeless)
        return kFormatR16G16B16A16Float;
    if (format == kFormatR11G11B10Float)
        return kFormatR11G11B10Float;
    return 0;
}

constexpr bool IsFlippingFloatFormat(int format) { return FloatFormatFamily(format) != 0; }

// True when consecutive frames move between RGBA16F (including TYPELESS) and R11G11B10_FLOAT.
constexpr bool IsFloatFlip(int lastFormat, int nowFormat)
{
    const int a = FloatFormatFamily(lastFormat);
    const int b = FloatFormatFamily(nowFormat);
    return a != 0 && b != 0 && a != b;
}

// Canonical scratch/model format: BGRA8 -> RGBA8, R16G16B16A16_TYPELESS -> R16G16B16A16_FLOAT,
// and R11G11B10_FLOAT -> R16G16B16A16_FLOAT once a float flip was observed while KeepModelOnFormatFlip is enabled.
constexpr int CanonicalColourFormat(int format, bool keepFloatFlipInRgba16f = false)
{
    switch (format)
    {
    case kFormatR16G16B16A16Typeless:
        return kFormatR16G16B16A16Float;
    case kFormatR11G11B10Float:
        return keepFloatFlipInRgba16f ? kFormatR16G16B16A16Float : kFormatR11G11B10Float;
    case kFormatB8G8R8A8Typeless:
        return kFormatR8G8B8A8Typeless;
    case kFormatB8G8R8A8Unorm:
        return kFormatR8G8B8A8Unorm;
    case kFormatB8G8R8A8UnormSrgb:
        return kFormatR8G8B8A8UnormSrgb;
    default:
        return format;
    }
}

constexpr bool SurfacesStale(int haveFormat, int wantFormat)
{
    return haveFormat != 0 && haveFormat != wantFormat;
}

struct ModelSetup
{
    int colourFormat = 0;
    uint32_t width = 0, height = 0, workWidth = 0, workHeight = 0;
    bool beforeUpscale = false, rayReconstruction = false;

    constexpr bool operator==(const ModelSetup&) const = default;
};

// Stands NR down if a game alternates two incompatible model setups (A -> B -> A) kLimit times within kWindow calls,
// resuming once one setup holds steady for kSettle consecutive calls. Dynamic resolution ramps never trip this guard.
struct RebuildGuard
{
    static constexpr unsigned kLimit = 3;
    static constexpr uint64_t kWindow = 16;
    static constexpr unsigned kSettle = 30;
    static constexpr uint64_t kWary = 600;

    uint64_t calls = 0;
    ModelSetup built {}, before {};
    bool haveBuilt = false, haveBefore = false;
    uint64_t returns[kLimit] = {};
    unsigned count = 0;
    bool standing = false;
    ModelSetup waiting {};
    unsigned settled = 0;
    uint64_t resumedAt = 0;
    bool resumed = false;

    bool Allow(const ModelSetup& setup, bool wouldRebuild)
    {
        ++calls;
        if (standing)
        {
            settled = (setup == waiting) ? (settled + 1) : 1;
            waiting = setup;
            if (settled < kSettle)
                return false;

            standing = false;
            count = 0;
            resumed = true;
            resumedAt = calls;
            RecordBuilt(setup);
            return true;
        }

        if (!wouldRebuild)
        {
            if (!haveBuilt)
            {
                built = setup;
                haveBuilt = true;
            }
            return true;
        }

        if (!(haveBefore && setup == before))
        {
            RecordBuilt(setup);
            return true;
        }

        if (count == kLimit)
        {
            for (unsigned i = 1; i < kLimit; ++i)
                returns[i - 1] = returns[i];
            --count;
        }
        returns[count++] = calls;

        const bool wary = resumed && (calls - resumedAt < kWary);
        if ((count == kLimit && (calls - returns[0] < kWindow)) || wary)
        {
            standing = true;
            waiting = setup;
            settled = 1;
            return false;
        }

        RecordBuilt(setup);
        return true;
    }

  private:
    void RecordBuilt(const ModelSetup& setup)
    {
        if (haveBuilt && !(setup == built))
        {
            before = built;
            haveBefore = true;
        }
        built = setup;
        haveBuilt = true;
    }
};
} // namespace DlssNr
