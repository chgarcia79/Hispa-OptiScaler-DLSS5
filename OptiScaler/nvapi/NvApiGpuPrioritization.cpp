#include "pch.h"
#include "NvApiGpuPrioritization.h"
#include "NvApiHooks.h"

#include <Logger.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#include <algorithm>
#include <mutex>

namespace NvApiGpuPrioritization
{
namespace
{
constexpr uint32_t kIdNvApiInitialize = 0x0150E828u;
constexpr uint32_t kIdEnumPhysicalGpus = 0xE5AC921Fu;
constexpr uint32_t kIdCmcYieldPctControl = 0xC742BB22u;
constexpr uint32_t kStructVersionV1 = 0x0001001Cu;

constexpr int NVAPI_INVALID_USER_PRIVILEGE_CODE = -137; // NVAPI_INVALID_USER_PRIVILEGE (0xFFFFFF77)
constexpr int kStatusNoImplementation = -3;
constexpr int kStatusInvalidArgument = -5;
constexpr int kStatusNotSupported = -104;

#pragma pack(push, 4)
struct NV_SYS_CMC_YIELD_PCT_CONTROL_V1
{
    uint32_t version, command, yieldPct, reserved[4];
};
#pragma pack(pop)
static_assert(sizeof(NV_SYS_CMC_YIELD_PCT_CONTROL_V1) == 28, "NV_SYS_CMC_YIELD_PCT_CONTROL_V1 debe medir 28 bytes");

using PfnInitialize = int(__cdecl*)();
using PfnEnumPhysicalGpus = int(__cdecl*)(void* handles[64], uint32_t* count);
using PfnCmcYieldPctControl = int(__cdecl*)(void* hPhysicalGpu, NV_SYS_CMC_YIELD_PCT_CONTROL_V1* params);

std::mutex g_mutex;
LiveState g_cached;
uint64_t g_seq = 0;

PFN_NvApi_QueryInterface ResolveQueryInterface()
{
    if (NvApiHooks::o_NvAPI_QueryInterface != nullptr)
        return NvApiHooks::o_NvAPI_QueryInterface;
    HMODULE mod = GetModuleHandleW(L"nvapi64.dll");
    if (mod == nullptr)
        mod = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return mod ? reinterpret_cast<PFN_NvApi_QueryInterface>(GetProcAddress(mod, "nvapi_QueryInterface")) : nullptr;
}

int ExecuteDriverCommand(uint32_t command, uint32_t inYield, uint32_t& outYield, uint32_t& outGpus, bool& hasDll, bool& hasEndpoint)
{
    hasDll = hasEndpoint = false;
    outGpus = 0;
    auto qi = ResolveQueryInterface();
    if (qi == nullptr)
        return -1;
    hasDll = true;

    auto pfnInit = reinterpret_cast<PfnInitialize>(qi(kIdNvApiInitialize));
    auto pfnEnum = reinterpret_cast<PfnEnumPhysicalGpus>(qi(kIdEnumPhysicalGpus));
    auto pfnCmc = reinterpret_cast<PfnCmcYieldPctControl>(qi(kIdCmcYieldPctControl));
    if (pfnEnum == nullptr || pfnCmc == nullptr)
        return kStatusNoImplementation;

    hasEndpoint = true;
    if (pfnInit != nullptr)
        pfnInit();

    void* handles[64] = {};
    uint32_t count = 0;
    const int enumStatus = pfnEnum(handles, &count);
    if (enumStatus != 0 || count == 0)
        return enumStatus != 0 ? enumStatus : -1;

    outGpus = std::min<uint32_t>(count, 64u);
    int lastStatus = kStatusNotSupported;
    for (uint32_t i = 0; i < outGpus; ++i)
    {
        NV_SYS_CMC_YIELD_PCT_CONTROL_V1 params { kStructVersionV1, command, inYield, { 0, 0, 0, 0 } };
        const int st = pfnCmc(handles[i], &params);
        lastStatus = st;
        if (st == 0)
        {
            outYield = params.yieldPct;
            if (command == 0u) return 0;
        }
        else if (st == NVAPI_INVALID_USER_PRIVILEGE_CODE)
            return st;
    }
    return lastStatus;
}

bool RunElevatedHelper(uint32_t yieldPct, bool& cancelledByUser)
{
    cancelledByUser = false;
    const wchar_t* helperExe = L"C:\\Soft\\Juegos\\HispaGameControl\\HispaGameControl.exe";
    if (GetFileAttributesW(helperExe) == INVALID_FILE_ATTRIBUTES)
        return false;

    const std::wstring args = L"--apply-gpu-yield " + std::to_wstring(yieldPct);
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = helperExe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei))
    {
        cancelledByUser = (GetLastError() == ERROR_CANCELLED);
        return false;
    }
    if (sei.hProcess == nullptr)
        return false;
    WaitForSingleObject(sei.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    return exitCode == 0;
}
} // namespace

LiveState RefreshLiveState()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LiveState next {};
    next.refreshSeq = ++g_seq;
    uint32_t readYield = 20u;
    const int st = ExecuteDriverCommand(0u, 0u, readYield, next.gpuCount, next.driverAvailable, next.supported);
    next.lastStatus = st;

    if (!next.driverAvailable)
        next.supported = false, next.statusMessage = "nvapi64.dll no disponible (requiere GPU NVIDIA RTX 50).";
    else if (!next.supported || st == kStatusNoImplementation || st == kStatusNotSupported)
        next.supported = false, next.statusMessage = "No soportado en driver actual — Requiere RTX 50 + Driver >= 620.55";
    else if (st == 0)
    {
        next.supported = true;
        next.currentYieldPct = readYield;
        next.foregroundPct = (readYield == 0u) ? 100u : (100u - std::min<uint32_t>(readYield, 100u));
        next.activeMode = ClassifyYieldPct(readYield);
        next.statusMessage = (readYield == 0u)
                                 ? "Activo en Driver: Off (Sin cuota CMC forzada)"
                                 : ("Activo en Driver: " + std::to_string(next.foregroundPct) + "% Juego / " + std::to_string(readYield) + "% Fondo");
    }
    else
        next.supported = false, next.statusMessage = "Error NVAPI al consultar prioridad GPU (" + std::to_string(st) + ").";

    g_cached = next;
    return g_cached;
}

LiveState GetCachedState()
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_cached.refreshSeq != 0)
        return g_cached;
    lock.unlock();
    return RefreshLiveState();
}

LiveState ApplyLiveYieldPct(uint32_t yieldPct)
{
    if (!IsValidYieldPct(yieldPct))
    {
        LiveState cur = RefreshLiveState();
        cur.lastStatus = kStatusInvalidArgument;
        cur.statusMessage = "Porcentaje invalido (0 o multiplo de 5 entre 5 y 50).";
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_cached = cur;
    }

    uint32_t dummy = yieldPct, gpus = 0;
    bool hasDll = false, hasEndpoint = false, usedElevation = false, cancelled = false;
    const int st = ExecuteDriverCommand(1u, yieldPct, dummy, gpus, hasDll, hasEndpoint);
    if (st == NVAPI_INVALID_USER_PRIVILEGE_CODE)
        usedElevation = RunElevatedHelper(yieldPct, cancelled);

    LiveState verified = RefreshLiveState();
    verified.elevatedUsedLastApply = usedElevation;
    if (cancelled)
        verified.statusMessage = "Elevacion cancelada por el usuario — se mantiene el valor actual del driver.";
    else if (st == NVAPI_INVALID_USER_PRIVILEGE_CODE && !usedElevation)
        verified.statusMessage = "Requiere permisos de Administrador (NVAPI_INVALID_USER_PRIVILEGE -137).";
    else if (st != 0 && st != NVAPI_INVALID_USER_PRIVILEGE_CODE)
        verified.statusMessage = "No se pudo aplicar en driver (NVAPI " + std::to_string(st) + ").";

    std::lock_guard<std::mutex> lock(g_mutex);
    return g_cached = verified;
}
} // namespace NvApiGpuPrioritization
