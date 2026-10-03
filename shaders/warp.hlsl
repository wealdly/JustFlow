// Frame generation by extrapolation (engine=warp): the real frame pushed `t` intervals AHEAD along its
// own motion. mv is the pipeline's backward flow (previous minus current, work-res pixels), so the pixel
// shown at p a time t later was at p + t * mv in this frame: sample there (bilinear, clamped at the
// edge). Scored against the true frame half an interval ahead (tools/scene): 1.21 grey levels, vs 5.87
// for repeating the frame and 1.09 for DLSS-G's interpolation - which is shown half an interval later.
// Inside a UI rect the pixel stays exactly the real frame's.
// Static pixels outside the rects (a HUD without the addon, text over a moving world): one flow vector
// spreads over its neighbourhood, so static text inherited the world's motion and was pushed along with
// it. Each pixel also gets the zero-motion hypothesis, scored on the OFA's own gray pair: luma
// disagreement of the two frames at the same place (e0) vs along the flow (ef), over a +-1 gray px
// cross. Where standing still is clearly better it stands still; ties go to the flow, since standing
// still there freezes low-contrast moving texture. (From justflow_xe, where this took static UI from
// 33.0 to 47.1 dB with extrapolation and cost the whole frame 0.18 dB.)
Texture2D<float4>   src   : register(t0);
Texture2D<float2>   mv    : register(t1);
Texture2D<int>      rects : register(t2);
Texture2D<float>    gcur  : register(t3);   // OFA input of this frame (gray, gw x gh)
Texture2D<float>    gprev : register(t4);   // OFA input of the previous frame
RWTexture2D<float4> dst   : register(u0);
SamplerState        samp  : register(s0);
cbuffer C : register(b0) { uint w, h, ww, wh; float t; uint nrects, gw, gh; };

float StaticWeight(float2 uv, float2 duv)   // duv: this pixel's flow in uv units
{
    const float2 px = 1.0 / float2(gw, gh);
    const float2 cross[5] = { float2(0, 0), float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1) };
    float e0 = 0, ef = 0;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        const float2 p = uv + cross[i] * px;
        const float c = gcur.SampleLevel(samp, p, 0);
        e0 += abs(gprev.SampleLevel(samp, p, 0) - c);
        ef += abs(gprev.SampleLevel(samp, p + duv, 0) - c);
    }
    return saturate((0.6 * ef - e0) / (0.4 * ef + 1e-3));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    const float2 duv = mv.SampleLevel(samp, uv, 0) / float2(ww, wh);
    const float2 suv = uv + duv * t;
    float4 c = src.SampleLevel(samp, saturate(suv), 0);
    const float ws = StaticWeight(uv, duv);
    [branch] if (ws > 0.001) c = lerp(c, src.SampleLevel(samp, uv, 0), ws);
    const float2 p = float2(id.xy);
    [loop] for (uint i = 0; i < min(nrects, 64u); ++i)
    {
        const float x0 = rects[uint2(i * 4 + 0, 0)], y0 = rects[uint2(i * 4 + 1, 0)];
        const float x1 = rects[uint2(i * 4 + 2, 0)], y1 = rects[uint2(i * 4 + 3, 0)];
        if (p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1) c = src[id.xy];
    }
    dst[id.xy] = float4(c.rgb, 1);
}
