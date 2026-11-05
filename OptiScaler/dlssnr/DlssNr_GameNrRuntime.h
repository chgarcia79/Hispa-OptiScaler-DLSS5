#pragma once

#include <SysUtils.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

// Tracks and executes a game's native NR features (NGX feature 18), both when created natively by the driver (RTX 5090)
// and when created through the direct runtime (nvngx.dll_dlssnr.dll -> nvngx_dlssnr.dll).
namespace DlssNr::GameNr
{
bool HasCandidates();
NVSDK_NGX_Result CreateDirect(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Parameter* params,
                              NVSDK_NGX_Handle** handle);
void RecordDriverCreate(const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params);
bool Owns(const NVSDK_NGX_Handle* handle);
bool ForgetDriverHandle(const NVSDK_NGX_Handle* handle);
NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Handle* handle,
                          NVSDK_NGX_Parameter* params);
NVSDK_NGX_Result Release(NVSDK_NGX_Handle* handle);
void Preload(ID3D12Device* device);
void PreloadRuntimeIfEnabled(ID3D12Device* device);
bool IsGameNativeNrRunning();
inline bool Active() { return IsGameNativeNrRunning(); }
void Shutdown();
} // namespace DlssNr::GameNr
