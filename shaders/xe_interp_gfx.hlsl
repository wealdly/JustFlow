// xe_interp as a full-screen triangle into a render target: the swapchain's backbuffer. This driver
// refuses DXGI_USAGE_UNORDERED_ACCESS on swapchains (CreateSwapChainForHwnd -> DXGI_ERROR_INVALID_CALL),
// so the compute version could only write a staging texture that OverlayPresent then copied - a full
// frame of extra traffic per present. A render target needs no copy.
#define XE_INTERP_NO_CS
#include "xe_interp.hlsl"

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target { return Interpolate(pos.xy); }
