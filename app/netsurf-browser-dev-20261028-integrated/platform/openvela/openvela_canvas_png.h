#ifndef OV_CANVAS_PNG_H
#define OV_CANVAS_PNG_H
/* The canvas.toDataURL() path only needs a PNG encoder.  This tiny
 * lodepng-compatible subset is implemented in src/nsw_png.c using zlib. */
#include <stddef.h>

unsigned ov_canvas_png_encode32(unsigned char **out, size_t *outsize,
			  const unsigned char *image, unsigned w, unsigned h);
const char *ov_canvas_png_error_text(unsigned code);

#endif
