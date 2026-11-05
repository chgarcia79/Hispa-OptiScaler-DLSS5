#pragma once

#include <cwctype>
#include <string_view>

// Non-intrusive Streamline log capture shared by the full slInit hooks and the log-only slInit hooks when the
// interposer is loaded directly by the game/OS without full Streamline interposition.
namespace StreamlineLogPrefs
{
template <typename Prefs, typename Callback, typename Level>
inline void ApplyLogCapture(Prefs& prefs, Callback ours, Level level, Callback& savedGameCallback)
{
    if (prefs.logMessageCallback != ours)
        savedGameCallback = prefs.logMessageCallback;

    prefs.logLevel = level;
    prefs.logMessageCallback = ours;
}

inline bool IsInterposerName(std::wstring_view name)
{
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos)
        name.remove_prefix(slash + 1);

    constexpr std::wstring_view interposer = L"sl.interposer.dll";
    if (name.size() != interposer.size())
        return false;

    for (size_t i = 0; i < name.size(); ++i)
    {
        if (static_cast<wchar_t>(std::towlower(name[i])) != interposer[i])
            return false;
    }

    return true;
}

inline bool ShouldNotifyInterposerLoad(bool interposerUnhooked, bool captureEnabled, std::wstring_view name,
                                       bool loaded)
{
    return interposerUnhooked && captureEnabled && loaded && IsInterposerName(name);
}
} // namespace StreamlineLogPrefs
