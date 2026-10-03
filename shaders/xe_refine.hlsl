// Same-level propagation for xe_flow: each block tries its own vector and its 8 neighbours' (as found
// on this level), keeps the one that matches best, then a +-1 px search around it and (subpixel != 0)
// the parabolic fit. The coarse-to-fine search can only move a block +-R around what its PARENT
// predicted; where the parent level was wrong across several blocks (a thin pole or an edge lost in
// the downscale), every predictor is wrong and the median cannot help. A correct neighbour on the
// same level can - run once per level, a mistake is corrected while it is still a block or two wide.
#define B 8
#define NC 9

Texture2D<float>    cur  : register(t0);
Texture2D<float>    ref  : register(t1);
Texture2D<float2>   src  : register(t2);   // this level's flow grid
RWTexture2D<float2> flow : register(u0);
cbuffer C : register(b0) { uint lw, lh, gw, gh, subpixel; };

groupshared float s_cur[B * B];
groupshared int2  s_cand[NC];
groupshared float s_diff[NC * B * B];   // |cur - ref| per candidate and pixel
groupshared float s_part[NC * B];       // per candidate and row
groupshared float s_cost[NC];
groupshared int2  s_best;

// SAD of every candidate at once: each thread takes its own pixel for all NC candidates, then rows,
// then candidates are summed - the whole group works instead of NC threads looping 64 loads each.
void Costs(int2 org, uint ti, int2 base)
{
    const int2 hi = int2(lw - 1, lh - 1), px = org + int2(ti % B, ti / B);
    [unroll] for (uint c = 0; c < NC; ++c)
        s_diff[c * B * B + ti] = abs(s_cur[ti] - ref.Load(int3(clamp(px + base + s_cand[c], int2(0, 0), hi), 0)));
    GroupMemoryBarrierWithGroupSync();
    for (uint t = ti; t < NC * B; t += B * B)
    {
        float r = 0;
        [unroll] for (uint x = 0; x < B; ++x) r += s_diff[t * B + x];
        s_part[t] = r;
    }
    GroupMemoryBarrierWithGroupSync();
    if (ti < NC)
    {
        float c = 0;
        [unroll] for (uint y = 0; y < B; ++y) c += s_part[ti * B + y];
        s_cost[ti] = c;
    }
    GroupMemoryBarrierWithGroupSync();
}

[numthreads(B, B, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint ti : SV_GroupIndex)
{
    const int2 org = int2(gid.xy) * B;
    s_cur[ti] = cur.Load(int3(min(org + int2(tid.xy), int2(lw - 1, lh - 1)), 0));
    if (ti < NC)
    {
        // own vector first: ties keep it
        const int2 off[NC] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1), int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
        s_cand[ti] = int2(round(src[clamp(int2(gid.xy) + off[ti], int2(0, 0), int2(gw - 1, gh - 1))]));
    }
    GroupMemoryBarrierWithGroupSync();
    Costs(org, ti, int2(0, 0));
    if (ti == 0)
    {
        uint best = 0;
        [unroll] for (uint i = 1; i < NC; ++i) if (s_cost[i] < s_cost[best]) best = i;
        s_best = s_cand[best];
    }
    GroupMemoryBarrierWithGroupSync();
    if (ti < NC) s_cand[ti] = int2(ti % 3 - 1, ti / 3 - 1);   // now offsets around s_best
    GroupMemoryBarrierWithGroupSync();
    Costs(org, ti, s_best);
    if (ti == 0)
    {
        uint best = 4;   // the centre wins ties
        [unroll] for (uint i = 0; i < NC; ++i) if (s_cost[i] < s_cost[best]) best = i;
        const int bx = best % 3, by = best / 3;
        float2 sub = 0;
        if (subpixel)
        {
            if (bx == 1) { const float l = s_cost[best - 1], m = s_cost[best], r = s_cost[best + 1], d = l - 2 * m + r; if (d > 1e-6) sub.x = clamp(0.5 * (l - r) / d, -0.5, 0.5); }
            if (by == 1) { const float u = s_cost[best - 3], m = s_cost[best], dn = s_cost[best + 3], d = u - 2 * m + dn; if (d > 1e-6) sub.y = clamp(0.5 * (u - dn) / d, -0.5, 0.5); }
        }
        flow[gid.xy] = float2(s_best + int2(bx, by) - 1) + sub;
    }
}
