// Model residual at work resolution: dst = nr_out - nr_in (signed, sRGB bytes as 0..1 floats; the
// same difference CsCompose adds inline). Stored RGBA16F so a later frame can warp + add it.
// Alpha carries the luma of the frame the model SAW: the later compose warps it along with the edit
// and compares it with the frame it is composing onto (compose_residual.hlsl, `reject`).
Texture2D<float4>   nr_in  : register(t0);
Texture2D<float4>   nr_out : register(t1);
RWTexture2D<float4> dst    : register(u0);
cbuffer C : register(b0) { uint ww, wh; };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= ww || id.y >= wh) return;
    const float3 a = nr_in[id.xy].rgb;
    dst[id.xy] = float4(nr_out[id.xy].rgb - a, dot(a, float3(0.2126, 0.7152, 0.0722)));
}
