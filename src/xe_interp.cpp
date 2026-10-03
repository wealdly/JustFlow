#include "xe_interp.h"
#include "cs_xe_interp.h"
#include "vs_xe_interp_gfx.h"
#include "ps_xe_interp_gfx.h"
#include "log.h"

static const D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

bool XeInterpInit(Gpu& g, XeInterp& x)
{
    if (!GpuMakeCompute(g, g_cs_xe_interp, sizeof g_cs_xe_interp, 4, 1, 9, x.pso, L"xe_interp")) return false;
    // Graphics: b0 = 9 root constants, t0..t3 one table, s0 linear clamp - the compute layout minus the UAV.
    D3D12_DESCRIPTOR_RANGE range = {}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants.Num32BitValues = 9;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 2; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    { Log("[xeinterp] root signature: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
    const HRESULT hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&x.gfx_root);
    blob->Release();
    if (FAILED(hr)) { Log("[xeinterp] CreateRootSignature 0x%08X", hr); return false; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = x.gfx_root;
    pd.VS = { g_vs_xe_interp_gfx, sizeof g_vs_xe_interp_gfx }; pd.PS = { g_ps_xe_interp_gfx, sizeof g_ps_xe_interp_gfx };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    if (FAILED(g.dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&x.gfx_pso))) { Log("[xeinterp] graphics PSO failed"); return false; }
    x.gfx_pso->SetName(L"xe_interp_gfx");
    // One RTV slot, rewritten per draw: OMSetRenderTargets copies the descriptor into the list.
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    return SUCCEEDED(g.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&x.rtv_heap));
}

void XeInterpRelease(XeInterp& x)
{
    if (x.pso.pso) { x.pso.pso->Release(); x.pso.pso = nullptr; }
    if (x.pso.root) { x.pso.root->Release(); x.pso.root = nullptr; }
    if (x.gfx_pso) { x.gfx_pso->Release(); x.gfx_pso = nullptr; }
    if (x.gfx_root) { x.gfx_root->Release(); x.gfx_root = nullptr; }
    if (x.rtv_heap) { x.rtv_heap->Release(); x.rtv_heap = nullptr; }
}

void XeInterpRecordRT(Gpu& g, ID3D12GraphicsCommandList* cl, const XeInterp& x,
                      ID3D12Resource* prev, ID3D12Resource* cur, ID3D12Resource* fwd, ID3D12Resource* bwd,
                      UINT gw, UINT gh, UINT factor, ID3D12Resource* rt, D3D12_RESOURCE_STATES rt_rest, UINT w, UINT h, float t)
{
    struct { UINT w, h, gw, gh; float f, t, tol, tol_rel; UINT debug; } c = { w, h, gw, gh, (float)factor, t, x.tol, x.tol_rel, 0u };
    const GpuView srv[4] = { { prev, DXGI_FORMAT_UNKNOWN }, { cur, DXGI_FORMAT_UNKNOWN }, { fwd, DXGI_FORMAT_UNKNOWN }, { bwd, DXGI_FORMAT_UNKNOWN } };
    const D3D12_GPU_DESCRIPTOR_HANDLE table = GpuSrvTable(g, cl, srv, 4);   // sets the heap
    if (!table.ptr) return;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = x.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.dev->CreateRenderTargetView(rt, nullptr, rtv);
    GpuBarrier(cl, rt, rt_rest, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->SetGraphicsRootSignature(x.gfx_root);
    cl->SetPipelineState(x.gfx_pso);
    cl->SetGraphicsRoot32BitConstants(0, 9, &c, 0);
    cl->SetGraphicsRootDescriptorTable(1, table);
    const D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 }; const D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sc);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
    GpuBarrier(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, rt_rest);
}

void XeInterpRecord(Gpu& g, ID3D12GraphicsCommandList* cl, const XeInterp& x,
                    ID3D12Resource* prev, ID3D12Resource* cur, ID3D12Resource* fwd, ID3D12Resource* bwd,
                    UINT gw, UINT gh, UINT factor, ID3D12Resource* dst, UINT w, UINT h, float t, bool debug, D3D12_RESOURCE_STATES dst_rest)
{
    struct { UINT w, h, gw, gh; float f, t, tol, tol_rel; UINT debug; } c = { w, h, gw, gh, (float)factor, t, x.tol, x.tol_rel, debug ? 1u : 0u };
    const GpuView srv[4] = { { prev, DXGI_FORMAT_UNKNOWN }, { cur, DXGI_FORMAT_UNKNOWN }, { fwd, DXGI_FORMAT_UNKNOWN }, { bwd, DXGI_FORMAT_UNKNOWN } };
    const GpuView out = { dst, DXGI_FORMAT_UNKNOWN };
    GpuBarrier(cl, dst, dst_rest, UAV);
    GpuDispatch(g, cl, x.pso, srv, &out, &c, GpuGroups(w, 8), GpuGroups(h, 8));
    GpuBarrier(cl, dst, UAV, dst_rest);
}
