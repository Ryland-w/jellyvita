#pragma once
#include <vita2d.h>

void img_init(void);
/* Returns a ready texture, or NULL while it loads in the background. Main thread only. */
vita2d_texture *img_get(const char *id, int maxw);
