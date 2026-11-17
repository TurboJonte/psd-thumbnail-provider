#include "PsdThumbnailProvider.h"
#include "GetThumbnail.h"
#include <Shlwapi.h>

#pragma comment(lib, "Shlwapi.lib")

extern HINSTANCE g_hInst;
extern long g_cDllRef;

PsdThumbnailProvider::PsdThumbnailProvider() : m_cRef(1), m_pStream(NULL) {
	InterlockedIncrement(&g_cDllRef);
}

PsdThumbnailProvider::~PsdThumbnailProvider() {
	if (m_pStream) {
		m_pStream->Release();
		m_pStream = NULL;
	}
	InterlockedDecrement(&g_cDllRef);
}

// IUnknown

IFACEMETHODIMP PsdThumbnailProvider::QueryInterface(REFIID riid, void **ppv) {
	static const QITAB qit[] =
	{
		QITABENT(PsdThumbnailProvider, IThumbnailProvider),
		QITABENT(PsdThumbnailProvider, IInitializeWithStream),
		{ 0 },
	};
	return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) PsdThumbnailProvider::AddRef() {
	return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) PsdThumbnailProvider::Release() {
	ULONG cRef = InterlockedDecrement(&m_cRef);

	if (0 == cRef)
		delete this;

	return cRef;
}

// IInitializeWithStream

IFACEMETHODIMP PsdThumbnailProvider::Initialize(IStream *pStream, DWORD grfMode) {
	HRESULT hr = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

	if (m_pStream == NULL)
		hr = pStream->QueryInterface(&m_pStream);

	return hr;
}

// IThumbnailProvider

IFACEMETHODIMP PsdThumbnailProvider::GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha) {
	if (!phbmp || !pdwAlpha) return E_POINTER;

	*phbmp = NULL;
	*pdwAlpha = WTSAT_ARGB;

	if (!m_pStream) return E_UNEXPECTED;

	// cx is the requested bounding box; honouring it lets the decoder skip rows.
	*phbmp = GetPSDThumbnail(m_pStream, cx);

	return *phbmp != NULL ? S_OK : E_FAIL;
}
