#include "pch.h"
#include "XeSS_UE_Dx12_Internal.h"

#include <Config.h>
#include <psapi.h>

namespace XeSSUnreal::Detail
{
PFN_AddVelocityFlatteningXeSSPass o_AddVelocityFlatteningXeSSPass = nullptr;
PFN_FlattenVelocity o_FlattenVelocity = nullptr;
PFN_GetSecondaryViewRectSize o_GetSecondaryViewRectSize = nullptr;
const wchar_t* g_xessVelocityName = nullptr;
int32_t g_viewRectOffset = 0;
thread_local void* t_flatteningView = nullptr;

int32_t* __fastcall hkGetSecondaryViewRectSize(void* view, int32_t* outSize, void* r8, void* r9)
{
    auto result = o_GetSecondaryViewRectSize(view, outSize, r8, r9);
    if (!Config::Instance()->XeSSUnrealDepthMvFix.value_or_default() || view == nullptr ||
        view != t_flatteningView || result == nullptr)
        return result;

    int32_t rect[4] {};
    if (!SafeRead((uintptr_t) view + g_viewRectOffset, rect))
        return result;

    auto width = rect[2] - rect[0];
    auto height = rect[3] - rect[1];
    if (rect[0] < 0 || rect[1] < 0 || width <= 0 || height <= 0 || width > result[0] || height > result[1])
        return result;

    result[0] = width;
    result[1] = height;
    return result;
}

void* __fastcall hkAddVelocityFlatteningXeSSPass(void* graphBuilder, void* sceneDepth, void* sceneVelocity, void* view,
                                                 void* arg5, void* arg6, void* arg7, void* arg8)
{
    if (Config::Instance()->XeSSUnrealDepthMvFix.value_or_default())
    {
        RecordSceneDepth(sceneDepth);
        t_flatteningView = o_GetSecondaryViewRectSize != nullptr ? view : nullptr;
    }
    auto result =
        o_AddVelocityFlatteningXeSSPass(graphBuilder, sceneDepth, sceneVelocity, view, arg5, arg6, arg7, arg8);
    t_flatteningView = nullptr;
    return result;
}

void* __fastcall hkFlattenVelocity(void* graphBuilder, void* sceneDepth, void* sceneVelocity, void* view,
                                   const wchar_t* name, void* event, bool upsample)
{
    if (Config::Instance()->XeSSUnrealDepthMvFix.value_or_default() && name == g_xessVelocityName)
    {
        RecordSceneDepth(sceneDepth);
        upsample = false;
    }
    return o_FlattenVelocity(graphBuilder, sceneDepth, sceneVelocity, view, name, event, upsample);
}
} // namespace XeSSUnreal::Detail

void XeSSUnreal::Hook()
{
    if (!Config::Instance()->XeSSUnrealDepthMvFix.value_or_default())
        return;

    static std::mutex mutex;
    static std::unordered_set<HMODULE> checkedModules;
    std::scoped_lock lock(mutex);

    if (Hooked())
        return;

    auto exe = GetModuleHandle(nullptr);
    if (checkedModules.insert(exe).second && Detail::HookModule(exe))
        return;

    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(), (DWORD) (modules.size() * sizeof(HMODULE)), &needed))
        return;
    modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));

    for (auto module : modules)
    {
        if (checkedModules.contains(module))
            continue;
        wchar_t path[MAX_PATH] {};
        if (GetModuleFileNameW(module, path, MAX_PATH) == 0)
            continue;
        std::wstring fileName = std::filesystem::path(path).filename().wstring();
        std::transform(fileName.begin(), fileName.end(), fileName.begin(), ::towlower);
        if (fileName.find(L"xess") == std::wstring::npos || fileName.starts_with(L"libxess"))
            continue;
        checkedModules.insert(module);
        if (Detail::HookModule(module))
            return;
    }
}

bool XeSSUnreal::Hooked()
{
    return Detail::o_AddVelocityFlatteningXeSSPass != nullptr || Detail::o_FlattenVelocity != nullptr;
}

bool XeSSUnreal::HasCapturedDepth() { return Detail::State().capturedDepth.load(std::memory_order_relaxed); }

ID3D12Resource* XeSSUnreal::SceneDepth(ID3D12GraphicsCommandList* commandList,
                                       const xess_d3d12_execute_params_t* params, D3D12_RESOURCE_STATES& state)
{
    auto& s = Detail::State();
    if (!Config::Instance()->XeSSUnrealDepthMvFix.value_or_default() || params->pColorTexture == nullptr ||
        s.sceneDepthRdg.load() == nullptr)
        return nullptr;

    s.resourceVtable = *(uintptr_t*) params->pColorTexture;
    s.minWidth = params->inputColorBase.x + params->inputWidth;
    s.minHeight = params->inputColorBase.y + params->inputHeight;
    Detail::ProbeSceneDepth();
    state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return Detail::RequestCopy(commandList, params->pColorTexture);
}
