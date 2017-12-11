#ifndef BLKMK_PRIVATE_H
#define BLKMK_PRIVATE_H

#include <stdbool.h>
#include <string.h>

// blkmaker.c
extern bool _blkmk_dblsha256(void *hash, const void *data, size_t datasz);

// hex.c
extern void _blkmk_bin2hex(char *out, const void *data, size_t datasz);
extern bool _blkmk_hex2bin(void *o, const char *x, size_t len);

// base58.c
extern bool _blkmk_b58tobin(void *bin, size_t binsz, const char *b58, size_t b58sz);
extern int _blkmk_b58check(void *bin, size_t binsz, const char *b58);

#endif
