#pragma once
// [DlssNr] WeightsSetA / WeightsSetB: weight set folders for the two Neural Rendering models (Preset A, Preset B).
// The forwarder (nvngx.dll_dlssnr.dll) serves the files; this only says where they are.
#include <Util.h>
#include <dlssnr/DlssNr_WeightsRules.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace DlssNr::Weights
{
inline bool IsValidWeightsName(const std::string& name)
{
    if (name.empty() || name == "." || name == "..")
        return false;
    return name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name.find(':') == std::string::npos && name.find("..") == std::string::npos;
}

// OptiScaler\nr_weights, beside the other OptiScaler folders
inline std::filesystem::path Root()
{
    return Util::DllPath().parent_path() / L"OptiScaler" / L"nr_weights";
}

// The chosen set's folder, or empty for the DLL's own weights. A name that is not a plain folder name is ignored.
inline std::filesystem::path Folder(const std::string& name)
{
    if (!IsValidWeightsName(name))
        return {};
    return Root() / string_to_wstring(name);
}

inline std::vector<std::string> ListAvailableSets()
{
    std::vector<std::string> sets;
    std::error_code ec;
    const auto root = Root();
    if (!std::filesystem::is_directory(root, ec))
        return sets;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec)
            break;
        if (!entry.is_directory(ec))
            continue;
        std::string name = wstring_to_string(entry.path().filename().wstring());
        if (IsValidWeightsName(name))
            sets.push_back(std::move(name));
    }
    std::sort(sets.begin(), sets.end());
    return sets;
}

using SelectFn = void (*)(const wchar_t*);
using Select2Fn = void (*)(const wchar_t*, const wchar_t*);
using TakeLogFn = int (*)(char*, int);

struct Exports
{
    Select2Fn select2 = nullptr;
    SelectFn select = nullptr;
    bool Available() const { return select2 != nullptr || select != nullptr; }
};

inline std::atomic<bool> g_needsNewForwarder { false };

inline void Load(Exports& exports, HMODULE forwarder)
{
    exports.select2 = (Select2Fn) GetProcAddress(forwarder, "dlssnr_weights_select2");
    exports.select = (SelectFn) GetProcAddress(forwarder, "dlssnr_weights_select");
    if (!exports.Available())
        g_needsNewForwarder = true;
}

inline std::wstring Dir(const std::string& set, const char* api)
{
    const auto dir = Folder(set).wstring();
    if (!set.empty() && dir.empty())
        LOG_WARN("DLSS-NR{}: Model weights '{}' is not a folder name; the DLL's own weights are used", api, set);
    return dir;
}

inline void Select(const Exports& exports, const std::string& setA, const std::string& setB, const char* api)
{
    g_needsNewForwarder = !exports.Available();
    if (exports.select2 != nullptr)
    {
        const auto dirA = Dir(setA, api);
        const auto dirB = Dir(setB, api);
        exports.select2(dirA.c_str(), dirB.c_str());
        return;
    }
    if (exports.select == nullptr)
    {
        if (!setA.empty() || !setB.empty())
        {
            LOG_WARN("DLSS-NR{}: Model weights need the forwarder from this release (nvngx.dll_dlssnr.dll); "
                     "the DLL's own weights are used",
                     api);
        }
        return;
    }

    WeightsRules::Models onlySet;
    const std::string& single = setA.empty() ? setB : setA;
    if (setA.empty() != setB.empty() && !Folder(single).empty())
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(Folder(single), ec))
            WeightsRules::NoteFile(onlySet, wstring_to_string(entry.path().filename().wstring()));
    }
    std::string set;
    if (!WeightsRules::OldForwarderCanServe(setA, setB, onlySet, set))
    {
        g_needsNewForwarder = true;
        LOG_WARN("DLSS-NR{}: This forwarder (nvngx.dll_dlssnr.dll) is too old for these Model A and Model B weights "
                 "('{}', '{}'); both models run the DLL's own weights. Use the forwarder from this release.",
                 api, setA, setB);
        exports.select(L"");
        return;
    }
    const auto dir = Dir(set, api);
    exports.select(dir.c_str());
}

inline void LogReads(TakeLogFn takeLog, const char* api)
{
    if (takeLog == nullptr)
        return;
    char text[4096] {};
    if (takeLog(text, (int) sizeof(text)) <= 0)
        return;
    std::string_view rest(text);
    while (!rest.empty())
    {
        const auto end = rest.find('\n');
        const auto line = rest.substr(0, end);
        if (!line.empty())
            LOG_INFO("DLSS-NR{} weights: {}", api, line);
        if (end == std::string_view::npos)
            break;
        rest.remove_prefix(end + 1);
    }
}
} // namespace DlssNr::Weights
