#pragma once
// [DlssNr] WeightsSet: weight set folders for the Neural Rendering model.
// The forwarder (nvngx.dll_dlssnr.dll) serves the files; this only says where they are.
#include <Util.h>

#include <algorithm>
#include <filesystem>
#include <string>
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
} // namespace DlssNr::Weights
