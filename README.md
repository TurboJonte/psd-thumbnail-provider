# PSD thumbnail provider and preview handler

Native Windows Shell extension that gives Explorer **thumbnails** and a **Preview Pane**
for Photoshop `.psd` and `.psb` files.

A single x64 DLL implements both:

| Interface | What it does |
|---|---|
| `IThumbnailProvider` | Icons in Explorer at every size (16 - 256 px and beyond) |
| `IPreviewHandler` | Live render in the Preview Pane (Alt+P), with alpha, dark mode and a format caption |

This is a fork of [Agamnentzar/psd-thumbnail-provider](https://github.com/Agamnentzar/psd-thumbnail-provider) (MIT),
which provides the thumbnail half. Added here: the preview handler, a scale-aware
decoder, 32-bit float support, and a test suite.

## What's different from upstream

**Preview Pane support.** Upstream has no `IPreviewHandler` — neither do the other
PSD thumbnail projects on GitHub. The preview handler renders the composite at pane
resolution (not the 256 px embedded preview), composites alpha over a checkerboard,
follows the host's light/dark colours, and shows `3543 × 4724 · RGB · 16-bit`.

**Scale-aware decoding.** Upstream decodes every row at full resolution, then shrinks:
a 10000×10000 PSD means a 400 MB buffer to produce a 256 px icon. This decoder reads
the PackBits scanline-length table, turns it into a prefix-offset table, and **seeks
past the rows it does not need** — decoding to roughly 2× the requested size and
filtering down from there. (Technique from
[nurfarhad/psd-thumbnail-provider](https://github.com/nurfarhad/psd-thumbnail-provider), MIT.)
Raw (uncompressed) composites get the same treatment by direct offset arithmetic.

**Lossless by preference.** The embedded preview resource (ID 1036) is a *lossy JPEG*
capped at 256 px. Upstream uses it for any document over 256 px, so a 256 px icon of a
large file is an upscaled 120×160 JPEG. Here it is only preferred when the document is
≥ 16 MP *and* the request fits within it — where skipping the composite is a real
latency win and the artefacts are invisible at icon size. Otherwise the composite is
decoded. It remains the fallback whenever the composite is missing, blank or unsupported.

**32-bit float images.** Upstream rejects them. Taking the high byte of a big-endian
float32 is meaningless (it is sign and exponent), so samples are decoded as floats and
converted linear → sRGB through a lookup table.

**Fixes carried in:** the leaked thumbnail buffer (upstream's own `TODO`),
`GdiplusShutdown` on an uninitialised token when startup fails, `IStream` released in
`GetThumbnail` rather than the destructor (leaks if never called, double-release if
called twice), and `cx` ignored so every request decoded the same size. GDI+ is gone;
WIC does the JPEG decode and Fant-filtered scaling.

## Format support

| | Supported |
|---|---|
| Container | PSD, PSB (large document) |
| Colour modes | RGB, CMYK, Grayscale, Duotone, Indexed (with palette + transparency index) |
| Bit depths | 8, 16, 32 (float) |
| Compression | Raw, PackBits RLE |
| Alpha | Yes, composited over a checkerboard in the preview |

Unsupported modes fail cleanly so Explorer shows the generic icon rather than caching
a blank or black thumbnail. Bitmap (1 bpp) and Lab are not decoded; files in those
modes fall back to the embedded preview if present.

## Measured performance

Warm cache, x64 Release, this machine. "req" is the requested bounding box.

**Real 189 MB file** (`jj-test.psd`, 3543×4724, 16-bit RGB + alpha, 16.7 MP):

| req | decoded at | path | time |
|---|---|---|---|
| 96 px | 120×160 | embedded | **~8 ms** |
| 256 px | 443×591 | composite | **~33 ms** |
| 1024 px | 1772×2362 | composite | ~170 ms |
| full | 3543×4724 | composite | ~357 ms |

**Synthetic large files:**

| file | document | req 96 | req 256 | full decode |
|---|---|---|---|---|
| 6000×4000 RLE (4.9 MB) | 24 MP | **10.5 ms** | 21.5 ms | 336 ms |
| 4000×3000 raw (34 MB) | 12 MP | **7 ms** | 32 ms | 190 ms |

Row-skipping is worth **28-32×** against a full decode at icon sizes — that gap is the
whole point of the optimisation, since Explorer asks for icons far more often than
full-resolution renders.

## Building

Needs Visual Studio 2022 (or Build Tools) with the C++ workload. Nothing else —
no vcpkg, no external libraries.

```powershell
.\Build.ps1
```

Add `-Tests` to build the test harnesses too. Output lands at
`PsdThumbnailProvider\x64\Release\PsdThumbnailProvider.dll`.

The MSI in `Setup\` additionally needs the [WiX Toolset](http://wixtoolset.org/).

## Installing

From an **elevated** PowerShell:

```powershell
.\Install.ps1
```

This copies the DLL to `%ProgramFiles%\PsdPreview`, registers it with `regsvr32`
(the DLL self-registers both handlers), and clears the thumbnail cache. Then restart
Explorer:

```powershell
Stop-Process -Name explorer -Force
```

To remove it:

```powershell
.\Uninstall.ps1
```

Use `-InPlace` on `Install.ps1` to register the build output where it sits, which is
handier while developing. Note that `prevhost.exe` caches the DLL — kill it after
rebuilding, or the old copy stays loaded.

> **If upstream's installer is already present**, uninstall it first. This fork reuses
> its thumbnail CLSID `{5BB45D32-...}`, so both would fight over the same registration.
> The MSI keeps upstream's `UpgradeCode` and will upgrade an existing install in place.

### What gets registered

| Key | Purpose |
|---|---|
| `HKCR\CLSID\{5BB45D32-…}` | Thumbnail provider, in-process |
| `HKCR\CLSID\{C4A4EC15-…}` | Preview handler, `AppID` = prevhost surrogate |
| `HKCR\.psd\shellex\{e357fccd-…}` | Thumbnail slot (also `.psb`) |
| `HKCR\.psd\shellex\{8895b1c6-…}` | Preview slot (also `.psb`) |
| `HKCR\SystemFileAssociations\.psd\…` | Both slots again, so it survives a ProgID owning the extension |
| `HKLM\…\PreviewHandlers` | Where Explorer looks preview handlers up |

The preview handler runs **out of process** in `prevhost.exe` via its AppID, so a
malformed file cannot take Explorer down with it.

## Testing

Four harnesses in `Test\`, all built by `.\Build.ps1 -Tests`:

| Harness | What it does |
|---|---|
| `decode_test.exe` | Decodes every `*.psd` / `*.psb` in the folder at 96/256/1024/full, reports size, path taken and timing, writes PNGs |
| `compare_test.exe` | Runs **upstream's original decoder** and this one over the same files and reports per-pixel differences |
| `preview_test.exe` | Drives the full `IPreviewHandler` lifecycle in-process and captures what it paints to PNG — no registration needed |
| `dll_test.exe` | Loads the **built DLL** and exercises its COM surface the way the shell does: both class factories, every required interface, and a negative CLSID check |

`gen_psd.py` generates synthetic coverage files. The checked-in corpus is all 8-bit
RGB, which exercises none of the CMYK / 16-bit / indexed / grayscale / float paths:

```powershell
python gen_psd.py
```

### Current regression status

`compare_test.exe` over 18 files:

- **13 pixel-exact** against upstream
- **2 close** (`gen_big_*`) — supersampled Fant filtering vs upstream's `AlphaBlend` shrink
- **2 divergent** (`test3.psd`, `test7.psb`) — verified by eye as *this* decoder being
  better: upstream serves a lossy JPEG where a lossless composite is available, and
  point-drops pixels when downscaling
- **1 upstream failure** (`gen_float32.psd`) — 32-bit float, which upstream rejects

The float path is self-checking: `gen_float32.psd` and `gen_rgb8.psd` hold the same
content, and decode **pixel-identically** (mean diff 0.000), confirming the
linear → sRGB conversion.

## How it works

```
PsdDecoder.cpp      parsing, row-skipping composite decode, WIC scaling
GetThumbnail.cpp    thin IThumbnailProvider wrapper over the decoder
PsdPreviewHandler   IPreviewHandler + window, painting, alpha compositing
ClassFactory        produces either COM class by CLSID
dllmain.cpp         exports, routing, self-registration
```

Decode path selection, in order:

1. Embedded preview (ID 1036/1033) — only if the document is ≥ 16 MP and the request
   fits inside the stored preview
2. Composite image, decimated by a power-of-two `step` chosen so the decode lands at
   roughly 2× the requested size, box-filtered horizontally, Fant-filtered down
3. Embedded preview as fallback, if the composite is absent, blank or an unsupported mode

A fully white composite is treated as "no usable merged image" and falls through to
step 3 — some files store a blank composite when saved without maximised compatibility.

## Licence

MIT, as upstream. See `LICENSE`.

Credits: [Agamnentzar](https://github.com/Agamnentzar/psd-thumbnail-provider) for the
original provider, parsing and the CMYK→BGR SWOP approximation;
[nurfarhad](https://github.com/nurfarhad/psd-thumbnail-provider) for the scanline
prefix-offset row-skipping technique.
