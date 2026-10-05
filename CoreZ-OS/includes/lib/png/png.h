#ifndef LIB_PNG_PNG_H
#define LIB_PNG_PNG_H

#include <stdint.h>

struct PNG_IMAGE {
    int w;
    int h;
    uint32_t *pixels;
};

#define PNG_OK 0
#define PNG_ERR_FORMAT (-1)
#define PNG_ERR_UNSUPPORTED (-2)
#define PNG_ERR_NOMEM (-3)
#define PNG_ERR_DATA (-4)

int png_decode(const void *data, uint32_t len, struct PNG_IMAGE *out);
void png_image_free(struct PNG_IMAGE *img);
int png_probe(const void *data, uint32_t len, int *w, int *h, int *depth,
              int *color);

#endif
