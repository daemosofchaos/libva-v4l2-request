#include "tiled_yuv.h"

void tiled_to_planar(void *src, void *dst, unsigned int dst_pitch,
                     unsigned int width, unsigned int height)
{
    // Naive (slow) fallback: just memcpy row by row assuming src is already planar.
    unsigned char *in = src, *out = dst;
    for (unsigned int y = 0; y < height; ++y) {
        memcpy(out, in, width);
        in += width;
        out += dst_pitch;
    }
}
