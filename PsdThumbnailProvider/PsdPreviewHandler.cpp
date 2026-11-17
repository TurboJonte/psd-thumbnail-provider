#include "PsdPreviewHandler.h"

#include <Shlwapi.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

extern HINSTANCE g_hInst;
extern long g_cDllRef;

// {C4A4EC15-9C49-40A5-BD02-269CB227A29D}
const CLSID CLSID_PsdPreviewHandler =
	{ 0xc4a4ec15, 0x9c49, 0x40a5, { 0xbd, 0x02, 0x26, 0x9c, 0xb2, 0x27, 0xa2, 0x9d } };

namespace {

const wchar_t* kWindowClass = L"PsdPreviewHandlerWindow";
const int kMinDecodeEdge = 512;
const int kMaxDecodeEdge = 2048;
const int kInfoBarHeight = 22;
const int kCheckerSize = 8;

inline uint32_t FromColorRef(COLORREF c) {
	return 0xFF000000u | (static_cast<uint32_t>(GetRValue(c)) << 16) |
		(static_cast<uint32_t>(GetGValue(c)) << 8) | GetBValue(c);
}

inline int Luminance(COLORREF c) {
	return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

const wchar_t* ColorModeName(int mode) {
	switch (mode) {
	case 0: return L"Bitmap";
	case 1: return L"Grayscale";
	case 2: return L"Indexed";
	case 3: return L"RGB";
	case 4: return L"CMYK";
	case 7: return L"Multichannel";
	case 8: return L"Duotone";
	case 9: return L"Lab";
	default: return L"Unknown";
	}
}

} // namespace

PsdPreviewHandler::PsdPreviewHandler()
	: m_cRef(1), m_hwndParent(NULL), m_hwnd(NULL), m_pStream(NULL), m_punkSite(NULL),
	m_backColor(GetSysColor(COLOR_WINDOW)), m_textColor(GetSysColor(COLOR_WINDOWTEXT)),
	m_font(NULL), m_decodeAttempted(false), m_decodeOk(false), m_decodedEdge(0),
	m_atFullRes(false), m_scaledW(0), m_scaledH(0) {
	m_rcParent = RECT();
	InterlockedIncrement(&g_cDllRef);
}

PsdPreviewHandler::~PsdPreviewHandler() {
	if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = NULL; }
	if (m_font) { DeleteObject(m_font); m_font = NULL; }
	if (m_pStream) { m_pStream->Release(); m_pStream = NULL; }
	if (m_punkSite) { m_punkSite->Release(); m_punkSite = NULL; }
	InterlockedDecrement(&g_cDllRef);
}

// ----------------------------------------------------------------- IUnknown

IFACEMETHODIMP PsdPreviewHandler::QueryInterface(REFIID riid, void** ppv) {
	static const QITAB qit[] = {
		QITABENT(PsdPreviewHandler, IPreviewHandler),
		QITABENT(PsdPreviewHandler, IInitializeWithStream),
		QITABENT(PsdPreviewHandler, IObjectWithSite),
		QITABENT(PsdPreviewHandler, IOleWindow),
		QITABENT(PsdPreviewHandler, IPreviewHandlerVisuals),
		{ 0 },
	};
	return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) PsdPreviewHandler::AddRef() {
	return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) PsdPreviewHandler::Release() {
	ULONG cRef = InterlockedDecrement(&m_cRef);
	if (cRef == 0) delete this;
	return cRef;
}

// ------------------------------------------------------ IInitializeWithStream

IFACEMETHODIMP PsdPreviewHandler::Initialize(IStream* pStream, DWORD grfMode) {
	if (m_pStream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
	if (!pStream) return E_INVALIDARG;
	return pStream->QueryInterface(&m_pStream);
}

// ----------------------------------------------------------- window plumbing

void PsdPreviewHandler::EnsureWindowClass() {
	static bool registered = false;
	if (registered) return;

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = PsdPreviewHandler::WndProc;
	wc.hInstance = g_hInst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL; // we paint everything ourselves
	wc.lpszClassName = kWindowClass;

	RegisterClassExW(&wc);
	registered = true;
}

LRESULT CALLBACK PsdPreviewHandler::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	PsdPreviewHandler* self = reinterpret_cast<PsdPreviewHandler*>(
		GetWindowLongPtr(hwnd, GWLP_USERDATA));

	switch (msg) {
	case WM_NCCREATE: {
		CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
		break;
	}
	case WM_ERASEBKGND:
		return 1; // avoid flicker; WM_PAINT covers the whole client area
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		if (self) self->Paint(hdc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_SIZE:
		if (self) {
			// Re-decode at a higher resolution if the pane grew a lot.
			self->EnsureDecoded(self->DesiredDecodeEdge());
			self->m_scaledW = self->m_scaledH = 0; // force rescale
			InvalidateRect(hwnd, NULL, FALSE);
		}
		return 0;
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

void PsdPreviewHandler::CreatePreviewWindow() {
	if (m_hwnd || !m_hwndParent) return;
	EnsureWindowClass();

	m_hwnd = CreateWindowExW(
		0, kWindowClass, L"",
		WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
		m_rcParent.left, m_rcParent.top,
		m_rcParent.right - m_rcParent.left,
		m_rcParent.bottom - m_rcParent.top,
		m_hwndParent, NULL, g_hInst, this);
}

int PsdPreviewHandler::DesiredDecodeEdge() const {
	int w = m_rcParent.right - m_rcParent.left;
	int h = m_rcParent.bottom - m_rcParent.top;
	if (m_hwnd) {
		RECT rc;
		if (GetClientRect(m_hwnd, &rc)) { w = rc.right; h = rc.bottom; }
	}
	int edge = (std::max)(w, h);
	edge = ((edge + 511) / 512) * 512; // round up, so small resizes do not re-decode
	return (std::min)((std::max)(edge, kMinDecodeEdge), kMaxDecodeEdge);
}

void PsdPreviewHandler::EnsureDecoded(int wantEdge) {
	if (!m_pStream) return;
	// Already good enough?
	if (m_decodeAttempted && (m_atFullRes || wantEdge <= m_decodedEdge)) return;
	if (m_decodeAttempted && !m_decodeOk) return; // do not retry a failed file

	PsdImage image;
	const bool ok = DecodePsdImage(m_pStream, static_cast<UINT>(wantEdge), image);

	m_decodeAttempted = true;
	if (ok) {
		m_image = std::move(image);
		m_decodeOk = true;
		m_decodedEdge = wantEdge;
		m_atFullRes = (m_image.width >= m_image.fullWidth && m_image.height >= m_image.fullHeight);
		m_scaledW = m_scaledH = 0;
	} else {
		m_decodeOk = false;
	}
}

void PsdPreviewHandler::Repaint() {
	if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
}

// -------------------------------------------------------------------- paint

void PsdPreviewHandler::Paint(HDC hdc) {
	RECT rc;
	if (!GetClientRect(m_hwnd, &rc)) return;
	const int W = rc.right, H = rc.bottom;
	if (W <= 0 || H <= 0) return;

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = W;
	bmi.bmiHeader.biHeight = -H;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* bits = NULL;
	HBITMAP back = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!back || !bits) { if (back) DeleteObject(back); return; }

	HDC mem = CreateCompatibleDC(hdc);
	HGDIOBJ oldBmp = SelectObject(mem, back);

	uint32_t* buf = static_cast<uint32_t*>(bits);
	const uint32_t bg = FromColorRef(m_backColor);
	std::fill(buf, buf + static_cast<size_t>(W) * H, bg);

	const bool dark = Luminance(m_backColor) < 128;
	const uint32_t checkA = dark ? 0xFF3A3A3Au : 0xFFFFFFFFu;
	const uint32_t checkB = dark ? 0xFF2E2E2Eu : 0xFFCCCCCCu;

	wchar_t info[128] = L"";

	if (m_decodeOk && m_image.valid()) {
		const int availH = (std::max)(1, H - kInfoBarHeight);
		const double sx = static_cast<double>(W) / m_image.width;
		const double sy = static_cast<double>(availH) / m_image.height;
		const double scale = (std::min)(sx, sy);

		int tw = (std::max)(1, static_cast<int>(m_image.width * scale + 0.5));
		int th = (std::max)(1, static_cast<int>(m_image.height * scale + 0.5));
		tw = (std::min)(tw, W);
		th = (std::min)(th, availH);

		if (tw != m_scaledW || th != m_scaledH) {
			if (ScalePsdImage(m_image, tw, th, m_scaled)) {
				m_scaledW = tw;
				m_scaledH = th;
			}
		}

		if (m_scaledW == tw && m_scaledH == th && !m_scaled.empty()) {
			const int ox = (W - tw) / 2;
			const int oy = (availH - th) / 2;

			for (int y = 0; y < th; y++) {
				uint32_t* dstRow = buf + static_cast<size_t>(y + oy) * W + ox;
				const uint32_t* srcRow = m_scaled.data() + static_cast<size_t>(y) * tw;
				for (int x = 0; x < tw; x++) {
					const uint32_t s = srcRow[x];
					const uint32_t a = s >> 24;

					if (a == 255 && !m_image.hasAlpha) { dstRow[x] = 0xFF000000u | s; continue; }

					uint32_t base = bg;
					if (m_image.hasAlpha) {
						base = (((x + ox) / kCheckerSize) + ((y + oy) / kCheckerSize)) & 1
							? checkB : checkA;
					}
					if (a == 255) { dstRow[x] = 0xFF000000u | s; continue; }
					if (a == 0) { dstRow[x] = base; continue; }

					const uint32_t ia = 255 - a;
					const uint32_t r = ((( s >> 16) & 0xFF) * a + ((base >> 16) & 0xFF) * ia) / 255;
					const uint32_t g = ((( s >> 8) & 0xFF) * a + ((base >> 8) & 0xFF) * ia) / 255;
					const uint32_t b = (( s & 0xFF) * a + (base & 0xFF) * ia) / 255;
					dstRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
				}
			}
		}

		swprintf_s(info, L"%d × %d  ·  %s  ·  %d-bit%s",
			m_image.fullWidth, m_image.fullHeight,
			ColorModeName(m_image.colorMode),
			m_image.depth,
			m_image.fromEmbedded ? L"  ·  embedded preview" : L"");
	} else {
		wcscpy_s(info, L"Cannot preview this file");
	}

	// Info bar
	SetBkMode(mem, TRANSPARENT);
	::SetTextColor(mem, m_textColor);
	HGDIOBJ oldFont = NULL;
	if (m_font) oldFont = SelectObject(mem, m_font);

	RECT textRect = { 6, H - kInfoBarHeight, W - 6, H };
	DrawTextW(mem, info, -1, &textRect,
		DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

	if (oldFont) SelectObject(mem, oldFont);

	BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);

	SelectObject(mem, oldBmp);
	DeleteDC(mem);
	DeleteObject(back);
}

// ----------------------------------------------------------- IPreviewHandler

IFACEMETHODIMP PsdPreviewHandler::SetWindow(HWND hwnd, const RECT* prc) {
	if (!hwnd || !prc) return E_INVALIDARG;
	m_hwndParent = hwnd;
	m_rcParent = *prc;

	if (m_hwnd) {
		SetParent(m_hwnd, m_hwndParent);
		SetWindowPos(m_hwnd, NULL, m_rcParent.left, m_rcParent.top,
			m_rcParent.right - m_rcParent.left,
			m_rcParent.bottom - m_rcParent.top,
			SWP_NOACTIVATE | SWP_NOZORDER);
	}
	return S_OK;
}

IFACEMETHODIMP PsdPreviewHandler::SetRect(const RECT* prc) {
	if (!prc) return E_INVALIDARG;
	m_rcParent = *prc;

	if (m_hwnd) {
		SetWindowPos(m_hwnd, NULL, m_rcParent.left, m_rcParent.top,
			m_rcParent.right - m_rcParent.left,
			m_rcParent.bottom - m_rcParent.top,
			SWP_NOACTIVATE | SWP_NOZORDER);
		Repaint();
	}
	return S_OK;
}

IFACEMETHODIMP PsdPreviewHandler::DoPreview() {
	if (!m_pStream) return E_UNEXPECTED;
	if (m_hwnd) return S_OK; // already previewing

	CreatePreviewWindow();
	if (!m_hwnd) return E_FAIL;

	EnsureDecoded(DesiredDecodeEdge());
	Repaint();
	return S_OK;
}

IFACEMETHODIMP PsdPreviewHandler::Unload() {
	if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = NULL; }
	if (m_pStream) { m_pStream->Release(); m_pStream = NULL; }

	m_image = PsdImage();
	m_scaled.clear();
	m_scaledW = m_scaledH = 0;
	m_decodeAttempted = false;
	m_decodeOk = false;
	m_decodedEdge = 0;
	m_atFullRes = false;
	return S_OK;
}

IFACEMETHODIMP PsdPreviewHandler::SetFocus() {
	if (!m_hwnd) return S_FALSE;
	::SetFocus(m_hwnd);
	return S_OK;
}

IFACEMETHODIMP PsdPreviewHandler::QueryFocus(HWND* phwnd) {
	if (!phwnd) return E_INVALIDARG;
	*phwnd = ::GetFocus();
	return *phwnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

IFACEMETHODIMP PsdPreviewHandler::TranslateAccelerator(MSG* pmsg) {
	// Hand unhandled keys back to the preview frame so Explorer keeps working.
	if (!m_punkSite) return S_FALSE;

	IPreviewHandlerFrame* frame = NULL;
	HRESULT hr = m_punkSite->QueryInterface(IID_PPV_ARGS(&frame));
	if (SUCCEEDED(hr) && frame) {
		hr = frame->TranslateAccelerator(pmsg);
		frame->Release();
		return hr;
	}
	return S_FALSE;
}

// ------------------------------------------------------------ IObjectWithSite

IFACEMETHODIMP PsdPreviewHandler::SetSite(IUnknown* punkSite) {
	if (m_punkSite) { m_punkSite->Release(); m_punkSite = NULL; }
	if (punkSite) { m_punkSite = punkSite; m_punkSite->AddRef(); }
	return S_OK;
}

IFACEMETHODIMP PsdPreviewHandler::GetSite(REFIID riid, void** ppv) {
	if (!ppv) return E_INVALIDARG;
	*ppv = NULL;
	if (!m_punkSite) return E_FAIL;
	return m_punkSite->QueryInterface(riid, ppv);
}

// ----------------------------------------------------------------- IOleWindow

IFACEMETHODIMP PsdPreviewHandler::GetWindow(HWND* phwnd) {
	if (!phwnd) return E_INVALIDARG;
	*phwnd = m_hwnd;
	return m_hwnd ? S_OK : E_FAIL;
}

IFACEMETHODIMP PsdPreviewHandler::ContextSensitiveHelp(BOOL) {
	return E_NOTIMPL;
}

// --------------------------------------------------- IPreviewHandlerVisuals

IFACEMETHODIMP PsdPreviewHandler::SetBackgroundColor(COLORREF color) {
	m_backColor = color;
	Repaint();
	return S_OK;
}

IFACEMETHODIMP PsdPreviewHandler::SetFont(const LOGFONTW* plf) {
	if (!plf) return E_INVALIDARG;
	if (m_font) DeleteObject(m_font);
	m_font = CreateFontIndirectW(plf);
	Repaint();
	return S_OK;
}

IFACEMETHODIMP PsdPreviewHandler::SetTextColor(COLORREF color) {
	m_textColor = color;
	Repaint();
	return S_OK;
}
