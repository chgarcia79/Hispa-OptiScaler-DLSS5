#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <xess_d3d12.h>

// Intel's UE XeSS plugin runs XeSS with display resolution motion vectors and without depth. Its velocity flattening
// is hooked to get the scene depth and render resolution motion vectors.
namespace XeSSUnreal
{
void Hook();
bool Hooked();
bool HasCapturedDepth();
ID3D12Resource* SceneDepth(ID3D12GraphicsCommandList* commandList, const xess_d3d12_execute_params_t* params,
                           D3D12_RESOURCE_STATES& state);
} // namespace XeSSUnreal
