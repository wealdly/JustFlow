// Frame interpolation at time t in (0, 1) between prev (t = 0) and cur (t = 1), from the two flow
// grids of xe_flow: fwd (prev -> cur, on prev's pixels) and bwd (cur -> prev, on cur's pixels).
//
// 1. Flow from the intermediate frame to each real one, from the flows at the same pixel (Jiang et
//    al., Super SloMo, eq. 4 - exact for linear motion where both flows see the same surface):
//        Ft0 = -(1-t) t F01 + t^2 F10        Ft1 = (1-t)^2 F01 - t (1-t) F10
// 2. Sample prev at x + Ft0 and cur at x + Ft1.
// 3. Visibility per side from forward-backward consistency AT THE SAMPLED POSITION: a pixel of prev
//    whose forward vector does not come back through cur's backward vector is covered in cur (or the
//    flow is wrong there), so it should not be shown. Tolerance grows with the motion (block flow is
//    coarse on fast content). A side that fails loses its weight; the time weights ((1-t), t) blend
//    the rest.
// Flow grids are in L0 pixels, one vector per cell x cell L0 block (4 after xe_subsel, else 8): `f` native px per L0 px.
Texture2D<float4>   prev : register(t0);
Texture2D<float4>   cur  : register(t1);
Texture2D<float2>   fwd  : register(t2);
Texture2D<float2>   bwd  : register(t3);
Texture2D<float2>   stat : register(t4);   // xe_static: (e0, ef) at L0
RWTexture2D<float4> dst  : register(u0);
SamplerState        lin  : register(s0);
cbuffer C : register(b0) { uint w, h, gw, gh; float f, t, tol, tol_rel; uint debug, extrap; float cell; };   // cell: L0 px per grid vector


float2 Flow(Texture2D<float2> g, float2 p) { return g.SampleLevel(lin, p / (f * cell * float2(gw, gh)), 0) * f; }
float4 Color(Texture2D<float4> c, float2 p) { return c.SampleLevel(lin, p / float2(w, h), 0); }

float Visible(Texture2D<float2> there, Texture2D<float2> back, float2 p)
{
    const float2 v = Flow(there, p);
    const float e = length(v + Flow(back, p + v)), k = tol + tol_rel * length(v);
    return exp(-(e * e) / (k * k));
}

// ---- static-pixel test: the UI fix ----------------------------------------------------------------------
// One flow vector covers a 16x16 block and is bilinearly spread further, so static HUD text over a moving
// world inherits the world's motion: each real frame's text is sampled from a different wrong place and
// the two are blended - doubled, ghosted words (--ui bench: 35.3 dB inside the HUD vs 44.3 for just
// repeating the frame). So every pixel also gets the zero-motion hypothesis: xe_static scored both on the
// pair, per real frame at L0 - (e0, ef) = luma disagreement standing still / following the flow - and
// where standing still is clearly better, the pixel stands still. Static UI has e0 ~ 0 by construction.
// Ties (flat areas, where both are ~0) go to standing still when interpolating (it blends the two frames
// at one place: invisible, and measured +0.3 dB) but to the flow when extrapolating (standing still
// there freezes low-contrast moving texture: measured -2.5 dB).
float StaticWeight(float2 x)
{
    uint sw, sh; stat.GetDimensions(sw, sh);
    const float2 e = stat.SampleLevel(lin, x / (f * float2(sw, sh)), 0);
    return extrap ? saturate((0.6 * e.y - e.x) / (0.4 * e.y + 1e-3))
                  : saturate((1.0 - e.x / (e.y + 0.02)) / 0.4);
}

// Extrapolation (extrap = 1): cur pushed t frame intervals AHEAD along its own motion. bwd is previous
// minus current, so the pixel shown at x a time t later was at x + t * bwd(x) in cur - sampled there,
// clamped at the edge. Nothing to blend, so no visibility test: a disocclusion is filled by stretching
// what was beside it (justflow's engine=warp, which measured 1.21 grey levels against the true future
// frame vs 5.87 for repeating it and 1.09 for interpolation - which shows the frame a whole interval later).
// Both paths branch on the static map first: a pixel that clearly stands still skips the flow work, one
// that clearly moves skips the zero-motion samples, and only the boundary between them pays for both
// (without this the static test added two full-resolution samples to every pixel: 0.22 -> 0.32 ms).
float4 Extrapolate(float2 x)
{
    const float ws = StaticWeight(x);
    float3 c;
    [branch] if (ws >= 0.999) c = Color(cur, x).rgb;
    else
    {
        c = Color(cur, x + t * Flow(bwd, x)).rgb;
        [branch] if (ws > 0.001) c = lerp(c, Color(cur, x).rgb, ws);
    }
    return float4(c, 1);
}

float4 Blend(float2 x)
{
    const float ws = StaticWeight(x);   // the zero-motion hypothesis vs the flow's (see StaticWeight)
    float3 still = 0, c = 0;
    [branch] if (ws > 0.001) still = (1 - t) * Color(prev, x).rgb + t * Color(cur, x).rgb;
    [branch] if (ws >= 0.999) c = debug ? float3(0, 0, 1) : still;
    else
    {
        const float2 f01 = Flow(fwd, x), f10 = Flow(bwd, x);
        const float2 p0 = x + (-(1 - t) * t * f01 + t * t * f10);
        const float2 p1 = x + ((1 - t) * (1 - t) * f01 - t * (1 - t) * f10);
        const float v0 = Visible(fwd, bwd, p0), v1 = Visible(bwd, fwd, p1);
        // The floor keeps a both-sides-rejected pixel on the time blend; scaling it by the time weights
        // keeps t = 0 and t = 1 exactly the real frames (the presenter shows real frames as t = 1).
        const float w0 = (1 - t) * (v0 + 1e-3), w1 = t * (v1 + 1e-3);
        c = lerp((w0 * Color(prev, p0).rgb + w1 * Color(cur, p1).rgb) / (w0 + w1), still, ws);
        if (debug) c = float3(1 - v0, 1 - v1, ws);   // red: prev side rejected, green: cur side, blue: stands still
    }
    return float4(c, 1);
}

// The frame at time t at pixel centre x (also the body of xe_interp_gfx's pixel shader).
float4 Interpolate(float2 x)
{
    float4 r;   // [branch]: ?: would evaluate both sides for every pixel
    [branch] if (extrap) r = Extrapolate(x); else r = Blend(x);
    return r;
}

#ifndef XE_INTERP_NO_CS
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    dst[id.xy] = Interpolate(float2(id.xy) + 0.5);
}
#endif
