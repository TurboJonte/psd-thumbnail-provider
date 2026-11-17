// Hosts PsdPreviewHandler directly (no COM registration needed), drives the
// IPreviewHandler lifecycle, and captures what it paints to a PNG.

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>

#include "../PsdThumbnailProvider/PsdPreviewHandler.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")

// The handler expects these from the DLL.
HINSTANCE g_hInst = NULL;
long g_cDllRef = 0;

static bool SaveHwndPng(HWND hwnd, int w, int h, const wchar_t* path) {
	HDC screen = GetDC(NULL);
	HDC mem = CreateCompatibleDC(screen);

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biHeight = -h;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* bits = NULL;
	HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	HGDIOBJ old = SelectObject(mem, bmp);

	PrintWindow(hwnd, mem, PW_CLIENTONLY);

	// Force opaque alpha so the PNG is viewable.
	uint32_t* px = (uint32_t*)bits;
	for (int i = 0; i < w * h; i++) px[i] |= 0xFF000000;

	bool ok = false;
	IWICImagingFactory* f = NULL;
	if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&f)))) {
		IWICStream* st = NULL; IWICBitmapEncoder* en = NULL; IWICBitmapFrameEncode* fr = NULL;
		if (SUCCEEDED(f->CreateStream(&st)) &&
			SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
			SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, NULL, &en)) &&
			SUCCEEDED(en->Initialize(st, WICBitmapEncoderNoCache)) &&
			SUCCEEDED(en->CreateNewFrame(&fr, NULL)) &&
			SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->SetSize(w, h))) {
			WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
			if (SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
				SUCCEEDED(fr->WritePixels(h, w * 4, w * 4 * h, (BYTE*)bits)) &&
				SUCCEEDED(fr->Commit()) && SUCCEEDED(en->Commit())) ok = true;
		}
		if (fr) fr->Release();
		if (en) en->Release();
		if (st) st->Release();
		f->Release();
	}

	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(NULL, screen);
	return ok;
}

static void Pump(int ms) {
	DWORD end = GetTickCount() + ms;
	MSG msg;
	while (GetTickCount() < end) {
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		Sleep(5);
	}
}

static int RunOne(const wchar_t* file, int w, int h, bool dark, const wchar_t* outPng) {
	IStream* stream = NULL;
	if (FAILED(SHCreateStreamOnFileEx(file, STGM_READ | STGM_SHARE_DENY_WRITE,
		FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &stream))) {
		wprintf(L"  cannot open %s\n", file);
		return 1;
	}

	HWND host = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW,
		100, 100, w + 16, h + 39, NULL, NULL, g_hInst, NULL);
	if (!host) { stream->Release(); return 1; }

	PsdPreviewHandler* handler = new PsdPreviewHandler();

	IInitializeWithStream* init = NULL;
	IPreviewHandler* preview = NULL;
	IPreviewHandlerVisuals* visuals = NULL;
	handler->QueryInterface(IID_PPV_ARGS(&init));
	handler->QueryInterface(IID_PPV_ARGS(&preview));
	handler->QueryInterface(IID_PPV_ARGS(&visuals));

	int rc = 1;
	if (init && preview) {
		HRESULT hr = init->Initialize(stream, STGM_READ);
		if (SUCCEEDED(hr)) {
			if (visuals) {
				visuals->SetBackgroundColor(dark ? RGB(32, 32, 32) : RGB(255, 255, 255));
				visuals->SetTextColor(dark ? RGB(220, 220, 220) : RGB(60, 60, 60));
				LOGFONTW lf = {};
				lf.lfHeight = -12;
				wcscpy_s(lf.lfFaceName, L"Segoe UI");
				visuals->SetFont(&lf);
			}

			RECT rcPreview = { 0, 0, w, h };
			hr = preview->SetWindow(host, &rcPreview);
			if (SUCCEEDED(hr)) {
				hr = preview->DoPreview();
				if (SUCCEEDED(hr)) {
					ShowWindow(host, SW_SHOW);
					Pump(350);

					HWND child = NULL;
					IOleWindow* ow = NULL;
					if (SUCCEEDED(handler->QueryInterface(IID_PPV_ARGS(&ow)))) {
						ow->GetWindow(&child);
						ow->Release();
					}
					if (child && SaveHwndPng(child, w, h, outPng)) {
						wprintf(L"  wrote %s\n", outPng);
						rc = 0;
					} else {
						wprintf(L"  capture failed\n");
					}
				} else wprintf(L"  DoPreview failed 0x%08X\n", hr);
			} else wprintf(L"  SetWindow failed 0x%08X\n", hr);
		} else wprintf(L"  Initialize failed 0x%08X\n", hr);
	}

	if (preview) { preview->Unload(); preview->Release(); }
	if (visuals) visuals->Release();
	if (init) init->Release();
	handler->Release();

	DestroyWindow(host);
	stream->Release();
	return rc;
}

int wmain(int argc, wchar_t** argv) {
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	g_hInst = GetModuleHandle(NULL);

	struct Job { const wchar_t* file; int w, h; bool dark; const wchar_t* out; };
	std::vector<Job> jobs;

	if (argc > 1) {
		jobs.push_back({ argv[1], 520, 420, false, L"preview_custom.png" });
	} else {
		jobs.push_back({ L"test.psd",        420, 360, false, L"preview_alpha_light.png" });
		jobs.push_back({ L"test.psd",        420, 360, true,  L"preview_alpha_dark.png" });
		jobs.push_back({ L"gen_cmyk8.psd",   520, 420, false, L"preview_cmyk.png" });
		jobs.push_back({ L"gen_big_rle.psd", 640, 480, false, L"preview_big.png" });
		jobs.push_back({ L"gen_indexed.psd", 420, 360, true,  L"preview_indexed.png" });
	}

	int failures = 0;
	for (const auto& j : jobs) {
		wprintf(L"%s  (%dx%d, %s)\n", j.file, j.w, j.h, j.dark ? L"dark" : L"light");
		failures += RunOne(j.file, j.w, j.h, j.dark, j.out);
	}

	wprintf(L"%d failure(s)\n", failures);
	CoUninitialize();
	return failures;
}
