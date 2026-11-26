#pragma once
// Neural Rendering model weights, one set per model: [DlssNr] WeightsSetA (Preset A, Model A, WEIGHTS_HT.* or
// WEIGHTS_PL.*) and WeightsSetB (Preset B or Default, Model B, WEIGHTS_CC_SMALL_FP8.*). Plain rules, no config and no
// ImGui, so tests/nr_weights_per_model_smoke.cpp runs them.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace DlssNr::WeightsRules
{
inline constexpr std::string_view kResourceA = "WEIGHTS_HT";
inline constexpr std::string_view kResourceAAlt = "WEIGHTS_PL"; // some modified DLSS-NR DLLs name Model A this way
inline constexpr std::string_view kResourceB = "WEIGHTS_CC_SMALL_FP8";

// Which model's weights a set folder holds, from its file names (<RESOURCE>.<CRC>.bin)
struct Models
{
    bool a = false, b = false;
};

inline bool StartsWithNoCase(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin(), [](char l, char r)
                      { return std::tolower((unsigned char) l) == std::tolower((unsigned char) r); });
}

// A set folder's files are <RESOURCE>.<CRC32>.bin, where RESOURCE is the model DLL's resource name exactly:
// WEIGHTS_HT or WEIGHTS_PL (some modified DLSS-NR DLLs) = Model A, WEIGHTS_CC_SMALL_FP8 = Model B. The forwarder
// matches the resource name exactly too.
inline void NoteFile(Models& m, std::string_view file)
{
    if (StartsWithNoCase(file, "WEIGHTS_HT.") || StartsWithNoCase(file, "WEIGHTS_PL."))
        m.a = true;
    else if (StartsWithNoCase(file, "WEIGHTS_CC_SMALL_FP8."))
        m.b = true;
}

struct Set
{
    std::string name;
    Models models;
};

// The folders a model's list shows: only those with that model's file (a folder with both is in both lists)
inline std::vector<std::string> ListFor(const std::vector<Set>& sets, bool modelA)
{
    std::vector<std::string> names;
    for (const auto& s : sets)
        if (modelA ? s.models.a : s.models.b)
            names.push_back(s.name);
    return names;
}

struct Pair
{
    std::string a, b;
    bool operator==(const Pair&) const = default;
};

inline bool IsAuto(std::string_view value) { return StartsWithNoCase(value, "auto") && value.size() == 4; }

// The old [DlssNr] WeightsSet, taken for both models when neither new key is in the file. No folder is looked at: the
// first config read runs before the OptiScaler folder is known, so a folder check there found nothing and dropped the
// player's set. A model whose file the folder lacks shows that in the menu and runs stock (the forwarder logs it).
// Empty or "auto" = unset: nothing to migrate.
inline bool Migrate(const std::string& oldSet, bool newKeysPresent, Pair& out)
{
    out = {};
    if (oldSet.empty() || IsAuto(oldSet) || newKeysPresent)
        return false;
    out.a = oldSet;
    out.b = oldSet;
    return true;
}

// A forwarder without dlssnr_weights_select2 has one folder for both models. It can serve the sets when both models
// take the same one, or when only one model has a set and that folder holds only that model's file (so the other
// model finds nothing there and stays stock). Anything else cannot be served: select nothing, both run stock.
inline bool OldForwarderCanServe(const std::string& a, const std::string& b, Models folderOfTheOnlySet,
                                 std::string& set)
{
    set.clear();
    if (a == b)
    {
        set = a;
        return true;
    }
    if (!a.empty() && !b.empty())
        return false;
    const bool onlyA = !a.empty();
    if (onlyA ? (folderOfTheOnlySet.a && !folderOfTheOnlySet.b) : (folderOfTheOnlySet.b && !folderOfTheOnlySet.a))
    {
        set = onlyA ? a : b;
        return true;
    }
    return false;
}

// The amber "Not used now" line: a set is chosen for a model and no pass runs that model. Passes 1-3 count,
// passes 4 and up use Pass 1's, the driver proxy runs one pass; Default and B run Model B.
inline bool NotUsedNow(bool setChosen, bool modelA, bool anyPassRunsA, bool anyPassRunsB)
{
    return setChosen && !(modelA ? anyPassRunsA : anyPassRunsB);
}

// The key the renderers compare to know when to rebuild
inline std::string BuiltKey(const std::string& a, const std::string& b) { return a + "|" + b; }
} // namespace DlssNr::WeightsRules
