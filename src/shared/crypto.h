/*
 * ApTgeu shared - AES-256-CBC payload encryption helpers.
 *
 * Used by BOTH the compiler (packer.cpp, encrypt) and the stub (unpack.cpp, decrypt).
 *
 * Key derivation (M-01 fix): PBKDF2-HMAC-SHA256 with a per-package random 16-byte
 * salt, 100000 iterations, -> 32-byte key. The salt is stored verbatim in the
 * encrypted envelope. A legacy single-SHA-256 KDF is kept ONLY to open packages
 * packed before the fix (footer magic "APTGEU07", no salt field) and emits a
 * one-time WARN.
 *
 * M-02: encrypt-then-MAC. After CBC encryption the compiler computes an
 * HMAC-SHA256 over the whole encrypted envelope and stores the 32-byte MAC in
 * the 48-byte outer footer (magic "APTGEU09"). The stub verifies the MAC
 * BEFORE decrypting.
 *
 * Padding: PKCS#7.
 *
 * Wire layout for an encrypted payload (compiler writes, stub reads):
 *
 *   v2 (M-01 + M-02, footer magic "APTGEU09", 48-byte footer):
 *     inner payload:
 *       uint8[8]  inner_magic   e.g. "APTGEU03" (plaintext, tells stub the codec)
 *       uint8[16] salt          random, per-package; input to PBKDF2
 *       uint8[16] IV            random, generated at pack time
 *       uint8[N]  ciphertext    = AES-256-CBC(aes_key, IV, plaintext)
 *     outer footer (48 bytes):
 *       uint8[8]  "APTGEU09"
 *       uint64    payload_start_offset (LE)
 *       uint8[32] HMAC-SHA256(mac_key, inner_payload)
 *
 *   v1 (legacy, footer magic "APTGEU07", 16-byte footer, kept for compat):
 *       uint8[8]  inner_magic
 *       uint8[16] IV
 *       uint8[N]  ciphertext    = AES-256-CBC(SHA256(password), IV, plaintext)
 *     (no salt, no MAC; legacy KDF path)
 *
 * plaintext = "APTGEUSIG" (8 bytes, magic) || original compressed payload.
 * The stub checks "APTGEUSIG" after decryption; a mismatch means wrong password.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace atu {

/* M-01: PBKDF2 parameters. */
inline constexpr uint32_t kPbkdf2Iterations = 100000;
inline constexpr size_t   kSaltLen          = 16;

/*
 * Derive a 32-byte AES-256 key from a password via PBKDF2-HMAC-SHA256.
 * `salt` must be kSaltLen (16) random bytes, stored verbatim in the payload.
 * Used by every newly packed (footer "APTGEU09") installer.
 */
void derive_aes256_key(const std::string& password,
                       const uint8_t salt[kSaltLen],
                       uint8_t key_out[32]);

/*
 * Derive a 32-byte HMAC-SHA256 key from the SAME password + salt, but through
 * a domain-separated PBKDF2 salt (salt || label) so the confidentiality key
 * and the authentication key are never the same value.
 */
void derive_hmac_key(const std::string& password,
                     const uint8_t salt[kSaltLen],
                     uint8_t key_out[32]);

/*
 * Legacy KDF kept ONLY to decrypt packages packed before the M-01 fix
 * (footer magic "APTGEU07", no salt field). It is a single SHA-256 of the
 * password and is deliberately weak; emits a one-time WARN to the log.
 * New packages MUST NOT use this.
 */
void derive_aes256_key_legacy(const std::string& password, uint8_t key_out[32]);

/*
 * PBKDF2-HMAC-SHA256 (RFC 2898). Writes dk_len bytes (dk_len <= 32 here,
 * i.e. a single PRF block). Exposed for tests / symmetry.
 */
void pbkdf2_hmac_sha256(const std::string& password,
                        const uint8_t* salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t* dk_out, size_t dk_len);

/* HMAC-SHA256 (FIPS 198-1). `key` must be 32 bytes; writes a 32-byte MAC. */
void hmac_sha256(const uint8_t key[32],
                 const uint8_t* msg, size_t msg_len,
                 uint8_t mac_out[32]);

/*
 * Compute HMAC-SHA256(key, msg) and compare it in constant time against
 * `expected`. Returns true on match, false on mismatch.
 */
bool hmac_sha256_verify(const uint8_t key[32],
                        const uint8_t* msg, size_t msg_len,
                        const uint8_t expected[32]);

/* Generate n cryptographically-random bytes (uses CryptGenRandom on Windows). */
void random_bytes(uint8_t* out, size_t n);

/*
 * Encrypt `plaintext` with AES-256-CBC (PKCS#7 padded).
 * Writes iv[16] and appends ciphertext to `ciphertext_out`.
 * `key` must be 32 bytes.
 */
void aes256_cbc_encrypt(const uint8_t key[32],
                        const uint8_t iv[16],
                        const uint8_t* plaintext, size_t plaintext_len,
                        std::vector<uint8_t>& ciphertext_out);

/*
 * Decrypt `ciphertext` (must be multiple of 16, PKCS#7 padded).
 * Returns true on success; on PKCS#7 padding failure returns false (caller
 * should treat this as "wrong password").
 */
bool aes256_cbc_decrypt(const uint8_t key[32],
                        const uint8_t iv[16],
                        const uint8_t* ciphertext, size_t ciphertext_len,
                        std::vector<uint8_t>& plaintext_out);

} // namespace atu
