/* Minimal PNG encoder (RGBA8, no interlace) used by canvas.toDataURL().
 * Compatible with the two lodepng entry points qjs_canvas.c calls. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

#include "openvela_canvas_png.h"

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static unsigned char *chunk(unsigned char *out, const char type[4],
			    const unsigned char *data, uint32_t length)
{
	put32(out, length);
	memcpy(out + 4, type, 4);
	if (length)
		memcpy(out + 8, data, length);
	uLong crc = crc32(0, (const Bytef *)type, 4);
	if (length)
		crc = crc32(crc, data, length);
	put32(out + 8 + length, (uint32_t)crc);
	return out + 12 + length;
}

unsigned ov_canvas_png_encode32(unsigned char **out, size_t *outsize,
			  const unsigned char *image, unsigned w, unsigned h)
{
	*out = NULL;
	*outsize = 0;
	if (w == 0 || h == 0 || w > 0x7fffffffu / 4 || h > 0x7fffffffu)
		return 93;

	size_t stride = (size_t)w * 4;
	size_t raw_size = (stride + 1) * h;
	unsigned char *raw = malloc(raw_size);
	if (raw == NULL)
		return 83;
	for (unsigned y = 0; y < h; y++) {
		raw[y * (stride + 1)] = 0; /* filter: none */
		memcpy(raw + y * (stride + 1) + 1, image + y * stride, stride);
	}

	uLongf packed_size = compressBound((uLong)raw_size);
	unsigned char *packed = malloc(packed_size);
	if (packed == NULL) {
		free(raw);
		return 83;
	}
	if (compress2(packed, &packed_size, raw, (uLong)raw_size, 6) != Z_OK) {
		free(raw);
		free(packed);
		return 111;
	}
	free(raw);

	size_t total = 8 + (12 + 13) + (12 + packed_size) + 12;
	unsigned char *png = malloc(total);
	if (png == NULL) {
		free(packed);
		return 83;
	}
	static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	memcpy(png, signature, 8);
	unsigned char header[13];
	put32(header, w);
	put32(header + 4, h);
	header[8] = 8;  /* bit depth */
	header[9] = 6;  /* RGBA */
	header[10] = header[11] = header[12] = 0;
	unsigned char *p = png + 8;
	p = chunk(p, "IHDR", header, 13);
	p = chunk(p, "IDAT", packed, (uint32_t)packed_size);
	p = chunk(p, "IEND", NULL, 0);
	free(packed);

	*out = png;
	*outsize = (size_t)(p - png);
	return 0;
}

const char *ov_canvas_png_error_text(unsigned code)
{
	switch (code) {
	case 0: return "no error";
	case 83: return "memory allocation failed";
	case 93: return "invalid image dimensions";
	default: return "PNG encoding failed";
	}
}
