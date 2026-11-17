// PSD/PSB decoder shared by the thumbnail provider and the preview handler.
//
// Parsing, colour-mode coverage and the CMYK->BGR approximation are derived from
// Agamnentzar/psd-thumbnail-provider (MIT). The scanline prefix-offset table that
// lets the composite decoder seek past rows it does not need is derived from
// nurfarhad/psd-thumbnail-provider (MIT).

#include "PsdDecoder.h"

#include <Shlwapi.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "Shlwapi.lib")

namespace {

// Sanity limits. Photoshop caps PSD at 30000px and PSB at 300000px per side.
const int kMaxDimPsd = 30000;
const int kMaxDimPsb = 300000;
// Never allocate a decode target larger than this on either side.
const uint32_t kMaxDecodeEdge = 8192;
// PackBits can expand at most ~1:1 plus per-run headers.
const uint32_t kRleRowSlack = 16;
// Decode to this multiple of the requested size before filtering down.
const uint32_t kSupersample = 2;
// The embedded preview is lossy JPEG, so it is only worth preferring over a
// lossless composite decode once the document is big enough that skipping the
// composite is a real latency win. Below this, always decode the composite.
const uint64_t kPreferEmbeddedPixels = 16ull * 1000 * 1000;

// ---------------------------------------------------------------- stream io

bool ReadExact(IStream* stream, void* data, ULONG length) {
	ULONG total = 0;
	while (total < length) {
		ULONG read = 0;
		HRESULT hr = stream->Read(static_cast<BYTE*>(data) + total, length - total, &read);
		if (FAILED(hr) || read == 0) return false;
		total += read;
	}
	return true;
}

struct Reader {
	IStream* s;
	bool ok = true;

	explicit Reader(IStream* stream) : s(stream) {}

	bool read(void* p, ULONG n) {
		if (!ok) return false;
		ok = ReadExact(s, p, n);
		return ok;
	}

	uint8_t u8() { uint8_t b = 0; read(&b, 1); return b; }

	uint16_t u16() {
		BYTE b[2] = {};
		read(b, 2);
		return static_cast<uint16_t>((b[0] << 8) | b[1]);
	}

	uint32_t u32() {
		BYTE b[4] = {};
		read(b, 4);
		return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
			(static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
	}

	uint64_t u64() {
		BYTE b[8] = {};
		read(b, 8);
		uint64_t v = 0;
		for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
		return v;
	}

	// Section lengths are 4 bytes in PSD (version 1) and 8 bytes in PSB (version 2).
	uint64_t section(int version) { return version == 1 ? static_cast<uint64_t>(u32()) : u64(); }

	uint64_t tell() {
		LARGE_INTEGER zero = {};
		ULARGE_INTEGER pos = {};
		if (FAILED(s->Seek(zero, STREAM_SEEK_CUR, &pos))) ok = false;
		return pos.QuadPart;
	}

	bool seekTo(uint64_t offset) {
		if (!ok) return false;
		LARGE_INTEGER li;
		li.QuadPart = static_cast<LONGLONG>(offset);
		if (FAILED(s->Seek(li, STREAM_SEEK_SET, NULL))) { ok = false; return false; }
		return true;
	}

	bool skip(int64_t count) {
		if (!ok || count <= 0) return ok;
		LARGE_INTEGER li;
		li.QuadPart = count;
		if (FAILED(s->Seek(li, STREAM_SEEK_CUR, NULL))) { ok = false; return false; }
		return true;
	}

	uint64_t size() {
		STATSTG st = {};
		if (FAILED(s->Stat(&st, STATFLAG_NONAME))) return 0;
		return st.cbSize.QuadPart;
	}
};

// ------------------------------------------------------------------ colour

// 32-bit PSD samples are linear-light floats, not integers: the high byte is
// the sign and exponent, so it cannot be used as an 8-bit value. Convert
// properly through a small sRGB table.
struct SrgbTable {
	BYTE v[4096];
	SrgbTable() {
		for (int i = 0; i < 4096; i++) {
			const double lin = i / 4095.0;
			const double s = (lin <= 0.0031308)
				? lin * 12.92
				: 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
			int b = static_cast<int>(s * 255.0 + 0.5);
			v[i] = static_cast<BYTE>(b < 0 ? 0 : (b > 255 ? 255 : b));
		}
	}
};

const SrgbTable& Srgb() {
	static const SrgbTable table;
	return table;
}

inline BYTE LinearFloatToSrgb8(const BYTE* p) {
	const uint32_t bits = (static_cast<uint32_t>(p[0]) << 24) |
		(static_cast<uint32_t>(p[1]) << 16) |
		(static_cast<uint32_t>(p[2]) << 8) | p[3];
	float f;
	memcpy(&f, &bits, 4);
	if (!(f > 0.0f)) return 0;       // also catches NaN
	if (f >= 1.0f) return 255;
	return Srgb().v[static_cast<int>(f * 4095.0f + 0.5f)];
}

inline BYTE ClampToByte(double value) {
	value = std::floor(value + 0.5);
	return value <= 0 ? 0 : (value >= 255 ? 255 : static_cast<BYTE>(value));
}

// Converts CMYK (0 = 100% ink, as Photoshop stores it) to BGR using an
// approximation of the U.S. Web Coated (SWOP) v2 profile.
void CmykToBgr(const BYTE* cmyk, BYTE* bgr) {
	double c = 1 - cmyk[0] / 255.0;
	double m = 1 - cmyk[1] / 255.0;
	double y = 1 - cmyk[2] / 255.0;
	double k = 1 - cmyk[3] / 255.0;

	bgr[2] = ClampToByte(255 +
		c * (-4.387332384609988 * c + 54.48615194189176 * m + 18.82290502165302 * y + 212.25662451639585 * k - 285.2331026137004) +
		m * (1.7149763477362134 * m - 5.6096736904047315 * y - 17.873870861415444 * k - 5.497006427196366) +
		y * (-2.5217340131683033 * y - 21.248923337353073 * k + 17.5119270841813) +
		k * (-21.86122147463605 * k - 189.48180835922747));
	bgr[1] = ClampToByte(255 +
		c * (8.841041422036149 * c + 60.118027045597366 * m + 6.871425592049007 * y + 31.159100130055922 * k - 79.2970844816548) +
		m * (-15.310361306967817 * m + 17.575251261109482 * y + 131.35250912493976 * k - 190.9453302588951) +
		y * (4.444339102852739 * y + 9.8632861493405 * k - 24.86741582555878) +
		k * (-20.737325471181034 * k - 187.80453709719578));
	bgr[0] = ClampToByte(255 +
		c * (0.8842522430003296 * c + 8.078677503112928 * m + 30.89978309703729 * y - 0.23883238689178934 * k - 14.183576799673286) +
		m * (10.49593273432072 * m + 63.02378494754052 * y + 50.606957656360734 * k - 112.23884253719248) +
		y * (0.03296041114873217 * y + 115.60384449646641 * k - 193.58209356861505) +
		k * (-22.33816807309886 * k - 180.12613974708367));
}

// ------------------------------------------------------------------ layout

struct PsdHeader {
	int version = 1;
	uint16_t channels = 0;
	int width = 0;
	int height = 0;
	uint16_t depth = 0;
	uint16_t colorMode = 0;

	bool hasPalette = false;
	BYTE palette[768] = {};
	int transparencyIndex = -1;

	// Embedded preview resource (1036 preferred, 1033 legacy).
	uint64_t thumbOffset = 0;
	uint32_t thumbLength = 0;
	uint16_t thumbResourceId = 0;
	int thumbWidth = 0;
	int thumbHeight = 0;

	uint64_t compositeOffset = 0; // offset of the compression word
	bool hasComposite = false;
};

// Reads everything up to (but not including) the composite image data.
bool ParseStructure(Reader& r, PsdHeader& h) {
	if (!r.seekTo(0)) return false;

	if (r.u32() != 0x38425053) return false; // '8BPS'
	h.version = r.u16();
	if (h.version != 1 && h.version != 2) return false;

	r.skip(6); // reserved
	h.channels = r.u16();
	h.height = static_cast<int>(r.u32());
	h.width = static_cast<int>(r.u32());
	h.depth = r.u16();
	h.colorMode = r.u16();

	if (!r.ok) return false;

	const int maxDim = h.version == 1 ? kMaxDimPsd : kMaxDimPsb;
	if (h.width <= 0 || h.height <= 0 || h.width > maxDim || h.height > maxDim) return false;
	if (h.channels == 0 || h.channels > 56) return false;

	// Colour mode data: for indexed images this is a 768-byte RGB palette.
	uint64_t colorModeLength = r.u32();
	h.hasPalette = (h.colorMode == 2 && colorModeLength == 768);
	if (h.hasPalette) {
		r.read(h.palette, 768);
	} else {
		r.skip(static_cast<int64_t>(colorModeLength));
	}
	if (!r.ok) return false;

	// Image resources.
	uint64_t resourcesLength = r.u32();
	uint64_t resourceStart = r.tell();
	uint64_t resourceEnd = resourceStart + resourcesLength;
	const uint64_t streamSize = r.size();
	if (streamSize && resourceEnd > streamSize) return false;

	while (r.ok && r.tell() + 12 <= resourceEnd) {
		uint32_t sig = r.u32();
		if (sig != 0x3842494D) break; // '8BIM'

		uint16_t id = r.u16();

		uint8_t nameLength = r.u8();
		// Pascal string padded to an even total length (including the length byte).
		r.skip((nameLength + 1) % 2 ? nameLength + 1 : nameLength);

		uint64_t length = r.u32();
		uint64_t offset = r.tell();
		if (!r.ok) break;
		if (streamSize && offset + length > streamSize) break;

		if ((id == 1036 || id == 1033) && length > 28) {
			// Prefer 1036; only fall back to 1033 when 1036 has not been seen.
			if (h.thumbResourceId != 1036) {
				r.u32(); // format: 0 = raw RGB, 1 = JPEG
				uint32_t tw = r.u32();
				uint32_t th = r.u32();
				if (r.ok && tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
					h.thumbResourceId = id;
					h.thumbOffset = offset;
					h.thumbLength = static_cast<uint32_t>(length);
					h.thumbWidth = static_cast<int>(tw);
					h.thumbHeight = static_cast<int>(th);
				}
			}
		} else if (id == 1047 && length >= 2) {
			h.transparencyIndex = r.u16();
		}

		r.seekTo(offset + length);
		if (r.tell() % 2) r.skip(1); // resources are padded to even sizes
	}

	if (!r.seekTo(resourceEnd)) return false;

	// Layer and mask information - skipped wholesale; we only want the composite.
	uint64_t layerMaskLength = r.section(h.version);
	if (!r.skip(static_cast<int64_t>(layerMaskLength))) return false;

	h.compositeOffset = r.tell();
	h.hasComposite = r.ok && (!streamSize || h.compositeOffset + 2 <= streamSize);
	return true;
}

// ------------------------------------------------------- composite decoding

struct ChannelPlan {
	int colorChannels = 3;
	int readCount = 4;      // channels actually read from the file
	bool grayscale = false;
	bool indexed = false;
	bool cmyk = false;
	bool supported = false;
};

ChannelPlan PlanChannels(const PsdHeader& h) {
	ChannelPlan p;
	p.grayscale = (h.colorMode == 1);
	p.indexed = (h.colorMode == 2);
	p.cmyk = (h.colorMode == 4);

	// Duotone (8) stores a single channel just like grayscale.
	const bool duotone = (h.colorMode == 8);
	if (duotone) p.grayscale = true;

	p.colorChannels = (p.grayscale || p.indexed) ? 1 : (p.cmyk ? 4 : 3);

	const bool modeOk = h.colorMode == 3 || p.cmyk || p.grayscale || duotone ||
		(p.indexed && h.hasPalette);
	const bool depthOk = h.depth == 8 || (h.depth == 16 && !p.indexed) ||
		(h.depth == 32 && !p.indexed);

	p.supported = modeOk && depthOk && h.channels >= p.colorChannels;

	// The first extra channel past the colour channels is transparency.
	// Indexed images signal transparency through a palette index instead.
	p.readCount = (std::min)(static_cast<int>(h.channels),
		p.indexed ? 1 : p.colorChannels + 1);
	return p;
}

// Decodes one PackBits row. Stops once `count` output bytes exist.
void DecodeRleRow(const BYTE* src, uint32_t length, BYTE* dst, uint32_t count) {
	uint32_t i = 0, x = 0;
	while (i < length && x < count) {
		BYTE header = src[i++];
		if (header > 128) {
			if (i >= length) break;
			BYTE value = src[i++];
			uint32_t n = (std::min)(static_cast<uint32_t>(257 - header), count - x);
			memset(dst + x, value, n);
			x += n;
		} else if (header < 128) {
			uint32_t n = (std::min)((std::min)(static_cast<uint32_t>(header) + 1, length - i), count - x);
			memcpy(dst + x, src + i, n);
			x += n;
			i += n;
		}
		// 128 is a no-op
	}
}

// Picks a power-of-two decimation factor. Vertical decimation is point
// sampling, so we deliberately stop while the decoded image is still 2-4x the
// requested size: that leaves the Fant filter real data to work with and keeps
// the result close to a full-resolution decode, while still skipping the vast
// majority of rows.
uint32_t ChooseStep(int width, int height, UINT maxEdge) {
	if (maxEdge == 0) return 1;
	const uint32_t maxDim = static_cast<uint32_t>((std::max)(width, height));
	const uint32_t target = maxEdge * kSupersample;
	uint32_t step = 1;
	while (maxDim / (step * 2) >= target && step * 2 <= 512) step *= 2;
	// Always decimate enough to stay under the allocation ceiling.
	while (maxDim / step > kMaxDecodeEdge && step < 4096) step *= 2;
	return step;
}

// Reads `plan.readCount` composite channels into an interleaved byte buffer,
// decimating by `step` and box-averaging horizontally within each step column.
bool ReadComposite(Reader& r, const PsdHeader& h, const ChannelPlan& plan,
	uint16_t compression, uint32_t step, uint32_t outW, uint32_t outH,
	std::vector<BYTE>& comp) {
	const uint32_t width = static_cast<uint32_t>(h.width);
	const uint32_t height = static_cast<uint32_t>(h.height);
	const uint32_t bytesPerChannel = h.depth / 8;
	const uint32_t rowSize = width * bytesPerChannel;
	const int stride = plan.readCount;

	if (rowSize == 0) return false;

	comp.assign(static_cast<size_t>(outW) * outH * stride, 0xFF);

	std::vector<BYTE> row(rowSize);
	std::vector<BYTE> row8(bytesPerChannel == 1 ? 0 : width);
	const uint64_t dataStart = r.tell();
	const uint64_t streamSize = r.size();

	std::vector<uint64_t> rowOffsets;
	std::vector<uint32_t> rowLengths;
	uint64_t rleStart = 0;

	if (compression == 1) {
		const size_t numRows = static_cast<size_t>(height) * h.channels;
		const size_t entrySize = (h.version == 2) ? 4 : 2;
		if (numRows > (64u << 20) / entrySize) return false;

		std::vector<BYTE> counts(numRows * entrySize);
		if (!r.read(counts.data(), static_cast<ULONG>(counts.size()))) return false;

		rowOffsets.resize(numRows);
		rowLengths.resize(numRows);
		uint64_t running = 0;
		for (size_t i = 0; i < numRows; i++) {
			uint32_t len = (h.version == 2)
				? ((static_cast<uint32_t>(counts[i * 4]) << 24) | (static_cast<uint32_t>(counts[i * 4 + 1]) << 16) |
				   (static_cast<uint32_t>(counts[i * 4 + 2]) << 8) | counts[i * 4 + 3])
				: ((static_cast<uint32_t>(counts[i * 2]) << 8) | counts[i * 2 + 1]);
			rowLengths[i] = len;
			rowOffsets[i] = running;
			running += len;
		}
		rleStart = dataStart + counts.size();
		if (streamSize && rleStart + running > streamSize) return false;
	}

	const uint32_t maxRowLen = rowSize + rowSize / 2 + kRleRowSlack;
	std::vector<BYTE> packed;

	for (int c = 0; c < plan.readCount; c++) {
		for (uint32_t y = 0; y < height; y += step) {
			const uint32_t outY = y / step;
			if (outY >= outH) break;

			if (compression == 1) {
				const size_t index = static_cast<size_t>(c) * height + y;
				if (index >= rowLengths.size()) return false;
				const uint32_t len = rowLengths[index];
				if (len > maxRowLen) return false;
				if (len > packed.size()) packed.resize(len);

				if (!r.seekTo(rleStart + rowOffsets[index])) return false;
				if (len && !r.read(packed.data(), len)) return false;

				memset(row.data(), 0, rowSize);
				DecodeRleRow(packed.data(), len, row.data(), rowSize);
			} else {
				const uint64_t offset = dataStart +
					(static_cast<uint64_t>(c) * height + y) * rowSize;
				if (streamSize && offset + rowSize > streamSize) return false;
				if (!r.seekTo(offset)) return false;
				if (!r.read(row.data(), rowSize)) return false;
			}

			// Normalise the row to 8 bits once, so the filter below stays a
			// tight branch-free loop regardless of the source depth.
			const BYTE* samples = row.data();
			if (bytesPerChannel == 2) {
				for (uint32_t x = 0; x < width; x++) row8[x] = row[x * 2]; // high byte
				samples = row8.data();
			} else if (bytesPerChannel == 4) {
				for (uint32_t x = 0; x < width; x++) row8[x] = LinearFloatToSrgb8(&row[x * 4]);
				samples = row8.data();
			}

			// Horizontal box filter across each step-wide column. Vertical
			// decimation is point sampling - averaging there would defeat the
			// row skipping that makes this fast.
			BYTE* dst = comp.data() + (static_cast<size_t>(outY) * outW) * stride + c;
			for (uint32_t ox = 0; ox < outW; ox++, dst += stride) {
				const uint32_t x0 = ox * step;
				uint32_t sum = 0, n = 0;
				for (uint32_t x = x0; x < x0 + step && x < width; x++, n++) {
					sum += samples[x];
				}
				*dst = n ? static_cast<BYTE>(sum / n) : 0;
			}
		}
	}

	return true;
}

bool DecodeComposite(Reader& r, const PsdHeader& h, UINT maxEdge, PsdImage& out) {
	if (!h.hasComposite) return false;

	const ChannelPlan plan = PlanChannels(h);
	if (!plan.supported) return false;

	if (!r.seekTo(h.compositeOffset)) return false;
	const uint16_t compression = r.u16();
	if (!r.ok) return false;
	// 2 and 3 are ZIP variants, which Photoshop does not use for the composite.
	if (compression != 0 && compression != 1) return false;

	const uint32_t step = ChooseStep(h.width, h.height, maxEdge);
	const uint32_t outW = (static_cast<uint32_t>(h.width) + step - 1) / step;
	const uint32_t outH = (static_cast<uint32_t>(h.height) + step - 1) / step;
	if (outW == 0 || outH == 0 || outW > kMaxDecodeEdge || outH > kMaxDecodeEdge) return false;

	std::vector<BYTE> comp;
	if (!ReadComposite(r, h, plan, compression, step, outW, outH, comp)) return false;

	const size_t pixelCount = static_cast<size_t>(outW) * outH;
	out.pixels.assign(pixelCount, 0xFF000000);
	BYTE* data = reinterpret_cast<BYTE*>(out.pixels.data());
	const int stride = plan.readCount;
	const bool hasAlphaChannel = !plan.indexed && plan.readCount > plan.colorChannels;

	bool allWhite = true;

	for (size_t i = 0; i < pixelCount; i++) {
		const BYTE* src = comp.data() + i * stride;
		BYTE* dst = data + i * 4;

		if (plan.indexed) {
			const BYTE index = src[0];
			if (h.transparencyIndex >= 0 && index == h.transparencyIndex) {
				memset(dst, 0, 4);
			} else {
				dst[0] = h.palette[512 + index];
				dst[1] = h.palette[256 + index];
				dst[2] = h.palette[index];
				dst[3] = 0xFF;
			}
		} else if (plan.cmyk) {
			CmykToBgr(src, dst);
			dst[3] = hasAlphaChannel ? src[4] : 0xFF;
		} else if (plan.grayscale) {
			dst[0] = dst[1] = dst[2] = src[0];
			dst[3] = hasAlphaChannel ? src[1] : 0xFF;
		} else {
			dst[2] = src[0];
			dst[1] = src[1];
			dst[0] = src[2];
			dst[3] = hasAlphaChannel ? src[3] : 0xFF;
		}

		if (allWhite && (dst[0] != 0xFF || dst[1] != 0xFF || dst[2] != 0xFF || dst[3] != 0xFF))
			allWhite = false;
	}

	// A fully white composite usually means the file stores no usable merged
	// image; let the caller fall back to the embedded preview.
	if (allWhite) return false;

	out.width = static_cast<int>(outW);
	out.height = static_cast<int>(outH);
	out.fullWidth = h.width;
	out.fullHeight = h.height;
	out.hasAlpha = hasAlphaChannel || plan.indexed;
	out.fromEmbedded = false;
	return true;
}

// ------------------------------------------------------- embedded preview

IWICImagingFactory* GetWicFactory() {
	static IWICImagingFactory* factory = NULL;
	if (!factory) {
		IWICImagingFactory* created = NULL;
		if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&created)))) {
			if (InterlockedCompareExchangePointer(reinterpret_cast<void**>(&factory), created, NULL))
				created->Release();
		}
	}
	return factory;
}

bool DecodeEmbedded(Reader& r, const PsdHeader& h, PsdImage& out) {
	if (!h.thumbOffset || h.thumbLength <= 28) return false;

	IWICImagingFactory* factory = GetWicFactory();
	if (!factory) return false;

	if (!r.seekTo(h.thumbOffset)) return false;
	const uint32_t format = r.u32();
	r.skip(24); // width, height, widthbytes, totalsize, sizeafter, bitspixel, planes
	if (!r.ok) return false;

	const uint32_t payload = h.thumbLength - 28;
	std::vector<BYTE> buffer(payload);
	if (!r.read(buffer.data(), payload)) return false;

	if (format != 1) return false; // only JPEG-compressed previews are stored in practice

	IStream* memory = SHCreateMemStream(buffer.data(), payload);
	if (!memory) return false;

	bool result = false;
	IWICBitmapDecoder* decoder = NULL;
	IWICBitmapFrameDecode* frame = NULL;
	IWICFormatConverter* converter = NULL;

	if (SUCCEEDED(factory->CreateDecoderFromStream(memory, NULL, WICDecodeMetadataCacheOnDemand, &decoder)) &&
		SUCCEEDED(decoder->GetFrame(0, &frame)) &&
		SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
		SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
			WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
		UINT w = 0, hgt = 0;
		if (SUCCEEDED(converter->GetSize(&w, &hgt)) && w > 0 && hgt > 0) {
			out.pixels.resize(static_cast<size_t>(w) * hgt);
			if (SUCCEEDED(converter->CopyPixels(NULL, w * 4,
				static_cast<UINT>(out.pixels.size() * 4),
				reinterpret_cast<BYTE*>(out.pixels.data())))) {
				out.width = static_cast<int>(w);
				out.height = static_cast<int>(hgt);
				out.fullWidth = h.width;
				out.fullHeight = h.height;
				out.hasAlpha = false;
				out.fromEmbedded = true;

				// Resource 1033 stores its channels in BGR order.
				if (h.thumbResourceId == 1033) {
					for (auto& px : out.pixels) {
						const uint32_t b = (px & 0x00FF0000) >> 16;
						const uint32_t rr = (px & 0x000000FF) << 16;
						px = (px & 0xFF00FF00) | b | rr;
					}
				}
				result = true;
			}
		}
	}

	if (converter) converter->Release();
	if (frame) frame->Release();
	if (decoder) decoder->Release();
	memory->Release();
	return result;
}

} // namespace

// ------------------------------------------------------------------ public

bool ReadPsdSize(IStream* stream, int* width, int* height) {
	if (!stream) return false;
	Reader r(stream);
	PsdHeader h;
	if (!ParseStructure(r, h)) return false;
	if (width) *width = h.width;
	if (height) *height = h.height;
	return true;
}

bool DecodePsdImage(IStream* stream, UINT maxEdge, PsdImage& out) {
	if (!stream) return false;

	Reader r(stream);
	PsdHeader h;
	if (!ParseStructure(r, h)) return false;

	// The embedded preview is a lossy JPEG capped at 256px, so it can only
	// serve small requests - and it is only worth preferring over a lossless
	// composite decode when the document is large enough that skipping the
	// composite actually saves time.
	const uint64_t docPixels =
		static_cast<uint64_t>(h.width) * static_cast<uint64_t>(h.height);
	const bool embeddedFits = h.thumbOffset != 0 && maxEdge != 0 &&
		static_cast<int>(maxEdge) <= (std::max)(h.thumbWidth, h.thumbHeight);
	const bool preferEmbedded = embeddedFits && docPixels >= kPreferEmbeddedPixels;

	const bool ok =
		(preferEmbedded && DecodeEmbedded(r, h, out)) ||
		DecodeComposite(r, h, maxEdge, out) ||
		(!preferEmbedded && DecodeEmbedded(r, h, out));

	if (ok) {
		out.depth = h.depth;
		out.colorMode = h.colorMode;
	}
	return ok;
}

bool ScalePsdImage(const PsdImage& src, int w, int h, std::vector<uint32_t>& dst) {
	if (!src.valid() || w <= 0 || h <= 0) return false;

	dst.assign(static_cast<size_t>(w) * h, 0);

	if (w == src.width && h == src.height) {
		memcpy(dst.data(), src.pixels.data(), dst.size() * 4);
		return true;
	}

	const UINT srcStride = static_cast<UINT>(src.width) * 4;
	const UINT dstStride = static_cast<UINT>(w) * 4;
	const UINT dstSize = dstStride * static_cast<UINT>(h);

	IWICImagingFactory* factory = GetWicFactory();
	IWICBitmap* source = NULL;
	IWICBitmapScaler* scaler = NULL;
	bool scaled = false;

	if (factory &&
		SUCCEEDED(factory->CreateBitmapFromMemory(src.width, src.height,
			GUID_WICPixelFormat32bppBGRA, srcStride,
			static_cast<UINT>(src.pixels.size() * 4),
			const_cast<BYTE*>(reinterpret_cast<const BYTE*>(src.pixels.data())), &source)) &&
		SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
		SUCCEEDED(scaler->Initialize(source, w, h, WICBitmapInterpolationModeFant))) {
		scaled = SUCCEEDED(scaler->CopyPixels(NULL, dstStride, dstSize,
			reinterpret_cast<BYTE*>(dst.data())));
	}

	if (scaler) scaler->Release();
	if (source) source->Release();

	if (!scaled) {
		// Nearest-neighbour fallback if WIC is unavailable.
		for (int y = 0; y < h; y++) {
			const int sy = static_cast<int>(static_cast<int64_t>(y) * src.height / h);
			const uint32_t* srcRow = src.pixels.data() + static_cast<size_t>(sy) * src.width;
			uint32_t* dstRow = dst.data() + static_cast<size_t>(y) * w;
			for (int x = 0; x < w; x++) {
				const int sx = static_cast<int>(static_cast<int64_t>(x) * src.width / w);
				dstRow[x] = srcRow[sx];
			}
		}
	}

	return true;
}

HBITMAP CreateDibFromImage(const PsdImage& img, UINT maxEdge) {
	if (!img.valid()) return NULL;

	int targetW = img.width;
	int targetH = img.height;

	if (maxEdge > 0 && (img.width > static_cast<int>(maxEdge) || img.height > static_cast<int>(maxEdge))) {
		if (img.width >= img.height) {
			targetW = static_cast<int>(maxEdge);
			targetH = (std::max)(1, static_cast<int>(
				static_cast<int64_t>(img.height) * maxEdge / img.width));
		} else {
			targetH = static_cast<int>(maxEdge);
			targetW = (std::max)(1, static_cast<int>(
				static_cast<int64_t>(img.width) * maxEdge / img.height));
		}
	}

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = targetW;
	bmi.bmiHeader.biHeight = -targetH; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* bits = NULL;
	HBITMAP bitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bitmap || !bits) {
		if (bitmap) DeleteObject(bitmap);
		return NULL;
	}

	std::vector<uint32_t> scaled;
	if (!ScalePsdImage(img, targetW, targetH, scaled)) {
		DeleteObject(bitmap);
		return NULL;
	}

	memcpy(bits, scaled.data(), scaled.size() * 4);
	return bitmap;
}
