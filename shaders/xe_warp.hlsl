// Validation warp: rebuild a frame by sampling `src` along a flow grid (bench only). dst(x) =
// src(x + flow(x)); with the backward flow (cur -> prev) and src = prev, dst approximates cur.
// The grid is bilinearly interpolated; flow is in L0 pixels, `f` native pixels per L0 pixel.
Texture2D<float4>   src  : register(t0);
Texture2D<float2>   flow : register(t1);
RWTexture2D<float4> dst  : register(u0);
SamplerState        lin  : register(s0);
cbuffer C : register(b0) { uint w, h, gw, gh; float f; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const float2 p = float2(id.xy) + 0.5;
    const float2 v = flow.SampleLevel(lin, p / (f * 8.0 * float2(gw, gh)), 0) * f;
    dst[id.xy] = src.SampleLevel(lin, (p + v) / float2(w, h), 0);
}
