#pragma once

#include <windows.h>
#include <wincodec.h>
#include <vector>
#include <cstdint>

#pragma comment(lib, "windowscodecs.lib")

// A decoded PSD/PSB image: 32-bit BGRA, top-down, straight (non-premultiplied) alpha.
struct PsdImage {
	int width = 0;        // decoded width (may be downsampled)
	int height = 0;       // decoded height
	int fullWidth = 0;    // width of the document as stored
	int fullHeight = 0;   // height of the document as stored
	bool hasAlpha = false;
	bool fromEmbedded = false; // true when taken from the embedded preview resource
	int depth = 0;             // bits per channel as stored
	int colorMode = -1;        // PSD colour mode id
	std::vector<uint32_t> pixels;

	bool valid() const {
		return width > 0 && height > 0 &&
			pixels.size() >= static_cast<size_t>(width) * static_cast<size_t>(height);
	}
};

// Reads header/metadata only (cheap). Returns false if the stream is not a PSD/PSB.
bool ReadPsdSize(IStream* stream, int* width, int* height);

// Decodes a PSD/PSB to at most `maxEdge` pixels on its longest side.
// Pass maxEdge == 0 for full resolution. Picks the cheapest path that can satisfy
// the requested size: the embedded preview resource when it is large enough,
// otherwise the composite image decoded with row-skipping.
bool DecodePsdImage(IStream* stream, UINT maxEdge, PsdImage& out);

// Scales a decoded image to exactly w x h (WIC Fant, nearest-neighbour fallback).
bool ScalePsdImage(const PsdImage& src, int w, int h, std::vector<uint32_t>& dst);

// Scales (WIC Fant) to fit `maxEdge` and returns a top-down 32bpp DIB section.
// maxEdge == 0 keeps the image at its decoded size. Caller owns the HBITMAP.
HBITMAP CreateDibFromImage(const PsdImage& img, UINT maxEdge);
