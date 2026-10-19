/*
 * AMD PSP emulation
 *
 * Copyright (C) 2020 Robert Buhren <robert@robertbuhren.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * CCP5 gnu nettle bindings
 */

#ifndef AMD_CCP_V5_NETTLE_H
#define AMD_CCP_V5_NETTLE_H
#include <glib.h>
#include <nettle/aes.h>
#include <nettle/rsa.h>
#include <nettle/sha2.h>

/* TODO: Should this be in a dedicated c file? */

/* The CCP holds it's SHA context in the LSBs. However, to make use of the SHA
 * functionality of libraries such as nettle, we need storage for the library
 * specific sha context. The "CcpV5ShaCtx" represents such a context. It will
 * be allocated on demand and freed when the hash is computed.
 * The contexts here are "nettle" specific.
 */
typedef union {
    struct sha256_ctx* ctx_256;
    struct sha384_ctx* ctx_384;
    void* raw;
} CcpV5ShaCtx;

typedef struct CcpV5RsaPubKey {
    mpz_t mod;
    mpz_t exp;
    uint32_t rsa_len;
} CcpV5RsaPubKey;

/* typedef struct rsa_public_key CcpV5RsaPubKey; */

static inline void ccp_aes128ecb(uint8_t* dst, const uint8_t* src,
                                 size_t length, const uint8_t* key,
                                 bool encrypt)
{
    struct aes128_ctx ctx;
    if (encrypt)
        aes128_set_encrypt_key(&ctx, key);
    else
        aes128_set_decrypt_key(&ctx, key);

    if (encrypt)
        aes128_encrypt(&ctx, length, dst, src);
    else
        aes128_decrypt(&ctx, length, dst, src);
}

static inline void ccp_aes256ecb(uint8_t * dst, const uint8_t * src, size_t length, const uint8_t * key, bool encrypt) {
    struct aes256_ctx ctx;
    if (encrypt)
        aes256_set_encrypt_key(&ctx, key);
    else
        aes256_set_decrypt_key(&ctx, key);

    if (encrypt)
        aes256_encrypt(&ctx, length, dst, src);
    else
        aes256_decrypt(&ctx, length, dst, src);
}

static inline void ccp_init_sha256_ctx(CcpV5ShaCtx* ctx)
{
    if (ctx != NULL) {
        ctx->ctx_256 = (struct sha256_ctx*)g_malloc(sizeof(struct sha256_ctx));
        nettle_sha256_init(ctx->ctx_256);
    }
}

static inline void ccp_init_sha384_ctx(CcpV5ShaCtx* ctx)
{
    if (ctx != NULL) {
        ctx->ctx_384 = (struct sha384_ctx*)g_malloc(sizeof(struct sha384_ctx));
        nettle_sha384_init(ctx->ctx_384);
    }
}

static inline void ccp_update_sha256(CcpV5ShaCtx* ctx, uint32_t len, void* src)
{
    nettle_sha256_update(ctx->ctx_256, len, src);
}

static inline void ccp_update_sha384(CcpV5ShaCtx* ctx, uint32_t len, void* src)
{
    sha384_update(ctx->ctx_384, len, src);
}

static inline void ccp_clear_sha_ctx(CcpV5ShaCtx* ctx)
{
    if (ctx != NULL) {
        g_free(ctx->raw);
        ctx->raw = NULL;
    }
}

static inline void ccp_digest_sha256(CcpV5ShaCtx* ctx, uint8_t* lsb_ctx)
{
    nettle_sha256_digest(ctx->ctx_256, SHA256_DIGEST_SIZE, lsb_ctx);
    ccp_clear_sha_ctx(ctx);
}

static inline void ccp_digest_sha384(CcpV5ShaCtx* ctx, uint8_t* lsb_ctx)
{
    nettle_sha384_digest(ctx->ctx_384, SHA384_DIGEST_SIZE, lsb_ctx);
    ccp_clear_sha_ctx(ctx);
}
static inline void ccp_rsa_init_key(CcpV5RsaPubKey* key, uint8_t* m, uint8_t* e)
{

    mpz_init(key->exp);
    mpz_init(key->mod);

    mpz_import(key->exp, key->rsa_len, -1, 1, 0, 0, e);
    mpz_import(key->mod, key->rsa_len, -1, 1, 0, 0, m);
}

static inline void ccp_rsa_clear_key(CcpV5RsaPubKey* key)
{
    mpz_clear(key->exp);
    mpz_clear(key->mod);
    key->rsa_len = 0;
}

/* TODO: Check for errors */
static inline void ccp_rsa_encrypt(CcpV5RsaPubKey* key, uint8_t* m,
                                   uint8_t* res)
{
    mpz_t result;
    mpz_t message;

    /*
     * The modulus comes from guest memory. GMP raises SIGFPE (killing QEMU)
     * for a zero modulus, which hardware simply fails on: return all zeros.
     */
    if (mpz_sgn(key->mod) == 0) {
        memset(res, 0, key->rsa_len);
        return;
    }

    mpz_init(result);
    mpz_init(message);
    mpz_import(message, key->rsa_len, -1, 1, 0, 0, m);

    mpz_powm(result, message, key->exp, key->mod);

    mpz_export(res, NULL, -1, key->rsa_len, 0, 0, result);

    mpz_clear(result);
    mpz_clear(message);
}
#endif
