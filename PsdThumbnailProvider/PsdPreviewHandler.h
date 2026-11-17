#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <vector>

#include "PsdDecoder.h"

// {C4A4EC15-9C49-40A5-BD02-269CB227A29D}
extern const CLSID CLSID_PsdPreviewHandler;

// Preview Pane handler for PSD/PSB. Runs inside prevhost.exe.
class PsdPreviewHandler :
	public IPreviewHandler,
	public IInitializeWithStream,
	public IObjectWithSite,
	public IOleWindow,
	public IPreviewHandlerVisuals {
public:
	PsdPreviewHandler();

	// IUnknown
	IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv);
	IFACEMETHODIMP_(ULONG) AddRef();
	IFACEMETHODIMP_(ULONG) Release();

	// IInitializeWithStream
	IFACEMETHODIMP Initialize(IStream* pStream, DWORD grfMode);

	// IPreviewHandler
	IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* prc);
	IFACEMETHODIMP SetRect(const RECT* prc);
	IFACEMETHODIMP DoPreview();
	IFACEMETHODIMP Unload();
	IFACEMETHODIMP SetFocus();
	IFACEMETHODIMP QueryFocus(HWND* phwnd);
	IFACEMETHODIMP TranslateAccelerator(MSG* pmsg);

	// IObjectWithSite
	IFACEMETHODIMP SetSite(IUnknown* punkSite);
	IFACEMETHODIMP GetSite(REFIID riid, void** ppv);

	// IOleWindow
	IFACEMETHODIMP GetWindow(HWND* phwnd);
	IFACEMETHODIMP ContextSensitiveHelp(BOOL fEnterMode);

	// IPreviewHandlerVisuals
	IFACEMETHODIMP SetBackgroundColor(COLORREF color);
	IFACEMETHODIMP SetFont(const LOGFONTW* plf);
	IFACEMETHODIMP SetTextColor(COLORREF color);

protected:
	~PsdPreviewHandler();

private:
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	static void EnsureWindowClass();

	void CreatePreviewWindow();
	void EnsureDecoded(int wantEdge);
	void Paint(HDC hdc);
	void Repaint();
	int DesiredDecodeEdge() const;

	long m_cRef;

	HWND m_hwndParent;
	HWND m_hwnd;
	RECT m_rcParent;

	IStream* m_pStream;
	IUnknown* m_punkSite;

	COLORREF m_backColor;
	COLORREF m_textColor;
	HFONT m_font;

	PsdImage m_image;
	bool m_decodeAttempted;
	bool m_decodeOk;
	int m_decodedEdge;   // maxEdge the current decode was made with
	bool m_atFullRes;

	std::vector<uint32_t> m_scaled;
	int m_scaledW;
	int m_scaledH;
};
