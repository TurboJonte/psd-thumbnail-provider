// Loads the built DLL and exercises its COM entry points the way the shell
// would: class factory for both CLSIDs, then the interfaces each must expose.

#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <cstdio>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

// {5BB45D32-AF01-414F-B60A-5E999B986681}
static const CLSID CLSID_Thumb =
{ 0x5bb45d32, 0xaf01, 0x414f, { 0xb6, 0x0a, 0x5e, 0x99, 0x9b, 0x98, 0x66, 0x81 } };
// {C4A4EC15-9C49-40A5-BD02-269CB227A29D}
static const CLSID CLSID_Preview =
{ 0xc4a4ec15, 0x9c49, 0x40a5, { 0xbd, 0x02, 0x26, 0x9c, 0xb2, 0x27, 0xa2, 0x9d } };

typedef HRESULT(STDAPICALLTYPE* PFN_GETCLASSOBJECT)(REFCLSID, REFIID, void**);
typedef HRESULT(STDAPICALLTYPE* PFN_CANUNLOAD)(void);

static int g_fail = 0;

static void Check(bool ok, const char* what) {
	printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
	if (!ok) g_fail++;
}

int wmain(int argc, wchar_t** argv) {
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	const wchar_t* dllPath = argc > 1 ? argv[1]
		: L"..\\PsdThumbnailProvider\\x64\\Release\\PsdThumbnailProvider.dll";
	const wchar_t* psd = argc > 2 ? argv[2] : L"test.psd";

	wprintf(L"DLL:  %s\n", dllPath);
	wprintf(L"File: %s\n\n", psd);

	HMODULE mod = LoadLibraryW(dllPath);
	if (!mod) { printf("cannot load DLL (%lu)\n", GetLastError()); return 1; }

	PFN_GETCLASSOBJECT getClassObject =
		(PFN_GETCLASSOBJECT)GetProcAddress(mod, "DllGetClassObject");
	PFN_CANUNLOAD canUnload = (PFN_CANUNLOAD)GetProcAddress(mod, "DllCanUnloadNow");

	Check(getClassObject != NULL, "DllGetClassObject exported");
	Check(canUnload != NULL, "DllCanUnloadNow exported");
	Check(GetProcAddress(mod, "DllRegisterServer") != NULL, "DllRegisterServer exported");
	Check(GetProcAddress(mod, "DllUnregisterServer") != NULL, "DllUnregisterServer exported");
	if (!getClassObject) return 1;

	// ---- thumbnail provider ----
	printf("\nThumbnail provider:\n");
	IClassFactory* factory = NULL;
	HRESULT hr = getClassObject(CLSID_Thumb, IID_PPV_ARGS(&factory));
	Check(SUCCEEDED(hr) && factory, "class factory for thumbnail CLSID");

	if (SUCCEEDED(hr) && factory) {
		IThumbnailProvider* thumb = NULL;
		hr = factory->CreateInstance(NULL, IID_PPV_ARGS(&thumb));
		Check(SUCCEEDED(hr) && thumb, "CreateInstance -> IThumbnailProvider");

		IInitializeWithStream* init = NULL;
		if (thumb) {
			Check(SUCCEEDED(thumb->QueryInterface(IID_PPV_ARGS(&init))),
				"QueryInterface -> IInitializeWithStream");
		}

		if (init) {
			IStream* stream = NULL;
			if (SUCCEEDED(SHCreateStreamOnFileEx(psd, STGM_READ | STGM_SHARE_DENY_WRITE,
				FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &stream))) {
				Check(SUCCEEDED(init->Initialize(stream, STGM_READ)), "Initialize(stream)");

				HBITMAP bmp = NULL;
				WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
				hr = thumb->GetThumbnail(256, &bmp, &alpha);
				Check(SUCCEEDED(hr) && bmp != NULL, "GetThumbnail(256) returned a bitmap");

				if (bmp) {
					BITMAP bm = {};
					GetObject(bmp, sizeof(bm), &bm);
					printf("         -> %ldx%ld, %d bpp, alpha=%d\n",
						bm.bmWidth, bm.bmHeight, bm.bmBitsPixel, (int)alpha);
					Check(bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmBitsPixel == 32,
						"bitmap is a valid 32bpp image");
					DeleteObject(bmp);
				}
				stream->Release();
			}
			init->Release();
		}
		if (thumb) thumb->Release();
		factory->Release();
	}

	// ---- preview handler ----
	printf("\nPreview handler:\n");
	IClassFactory* pfactory = NULL;
	hr = getClassObject(CLSID_Preview, IID_PPV_ARGS(&pfactory));
	Check(SUCCEEDED(hr) && pfactory, "class factory for preview CLSID");

	if (SUCCEEDED(hr) && pfactory) {
		IPreviewHandler* preview = NULL;
		hr = pfactory->CreateInstance(NULL, IID_PPV_ARGS(&preview));
		Check(SUCCEEDED(hr) && preview, "CreateInstance -> IPreviewHandler");

		if (preview) {
			struct { REFIID iid; const char* name; } probes[] = {
				{ IID_IInitializeWithStream, "IInitializeWithStream" },
				{ IID_IObjectWithSite,       "IObjectWithSite" },
				{ IID_IOleWindow,            "IOleWindow" },
				{ IID_IPreviewHandlerVisuals,"IPreviewHandlerVisuals" },
			};
			for (int i = 0; i < ARRAYSIZE(probes); i++) {
				IUnknown* p = NULL;
				bool ok = SUCCEEDED(preview->QueryInterface(probes[i].iid, (void**)&p)) && p;
				Check(ok, probes[i].name);
				if (p) p->Release();
			}
			preview->Release();
		}
		pfactory->Release();
	}

	// ---- unrelated CLSID must be refused ----
	printf("\nNegative check:\n");
	IClassFactory* bogus = NULL;
	// an arbitrary CLSID this DLL knows nothing about
	static const CLSID CLSID_Nonsense =
	{ 0x11112222, 0x3333, 0x4444, { 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc } };
	hr = getClassObject(CLSID_Nonsense, IID_PPV_ARGS(&bogus));
	Check(hr == CLASS_E_CLASSNOTAVAILABLE, "unknown CLSID -> CLASS_E_CLASSNOTAVAILABLE");
	if (bogus) bogus->Release();

	if (canUnload) {
		Check(canUnload() == S_OK, "DllCanUnloadNow -> S_OK after release");
	}

	printf("\n%d failure(s)\n", g_fail);
	CoUninitialize();
	return g_fail;
}
