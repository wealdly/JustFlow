// RGBA8 (w x h) -> RGBA8 (dw x dh): exact area box filter over the fractional source footprint
// (partial pixels weighted by coverage). NeuralScreen quality_shaders.h kScaleHlsl4 area branch.
//
// The three extra parameters shape WHAT the model sees, not the picture: the result is both the
// model's input and the baseline its edit is measured against (compose takes nr_out - nr_in), so
// nothing done here reaches the screen except through the model's changed response. They exist
// because the model is scale-sensitive and was trained on native renders, which are not
// box-filtered supersamples:
//   sharp     unsharp against a 2x wider footprint - restores local contrast the box filter removes
//   point_mix blend toward the nearest source sample - the aliased look of a native render
//   linear    average in linear light instead of on sRGB-encoded values
Texture2D<float4>   src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { uint w, h, dw, dh; float sharp, point_mix; uint linear_light; };

float3 Dec(float3 c) { return linear_light ? pow(max(c, 0), 2.2) : c; }
float3 Enc(float3 c) { return linear_light ? pow(max(c, 0), 1.0 / 2.2) : c; }

float3 Box(float2 lo, float2 end, int2 hi)
{
    float3 sum = 0;
    [loop] for (int y = int(floor(lo.y)); y < int(ceil(end.y)); ++y)
    {
        const float wy = max(0, min(end.y, float(y + 1)) - max(lo.y, float(y)));
        [loop] for (int x = int(floor(lo.x)); x < int(ceil(end.x)); ++x)
        {
            const float wx = max(0, min(end.x, float(x + 1)) - max(lo.x, float(x)));
            sum += Dec(src[clamp(int2(x, y), int2(0, 0), hi)].rgb) * wx * wy;
        }
    }
    return sum / ((end.x - lo.x) * (end.y - lo.y));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dw || id.y >= dh) return;
    const int2 hi = int2(w - 1, h - 1);
    const float2 scale = float2(w, h) / float2(dw, dh);
    const float2 lo = float2(id.xy) * scale, end = float2(id.xy + 1) * scale;
    float3 res = Box(lo, end, hi);
    if (sharp != 0)
    {
        const float2 half = (end - lo) * 0.5;
        const float3 wide = Box(lo - half, end + half, hi);
        res += sharp * (res - wide);
    }
    if (point_mix != 0)
    {
        const int2 c = clamp(int2((lo + end) * 0.5), int2(0, 0), hi);
        res = lerp(res, Dec(src[c].rgb), point_mix);
    }
    dst[id.xy] = float4(saturate(Enc(res)), 1);
}
