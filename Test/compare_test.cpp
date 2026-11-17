// Regression harness: runs the original decoder and the new one over the same
// files and reports where they disagree.

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

#include "../PsdThumbnailProvider/PsdDecoder.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "windowscodecs.lib")

HBITMAP OrigGetPSDThumbnail(IStream* stream);

struct Raster {
	int w = 0, h = 0;
	std::vector<uint32_t> px;
};

// Writes a raster out at an integer zoom so small images stay inspectable.
static bool SaveRaster(const Raster& r, const wchar_t* path, int zoom) {
	if (r.w <= 0 || r.h <= 0) return false;
	const int w = r.w * zoom, h = r.h * zoom;
	std::vector<uint32_t> big((size_t)w * h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			big[(size_t)y * w + x] = r.px[(size_t)(y / zoom) * r.w + (x / zoom)];

	IWICImagingFactory* f = NULL;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&f)))) return false;

	IWICStream* st = NULL; IWICBitmapEncoder* en = NULL; IWICBitmapFrameEncode* fr = NULL;
	bool ok = false;
	if (SUCCEEDED(f->CreateStream(&st)) &&
		SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
		SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, NULL, &en)) &&
		SUCCEEDED(en->Initialize(st, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(en->CreateNewFrame(&fr, NULL)) &&
		SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->SetSize(w, h))) {
		WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
		if (SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
			SUCCEEDED(fr->WritePixels(h, w * 4, (UINT)(big.size() * 4), (BYTE*)big.data())) &&
			SUCCEEDED(fr->Commit()) && SUCCEEDED(en->Commit())) ok = true;
	}
	if (fr) fr->Release();
	if (en) en->Release();
	if (st) st->Release();
	f->Release();
	return ok;
}

static bool BitmapToRaster(HBITMAP bmp, Raster& out) {
	if (!bmp) return false;
	BITMAP bm = {};
	if (!GetObject(bmp, sizeof(bm), &bm)) return false;

	out.w = bm.bmWidth;
	out.h = bm.bmHeight;
	out.px.resize((size_t)out.w * out.h);

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = out.w;
	bi.bmiHeader.biHeight = -out.h; // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	HDC dc = CreateCompatibleDC(NULL);
	int got = GetDIBits(dc, bmp, 0, out.h, out.px.data(), &bi, DIB_RGB_COLORS);
	DeleteDC(dc);
	return got != 0;
}

int wmain(int argc, wchar_t** argv) {
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	std::vector<std::wstring> files;
	if (argc > 1) {
		for (int i = 1; i < argc; i++) files.push_back(argv[i]);
	} else {
		WIN32_FIND_DATAW fd;
		const wchar_t* pats[] = { L"*.psd", L"*.psb" };
		for (auto pat : pats) {
			HANDLE h = FindFirstFileW(pat, &fd);
			if (h == INVALID_HANDLE_VALUE) continue;
			do { files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
			FindClose(h);
		}
	}

	printf("%-13s %10s %10s %8s %8s  %s\n",
		"file", "original", "new", "meanDiff", "maxDiff", "verdict");
	printf("---------------------------------------------------------------------\n");

	int mismatches = 0;

	for (const auto& f : files) {
		IStream* s1 = NULL;
		SHCreateStreamOnFileEx(f.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
			FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &s1);
		HBITMAP ob = s1 ? OrigGetPSDThumbnail(s1) : NULL;
		Raster orig;
		bool origOk = BitmapToRaster(ob, orig);
		if (ob) DeleteObject(ob);
		if (s1) s1->Release();

		// Ask the new decoder for exactly the size the original produced.
		UINT cx = origOk ? (UINT)(orig.w > orig.h ? orig.w : orig.h) : 256;

		IStream* s2 = NULL;
		SHCreateStreamOnFileEx(f.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
			FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &s2);
		PsdImage img;
		bool newOk = s2 && DecodePsdImage(s2, cx, img);
		HBITMAP nb = newOk ? CreateDibFromImage(img, cx) : NULL;
		Raster next;
		bool newRasterOk = BitmapToRaster(nb, next);
		if (nb) DeleteObject(nb);
		if (s2) s2->Release();

		char od[24] = "none", nd[24] = "none";
		if (origOk) sprintf_s(od, "%dx%d", orig.w, orig.h);
		if (newRasterOk) sprintf_s(nd, "%dx%d", next.w, next.h);

		if (!origOk && !newRasterOk) {
			printf("%-13ws %10s %10s %8s %8s  both decline\n", f.c_str(), od, nd, "-", "-");
			continue;
		}
		if (origOk != newRasterOk) {
			printf("%-13ws %10s %10s %8s %8s  *** ONE SIDE FAILED\n", f.c_str(), od, nd, "-", "-");
			mismatches++;
			continue;
		}
		if (orig.w != next.w || orig.h != next.h) {
			printf("%-13ws %10s %10s %8s %8s  *** SIZE MISMATCH\n", f.c_str(), od, nd, "-", "-");
			mismatches++;
			continue;
		}

		double total = 0;
		int maxd = 0;
		size_t n = orig.px.size();
		for (size_t i = 0; i < n; i++) {
			const BYTE* a = (const BYTE*)&orig.px[i];
			const BYTE* b = (const BYTE*)&next.px[i];
			// Compare colour only where both are meaningfully opaque.
			for (int c = 0; c < 3; c++) {
				int d = abs((int)a[c] - (int)b[c]);
				total += d;
				if (d > maxd) maxd = d;
			}
		}
		double mean = n ? total / (n * 3) : 0;

		const char* verdict = (mean < 1.0) ? "match"
			: (mean < 8.0) ? "close (resampling)"
			: "*** DIVERGENT";
		if (mean >= 8.0) mismatches++;

		if (mean >= 1.0) {
			const int zoom = next.w < 128 ? 4 : 2;
			SaveRaster(orig, (L"cmp_orig_" + f + L".png").c_str(), zoom);
			SaveRaster(next, (L"cmp_new_" + f + L".png").c_str(), zoom);
		}

		printf("%-13ws %10s %10s %8.2f %8d  %s\n",
			f.c_str(), od, nd, mean, maxd, verdict);
	}

	printf("---------------------------------------------------------------------\n");
	printf("%d mismatch(es)\n", mismatches);
	CoUninitialize();
	return mismatches;
}
