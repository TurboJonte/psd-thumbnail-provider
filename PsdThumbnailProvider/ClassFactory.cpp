#include "ClassFactory.h"
#include "PsdThumbnailProvider.h"
#include "PsdPreviewHandler.h"
#include <new>
#include <Shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

extern long g_cDllRef;

ClassFactory::ClassFactory(REFCLSID clsid) : m_cRef(1), m_clsid(clsid) {
	InterlockedIncrement(&g_cDllRef);
}

ClassFactory::~ClassFactory() {
	InterlockedDecrement(&g_cDllRef);
}

// IUnknown

IFACEMETHODIMP ClassFactory::QueryInterface(REFIID riid, void **ppv) {
	static const QITAB qit[] =
	{
		QITABENT(ClassFactory, IClassFactory),
		{ 0 },
	};
	return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) ClassFactory::AddRef() {
	return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) ClassFactory::Release() {
	ULONG cRef = InterlockedDecrement(&m_cRef);
	if (0 == cRef) {
		delete this;
	}
	return cRef;
}

// IClassFactory

IFACEMETHODIMP ClassFactory::CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv) {
	HRESULT hr = CLASS_E_NOAGGREGATION;

	// pUnkOuter is used for aggregation, which we do not support.
	if (pUnkOuter == NULL) {
		hr = E_OUTOFMEMORY;

		if (IsEqualCLSID(m_clsid, CLSID_PsdPreviewHandler)) {
			PsdPreviewHandler *pExt = new (std::nothrow) PsdPreviewHandler();
			if (pExt) {
				hr = pExt->QueryInterface(riid, ppv);
				pExt->Release();
			}
		} else {
			PsdThumbnailProvider *pExt = new (std::nothrow) PsdThumbnailProvider();
			if (pExt) {
				hr = pExt->QueryInterface(riid, ppv);
				pExt->Release();
			}
		}
	}

	return hr;
}

IFACEMETHODIMP ClassFactory::LockServer(BOOL fLock) {
	if (fLock) {
		InterlockedIncrement(&g_cDllRef);
	} else {
		InterlockedDecrement(&g_cDllRef);
	}
	return S_OK;
}
