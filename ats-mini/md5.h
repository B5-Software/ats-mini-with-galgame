#pragma once
#include <stdint.h>
#include <stddef.h>

// Minimal MD5 wrapper using mbedTLS (ESP32 ROM / IDF provides it)
extern "C" {
#include "mbedtls/md5.h"
}

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mbedtls_md5_context ctx;
} MD5_CTX;

void MD5Init(MD5_CTX *ctx);
void MD5Update(MD5_CTX *ctx, const unsigned char *input, unsigned int len);
void MD5Final(unsigned char digest[16], MD5_CTX *ctx);

#ifdef __cplusplus
}
#endif
