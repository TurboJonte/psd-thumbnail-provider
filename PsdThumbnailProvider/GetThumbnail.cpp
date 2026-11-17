#include "GetThumbnail.h"
#include "PsdDecoder.h"

HBITMAP GetPSDThumbnail(IStream* stream, UINT cx) {
	if (!stream) return NULL;
	if (cx == 0) cx = 256;

	PsdImage image;
	if (!DecodePsdImage(stream, cx, image)) return NULL;

	return CreateDibFromImage(image, cx);
}
