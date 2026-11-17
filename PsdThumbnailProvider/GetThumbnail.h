#pragma once

#include <windows.h>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

// Decodes a PSD/PSB and returns a top-down 32bpp DIB fitting within cx pixels.
// Caller owns the returned HBITMAP.
HBITMAP GetPSDThumbnail(IStream* stream, UINT cx = 256);
