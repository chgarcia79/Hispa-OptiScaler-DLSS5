// DLSS Neural Rendering calls, isolated in a module the snippet will accept as a caller.
//
// The snippet resolves the module owning its return address and requires that module's path to contain
// "nvngx.dll" (the driver core is _nvngx.dll), rejecting anything else with FAIL_PlatformError before it
// inspects a single argument. Neither a ReShade add-on nor OptiScaler is named anything like that, so the
// calls are made from here instead and reached through the exports below.
//
// The parameter block is the core's capability block rather than a fresh one: it carries the snippet and
// preset callbacks a feature expects at create time. The core exports no Set/Get helpers (they are
// static-library inlines), so it is driven through its vtable. NVSDK_NGX_Parameter declares eight Set
// overloads then eight Get overloads, in this order: ULL, float, double, uint, int, ID3D11Resource*,
// ID3D12Resource*, void*.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <cstring>

// Model weights selector. The model reads its weights when a feature is
// created: FindResourceA(module, "WEIGHTS_...", RT_RCDATA), LoadResource, LockResource, SizeofResource. Only this
// module's import table is patched (no global hook); when the host has selected a weight set folder, a resource
// is answered from <folder>\<name>.<CRC32 of the stock resource, 8 hex>.bin. The CRC ties a file to the exact
// stock data it was made from (some modified DLSS-NR DLLs keep the names but store the records in another layout), and
// the file must be the stock size. Anything else -> the stock weights and a log line. The DLL on disk is never changed.
// The patch is on the loaded module: in a game that loads the same nvngx_dlssnr.dll for its own Neural Rendering,
// features the game creates after a set is chosen get the set too.
namespace nrweights
{

std::mutex g_mutex;
std::wstring g_dirA;   // Model A (WEIGHTS_HT or WEIGHTS_PL), empty = stock
std::wstring g_dirB;   // Model B (WEIGHTS_CC_SMALL_FP8*), empty = stock
std::string g_log;     // taken by the host (dlssnr_weights_take_log)
std::unordered_set<HMODULE> g_patched;
std::map<std::pair<HMODULE, std::string>, unsigned> g_stockCrc;
// Served copies stay alive for the process: the model may keep the pointer it locked (the stock one is in the
// image and never goes away), so a buffer is never freed under it. Keyed by file path, so a set is read once.
std::map<std::wstring, std::vector<unsigned char>> g_files;
std::unordered_map<HGLOBAL, const unsigned char*> g_served; // the model's LoadResource handle -> our copy
unsigned g_reads = 0;

using PFN_FindResourceA = HRSRC(WINAPI*)(HMODULE, LPCSTR, LPCSTR);
using PFN_LockResource = LPVOID(WINAPI*)(HGLOBAL);

void log(const std::string& line)
{
    if (g_log.size() < 64 * 1024)
        g_log += line + "\n";
}

unsigned crc32(const unsigned char* data, size_t size)
{
    static unsigned table[256];
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       for (unsigned i = 0; i < 256; i++)
                       {
                           unsigned c = i;
                           for (int k = 0; k < 8; k++)
                               c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                           table[i] = c;
                       }
                   });
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w)
        s += c < 128 ? (char) c : '?';
    return s;
}

enum class Read
{
    Ok,
    Missing,
    WrongSize,
    Failed
};

// Only a file of the stock size is read and kept (a wrong one is left on disk and asked again next time).
const std::vector<unsigned char>* readFile(const std::wstring& path, DWORD expected, Read& result, long long& onDisk)
{
    auto found = g_files.find(path);
    if (found != g_files.end())
    {
        result = Read::Ok;
        return &found->second;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        result = Read::Missing;
        return nullptr;
    }
    LARGE_INTEGER size {};
    std::vector<unsigned char> data;
    bool ok = GetFileSizeEx(file, &size) != 0;
    onDisk = ok ? size.QuadPart : -1;
    if (ok && size.QuadPart != (long long) expected)
    {
        CloseHandle(file);
        result = Read::WrongSize;
        return nullptr;
    }
    if (ok)
    {
        data.resize((size_t) size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(file, data.data(), (DWORD) data.size(), &got, nullptr) && got == data.size();
    }
    CloseHandle(file);
    if (!ok)
    {
        result = Read::Failed;
        return nullptr;
    }
    result = Read::Ok;
    return &(g_files[path] = std::move(data));
}

HRSRC WINAPI hkFindResourceA(HMODULE module, LPCSTR name, LPCSTR type)
{
    HRSRC resource = FindResourceA(module, name, type);
    if (resource == nullptr || IS_INTRESOURCE(name) || type != MAKEINTRESOURCEA(10) ||
        strncmp(name, "WEIGHTS_", 8) != 0)
        return resource;

    std::lock_guard<std::mutex> lock(g_mutex);
    HGLOBAL handle = LoadResource(module, resource); // the same handle the model's own LoadResource gets
    g_served.erase(handle);
    // The resource name exactly (a set's files are <name>.<CRC32>.bin, which is what OptiScaler lists them by)
    // WEIGHTS_PL is Model A under that name in some modified DLSS-NR DLLs
    const bool isA = strcmp(name, "WEIGHTS_HT") == 0 || strcmp(name, "WEIGHTS_PL") == 0;
    const bool isB = strcmp(name, "WEIGHTS_CC_SMALL_FP8") == 0;
    const std::wstring& setDir = isA ? g_dirA : g_dirB;
    if (!isA && !isB) // a resource that belongs to neither model: always the DLL's own
    {
        if (!g_dirA.empty() || !g_dirB.empty())
            log("read " + std::string(name) + ": stock (not a Model A or Model B weights resource)");
        return resource;
    }
    if (setDir.empty()) // the DLL's own weights: nothing to say, most players never pick a set
        return resource;
    g_reads++;
    const std::string head = std::string(isA ? "Model A" : "Model B") + " weights, read " + std::string(name) + " #" +
                             std::to_string(g_reads) + ": ";

    const auto* stock = (const unsigned char*) LockResource(handle);
    const DWORD size = SizeofResource(module, resource);
    if (stock == nullptr || size == 0)
    {
        log(head + "stock (the model's own resource could not be read)");
        return resource;
    }
    // Keyed by module: the forwarder never unloads the model DLL, so a module handle names one DLL for the process
    auto key = std::make_pair(module, std::string(name));
    auto crc = g_stockCrc.find(key);
    if (crc == g_stockCrc.end())
        crc = g_stockCrc.emplace(key, crc32(stock, size)).first;

    char file[96];
    snprintf(file, sizeof(file), "%s.%08X.bin", name, crc->second);
    const std::wstring path = setDir + L"\\" + std::wstring(file, file + strlen(file));
    Read result = Read::Failed;
    long long onDisk = -1;
    const auto* data = readFile(path, size, result, onDisk);
    if (data != nullptr)
    {
        g_served[handle] = data->data();
        log(head + "served " + narrow(path));
    }
    else if (result == Read::Missing)
    {
        log(head + "stock (no " + narrow(path) + ")");
    }
    else if (result == Read::WrongSize)
    {
        log(head + "stock (" + narrow(path) + " is " + std::to_string(onDisk) + " bytes, the model's is " +
            std::to_string(size) + ")");
    }
    else
    {
        log(head + "stock (" + narrow(path) + " could not be read)");
    }
    return resource;
}

LPVOID WINAPI hkLockResource(HGLOBAL handle)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto found = g_served.find(handle);
        if (found != g_served.end())
            return (LPVOID) found->second;
    }
    return LockResource(handle);
}

bool patchImport(HMODULE module, const char* importName, void* detour)
{
    auto* base = (unsigned char*) module;
    auto* dos = (IMAGE_DOS_HEADER*) base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto* nt = (IMAGE_NT_HEADERS*) (base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
        return false;
    bool patched = false;
    for (auto* desc = (IMAGE_IMPORT_DESCRIPTOR*) (base + dir.VirtualAddress); desc->Name != 0; ++desc)
    {
        if (desc->OriginalFirstThunk == 0)
            continue;
        auto* names = (IMAGE_THUNK_DATA*) (base + desc->OriginalFirstThunk);
        auto* iat = (IMAGE_THUNK_DATA*) (base + desc->FirstThunk);
        for (size_t i = 0; iat[i].u1.Function != 0; ++i)
        {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*) (base + names[i].u1.AddressOfData);
            if (strcmp((const char*) byName->Name, importName) != 0)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(&iat[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                continue;
            iat[i].u1.Function = (ULONG_PTR) detour;
            VirtualProtect(&iat[i].u1.Function, sizeof(void*), old, &old);
            patched = true;
        }
    }
    return patched;
}

// Called right after the model DLL is loaded, before any feature is created.
void watch(HMODULE module)
{
    if (module == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_patched.insert(module).second)
        return;
    const bool find = patchImport(module, "FindResourceA", (void*) &hkFindResourceA);
    const bool lockRes = patchImport(module, "LockResource", (void*) &hkLockResource);
    log(find && lockRes ? "weights selector ready on the model DLL"
                        : "weights selector NOT available: the model DLL does not import FindResourceA/LockResource");
}

// A folder as the host gives it, without a trailing separator
std::wstring trimDir(const wchar_t* dir)
{
    std::wstring d = dir ? dir : L"";
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
        d.pop_back();
    return d;
}

} // namespace nrweights

namespace {

// Slot indices confirmed by round-tripping values through the live block: a setter at N is read back by
// the getter at N+8. The unsigned setter is slot 3 (a feature create driven through it succeeds) and the
// resource getter answers at slot 8, so resources are written through slot 0 -- the 64-bit setter, which
// is what a resource handle is. Writing them through the typed D3D12 setter left them unset.
constexpr int VT_SET_ULL = 0;
// Where the float setter actually lives. The public header declares it at slot 1, and this block --
// the driver's own, not the header's implementation -- does not keep a float there: every float written
// to slot 1 reads back as FAIL_UnsupportedParameter while every uint lands. The host discovers the real
// slot by round-tripping a value and sets it here before anything else is written.
int g_floatSlot = 1;
constexpr int VT_SET_UINT = 3;

using PFN_SetULL = void(__thiscall *)(void *, const char *, unsigned long long);
using PFN_SetFloat = void(__thiscall *)(void *, const char *, float);
using PFN_SetUInt = void(__thiscall *)(void *, const char *, unsigned int);

void setUInt(void *params, const char *name, unsigned int v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetUInt>(vt[VT_SET_UINT])(params, name, v);
}

void setFloat(void *params, const char *name, float v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[g_floatSlot])(params, name, v);
}

void setResourcePtr(void *params, const char *name, void *v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetULL>(vt[VT_SET_ULL])(params, name, (unsigned long long) v);
}

void setResource(void *params, const char *name, ID3D12Resource *v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetULL>(vt[VT_SET_ULL])(params, name, (unsigned long long) v);
}

using PFN_NrInitExt = int(__cdecl *)(unsigned long long, const wchar_t *, ID3D12Device *, int,
                                     const void *);
// An explicit ControlMask takes precedence over the runtime's automatic mask.
// We do not supply one; clear it on the reusable parameter block instead of
// allowing another feature's stale mask/resource to silently override this toggle.
void setAutomaticMask(void *params, int enabled) {
    setResourcePtr(params, "DLSSNR.ControlMask", nullptr);
    setUInt(params, "DLSSNR.UseAutoMask", enabled != 0 ? 1u : 0u);
}
using PFN_NrCreate = int(__cdecl *)(ID3D12GraphicsCommandList *, int, const void *, void **);
using PFN_NrEvaluate = int(__cdecl *)(ID3D12GraphicsCommandList *, const void *, const void *, void *);
using PFN_NrRelease = int(__cdecl *)(void *);

struct Snippet {
    HMODULE module = nullptr;
    PFN_NrInitExt init = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    std::unordered_set<ID3D12Device*> initialisedDevices;

};

std::recursive_mutex g_snippetMutex;
std::map<std::wstring, Snippet> g_snippets;
std::unordered_map<void*, Snippet*> g_featureOwners;
thread_local std::string g_modelError;

void recordModelError(Snippet* snippet, const char* fallback) {
    auto message = snippet && snippet->module
        ? (const char*(*)())GetProcAddress(snippet->module, "NVFP4_GetLastError") : nullptr;
    const char* detail = message ? message() : nullptr;
    g_modelError = detail && *detail ? detail : fallback;
}

Snippet* loadSnippet(const wchar_t *path) {
    std::lock_guard<std::recursive_mutex> lock(g_snippetMutex);
    if (!path || !*path) return nullptr;
    auto& snippet = g_snippets[path];
    if (snippet.module) {
        return snippet.create && snippet.evaluate && snippet.release ? &snippet : nullptr;
    }
    snippet.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!snippet.module) {
        return nullptr;
    }
    nrweights::watch(snippet.module);
    snippet.init = (PFN_NrInitExt) GetProcAddress(snippet.module, "NVSDK_NGX_D3D12_Init_Ext");
    snippet.create = (PFN_NrCreate) GetProcAddress(snippet.module, "NVSDK_NGX_D3D12_CreateFeature");
    snippet.evaluate = (PFN_NrEvaluate) GetProcAddress(snippet.module, "NVSDK_NGX_D3D12_EvaluateFeature");
    snippet.release = (PFN_NrRelease) GetProcAddress(snippet.module, "NVSDK_NGX_D3D12_ReleaseFeature");


    return snippet.create && snippet.evaluate && snippet.release ? &snippet : nullptr;
}


} // namespace

extern "C" {

// Called once, after the host has worked out which slot this block keeps floats in.
__declspec(dllexport) void dlssnr_call_set_float_slot(int slot) {
    if (slot >= 0 && slot < 8) {
        g_floatSlot = slot;
    }
}

// Writes a float through an arbitrary slot, so the host can find the right one by testing.
__declspec(dllexport) void dlssnr_call_probe_float(void *params, const char *name, float value,
                                                   int slot) {
    if (!params || slot < 0 || slot >= 8) {
        return;
    }
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[slot])(params, name, value);
}

// Last init and create results, so the add-on can log why a feature never appeared.
__declspec(dllexport) int dlssnr_call_last_init = 0;
__declspec(dllexport) int dlssnr_call_last_create = 0;
__declspec(dllexport) const char* dlssnr_call_error() { return g_modelError.c_str(); }

// ---------------------------------------------------------------------------------------------
// Vulkan.
//
// The model ships a complete native Vulkan surface -- fourteen entry points against D3D12's ten --
// so a Vulkan game does not need the D3D12 bridge the pass currently reaches it through. It needs
// this: the same caller gate, because the snippet checks its caller's module path whichever API is
// being used, and only this DLL has a name that satisfies it.
//
// Handles are passed as void*. Every Vulkan handle this touches -- instance, physical device,
// device, command buffer -- is a dispatchable handle, which is a pointer on every 64-bit platform,
// and taking them opaquely keeps the forwarder free of a Vulkan dependency it would otherwise carry
// only to name types it never dereferences.
//
// Resources are the caller's problem for the same reason: NGX wants a pointer to an
// NVSDK_NGX_Resource_VK, the host builds it, and this passes the pointer through the same 64-bit
// parameter setter the D3D12 path uses for ID3D12Resource*.
// ---------------------------------------------------------------------------------------------

// nvsdk_ngx_vk.h: NVSDK_NGX_VULKAN_Init_Ext(ApplicationId, ApplicationDataPath, VkInstance, VkPhysicalDevice,
// VkDevice, NVSDK_NGX_Version InSDKVersion, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo) -- the version comes
// before the feature info. (It was declared the other way round here once, which handed NGX version 0 and a
// "pointer" of 0x15.)
using PFN_NrVkInitExt = int(__cdecl *)(unsigned long long, const wchar_t *, void *, void *, void *, int,
                                       const void *);
using PFN_NrVkCreate = int(__cdecl *)(void *, int, const void *, void **);
using PFN_NrVkEvaluate = int(__cdecl *)(void *, const void *, const void *, void *);
using PFN_NrVkShutdown1 = int(__cdecl *)(void *);

struct VkSnippet {
    HMODULE module = nullptr;
    PFN_NrVkInitExt init = nullptr;
    PFN_NrVkCreate create = nullptr;
    PFN_NrVkEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrVkShutdown1 shutdown = nullptr;
    // The device NGX is initialised on. A new device needs its own init; a dead one is forgotten, not shut down.
    void *initialisedDevice = nullptr;
};

VkSnippet g_vk;

bool loadVkSnippet(const wchar_t *path) {
    if (g_vk.module) {
        return g_vk.create != nullptr;
    }

    g_vk.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!g_vk.module) {
        return false;
    }
    nrweights::watch(g_vk.module);

    g_vk.init = (PFN_NrVkInitExt) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_Init_Ext");
    g_vk.create = (PFN_NrVkCreate) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_CreateFeature");
    g_vk.evaluate = (PFN_NrVkEvaluate) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_EvaluateFeature");
    g_vk.release = (PFN_NrRelease) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_ReleaseFeature");
    g_vk.shutdown = (PFN_NrVkShutdown1) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_Shutdown1");

    return g_vk.create != nullptr && g_vk.evaluate != nullptr;
}

// ---------------------------------------------------------------------------------------------
// The model's own scaling ratio, asked for the way NVIDIA asks for it.
//
// The snippet publishes callbacks into a parameter block through PopulateParameters_Impl -- the same
// mechanism DLSS uses for DLSSOptimalSettingsCallback. The strings in nvngx_dlssnr.dll spell the
// contract out in order: DLSSNRComputeScalingRatioCallback, ComputeScalingRatioCommon,
// PerfQualityValue, then the two failures "missing PerfQualityValue for DLSSNR scaling ratio
// computation" and "unsupported PerfQualityValue %u ...", then DLSSNR.ScalingRatio itself.
//
// So: populate a block, read the callback out of it, set PerfQualityValue, call it, read the ratio
// back. Read-only -- it creates no feature and changes no feature state, which is exactly why it is
// worth doing before anything is built on the answer.
//
// It lives here rather than in the host because PopulateParameters_Impl is the snippet's own export
// and the snippet resolves its caller's module path.
// ---------------------------------------------------------------------------------------------

using PFN_NrPopulate = int(__cdecl *)(void *);
using PFN_NrRatioCallback = int(__cdecl *)(void *);

// Getters mirror setters eight slots up -- the same rule that put the resource getter at 8 opposite
// the 64-bit setter at 0. A pointer published into the block is written as a 64-bit value, so it comes
// back through slot 8; a float comes back through the discovered float slot plus eight.
constexpr int VT_GET_ULL = VT_SET_ULL + 8;

using PFN_GetULL = int(__thiscall *)(void *, const char *, unsigned long long *);
using PFN_GetFloat = int(__thiscall *)(void *, const char *, float *);

__declspec(dllexport) int dlssnr_last_ratio_result = 0;
__declspec(dllexport) int dlssnr_last_ratio_stage = 0;

// Asks the model what resolution it wants for a given quality level.
//
// perfQuality is NVSDK_NGX_PerfQuality_Value: 0 MaxPerf, 1 Balanced, 2 MaxQuality, 3 UltraPerformance,
// 4 UltraQuality, 5 DLAA. Returns 1 and writes outRatio on success. 0 means the callback was never
// published, so the mechanism is not live in this snippet; -1 means it was published and refused, which
// for this callback means the quality value is not one it supports. dlssnr_last_ratio_stage says how
// far it got, so a zero can be told apart from a snippet that would not load at all.
__declspec(dllexport) int dlssnr_query_scaling_ratio(const wchar_t *snippetPath, void *capabilityParams,
                                                     unsigned int perfQuality, float *outRatio) {
    dlssnr_last_ratio_stage = 0;

    auto* snippet = loadSnippet(snippetPath);
    if (!snippet || !capabilityParams || !outRatio) {
        return 0;
    }

    dlssnr_last_ratio_stage = 1;

    // Publishing is what puts the callback in the block. Harmless if it has already happened.
    auto populate = (PFN_NrPopulate) GetProcAddress(snippet->module, "NVSDK_NGX_D3D12_PopulateParameters_Impl");

    if (populate != nullptr) {
        volatile int populated = populate(capabilityParams);
        (void) populated;
        dlssnr_last_ratio_stage = 2;
    }

    void **vt = *reinterpret_cast<void ***>(capabilityParams);
    unsigned long long raw = 0;

    if (reinterpret_cast<PFN_GetULL>(vt[VT_GET_ULL])(capabilityParams, "DLSSNRComputeScalingRatioCallback",
                                                     &raw) != 1 ||
        raw == 0) {
        return 0;
    }

    dlssnr_last_ratio_stage = 3;

    setUInt(capabilityParams, "PerfQualityValue", perfQuality);

    // Cleared first so a callback that writes nothing cannot be mistaken for one that wrote zero.
    setFloat(capabilityParams, "DLSSNR.ScalingRatio", -1.0f);

    // Assigned rather than returned, for the same reason every other call through this module is.
    volatile int result = reinterpret_cast<PFN_NrRatioCallback>((void *) raw)(capabilityParams);
    dlssnr_last_ratio_result = (int) result;

    if (result != 1) {
        return -1;
    }

    dlssnr_last_ratio_stage = 4;

    float ratio = -1.0f;

    if (reinterpret_cast<PFN_GetFloat>(vt[g_floatSlot + 8])(capabilityParams, "DLSSNR.ScalingRatio",
                                                            &ratio) != 1) {
        return -1;
    }

    dlssnr_last_ratio_stage = 5;
    *outRatio = ratio;
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Native Direct3D 11.
//
// The claim this tests is one nobody ever tested. "The model refuses to run on DX11, it answers
// FeatureNotSupported" has been in the notes and the release text for a long time, and this forwarder
// resolves no D3D11 entry point at all -- so whatever returned FeatureNotSupported, it was not the
// snippet's own D3D11 path, because that path has never been called.
//
// The binary says it should exist. nvngx_dlssnr.dll exports ten D3D11 entry points, implemented in
// source/features/dlssnr/ngx_d3d11.cpp, and its CreateFeature, EvaluateFeature and ReleaseFeature go
// through the same CreateFeatureCommon / EvaluateFeatureCommon / ReleaseFeatureCommon in
// ngx_templates.h that the D3D12 path uses. That is a real implementation sharing the same core, not
// a stub.
//
// If it works, a D3D11 game does not need the bridge -- which is what currently forces those games to
// give up DLSS as their upscaler, the biggest caveat in the whole feature.
//
// Probe first, exactly as the Vulkan path did: resolve the entry points and report which answer,
// before anything is created.
// ---------------------------------------------------------------------------------------------

// The _Ext variant puts the version BEFORE the feature info. That is the entire difference between
// NVSDK_NGX_D3D11_Init and NVSDK_NGX_D3D11_Init_Ext, and getting it backwards is not a wrong answer,
// it is a fault: NGX would take the version as a pointer and dereference 0x15 to read PathListInfo.
// The correct order is in this repo already, at proxies/NVNGX_Proxy.h.
using PFN_NrD3D11Init = int(__cdecl *)(unsigned long long, const wchar_t *, void *, int, const void *);
using PFN_NrD3D11Create = int(__cdecl *)(void *, int, const void *, void **);
using PFN_NrD3D11Evaluate = int(__cdecl *)(void *, const void *, const void *, void *);

struct D3D11Snippet {
    HMODULE module = nullptr;
    PFN_NrD3D11Init init = nullptr;
    PFN_NrD3D11Create create = nullptr;
    PFN_NrD3D11Evaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    bool initialised = false;
};

D3D11Snippet g_d3d11;

bool loadD3D11Snippet(const wchar_t *path) {
    if (g_d3d11.module) {
        return g_d3d11.create != nullptr;
    }

    // Loading the same path twice does not give two modules -- Windows returns the same HMODULE with
    // the reference count raised. So this would share one snippet's global NGX core with the D3D12
    // path: one device pointer, one set of caches, two independent "initialised" flags over the top.
    // Initialising it for D3D11 could then leave the D3D12 core holding a D3D11 device, which is a
    // device removal rather than a wrong answer.
    //
    // A probe must not be able to break the thing it is probing, so it takes its own copy under a
    // different name and the two cores never meet. If the copy cannot be made, the probe declines
    // rather than falling back to the shared module.
    wchar_t probeCopy[MAX_PATH] = {};
    wchar_t probeDir[MAX_PATH] = {};

    if (GetTempPathW(MAX_PATH, probeDir) == 0) {
        return false;
    }

    wcscpy_s(probeCopy, probeDir);
    wcscat_s(probeCopy, L"nvngx.dll_dlssnr_d3d11probe.dll");

    if (!CopyFileW(path, probeCopy, FALSE)) {
        return false;
    }

    g_d3d11.module = LoadLibraryExW(probeCopy, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!g_d3d11.module) {
        return false;
    }
    nrweights::watch(g_d3d11.module);

    g_d3d11.init = (PFN_NrD3D11Init) GetProcAddress(g_d3d11.module, "NVSDK_NGX_D3D11_Init_Ext");
    g_d3d11.create = (PFN_NrD3D11Create) GetProcAddress(g_d3d11.module, "NVSDK_NGX_D3D11_CreateFeature");
    g_d3d11.evaluate = (PFN_NrD3D11Evaluate) GetProcAddress(g_d3d11.module, "NVSDK_NGX_D3D11_EvaluateFeature");
    g_d3d11.release = (PFN_NrRelease) GetProcAddress(g_d3d11.module, "NVSDK_NGX_D3D11_ReleaseFeature");

    return g_d3d11.create != nullptr && g_d3d11.evaluate != nullptr;
}

// Ask the feature what it needs, rather than asking the device to start it.
//
// GetFeatureRequirements is the query NGX provides for exactly this question, and unlike Init it
// creates nothing and touches no device. A feature that genuinely does not run on an API answers here,
// and it answers without our having to interpret a failure that might have been ours.
//
// Worth asking because the first attempt proved less than it looked. Init_Ext returned
// FAIL_FeatureNotSupported, but the same process shows the driver's own NGX core initialising on
// D3D11 successfully -- so D3D11 NGX works here, and it was specifically the snippet's own init,
// called directly by us, that declined. The snippet carries the string "Error: Not called from NGX
// runtime", which is a check we may simply be failing rather than a statement about the platform.
using PFN_NrD3D11Requirements = int(__cdecl *)(const void *, const void *, void *);

__declspec(dllexport) int dlssnr_d3d11_requirements(const wchar_t *snippetPath, void *adapter,
                                                    unsigned int *outFlags, unsigned int *outArch,
                                                    unsigned int *outOsVersion) {
    if (!loadD3D11Snippet(snippetPath)) {
        return -2;
    }

    auto req = (PFN_NrD3D11Requirements) GetProcAddress(g_d3d11.module,
                                                        "NVSDK_NGX_D3D11_GetFeatureRequirements");

    if (req == nullptr) {
        return -1;
    }

    // NVSDK_NGX_FeatureDiscoveryInfo: SDK version, feature id, application identifier, log level,
    // feature-specific info. Only the first two matter for the answer.
    struct {
        int sdkVersion;
        int featureId;
        struct { int idType; unsigned long long appId; } identifier;
        const wchar_t *dataPath;
        const void *loggingInfo;
        const void *featureInfo;
    } discovery {};

    discovery.sdkVersion = 0x0000015;
    discovery.featureId = 18;
    discovery.identifier.idType = 0;
    discovery.identifier.appId = 0x24480451ull;
    discovery.dataPath = L".";

    // NVSDK_NGX_FeatureRequirement: the outputs. Generous so a longer struct cannot overrun.
    unsigned char requirement[512] = {};

    // The adapter is not optional. Passing null the first time produced AdapterUnsupported, which
    // reads like an answer about the hardware and is actually an answer about the question.
    volatile int result = req(adapter, &discovery, requirement);

    const unsigned int *out = reinterpret_cast<const unsigned int *>(requirement);

    // NVSDK_NGX_FeatureRequirement: FeatureSupported bit field, then the minimum architecture, then
    // the minimum OS version. The last two are worth having -- they say what it wanted, not just that
    // it was unhappy.
    if (outFlags != nullptr) {
        *outFlags = out[0];
    }

    if (outArch != nullptr) {
        *outArch = out[1];
    }

    if (outOsVersion != nullptr) {
        *outOsVersion = out[2];
    }

    return (int) result;
}

__declspec(dllexport) int dlssnr_d3d11_last_init = 0;
__declspec(dllexport) int dlssnr_d3d11_last_create = 0;

// Which entry points resolved, as a bit field: init 1, create 2, evaluate 4, release 8. Fifteen means
// the model's D3D11 surface is entirely reachable. Answered without creating anything.
__declspec(dllexport) int dlssnr_d3d11_probe(const wchar_t *snippetPath) {
    loadD3D11Snippet(snippetPath);

    int bits = 0;
    bits |= g_d3d11.init != nullptr ? 1 : 0;
    bits |= g_d3d11.create != nullptr ? 2 : 0;
    bits |= g_d3d11.evaluate != nullptr ? 4 : 0;
    bits |= g_d3d11.release != nullptr ? 8 : 0;

    return bits;
}

// The non-_Ext init. Different export, and it puts the feature info before the version -- the reverse
// of _Ext, which is the whole reason the two exist.
using PFN_NrD3D11InitPlain = int(__cdecl *)(unsigned long long, const wchar_t *, void *, const void *, int);

// Initialises NGX on a D3D11 device, four ways, reporting each.
//
// The feature itself says D3D11 is supported -- GetFeatureRequirements answers 0x0 with a minimum
// architecture of Blackwell, which is the card this runs on. So a refusal from Init is about the
// call, not the platform, and the candidates are few enough to try in one run:
//
//   1  _Ext on our own private copy of the snippet
//   2  plain Init on that copy
//   3  _Ext on the shared module the D3D12 path already initialised
//   4  plain Init on that shared module
//
// Three and four use the module the D3D12 path owns. That is what the review warned against, because
// one snippet keeps one global core and a D3D11 init could leave it holding a D3D11 device. They run
// last, after D3D12 has had its turn, and only behind the flag -- but they are also the variants most
// likely to work, since the snippet may expect to be reached through a core that is already up.
//
// Returns the first result that succeeds, or the last failure. attemptOut says which one answered.
__declspec(dllexport) int dlssnr_d3d11_init(const wchar_t *snippetPath, const wchar_t *dataPath,
                                            void *device, int sdkVersion, int *attemptOut,
                                            int *resultsOut) {
    if (device == nullptr) {
        return -1;
    }

    if (g_d3d11.initialised) {
        if (attemptOut != nullptr) *attemptOut = 0;
        return 1;
    }

    const bool haveCopy = loadD3D11Snippet(snippetPath);
    auto* shared = loadSnippet(snippetPath);

    struct Attempt { HMODULE module; bool ext; };

    const Attempt attempts[4] = {
        { haveCopy ? g_d3d11.module : nullptr, true },
        { haveCopy ? g_d3d11.module : nullptr, false },
        { shared ? shared->module : nullptr, true },
        { shared ? shared->module : nullptr, false },
    };

    int last = -1;

    for (int i = 0; i < 4; ++i) {
        if (attempts[i].module == nullptr) {
            if (resultsOut != nullptr) resultsOut[i] = -2;
            continue;
        }

        volatile int result = 0;

        if (attempts[i].ext) {
            auto fn = (PFN_NrD3D11Init) GetProcAddress(attempts[i].module, "NVSDK_NGX_D3D11_Init_Ext");

            if (fn == nullptr) {
                if (resultsOut != nullptr) resultsOut[i] = -3;
                continue;
            }

            result = fn(0x24480451ull, dataPath, device, sdkVersion, nullptr);
        } else {
            auto fn = (PFN_NrD3D11InitPlain) GetProcAddress(attempts[i].module, "NVSDK_NGX_D3D11_Init");

            if (fn == nullptr) {
                if (resultsOut != nullptr) resultsOut[i] = -3;
                continue;
            }

            result = fn(0x24480451ull, dataPath, device, nullptr, sdkVersion);
        }

        last = (int) result;

        if (resultsOut != nullptr) resultsOut[i] = last;

        if (last == 1) {
            g_d3d11.module = attempts[i].module;
            g_d3d11.create = (PFN_NrD3D11Create) GetProcAddress(attempts[i].module,
                                                                "NVSDK_NGX_D3D11_CreateFeature");
            g_d3d11.evaluate = (PFN_NrD3D11Evaluate) GetProcAddress(attempts[i].module,
                                                                    "NVSDK_NGX_D3D11_EvaluateFeature");
            g_d3d11.release = (PFN_NrRelease) GetProcAddress(attempts[i].module,
                                                             "NVSDK_NGX_D3D11_ReleaseFeature");
            g_d3d11.initialised = true;

            if (attemptOut != nullptr) *attemptOut = i + 1;

            dlssnr_d3d11_last_init = last;
            return last;
        }
    }

    if (attemptOut != nullptr) *attemptOut = 0;

    dlssnr_d3d11_last_init = last;
    return last;
}

// Creates the feature on a D3D11 device context. Tuning is set here for the same reason it is on the
// other two paths: the model reads it once, when it builds the feature.
__declspec(dllexport) void *dlssnr_d3d11_create(void *deviceContext, void *capabilityParams,
                                                unsigned int width, unsigned int height, int preset,
                                                float intensity, int style, float localStructure,
                                                float localTone, float skinStructure, int useAutoMask,
                                                int uiCorrection) {
    if (g_d3d11.create == nullptr || deviceContext == nullptr || capabilityParams == nullptr) {
        return nullptr;
    }

    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "CreationNodeMask", 1);
    setUInt(capabilityParams, "VisibilityNodeMask", 1);
    setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setAutomaticMask(capabilityParams, useAutoMask);
    setUInt(capabilityParams, "DLSSNR.UICorrection", (unsigned int) uiCorrection);

    void *feature = nullptr;
    volatile int result = g_d3d11.create(deviceContext, 18, capabilityParams, &feature);

    dlssnr_d3d11_last_create = (int) result;

    return result == 1 ? feature : nullptr;
}

__declspec(dllexport) void dlssnr_d3d11_release(void *feature) {
    if (g_d3d11.release != nullptr && feature != nullptr) {
        volatile int ignored = g_d3d11.release(feature);
        (void) ignored;
    }
}

__declspec(dllexport) int dlssnr_vk_last_init = 0;
__declspec(dllexport) int dlssnr_vk_last_create = 0;

// Which of the four entry points resolved, as a bit field: init 1, create 2, evaluate 4, release 8.
// 15 means the model's Vulkan surface is entirely reachable from here. Answered without creating
// anything, so the host can decide whether the native path exists before committing to it.
__declspec(dllexport) int dlssnr_vk_probe(const wchar_t *snippetPath) {
    loadVkSnippet(snippetPath);

    int bits = 0;
    bits |= g_vk.init != nullptr ? 1 : 0;
    bits |= g_vk.create != nullptr ? 2 : 0;
    bits |= g_vk.evaluate != nullptr ? 4 : 0;
    bits |= g_vk.release != nullptr ? 8 : 0;

    return bits;
}

__declspec(dllexport) int dlssnr_vk_init(const wchar_t *snippetPath, const wchar_t *dataPath,
                                         void *instance, void *physicalDevice, void *device,
                                         int sdkVersion) {
    if (!loadVkSnippet(snippetPath) || g_vk.init == nullptr) {
        return -1;
    }

    if (device != nullptr && device == g_vk.initialisedDevice) {
        return 1;
    }

    // Assigned rather than returned directly. A tail call becomes a jmp, and the snippet resolves its
    // caller from the return address -- so tail calling hands it whoever called this instead of this
    // module, and the caller gate rejects it before a single argument is read.
    volatile int result = g_vk.init(0x0, dataPath, instance, physicalDevice, device, sdkVersion, nullptr);

    dlssnr_vk_last_init = (int) result;

    // Only a success changes what is tracked: a failed init on another device must not lose the one that is live.
    if (result == 1) {
        g_vk.initialisedDevice = device;
    }

    return (int) result;
}

// Real teardown on a live device: NGX frees what it holds for it. The host releases the features and the parameter
// block first. Returns NGX's result, or 0 if the model has no Shutdown1 or this device was never initialised here.
__declspec(dllexport) int dlssnr_vk_shutdown(void *device) {
    if (device == nullptr || device != g_vk.initialisedDevice) {
        return 0;
    }

    // The device is going either way, so it stops being tracked even when there is nothing to call.
    if (g_vk.shutdown == nullptr) {
        g_vk.initialisedDevice = nullptr;
        return 0;
    }

    volatile int result = g_vk.shutdown(device);
    g_vk.initialisedDevice = nullptr;

    return (int) result;
}

// The device is gone (the game destroyed it): nothing may be called on it, but the next device -- which can come
// back with the same handle value -- needs its own init.
__declspec(dllexport) void dlssnr_vk_forget() { g_vk.initialisedDevice = nullptr; }

// Creates the feature on a Vulkan command buffer. Same contract as the D3D12 one: initialisation work
// is recorded into the buffer, so the handle has to outlive its execution.
// Creates the feature on a Vulkan command buffer. Same contract as the D3D12 one: initialisation work
// is recorded into the buffer, so the handle has to outlive its execution.
//
// The tuning is set here and not at evaluate, for the same reason it is on the D3D12 path: the model
// reads these once, when it builds the feature, and anything set only at evaluate is ignored. That was
// why none of these controls did anything for a long time.
__declspec(dllexport) void *dlssnr_vk_create(void *cmdBuffer, void *capabilityParams, unsigned int width,
                                             unsigned int height, int preset, float intensity, int style,
                                             float localStructure, float localTone, float skinStructure,
                                             int useAutoMask, int uiCorrection) {
    if (g_vk.create == nullptr || cmdBuffer == nullptr || capabilityParams == nullptr) {
        return nullptr;
    }

    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "CreationNodeMask", 1);
    setUInt(capabilityParams, "VisibilityNodeMask", 1);

    // Written unconditionally, including zero. The block belongs to the driver and outlives the
    // feature, so skipping the write for "default" leaves the last chosen preset sitting in it.
    setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setAutomaticMask(capabilityParams, useAutoMask);
    setUInt(capabilityParams, "DLSSNR.UICorrection", (unsigned int) uiCorrection);

    void *feature = nullptr;
    volatile int result = g_vk.create(cmdBuffer, 18, capabilityParams, &feature);

    dlssnr_vk_last_create = (int) result;

    return result == 1 ? feature : nullptr;
}

// Evaluates on Vulkan. Same parameter block as the D3D12 path, filled the same way and in the same
// order, because it is the same feature -- only the four resources differ, and only in that each is a
// pointer to an NVSDK_NGX_Resource_VK the host built rather than an ID3D12Resource. Both go through
// the block's 64-bit setter, so the snippet sees the same shape either way.
//
// Filling it here rather than in the host keeps the two APIs from drifting: a parameter added to one
// evaluate and forgotten in the other would be a bug that only appears on one backend.
__declspec(dllexport) int dlssnr_vk_evaluate_v2(void *cmdBuffer, void *feature, void *capabilityParams,
                                             void *color, void *depth, void *motion, void *output,
                                             unsigned int width, unsigned int height,
                                             unsigned int guideWidth, unsigned int guideHeight,
                                             unsigned int motionWidth, unsigned int motionHeight,
                                             unsigned int depthBaseX, unsigned int depthBaseY,
                                             unsigned int motionBaseX, unsigned int motionBaseY,
                                             int depthInverted, int reset, float intensity, int style,
                                             float localStructure, float localTone, float skinStructure,
                                             int useAutoMask, float mvScaleX, float mvScaleY);

__declspec(dllexport) int dlssnr_vk_evaluate(void *cmdBuffer, void *feature, void *capabilityParams,
                                             void *color, void *depth, void *motion, void *output,
                                             unsigned int width, unsigned int height,
                                             unsigned int guideWidth, unsigned int guideHeight,
                                             int depthInverted, int reset, float intensity, int style,
                                             float localStructure, float localTone, float skinStructure,
                                             int useAutoMask, float mvScaleX, float mvScaleY) {
    return dlssnr_vk_evaluate_v2(cmdBuffer, feature, capabilityParams, color, depth, motion, output, width, height,
                                 guideWidth, guideHeight, guideWidth, guideHeight, 0, 0, 0, 0, depthInverted, reset,
                                 intensity, style, localStructure, localTone, skinStructure, useAutoMask, mvScaleX,
                                 mvScaleY);
}

__declspec(dllexport) int dlssnr_vk_evaluate_v2(void *cmdBuffer, void *feature, void *capabilityParams,
                                             void *color, void *depth, void *motion, void *output,
                                             unsigned int width, unsigned int height,
                                             unsigned int guideWidth, unsigned int guideHeight,
                                             unsigned int motionWidth, unsigned int motionHeight,
                                             unsigned int depthBaseX, unsigned int depthBaseY,
                                             unsigned int motionBaseX, unsigned int motionBaseY,
                                             int depthInverted, int reset, float intensity, int style,
                                             float localStructure, float localTone, float skinStructure,
                                             int useAutoMask, float mvScaleX, float mvScaleY) {
    if (g_vk.evaluate == nullptr || feature == nullptr || capabilityParams == nullptr) {
        return -1;
    }

    setResourcePtr(capabilityParams, "DLSSNR.Color", color);
    setResourcePtr(capabilityParams, "DLSSNR.Depth", depth);
    setResourcePtr(capabilityParams, "DLSSNR.MVec", motion);
    setResourcePtr(capabilityParams, "DLSSNR.Output", output);

    // The block is shared with the game's own DLSS, which overwrites these between frames, so every
    // value the feature reads is set again here rather than relying on what create left behind.
    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "DLSSNR.DepthInverted", (unsigned int) depthInverted);
    setUInt(capabilityParams, "DLSSNR.Reset", (unsigned int) reset);

    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseX", depthBaseX);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseY", depthBaseY);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectWidth", guideWidth);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectHeight", guideHeight);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseX", motionBaseX);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseY", motionBaseY);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectWidth", motionWidth);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectHeight", motionHeight);

    // The game's own encoding, passed through rather than derived. Deriving it from the resolutions
    // came out as exactly 1.0 at native, which told the model almost nothing had moved.
    setFloat(capabilityParams, "DLSSNR.MVecScaleX", mvScaleX);
    setFloat(capabilityParams, "DLSSNR.MVecScaleY", mvScaleY);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setAutomaticMask(capabilityParams, useAutoMask);

    // Assigned rather than returned. A tail call becomes a jmp and the snippet would resolve its
    // caller past this module, which the gate rejects.
    volatile int result = g_vk.evaluate(cmdBuffer, feature, capabilityParams, nullptr);

    return (int) result;
}

__declspec(dllexport) void dlssnr_vk_release(void *feature) {
    if (g_vk.release != nullptr && feature != nullptr) {
        volatile int ignored = g_vk.release(feature);
        (void) ignored;
    }
}

// Creates a persistent Neural Rendering feature. The handle records initialisation work into cmd, so it
// must outlive that command list's execution; releasing it early loses the device.
__declspec(dllexport) void *dlssnr_call_create(const wchar_t *snippetPath, const wchar_t *dataPath,
                                               ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                                               void *capabilityParams, unsigned int width,
                                               unsigned int height, int preset, float intensity,
                                               int style, float localStructure, float localTone,
                                               float skinStructure, int useAutoMask,
                                               int uiCorrection) {
    std::lock_guard<std::recursive_mutex> lock(g_snippetMutex);
    g_modelError.clear();
    auto* snippet = loadSnippet(snippetPath);
    if (!snippet || !capabilityParams) {
        recordModelError(snippet, "model DLL or capability parameters unavailable");
        return nullptr;
    }
    if (!snippet->initialisedDevices.count(device) && snippet->init) {
        // OptiScaler's own generic application id, the one it already hands DLSS when a game's id is
        // not wanted. What was here before was 0x4350324B -- "CP2K" -- so every game that ever loaded
        // this announced itself to the driver as Cyberpunk 2077.
        dlssnr_call_last_init = snippet->init(0x24480451ull, dataPath, device, 0x0000015, capabilityParams);
        if (dlssnr_call_last_init != 1) {
            recordModelError(snippet, "model initialization failed");
            return nullptr;
        }
        snippet->initialisedDevices.insert(device);
    }
    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "CreationNodeMask", 1);
    setUInt(capabilityParams, "VisibilityNodeMask", 1);
    // Written unconditionally, including zero. This block belongs to the driver and outlives the
    // feature, so skipping the write for "default" left whichever preset was chosen last still sitting
    // in it -- and going back to default did nothing at all.
    setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);

    // The tuning has to be here rather than at evaluate. Everything this sets before create takes
    // effect; everything set only at evaluate is ignored, which is why none of these controls did
    // anything for a long time. The model reads them once, when it builds the feature.
    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setAutomaticMask(capabilityParams, useAutoMask);
    setUInt(capabilityParams, "DLSSNR.UICorrection", (unsigned int) uiCorrection);
    void *handle = nullptr;
    dlssnr_call_last_create = snippet->create(cmd, 18, capabilityParams, &handle);
    if (dlssnr_call_last_create == 1 && handle) g_featureOwners[handle] = snippet;
    else recordModelError(snippet, "model feature creation failed");
    // Match the DX11/Vulkan paths: a partial handle on failure is not usable.
    return dlssnr_call_last_create == 1 ? handle : nullptr;
}

__declspec(dllexport) int dlssnr_call_evaluate_v2(ID3D12GraphicsCommandList *cmd, void *feature,
                                               void *capabilityParams, ID3D12Resource *color,
                                               ID3D12Resource *depth, ID3D12Resource *motion,
                                               ID3D12Resource *output, unsigned int width,
                                               unsigned int height, unsigned int guideWidth,
                                               unsigned int guideHeight, unsigned int motionWidth,
                                               unsigned int motionHeight, unsigned int depthBaseX,
                                               unsigned int depthBaseY, unsigned int motionBaseX,
                                               unsigned int motionBaseY, int depthInverted, int reset,
                                               float intensity, int style, float localStructure,
                                               float localTone, float skinStructure, int useAutoMask,
                                               float mvScaleX, float mvScaleY);

__declspec(dllexport) int dlssnr_call_evaluate(ID3D12GraphicsCommandList *cmd, void *feature,
                                               void *capabilityParams, ID3D12Resource *color,
                                               ID3D12Resource *depth, ID3D12Resource *motion,
                                               ID3D12Resource *output, unsigned int width,
                                               unsigned int height, unsigned int guideWidth,
                                               unsigned int guideHeight, int depthInverted, int reset,
                                               float intensity, int style, float localStructure,
                                               float localTone, float skinStructure, int useAutoMask,
                                               float mvScaleX, float mvScaleY) {
    return dlssnr_call_evaluate_v2(cmd, feature, capabilityParams, color, depth, motion, output, width, height,
                                   guideWidth, guideHeight, guideWidth, guideHeight, 0, 0, 0, 0, depthInverted, reset,
                                   intensity, style, localStructure, localTone, skinStructure, useAutoMask, mvScaleX,
                                   mvScaleY);
}

// Colour and output are display resolution; depth and motion come from the game's own DLSS evaluation and
// may be render resolution, so each resource carries its own subrect and motion scales by the ratio.
__declspec(dllexport) int dlssnr_call_evaluate_v2(ID3D12GraphicsCommandList *cmd, void *feature,
                                               void *capabilityParams, ID3D12Resource *color,
                                               ID3D12Resource *depth, ID3D12Resource *motion,
                                               ID3D12Resource *output, unsigned int width,
                                               unsigned int height, unsigned int guideWidth,
                                               unsigned int guideHeight, unsigned int motionWidth,
                                               unsigned int motionHeight, unsigned int depthBaseX,
                                               unsigned int depthBaseY, unsigned int motionBaseX,
                                               unsigned int motionBaseY, int depthInverted, int reset,
                                               float intensity, int style, float localStructure,
                                               float localTone, float skinStructure, int useAutoMask,
                                               float mvScaleX, float mvScaleY) {
    std::lock_guard<std::recursive_mutex> lock(g_snippetMutex);
    g_modelError.clear();
    const auto owner = g_featureOwners.find(feature);
    if (!feature || !capabilityParams || owner == g_featureOwners.end()) {
        g_modelError = "invalid model feature or capability parameters";
        return 0;
    }
    setResource(capabilityParams, "DLSSNR.Color", color);
    setResource(capabilityParams, "DLSSNR.Depth", depth);
    setResource(capabilityParams, "DLSSNR.MVec", motion);
    setResource(capabilityParams, "DLSSNR.Output", output);

    // The block is shared with the game's own DLSS, which overwrites these between frames, so every
    // value the feature reads is set again here rather than relying on what create left behind.
    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "DLSSNR.DepthInverted", (unsigned int) depthInverted);
    setUInt(capabilityParams, "DLSSNR.Reset", (unsigned int) reset);

    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseX", depthBaseX);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseY", depthBaseY);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectWidth", guideWidth);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectHeight", guideHeight);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseX", motionBaseX);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseY", motionBaseY);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectWidth", motionWidth);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectHeight", motionHeight);

    // The game's own encoding, passed through. Deriving this from the resolutions was a guess, and at
    // native resolution it came out as exactly 1.0 -- so a game using normalised vectors was telling
    // the model almost nothing had moved.
    setFloat(capabilityParams, "DLSSNR.MVecScaleX", mvScaleX);
    setFloat(capabilityParams, "DLSSNR.MVecScaleY", mvScaleY);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setAutomaticMask(capabilityParams, useAutoMask);

    // The result must not be returned directly. `return f(...)` is a tail call, and the compiler emits a
    // jmp rather than a call, which leaves this module's frame behind: the snippet then resolves its
    // caller to whoever called us and rejects it. Keeping the value in a volatile forces a real call and
    // a return through this module, which is the whole reason this file exists.
    volatile int result = owner->second->evaluate(cmd, feature, capabilityParams, nullptr);
    if (result != 1) recordModelError(owner->second, "model evaluation failed");
    return result;
}

// Inputs NVIDIA's own Streamline plugin sets that the positional exports predate: the model's global
// tone strength (read at create), and the interface as the game draws it -- its layer, its alpha, and
// the composited back buffer -- which is what the model's UI correction was designed around. Called
// before create and before every evaluate; absent resources are written as null, because the block
// outlives everything and a stale pointer is a freed resource.
__declspec(dllexport) void dlssnr_call_set_extras(void *capabilityParams, float globalTone,
                                                  ID3D12Resource *ui, ID3D12Resource *uiAlpha,
                                                  ID3D12Resource *backbuffer, unsigned int uiWidth,
                                                  unsigned int uiHeight, unsigned int bbWidth,
                                                  unsigned int bbHeight) {
    if (!capabilityParams) {
        return;
    }
    // DLSSNR.GlobalToneStrength is deliberately not written. The string does not appear anywhere in
    // nvngx_dlssnr.dll -- the model's own vocabulary is 61 DLSSNR.* names and this is not one of them,
    // so every write went into a map nothing reads. It exists in Streamline's sl.dlss_nr.dll, which is
    // presumably where it was copied from. Left as a parameter here so the signature does not churn.
    (void) globalTone;

    setResource(capabilityParams, "DLSSNR.UI", ui);
    setResource(capabilityParams, "DLSSNR.UIAlpha", uiAlpha);
    setResource(capabilityParams, "DLSSNR.Backbuffer", backbuffer);
    setUInt(capabilityParams, "DLSSNR.UISubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.UISubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.UISubrectWidth", uiWidth);
    setUInt(capabilityParams, "DLSSNR.UISubrectHeight", uiHeight);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectWidth", uiWidth);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectHeight", uiHeight);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectWidth", bbWidth);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectHeight", bbHeight);
}

__declspec(dllexport) void dlssnr_call_release(void *feature) {
    std::lock_guard<std::recursive_mutex> lock(g_snippetMutex);
    const auto owner = g_featureOwners.find(feature);
    if (feature && owner != g_featureOwners.end()) {
        volatile int result = owner->second->release(feature); // retain this module's caller frame
        (void) result;
        g_featureOwners.erase(owner);
    }
}

// Model weights selector: the folders the next feature creates read their weights from, one per model
// (dirA = Model A, WEIGHTS_HT or WEIGHTS_PL; dirB = Model B, WEIGHTS_CC_SMALL_FP8*; null or empty = the model DLL's
// own). Takes effect at the next create; features already built keep what they were built with.
__declspec(dllexport) void dlssnr_weights_select2(const wchar_t* dirA, const wchar_t* dirB)
{
    std::lock_guard<std::mutex> lock(nrweights::g_mutex);
    nrweights::g_dirA = nrweights::trimDir(dirA);
    nrweights::g_dirB = nrweights::trimDir(dirB);
}

// The first version of the selector (older OptiScaler builds): one folder for both models.
__declspec(dllexport) void dlssnr_weights_select(const wchar_t* setDir) { dlssnr_weights_select2(setDir, setDir); }

// What the selector did since the last call (one line per weights read), copied into the host's buffer and
// cleared. Returns the length written (the text is cut to fit and always terminated).
__declspec(dllexport) int dlssnr_weights_take_log(char* buffer, int size)
{
    std::lock_guard<std::mutex> lock(nrweights::g_mutex);
    if (!buffer || size <= 0)
        return 0;
    const size_t room = (size_t) size - 1;
    const int n = (int) (nrweights::g_log.size() < room ? nrweights::g_log.size() : room);
    memcpy(buffer, nrweights::g_log.data(), (size_t) n);
    buffer[n] = 0;
    nrweights::g_log.clear();
    return n;
}

} // extern "C"
