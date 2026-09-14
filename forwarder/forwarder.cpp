// nvngx.dll_nrfilter.dll - the caller-gate forwarder.
//
// nvngx_dlssnr.dll identifies its caller by the return address and refuses the call with
// FAIL_PlatformError (0xBAD00002) unless the path of the CALLING module contains the substring
// "nvngx.dll". This DLL's file name carries the substring, and every wrapper below keeps a real
// stack frame in this module (see FORWARD) so the return address the runtime inspects is ours.
//
// Lifted from NeuralScreen's ns_forwarder.cpp (MIT, perseval-BLR 2026); exports renamed.
#include <windows.h>
#include <d3d12.h>
#include "nvsdk_ngx.h"

using PFN_NR_InitExt  = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using PFN_NR_Create   = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_NR_Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using PFN_NR_Release  = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

static HMODULE g_nr;
static PFN_NR_InitExt g_init_ext;
static PFN_NR_Create g_create;
static PFN_NR_Evaluate g_evaluate;
static PFN_NR_Release g_release;

// With /O2 a body of the shape `return fn(args);` compiles to `jmp rax`: no frame is pushed here and
// the return address still belongs to the caller. Touching a volatile after the call keeps a real
// frame in this module.
static volatile LONG g_last;
#define FORWARD(expr) do { const NVSDK_NGX_Result r_ = (expr); InterlockedExchange(&g_last, (LONG)r_); return r_; } while (0)

extern "C" __declspec(dllexport) int NrfFwdLoad(const wchar_t* dlssnr_path)
{
    if (g_nr != nullptr) return 0;
    g_nr = LoadLibraryExW(dlssnr_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_nr == nullptr) { const DWORD e = GetLastError(); return e != 0 ? (int)e : -2; }
    g_init_ext = (PFN_NR_InitExt)GetProcAddress(g_nr, "NVSDK_NGX_D3D12_Init_Ext");
    g_create   = (PFN_NR_Create)GetProcAddress(g_nr, "NVSDK_NGX_D3D12_CreateFeature");
    g_evaluate = (PFN_NR_Evaluate)GetProcAddress(g_nr, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_release  = (PFN_NR_Release)GetProcAddress(g_nr, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!g_init_ext || !g_create || !g_evaluate || !g_release) return -1;
    return 0;
}

extern "C" __declspec(dllexport) void NrfFwdPath(wchar_t* out, unsigned int cch)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&NrfFwdPath, &self);
    if (out && cch) { out[0] = 0; GetModuleFileNameW(self, out, cch); }
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NrfFwdInitExt(unsigned long long app_id, const wchar_t* data_path, ID3D12Device* dev, NVSDK_NGX_Version version, const NVSDK_NGX_Parameter* params)
{
    if (!g_init_ext) return NVSDK_NGX_Result_FAIL_NotInitialized;
    FORWARD(g_init_ext(app_id, data_path, dev, version, params));
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NrfFwdCreate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out)
{
    if (!g_create) return NVSDK_NGX_Result_FAIL_NotInitialized;
    FORWARD(g_create(list, feature, params, out));
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NrfFwdEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback cb)
{
    if (!g_evaluate) return NVSDK_NGX_Result_FAIL_NotInitialized;
    FORWARD(g_evaluate(list, feature, params, cb));
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NrfFwdRelease(NVSDK_NGX_Handle* feature)
{
    if (!g_release) return NVSDK_NGX_Result_FAIL_NotInitialized;
    FORWARD(g_release(feature));
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
