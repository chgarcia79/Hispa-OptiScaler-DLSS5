#include "pch.h"
#include "XeMfgLoader.h"
#include "XeFGPacing.h"
#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <Util.h>
#include <scanner/scanner.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
constexpr uint32_t kDefaultMaxFrames = 3;
constexpr uint32_t kMaxSupportedFrames = 5;
constexpr std::string_view kPatternU1 = "0F 85 CC 00 00 00 83 FB 01 0F 86 C3 00 00 00";
constexpr std::string_view kPatternU2 = "74 09 80 79 64 00 74 03 B0 01 C3 32 C0 C3";
constexpr std::string_view kPatternU3 = "BB 03 00 00 00 E8 ? ? ? ? 84 C0 74 0D 48 8B";
constexpr std::string_view kPatternU4 = "C7 87 6C 01 00 00 01 00 00 00 C6 87 68 01 00 00";
constexpr std::string_view kPatternU5 = "B8 01 00 00 00 89 47 20 33 C0 48 8B 9C 24";
constexpr uintptr_t kRvaU1 = 0x20da4f, kRvaU2 = 0x1a5de4, kRvaU3 = 0x1a517d, kRvaU4 = 0x1a45c2, kRvaU5 = 0x20973b;

struct PatchRecord
{
    const char* name = nullptr;
    uint8_t* address = nullptr;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    DWORD originalProtection = 0;
    bool applied = false;
};

std::recursive_mutex g_mutex;
XeMfgLoader::Status g_status {};
bool g_applied = false;
std::vector<PatchRecord> g_appliedRecords {};

bool VerifyBytes(const uint8_t* address, const std::vector<uint8_t>& expected)
{
    return address != nullptr && memcmp(address, expected.data(), expected.size()) == 0;
}

uint8_t* ResolvePatchSite(uint8_t* base, size_t imageSize, uintptr_t rva, std::string_view pattern,
                          const std::vector<uint8_t>& expectedBytes)
{
    if (!base || imageSize == 0) return nullptr;
    if (rva + expectedBytes.size() <= imageSize && VerifyBytes(base + rva, expectedBytes))
        return base + rva;
    auto scanAddr = scanner::GetAddress(reinterpret_cast<HMODULE>(base), pattern);
    if (scanAddr != 0 && scanAddr >= reinterpret_cast<uintptr_t>(base) &&
        scanAddr + expectedBytes.size() <= reinterpret_cast<uintptr_t>(base) + imageSize)
    {
        uint8_t* candidate = reinterpret_cast<uint8_t*>(scanAddr);
        if (VerifyBytes(candidate, expectedBytes)) return candidate;
    }
    return nullptr;
}

bool TransactionalWrite(std::vector<PatchRecord>& records, XeMfgLoader::Status& status)
{
    for (auto& rec : records)
    {
        DWORD oldProtect = 0, restoredProtect = 0;
        if (!VirtualProtect(rec.address, rec.replacement.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            LOG_ERROR("XeMFG unlock: VirtualProtect failed for {} at {:p}", rec.name, (void*) rec.address);
            status.PatchFailed = true;
            return false;
        }
        rec.originalProtection = oldProtect;
        memcpy(rec.address, rec.replacement.data(), rec.replacement.size());
        rec.applied = true;
        VirtualProtect(rec.address, rec.replacement.size(), oldProtect, &restoredProtect);
        FlushInstructionCache(GetCurrentProcess(), rec.address, rec.replacement.size());
        status.PatchesApplied++;
    }
    return true;
}

void TransactionalRollback(std::vector<PatchRecord>& records, XeMfgLoader::Status& status)
{
    for (auto& rec : records)
    {
        if (!rec.applied) continue;
        DWORD oldProtect = 0, restoredProtect = 0;
        if (!VirtualProtect(rec.address, rec.original.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            status.RollbackFailed = true;
            continue;
        }
        memcpy(rec.address, rec.original.data(), rec.original.size());
        rec.applied = false;
        VirtualProtect(rec.address, rec.original.size(), rec.originalProtection ? rec.originalProtection : oldProtect, &restoredProtect);
        FlushInstructionCache(GetCurrentProcess(), rec.address, rec.original.size());
    }
    status.PacingDetours = 0;
    status.PacingInstalled = status.VerifiedPacing = false;
}
} // namespace

namespace XeMfgLoader
{
Status LastStatus() { std::scoped_lock lock(g_mutex); return g_status; }
bool EnabledForSession() {
    auto config = Config::Instance();
    return config != nullptr && !config->ExternalFrameGeneration.value_or_default();
}
bool IsSourceOptiScaler() { return true; }
bool Pending() { std::scoped_lock lock(g_mutex); return AnyEnabledForSession() && !g_status.Patched && !g_status.PatchFailed; }
unsigned int UnlockedMax() { std::scoped_lock lock(g_mutex); return g_status.Patched ? std::clamp(g_status.ConfiguredCeiling, 1u, kMaxSupportedFrames) : 1u; }
unsigned int EffectiveMax(unsigned int nativeMaximum) {
    std::scoped_lock lock(g_mutex);
    if (g_status.PatchFailed || g_status.RollbackFailed) return 1;
    return g_status.Patched ? std::max(nativeMaximum, UnlockedMax()) : nativeMaximum;
}
Failure LastFailure() {
    std::scoped_lock lock(g_mutex);
    if (g_status.RollbackFailed) return Failure::RollbackFailed;
    return g_status.PatchFailed ? Failure::PatchFailed : Failure::None;
}

bool ApplyToMemory(uint8_t* baseAddress, size_t imageSize, unsigned int maxFrames, bool enablePacing, Status& outStatus)
{
    outStatus = {};
    outStatus.ModuleFound = (baseAddress != nullptr && imageSize > 0);
    outStatus.ConfiguredCeiling = std::clamp(maxFrames, 1u, kMaxSupportedFrames);
    if (!outStatus.ModuleFound) return false;

    const uint8_t fb = static_cast<uint8_t>(outStatus.ConfiguredCeiling);
    std::vector<PatchRecord> patches;
    auto addPatch = [&](const char* name, uintptr_t rva, std::string_view pat, std::vector<uint8_t> exp, std::vector<uint8_t> rep) {
        uint8_t* addr = ResolvePatchSite(baseAddress, imageSize, rva, pat, exp);
        if (!addr) { LOG_WARN("XeMFG unlock: {} site not matched", name); return false; }
        patches.push_back({ name, addr, std::move(exp), std::move(rep) });
        return true;
    };

    if (!addPatch("U1/frame-count-fallback", kRvaU1, kPatternU1, { 0x0f, 0x85, 0xcc, 0x00, 0x00, 0x00 }, { 0xe9, 0xcd, 0x00, 0x00, 0x00, 0x90 }) ||
        !addPatch("U2/model-downgrade", kRvaU2, kPatternU2, { 0x74, 0x09 }, { 0xeb, 0x06 }) ||
        !addPatch("U3/default-ceiling", kRvaU3, kPatternU3, { 0xbb, 0x03, 0x00, 0x00, 0x00 }, { 0xbb, fb, 0x00, 0x00, 0x00 }) ||
        !addPatch("U4/override-clamp", kRvaU4, kPatternU4, { 0xc7, 0x87, 0x6c, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 }, { 0xc7, 0x87, 0x6c, 0x01, 0x00, 0x00, fb, 0x00, 0x00, 0x00 }) ||
        !addPatch("U5/reported-maximum", kRvaU5, kPatternU5, { 0xb8, 0x01, 0x00, 0x00, 0x00 }, { 0xb8, fb, 0x00, 0x00, 0x00 }))
        return false;

    if (!TransactionalWrite(patches, outStatus))
    {
        outStatus.ErrorMessage = "Memory protection or write failed";
        TransactionalRollback(patches, outStatus);
        return false;
    }

    outStatus.Patched = outStatus.Applied = (outStatus.PatchesApplied == 5);
    g_appliedRecords = patches;
    if (outStatus.Patched && enablePacing && XeFGPacing::Install(baseAddress))
    {
        outStatus.PacingDetours = XeFGPacing::InstalledDetourCount();
        outStatus.PacingInstalled = outStatus.VerifiedPacing = true;
    }
    return outStatus.Patched;
}

void RollbackMemory(uint8_t*, Status& outStatus)
{
    if (g_appliedRecords.empty()) return;
    TransactionalRollback(g_appliedRecords, outStatus);
    outStatus.Patched = outStatus.Applied = false;
    outStatus.PatchesApplied = 0;
    g_appliedRecords.clear();
}

void SetMaxGeneratedFrames(unsigned int maxFrames)
{
    std::scoped_lock lock(g_mutex);
    uint32_t clamped = std::clamp(maxFrames, 1u, kMaxSupportedFrames);
    g_status.ConfiguredCeiling = clamped;
    if (!g_applied || g_appliedRecords.empty()) return;

    uint8_t frameByte = static_cast<uint8_t>(clamped);
    for (auto& rec : g_appliedRecords)
    {
        if (!rec.address || !rec.applied) continue;
        size_t byteOffset = (strcmp(rec.name, "U4/override-clamp") == 0) ? 6 : 1;
        if (strcmp(rec.name, "U3/default-ceiling") != 0 && strcmp(rec.name, "U4/override-clamp") != 0 &&
            strcmp(rec.name, "U5/reported-maximum") != 0)
            continue;
        DWORD oldProtect = 0, restoredProtect = 0;
        if (VirtualProtect(rec.address, rec.replacement.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            rec.address[byteOffset] = frameByte;
            rec.replacement[byteOffset] = frameByte;
            VirtualProtect(rec.address, rec.replacement.size(), oldProtect, &restoredProtect);
            FlushInstructionCache(GetCurrentProcess(), rec.address, rec.replacement.size());
        }
    }
}

void TryApply(HMODULE module)
{
    std::scoped_lock lock(g_mutex);
    if (g_applied || g_status.Patched || g_status.PatchFailed || !AnyEnabledForSession()) return;
    if (!module) module = GetModuleHandleW(L"libxess_fg.dll");
    if (!module) module = GetModuleHandleW(L"igxess_fg.dll");
    if (!module) return;
    auto dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;
    if (ApplyToMemory(reinterpret_cast<uint8_t*>(module), ntHeaders->OptionalHeader.SizeOfImage, kMaxSupportedFrames, true, g_status))
    {
        g_applied = true;
        LOG_INFO("XeMFG unlock: multi-frame generation unlocked up to {}X (pacing detours: {})", g_status.ConfiguredCeiling + 1, g_status.PacingDetours);
    }
}

void ResetPacingContext() {}

void Shutdown()
{
    std::scoped_lock lock(g_mutex);
    XeFGPacing::Uninstall();
    if (!g_appliedRecords.empty()) { TransactionalRollback(g_appliedRecords, g_status); g_appliedRecords.clear(); }
    g_applied = false;
    g_status = {};
}

void RecordPresentStatus(unsigned int framesPresented, int frameGenResult, bool isFrameGenEnabled)
{
    std::scoped_lock lock(g_mutex);
    g_status.LastFramesPresented = framesPresented;
    g_status.LastFrameGenResult = frameGenResult;
    g_status.IsFrameGenEnabled = isFrameGenEnabled;
    g_status.HasPresentTelemetry = true;
}
} // namespace XeMfgLoader
