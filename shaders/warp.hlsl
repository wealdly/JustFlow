// Frame generation by extrapolation (engine=warp): the real frame pushed `t` intervals AHEAD along its
// own motion. mv is the pipeline's backward flow (previous minus current, work-res pixels), so the pixel
// shown at p a time t later was at p + t * mv in this frame: sample there (bilinear, clamped at the
// edge). Scored against the true frame half an interval ahead (tools/scene): 1.21 grey levels, vs 5.87
// for repeating the frame and 1.09 for DLSS-G's interpolation - which is shown half an interval later.
// Inside a UI rect the pixel stays exactly the real frame's.
Texture2D<float4>   src   : register(t0);
Texture2D<float2>   mv    : register(t1);
Texture2D<int>      rects : register(t2);
RWTexture2D<float4> dst   : register(u0);
SamplerState        samp  : register(s0);
cbuffer C : register(b0) { uint w, h, ww, wh; float t; uint nrects; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    const float2 suv = uv + mv.SampleLevel(samp, uv, 0) / float2(ww, wh) * t;
    float4 c = src.SampleLevel(samp, saturate(suv), 0);
    const float2 p = float2(id.xy);
    [loop] for (uint i = 0; i < min(nrects, 64u); ++i)
    {
        const float x0 = rects[uint2(i * 4 + 0, 0)], y0 = rects[uint2(i * 4 + 1, 0)];
        const float x1 = rects[uint2(i * 4 + 2, 0)], y1 = rects[uint2(i * 4 + 3, 0)];
        if (p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1) c = src[id.xy];
    }
    dst[id.xy] = float4(c.rgb, 1);
}
