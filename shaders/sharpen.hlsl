// Robust contrast-adaptive sharpening - AMD FidelityFX FSR1 RCAS (ffx_fsr1.h v1.20210629, Copyright (c)
// 2021 Advanced Micro Devices, MIT; see NOTICE), ported to plain HLSL with its noise detection on
// (FSR_RCAS_DENOISE): a 5-tap cross whose negative lobe is solved per pixel as the most it can be
// before the result would clip, limited to 0.25 - 1/16 and scaled by `strength` (0 = off, 1 = AMD's
// maximum). It replaced our CAS: the clip solve keeps edges from haloing, and the noise term keeps it
// off grain and compression noise in video. Pixels inside a UI rect pass through.
// This is the filter layer's last pass, and the order inside the layer is deliberate: deband first
// (deband.hlsl - sharpening would harden the bands), sharpen the image the neural layer produced, THEN
// saturation. Grading before sharpening would have the sharpener amplify colour it had just pushed.
Texture2D<float4>   src   : register(t0);
Texture2D<int>      rects : register(t1);
RWTexture2D<float4> dst   : register(u0);
cbuffer C : register(b0) { uint w, h; float strength, saturation; uint nrects; };

float3 Px(int2 p) { return src[clamp(p, int2(0, 0), int2(w - 1, h - 1))].rgb; }
float Max3(float a, float b, float c) { return max(a, max(b, c)); }
float Min3(float a, float b, float c) { return min(a, min(b, c)); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const int2 p = int2(id.xy);
    const float3 e = src[p].rgb;
    [loop] for (uint r = 0; r < min(nrects, 64u); ++r)
        if (p.x >= rects[uint2(r * 4 + 0, 0)] && p.y >= rects[uint2(r * 4 + 1, 0)] &&
            p.x <= rects[uint2(r * 4 + 2, 0)] && p.y <= rects[uint2(r * 4 + 3, 0)])
        { dst[id.xy] = float4(e, 1); return; }
    //    b
    //  d e f
    //    h
    const float3 b = Px(p + int2(0, -1)), d = Px(p + int2(-1, 0)), f = Px(p + int2(1, 0)), hh = Px(p + int2(0, 1));
    float3 res = e;
    if (strength > 0)
    {
        // luma x2, for the noise detection
        const float bL = b.b * 0.5 + (b.r * 0.5 + b.g), dL = d.b * 0.5 + (d.r * 0.5 + d.g), eL = e.b * 0.5 + (e.r * 0.5 + e.g);
        const float fL = f.b * 0.5 + (f.r * 0.5 + f.g), hL = hh.b * 0.5 + (hh.r * 0.5 + hh.g);
        float nz = 0.25 * (bL + dL + fL + hL) - eL;
        nz = saturate(abs(nz) / max(Max3(Max3(bL, dL, eL), fL, hL) - Min3(Min3(bL, dL, eL), fL, hL), 1e-5));
        nz = -0.5 * nz + 1.0;
        // min and max of the ring, then the lobe that keeps every channel inside 0..1
        const float3 mn4 = min(min(b, d), min(f, hh)), mx4 = max(max(b, d), max(f, hh));
        const float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
        const float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
        const float3 lobe3 = max(-hitMin, hitMax);
        float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * strength;
        lobe *= nz;
        res = saturate((lobe * (b + d + f + hh) + e) / (4.0 * lobe + 1.0));
    }
    const float l = dot(res, float3(0.2126, 0.7152, 0.0722));
    res = saturate(lerp(float3(l, l, l), res, saturation));
    dst[id.xy] = float4(res, 1);
}
