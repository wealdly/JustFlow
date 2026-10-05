#include "png.h"
#include "log.h"
#include <wincodec.h>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#define REL(x) if (x) { (x)->Release(); (x) = nullptr; }

static IWICImagingFactory* Wic()
{
    static IWICImagingFactory* f = nullptr;
    if (!f)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // same apartment kind as winrt::init_apartment
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) { Log("[png] WIC factory failed"); f = nullptr; }
    }
    return f;
}

bool LoadPngRgba(const wchar_t* path, std::vector<uint8_t>& rgba, UINT& w, UINT& h)
{
    IWICImagingFactory* f = Wic(); if (!f) return false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* frame = nullptr; IWICBitmapSource* src = nullptr;
    bool ok = false;
    if (FAILED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) { Log("[png] cannot open %ls", path); goto done; }
    if (FAILED(dec->GetFrame(0, &frame))) goto done;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &src))) goto done;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h) goto done;
    rgba.resize((size_t)w * h * 4);
    ok = SUCCEEDED(src->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
done:
    REL(src); REL(frame); REL(dec);
    return ok;
}

bool SavePngRgba(const wchar_t* path, const uint8_t* rgba, UINT w, UINT h)
{
    IWICImagingFactory* f = Wic(); if (!f) return false;
    IWICStream* stream = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* frame = nullptr;
    std::vector<uint8_t> bgra;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    bool ok = false;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) { Log("[png] cannot write %ls", path); goto done; }
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) goto done;
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt))) goto done;
    if (fmt == GUID_WICPixelFormat32bppBGRA)   // the encoder picked BGRA: swap in a copy
    {
        bgra.assign(rgba, rgba + (size_t)w * h * 4);
        for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
        rgba = bgra.data();
    }
    else if (fmt != GUID_WICPixelFormat32bppRGBA) { Log("[png] encoder refused RGBA"); goto done; }
    if (FAILED(frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)rgba))) goto done;
    ok = SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
done:
    REL(frame); REL(enc); REL(stream);
    return ok;
}
