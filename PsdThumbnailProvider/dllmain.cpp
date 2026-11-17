#include <windows.h>
#include <Guiddef.h>
#include <shlobj.h> // For SHChangeNotify
#include <strsafe.h>
#include "ClassFactory.h" // For the class factory
#include "PsdPreviewHandler.h"

// {5BB45D32-AF01-414F-B60A-5E999B986681}
const CLSID CLSID_RecipeThumbnailProvider =
	{ 0x5bb45d32, 0xaf01, 0x414f, { 0xb6, 0xa, 0x5e, 0x99, 0x9b, 0x98, 0x66, 0x81 } };

HINSTANCE g_hInst = NULL;
long g_cDllRef = 0;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
	switch (dwReason) {
	case DLL_PROCESS_ATTACH:
		// Hold the instance of this DLL module, we will use it to get the 
		// path of the DLL to register the component.
		g_hInst = hModule;
		DisableThreadLibraryCalls(hModule);
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
	HRESULT hr = CLASS_E_CLASSNOTAVAILABLE;

	if (IsEqualCLSID(CLSID_RecipeThumbnailProvider, rclsid) ||
		IsEqualCLSID(CLSID_PsdPreviewHandler, rclsid)) {
		hr = E_OUTOFMEMORY;

		ClassFactory *pClassFactory = new ClassFactory(rclsid);
		if (pClassFactory) {
			hr = pClassFactory->QueryInterface(riid, ppv);
			pClassFactory->Release();
		}
	}

	return hr;
}

STDAPI DllCanUnloadNow(void) {
	return g_cDllRef > 0 ? S_FALSE : S_OK;
}

// ------------------------------------------------------------- registration

namespace {

// Shell extension slots.
const wchar_t* kThumbnailShellEx = L"{e357fccd-a995-4576-b01f-234630154e96}"; // IThumbnailProvider
const wchar_t* kPreviewShellEx = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";   // IPreviewHandler
// Windows' own preview host surrogate (prevhost.exe), so a crash in the
// handler cannot take Explorer down with it.
const wchar_t* kPrevHostAppId = L"{534A1E02-D58F-44f0-B58B-36CBED287C7C}";

const wchar_t* kExtensions[] = { L".psd", L".psb" };

const wchar_t* kThumbnailName = L"PSD Thumbnail Provider";
const wchar_t* kPreviewName = L"PSD Preview Handler";

LONG SetValue(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
	HKEY key = NULL;
	LONG r = RegCreateKeyExW(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
		KEY_WRITE, NULL, &key, NULL);
	if (r != ERROR_SUCCESS) return r;
	r = RegSetValueExW(key, name, 0, REG_SZ,
		reinterpret_cast<const BYTE*>(value),
		static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
	RegCloseKey(key);
	return r;
}

void ClsidToString(REFCLSID clsid, wchar_t* out, int cch) {
	StringFromGUID2(clsid, out, cch);
}

LONG RegisterCoClass(REFCLSID clsid, const wchar_t* friendlyName,
	const wchar_t* modulePath, const wchar_t* appId) {
	wchar_t cls[64] = {};
	ClsidToString(clsid, cls, ARRAYSIZE(cls));

	wchar_t key[256] = {};
	swprintf_s(key, L"CLSID\\%s", cls);
	LONG r = SetValue(HKEY_CLASSES_ROOT, key, NULL, friendlyName);
	if (r != ERROR_SUCCESS) return r;

	// Running out of process means Explorer survives a bad file.
	if (appId) SetValue(HKEY_CLASSES_ROOT, key, L"AppID", appId);

	swprintf_s(key, L"CLSID\\%s\\InprocServer32", cls);
	r = SetValue(HKEY_CLASSES_ROOT, key, NULL, modulePath);
	if (r != ERROR_SUCCESS) return r;

	return SetValue(HKEY_CLASSES_ROOT, key, L"ThreadingModel", L"Apartment");
}

void RegisterAssociation(const wchar_t* ext, const wchar_t* shellExSlot, const wchar_t* clsidStr) {
	wchar_t key[256] = {};
	swprintf_s(key, L"%s\\shellex\\%s", ext, shellExSlot);
	SetValue(HKEY_CLASSES_ROOT, key, NULL, clsidStr);

	// SystemFileAssociations survives a user changing the default program.
	swprintf_s(key, L"SystemFileAssociations\\%s\\shellex\\%s", ext, shellExSlot);
	SetValue(HKEY_CLASSES_ROOT, key, NULL, clsidStr);
}

void UnregisterAssociation(const wchar_t* ext, const wchar_t* shellExSlot) {
	wchar_t key[256] = {};
	swprintf_s(key, L"%s\\shellex\\%s", ext, shellExSlot);
	RegDeleteTreeW(HKEY_CLASSES_ROOT, key);

	swprintf_s(key, L"SystemFileAssociations\\%s\\shellex\\%s", ext, shellExSlot);
	RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
}

void ApproveShellExtension(const wchar_t* clsidStr, const wchar_t* name, bool add) {
	const wchar_t* approved =
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
	if (add) {
		SetValue(HKEY_LOCAL_MACHINE, approved, clsidStr, name);
	} else {
		HKEY key = NULL;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, approved, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
			RegDeleteValueW(key, clsidStr);
			RegCloseKey(key);
		}
	}
}

} // namespace

STDAPI DllRegisterServer(void) {
	wchar_t module[MAX_PATH] = {};
	if (!GetModuleFileNameW(g_hInst, module, ARRAYSIZE(module)))
		return HRESULT_FROM_WIN32(GetLastError());

	wchar_t thumbClsid[64] = {}, previewClsid[64] = {};
	ClsidToString(CLSID_RecipeThumbnailProvider, thumbClsid, ARRAYSIZE(thumbClsid));
	ClsidToString(CLSID_PsdPreviewHandler, previewClsid, ARRAYSIZE(previewClsid));

	// The thumbnail provider is in-process; the preview handler is hosted by
	// prevhost.exe via the AppID above.
	LONG r = RegisterCoClass(CLSID_RecipeThumbnailProvider, kThumbnailName, module, NULL);
	if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);

	r = RegisterCoClass(CLSID_PsdPreviewHandler, kPreviewName, module, kPrevHostAppId);
	if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);

	for (int i = 0; i < ARRAYSIZE(kExtensions); i++) {
		RegisterAssociation(kExtensions[i], kThumbnailShellEx, thumbClsid);
		RegisterAssociation(kExtensions[i], kPreviewShellEx, previewClsid);
	}

	// Explorer looks preview handlers up in this list.
	SetValue(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers",
		previewClsid, kPreviewName);

	ApproveShellExtension(thumbClsid, kThumbnailName, true);
	ApproveShellExtension(previewClsid, kPreviewName, true);

	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
	return S_OK;
}

STDAPI DllUnregisterServer(void) {
	wchar_t thumbClsid[64] = {}, previewClsid[64] = {};
	ClsidToString(CLSID_RecipeThumbnailProvider, thumbClsid, ARRAYSIZE(thumbClsid));
	ClsidToString(CLSID_PsdPreviewHandler, previewClsid, ARRAYSIZE(previewClsid));

	for (int i = 0; i < ARRAYSIZE(kExtensions); i++) {
		UnregisterAssociation(kExtensions[i], kThumbnailShellEx);
		UnregisterAssociation(kExtensions[i], kPreviewShellEx);
	}

	HKEY key = NULL;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers",
		0, KEY_WRITE, &key) == ERROR_SUCCESS) {
		RegDeleteValueW(key, previewClsid);
		RegCloseKey(key);
	}

	ApproveShellExtension(thumbClsid, kThumbnailName, false);
	ApproveShellExtension(previewClsid, kPreviewName, false);

	wchar_t clsKey[256] = {};
	swprintf_s(clsKey, L"CLSID\\%s", thumbClsid);
	RegDeleteTreeW(HKEY_CLASSES_ROOT, clsKey);
	swprintf_s(clsKey, L"CLSID\\%s", previewClsid);
	RegDeleteTreeW(HKEY_CLASSES_ROOT, clsKey);

	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
	return S_OK;
}
