#ifndef PLAT_COMMON_H
#define PLAT_COMMON_H
#include "zw.h"
int host_key_by_name(const char *s, int *mods);
int host_save_pbm(const char *path, const uint8_t *fb);
int host_load_wikifont(const char *argv0);
#endif
