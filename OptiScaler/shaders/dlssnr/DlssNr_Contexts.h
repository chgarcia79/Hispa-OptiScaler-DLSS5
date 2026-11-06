#pragma once

// Main-camera isolation across multiple live game NGX DLSS handles (adapted from janblade PR #285 for HispaOptiScaler v1.0.4-dev9).
// Governs [DlssNr] IsolateMainFeature: runs DLSS-NR only on the primary camera (largest output area) and cleanly skips
// secondary DLSS features (PiP scopes, mirrors, 3D character UI previews), promoting a successor if the main camera is released.

#include <cstdint>

namespace DlssNr::Contexts
{
constexpr unsigned kMaxEntries = 8;
constexpr uint64_t kMainIdleCalls = 120;
constexpr uint64_t kSuccessorCalls = 4;
constexpr uint64_t kQuietCalls = 8;

struct Picture
{
    uint64_t resource = 0;
    uint32_t width = 0, height = 0;
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t outputWidth = 0, outputHeight = 0;
    int format = 0;

    uint32_t EffectiveWidth() const { return outputWidth != 0 ? outputWidth : width; }
    uint32_t EffectiveHeight() const { return outputHeight != 0 ? outputHeight : height; }
    uint64_t Area() const { return static_cast<uint64_t>(EffectiveWidth()) * static_cast<uint64_t>(EffectiveHeight()); }

    bool SameAllocation(const Picture& other) const
    {
        if (resource != 0 && other.resource != 0)
            return resource == other.resource;
        return width != 0 && width == other.width && height == other.height && format == other.format;
    }
};

struct AcquireResult
{
    bool run = true;
    bool firstSeen = false;
    bool firstSkipped = false;
    bool promotedToMain = false;
};

struct Snapshot
{
    uint32_t mainWidth = 0, mainHeight = 0;
    uint32_t activeFeatures = 0, activeSecondary = 0;
    uint64_t skippedEvaluates = 0;
};

class Tracker
{
  public:
    AcquireResult Acquire(uint32_t handle, bool IsolateMainFeature, const Picture& pic)
    {
        ++_calls;
        AcquireResult out {};
        if (handle == 0)
            return out;

        Entry* entry = FindOrCreate(handle, pic, out.firstSeen);

        if (!IsolateMainFeature)
        {
            if (!_haveMain || handle == _mainHandle || pic.Area() >= _mainPicture.Area())
                SetMain(handle, pic);
            return out;
        }

        if (_haveMain && handle == _mainHandle)
        {
            _mainLastCall = _calls;
            if (pic.Area() != 0)
                _mainPicture = pic;
            return out;
        }

        if (!_haveMain)
        {
            const bool sameFmt = (pic.format == _releasedPicture.format);
            const bool successor = !_haveReleased || pic.SameAllocation(_releasedPicture) ||
                                   (sameFmt && (entry == nullptr || entry->firstSeen + kSuccessorCalls >= _releasedAt)) ||
                                   (_calls - _releasedAt > kQuietCalls);
            if (successor)
                return PromoteToMain(handle, pic, _haveReleased && !pic.SameAllocation(_releasedPicture));
        }
        else
        {
            if (pic.SameAllocation(_mainPicture))
            {
                _mainHandle = handle;
                _mainLastCall = _calls;
                return out;
            }
            const bool largerPicture = pic.Area() > _mainPicture.Area() && _mainPicture.Area() > 0;
            if (largerPicture || (_calls - _mainLastCall) > kMainIdleCalls)
                return PromoteToMain(handle, pic, true);
        }

        out.run = false;
        ++_skippedEvaluates;
        if (entry != nullptr && !entry->reportedSkip)
            entry->reportedSkip = out.firstSkipped = true;
        return out;
    }

    void Release(uint32_t handle)
    {
        if (handle == 0)
            return;
        if (_haveMain && handle == _mainHandle)
        {
            _haveMain = false;
            _haveReleased = true;
            _releasedPicture = _mainPicture;
            _releasedAt = _calls;
        }
        for (auto& e : _entries)
            if (e.handle == handle)
                e = Entry {};
    }

    Snapshot GetSnapshot() const
    {
        Snapshot s { _mainPicture.EffectiveWidth(), _mainPicture.EffectiveHeight(), 0, 0, _skippedEvaluates };
        for (const auto& e : _entries)
        {
            if (e.handle != 0 && (_calls - e.lastSeen) <= kMainIdleCalls)
            {
                ++s.activeFeatures;
                if (!_haveMain || (e.handle != _mainHandle && !e.picture.SameAllocation(_mainPicture)))
                    ++s.activeSecondary;
            }
        }
        return s;
    }

    void Reset() { *this = Tracker {}; }

  private:
    struct Entry
    {
        uint32_t handle = 0;
        Picture picture {};
        uint64_t firstSeen = 0, lastSeen = 0;
        bool reportedSkip = false;
    };

    void SetMain(uint32_t handle, const Picture& pic)
    {
        _mainHandle = handle;
        _mainPicture = pic;
        _haveMain = true;
        _mainLastCall = _calls;
    }

    Entry* FindOrCreate(uint32_t handle, const Picture& pic, bool& isNew)
    {
        for (auto& e : _entries)
            if (e.handle == handle)
                return e.picture = pic, e.lastSeen = _calls, &e;

        Entry* slot = &_entries[0];
        for (auto& e : _entries)
        {
            if (e.handle == 0)
            {
                slot = &e;
                break;
            }
            if (e.lastSeen < slot->lastSeen)
                slot = &e;
        }
        *slot = Entry { handle, pic, _calls, _calls, false };
        isNew = true;
        return slot;
    }

    AcquireResult PromoteToMain(uint32_t handle, const Picture& pic, bool promoted)
    {
        SetMain(handle, pic);
        _haveReleased = false;
        return AcquireResult { true, false, false, promoted };
    }

    uint64_t _calls = 0, _mainLastCall = 0, _releasedAt = 0, _skippedEvaluates = 0;
    uint32_t _mainHandle = 0;
    Picture _mainPicture {}, _releasedPicture {};
    bool _haveMain = false, _haveReleased = false;
    Entry _entries[kMaxEntries] = {};
};
} // namespace DlssNr::Contexts
