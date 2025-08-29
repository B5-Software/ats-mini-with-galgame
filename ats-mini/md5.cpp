#include "md5.h"

extern "C" {

void MD5Init(MD5_CTX *ctx){
    if(!ctx) return;
    mbedtls_md5_init(&ctx->ctx);
#if defined(MBEDTLS_MD5_C)
  #if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    mbedtls_md5_starts(&ctx->ctx);
  #else
    mbedtls_md5_starts_ret(&ctx->ctx);
  #endif
#endif
}

void MD5Update(MD5_CTX *ctx, const unsigned char *input, unsigned int len){
    if(!ctx || !input || !len) return;
#if defined(MBEDTLS_MD5_C)
  #if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    mbedtls_md5_update(&ctx->ctx, input, len);
  #else
    mbedtls_md5_update_ret(&ctx->ctx, input, len);
  #endif
#endif
}

void MD5Final(unsigned char digest[16], MD5_CTX *ctx){
    if(!ctx || !digest) return;
#if defined(MBEDTLS_MD5_C)
  #if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    mbedtls_md5_finish(&ctx->ctx, digest);
  #else
    mbedtls_md5_finish_ret(&ctx->ctx, digest);
  #endif
#endif
    mbedtls_md5_free(&ctx->ctx);
}

}
