// Sub-block re-selection (FidelityFX optical flow's step when it doubles resolution): after the 8x8-block
// flow at L0, every 4x4 sub-block re-picks among its own block's vector and the 8 neighbours' by its own
// SAD, over a 6x6 window (the sub-block plus a 1 px rim). A sub-block straddling an object edge then takes
// the vector of the side it actually belongs to, instead of the whole 8x8 block's - the halo/fringe at
// moving edges. Vectors are kept as they are (sub-pixel included); the ref is sampled bilinearly so the
// comparison sees the sub-pixel part too. Ties keep the own block's vector (first candidate).
Texture2D<float>    cur  : register(t0);   // the frame the blocks belong to (L0 luma)
Texture2D<float>    ref  : register(t1);   // the frame the vectors point into
Texture2D<float2>   grid : register(t2);   // 8x8-block flow at L0, L0 pixels
RWTexture2D<float2> sub  : register(u0);   // 4x4-block flow, L0 pixels
SamplerState        lin  : register(s0);
cbuffer C : register(b0) { uint lw, lh, gw, gh, sw, sh; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= sw || id.y >= sh) return;
    const int2 own = int2(id.xy >> 1);   // the 8x8 block this 4x4 sub-block is in
    const int2 off[9] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1), int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
    const float2 size = float2(lw, lh);
    const int2 org = int2(id.xy) * 4 - 1;   // 6x6 window: the sub-block and a 1 px rim
    float cpx[36];
    [unroll] for (int i = 0; i < 36; ++i) cpx[i] = cur[clamp(org + int2(i % 6, i / 6), int2(0, 0), int2(lw - 1, lh - 1))];
    float best_cost = 1e30; float2 best = 0;
    [loop] for (int c = 0; c < 9; ++c)
    {
        const float2 v = grid[clamp(own + off[c], int2(0, 0), int2(gw - 1, gh - 1))];
        float cost = 0;
        [unroll] for (int i = 0; i < 36; ++i)
            cost += abs(cpx[i] - ref.SampleLevel(lin, (float2(org + int2(i % 6, i / 6)) + 0.5 + v) / size, 0));
        if (cost < best_cost) { best_cost = cost; best = v; }
    }
    sub[id.xy] = best;
}
