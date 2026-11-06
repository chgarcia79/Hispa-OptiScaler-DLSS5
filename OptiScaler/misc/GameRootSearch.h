#pragma once

#include <filesystem>
#include <optional>

// Some Unreal Engine games keep the exe outside <Project>\Binaries\Win64 (e.g. Phantom Blade Zero: PBZ\platform\steam
// with plugins under PBZ\Engine\Plugins), so the Win64/WinGDK fallback in Util::FindFilePath never reaches the game
// root. Returns the nearest ancestor of startDir, at most maxLevels up, that contains an Engine directory.
namespace GameRootSearch
{
inline constexpr int MaxEngineRootLevels = 3;

template <class IsDirectory>
std::optional<std::filesystem::path> FindEngineRoot(std::filesystem::path startDir, IsDirectory isDirectory,
                                                    int maxLevels = MaxEngineRootLevels)
{
    if (!startDir.has_filename())
        startDir = startDir.parent_path();

    auto dir = startDir;
    for (int level = 1; level <= maxLevels; ++level)
    {
        auto parent = dir.parent_path();
        if (parent.empty() || parent == dir)
            break;

        dir = parent;
        if (isDirectory(dir / "Engine"))
            return dir;
    }

    return std::nullopt;
}
} // namespace GameRootSearch
