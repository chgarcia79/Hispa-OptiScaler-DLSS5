#pragma once

// A feature the game released, kept alive a little longer before it is destroyed.
//
// The menu and the FPS overlay read State::currentFeature on the present thread (RenderMenu copies the pointer once
// per present, then calls its name, version and sizes). With frame generation that present can run on another
// thread than the game's NGX calls (F5Low presents from its own thread, Streamline from the game's), so a release
// that destroyed the feature on the spot could free it while a present was still reading it: Forza Horizon 6 aborted
// entering photo mode (2026-11-23, RenderPerformanceOverlay calling into the freed feature). The release now clears
// currentFeature at once and parks the object here; it is destroyed on the game's own thread (the next NGX call)
// once at least kPresents presents and kMs milliseconds have passed, by which time no present can still hold it.
// An NGX shutdown destroys everything parked first, before the runtime goes away. Kept free of D3D12 and NGX types so
// tests/retired_features_smoke.cpp runs the same rule.

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

template <typename Feature> class RetiredFeatures
{
  public:
    static constexpr uint64_t kPresents = 3;
    static constexpr double kMs = 500.0;

    void Park(std::unique_ptr<Feature> feature, uint64_t presents, double nowMs)
    {
        if (feature == nullptr)
            return;

        std::lock_guard<std::mutex> lock(_lock);
        _parked.push_back({ std::move(feature), presents, nowMs });
    }

    // Destroys what has waited long enough (everything with `all`), on the calling thread. The objects are moved out
    // under the lock and destroyed after it, so a destructor that calls back into NGX cannot deadlock against Park.
    void Collect(uint64_t presents, double nowMs, bool all = false)
    {
        std::vector<std::unique_ptr<Feature>> expired;
        {
            std::lock_guard<std::mutex> lock(_lock);

            for (size_t i = 0; i < _parked.size();)
            {
                const Parked& p = _parked[i];
                const bool due = all || (presents >= p.presents + kPresents && nowMs - p.parkedMs >= kMs);

                if (!due)
                {
                    ++i;
                    continue;
                }

                expired.push_back(std::move(_parked[i].feature));
                _parked.erase(_parked.begin() + i);
            }
        }

        expired.clear();
    }

    size_t Size()
    {
        std::lock_guard<std::mutex> lock(_lock);
        return _parked.size();
    }

  private:
    struct Parked
    {
        std::unique_ptr<Feature> feature;
        uint64_t presents;
        double parkedMs;
    };

    std::mutex _lock;
    std::vector<Parked> _parked;
};
