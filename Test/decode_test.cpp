// Standalone harness: decodes every test file at several sizes, reports timing
// and the path taken, and writes PNGs for visual inspection.

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>

#include "../PsdThumbnailProvider/PsdDecoder.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

static bool SavePng(const PsdImage& img, const wchar_t* path) {
	IWICImagingFactory* factory = NULL;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory)))) return false;

	IWICStream* stream = NULL;
	IWICBitmapEncoder* encoder = NULL;
	IWICBitmapFrameEncode* frame = NULL;
	bool ok = false;

	if (SUCCEEDED(factory->CreateStream(&stream)) &&
		SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
		SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, NULL, &encoder)) &&
		SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(encoder->CreateNewFrame(&frame, NULL)) &&
		SUCCEEDED(frame->Initialize(NULL)) &&
		SUCCEEDED(frame->SetSize(img.width, img.height))) {
		WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
		if (SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
			SUCCEEDED(frame->WritePixels(img.height, img.width * 4,
				(UINT)(img.pixels.size() * 4),
				(BYTE*)const_cast<uint32_t*>(img.pixels.data()))) &&
			SUCCEEDED(frame->Commit()) &&
			SUCCEEDED(encoder->Commit())) {
			ok = true;
		}
	}

	if (frame) frame->Release();
	if (encoder) encoder->Release();
	if (stream) stream->Release();
	factory->Release();
	return ok;
}

static double Now() {
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
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

	const UINT sizes[] = { 96, 256, 1024, 0 };

	printf("%-14s %6s %11s %5s %9s %8s %7s\n",
		"file", "MB", "document", "req", "decoded", "source", "ms");
	printf("--------------------------------------------------------------------\n");

	int failures = 0;

	for (const auto& f : files) {
		WIN32_FILE_ATTRIBUTE_DATA fad = {};
		GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fad);
		double mb = (double)(((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) / (1024.0 * 1024.0);

		for (UINT cx : sizes) {
			IStream* stream = NULL;
			if (FAILED(SHCreateStreamOnFileEx(f.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
				FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &stream))) {
				printf("%-14ws  <cannot open>\n", f.c_str());
				failures++;
				break;
			}

			PsdImage img;
			double t0 = Now();
			bool ok = DecodePsdImage(stream, cx, img);
			double ms = Now() - t0;
			stream->Release();

			if (!ok) {
				printf("%-14ws %6.1f %11s %5u %9s %8s %7.1f  FAILED\n",
					f.c_str(), mb, "-", cx, "-", "-", ms);
				failures++;
				continue;
			}

			char dims[32], dec[32];
			sprintf_s(dims, "%dx%d", img.fullWidth, img.fullHeight);
			sprintf_s(dec, "%dx%d", img.width, img.height);

			printf("%-14ws %6.1f %11s %5u %9s %8s %7.1f\n",
				f.c_str(), mb, dims, cx, dec,
				img.fromEmbedded ? "embedded" : "composite", ms);

			if (cx == 256) {
				std::wstring out = L"out_" + f + L".png";
				SavePng(img, out.c_str());
			}
		}
	}

	printf("--------------------------------------------------------------------\n");
	printf("%d failure(s)\n", failures);

	CoUninitialize();
	return failures;
}
