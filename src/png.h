// WIC PNG load/save, RGBA8 rows of w * 4 bytes.
#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

bool LoadPngRgba(const wchar_t* path, std::vector<uint8_t>& rgba, UINT& w, UINT& h);
bool SavePngRgba(const wchar_t* path, const uint8_t* rgba, UINT w, UINT h);
