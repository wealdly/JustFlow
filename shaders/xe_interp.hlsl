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
// Full-resolution check on top of the L0 map. The map is half resolution, so a 1-2 px text stroke is
// averaged with the panel behind it - and behind a semi-transparent panel a moving shadow line made the
// letters it crossed take the world's motion for a few frames ("UI clean, but sporadically smears").
// So per pixel: stand still where the colour is IDENTICAL in both real frames AND the flow's candidate
// disagrees with itself. Equality alone is ambiguous - a thin pole moving faster than its width is absent
// from x in both frames yet passes through it in between, and equality alone erased it (measured -6 dB
// extrapolating) - but there the flow's two samples agree (both see the pole), while for static text
// over a moving world they do not (they see the world under the text).
float Diff(float3 a, float3 b) { const float3 d = abs(a - b); return max(d.x, max(d.y, d.z)) * 255.0; }
float StaticHere(float3 a, float3 b, float3 fa, float3 fb)   // a/b: both frames at x; fa/fb: the flow's samples
{
    const float same = saturate(1.0 - (Diff(a, b) - 1.0) / 2.0);        // <= 1/255: 1, >= 3/255: 0
    const float flow_wrong = saturate((Diff(fa, fb) - 4.0) / 8.0);      // <= 4/255: 0, >= 12/255: 1
    return same * flow_wrong;
}

// Extrapolation keeps the L0 map only: with no frame at the target time the flow cannot be checked
// where it is going, and the full-resolution test fired on moving content (-1.1 dB on the whole frame
// for +0.2 inside the HUD).
float4 Extrapolate(float2 x)
{
    const float ws = StaticWeight(x);
    const float3 here = Color(cur, x).rgb;
    float3 c = here;
    [branch] if (ws < 0.999) c = lerp(Color(cur, x + t * Flow(bwd, x)).rgb, here, ws);
    return float4(c, 1);
}

float4 Blend(float2 x)
{
    const float ws = StaticWeight(x);   // the zero-motion hypothesis vs the flow's (see StaticWeight)
    const float3 a = Color(prev, x).rgb, b = Color(cur, x).rgb;
    const float3 still = (1 - t) * a + t * b;
    float3 c = 0;
    [branch] if (ws >= 0.999) c = debug ? float3(0, 0, 1) : still;
    else
    {
        const float2 f01 = Flow(fwd, x), f10 = Flow(bwd, x);
        const float2 p0 = x + (-(1 - t) * t * f01 + t * t * f10);
        const float2 p1 = x + ((1 - t) * (1 - t) * f01 - t * (1 - t) * f10);
        const float v0 = Visible(fwd, bwd, p0), v1 = Visible(bwd, fwd, p1);
        const float3 c0 = Color(prev, p0).rgb, c1 = Color(cur, p1).rgb;
        // The floor keeps a both-sides-rejected pixel on the time blend; scaling it by the time weights
        // keeps t = 0 and t = 1 exactly the real frames (the presenter shows real frames as t = 1).
        const float w0 = (1 - t) * (v0 + 1e-3), w1 = t * (v1 + 1e-3);
        const float w = max(ws, StaticHere(a, b, c0, c1));
        c = lerp((w0 * c0 + w1 * c1) / (w0 + w1), still, w);
        if (debug) c = float3(1 - v0, 1 - v1, w);   // red: prev side rejected, green: cur side, blue: stands still
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
