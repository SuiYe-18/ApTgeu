/*
 * ApTgeu shared - AES-256-CBC payload encryption helpers (implementation).
 */
#include "crypto.h"

#include <cstring>
#include <windows.h>
#include <wincrypt.h>

#include "aes.h"
#include "sha256.h"
#include "log.h"

namespace atu {

/* General HMAC-SHA256 over an arbitrary-length key (FIPS 198-1 / RFC 2104).
 * SHA-256 block size B=64, digest L=32. Used by the 32-byte-key public HMAC
 * and internally by PBKDF2 (where the PRF key is the password, arbitrary
 * length). */
static void hmac_sha256_impl(const uint8_t* key, size_t key_len,
                             const uint8_t* msg, size_t msg_len,
                             uint8_t out[32]) {
    uint8_t k[64];
    std::memset(k, 0, sizeof(k));
    if (key_len > sizeof(k)) {
        // Keys longer than the block size are hashed first.
        sha256(key, key_len, k);
    } else {
        std::memcpy(k, key, key_len);
    }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }

    // inner = SHA256(ipad || msg)
    uint8_t inner[32];
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    if (msg_len) sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    // outer = SHA256(opad || inner)
    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

void hmac_sha256(const uint8_t key[32],
                 const uint8_t* msg, size_t msg_len,
                 uint8_t mac_out[32]) {
    hmac_sha256_impl(key, 32, msg, msg_len, mac_out);
}

bool hmac_sha256_verify(const uint8_t key[32],
                        const uint8_t* msg, size_t msg_len,
                        const uint8_t expected[32]) {
    uint8_t mac[32];
    hmac_sha256(key, msg, msg_len, mac);
    // Constant-time compare: never short-circuit on the first differing byte.
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) {
        diff = (uint8_t)(diff | (mac[i] ^ expected[i]));
    }
    return diff == 0;
}

void pbkdf2_hmac_sha256(const std::string& password,
                        const uint8_t* salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t* dk_out, size_t dk_len) {
    // We always derive exactly 32 bytes = one PRF block (hLen). INT(BE,1).
    uint8_t data[512];
    if (salt_len > sizeof(data) - 4) salt_len = sizeof(data) - 4;
    std::memcpy(data, salt, salt_len);
    data[salt_len + 0] = 0;
    data[salt_len + 1] = 0;
    data[salt_len + 2] = 0;
    data[salt_len + 3] = 1;

    const uint8_t* pw = reinterpret_cast<const uint8_t*>(password.data());
    size_t pw_len = password.size();

    uint8_t u[32];
    hmac_sha256_impl(pw, pw_len, data, salt_len + 4, u);  // U_1
    uint8_t t[32];
    std::memcpy(t, u, 32);
    for (uint32_t i = 1; i < iterations; ++i) {
        hmac_sha256_impl(pw, pw_len, u, 32, u);           // U_{i+1} = PRF(P, U_i)
        for (int j = 0; j < 32; ++j) t[j] ^= u[j];        // T = U_1 ^ ... ^ U_c
    }

    size_t n = dk_len < 32 ? dk_len : 32;
    std::memcpy(dk_out, t, n);
}

void derive_aes256_key(const std::string& password,
                       const uint8_t salt[kSaltLen],
                       uint8_t key_out[32]) {
    pbkdf2_hmac_sha256(password, salt, kSaltLen, kPbkdf2Iterations, key_out, 32);
}

void derive_hmac_key(const std::string& password,
                     const uint8_t salt[kSaltLen],
                     uint8_t key_out[32]) {
    // Domain-separated from derive_aes256_key(): the PBKDF2 salt input is
    // salt || label, so the MAC key and the AES key are independent.
    static const char kLabel[] = "ApTgeu|HMAC-SHA256|v1";
    uint8_t s[kSaltLen + (sizeof(kLabel) - 1)];
    std::memcpy(s, salt, kSaltLen);
    std::memcpy(s + kSaltLen, kLabel, sizeof(kLabel) - 1);
    pbkdf2_hmac_sha256(password, s, sizeof(s), kPbkdf2Iterations, key_out, 32);
}

void derive_aes256_key_legacy(const std::string& password, uint8_t key_out[32]) {
    // Weak on purpose: kept only to open pre-M-01 "APTGEU07" packages.
    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        log_warn("Legacy password KDF (single SHA-256, no salt, no iterations) "
                 "used to open a pre-PBKDF2 installer package. Re-pack with a "
                 "Password to get PBKDF2-HMAC-SHA256 (100000 iters).");
    }
    sha256(password.data(), password.size(), key_out);
}

void random_bytes(uint8_t* out, size_t n) {
    HCRYPTPROV hProv = 0;
    if (::CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_FULL,
                               CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        BOOL ok = ::CryptGenRandom(hProv, (DWORD)n, out);
        ::CryptReleaseContext(hProv, 0);
        if (ok) return;
    }
    // Fallback: weak but better than zeroes.
    for (size_t i = 0; i < n; ++i) {
        out[i] = (uint8_t)((i * 2654435761u) ^ (GetTickCount() & 0xFF) ^ i);
    }
}

void aes256_cbc_encrypt(const uint8_t key[32],
                        const uint8_t iv[16],
                        const uint8_t* plaintext, size_t plaintext_len,
                        std::vector<uint8_t>& ciphertext_out) {
    // PKCS#7 pad to multiple of 16.
    size_t pad = 16 - (plaintext_len % 16);
    size_t padded_len = plaintext_len + pad;
    ciphertext_out.resize(padded_len);
    std::memcpy(ciphertext_out.data(), plaintext, plaintext_len);
    for (size_t i = plaintext_len; i < padded_len; ++i) {
        ciphertext_out[i] = (uint8_t)pad;
    }

    AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv);
    AES_CBC_encrypt_buffer(&ctx, ciphertext_out.data(), padded_len);
}

bool aes256_cbc_decrypt(const uint8_t key[32],
                        const uint8_t iv[16],
                        const uint8_t* ciphertext, size_t ciphertext_len,
                        std::vector<uint8_t>& plaintext_out) {
    if (ciphertext_len == 0 || (ciphertext_len % 16) != 0) return false;
    plaintext_out.assign(ciphertext, ciphertext + ciphertext_len);

    AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv);
    AES_CBC_decrypt_buffer(&ctx, plaintext_out.data(), ciphertext_len);

    // Strip PKCS#7 padding.
    uint8_t pad = plaintext_out.back();
    if (pad < 1 || pad > 16) return false;
    for (size_t i = plaintext_out.size() - pad; i < plaintext_out.size(); ++i) {
        if (plaintext_out[i] != pad) return false;
    }
    plaintext_out.resize(plaintext_out.size() - pad);
    return true;
}

} // namespace atu
