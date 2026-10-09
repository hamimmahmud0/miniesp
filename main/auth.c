#include "auth.h"
#include <string.h>
#include "creds.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define SALT_LEN 16
#define ROUNDS 2000

static void derive(const uint8_t *salt, const uint8_t *pw, size_t n, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, salt, SALT_LEN);
    mbedtls_sha256_update(&c, pw, n);
    mbedtls_sha256_finish(&c, out);
    for (int i = 1; i < ROUNDS; i++) {                // iterate to make guessing slower
        mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(&c, out, 32);
        mbedtls_sha256_update(&c, salt, SALT_LEN);
        mbedtls_sha256_finish(&c, out);
    }
    mbedtls_sha256_free(&c);
}

static bool ct_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static bool load(uint8_t salt[SALT_LEN], uint8_t hash[32])
{
    nvs_handle_t h;
    if (nvs_open("auth", NVS_READONLY, &h) != ESP_OK) return false;
    size_t a = SALT_LEN, b = 32;
    bool ok = nvs_get_blob(h, "salt", salt, &a) == ESP_OK && a == SALT_LEN
              && nvs_get_blob(h, "hash", hash, &b) == ESP_OK && b == 32;
    nvs_close(h);
    return ok;
}

bool auth_is_custom(void)
{
    uint8_t s[SALT_LEN], h[32];
    return load(s, h);
}

static bool check_pw(const uint8_t *pw, size_t n)
{
    uint8_t salt[SALT_LEN], want[32], got[32];
    if (load(salt, want)) {
        derive(salt, pw, n, got);
        return ct_eq(got, want, 32);
    }
    size_t dn = strlen(CRED_SSH_PASS);
    if (!dn) return false;                            // no default => no login until a password is set
    uint8_t diff = n != dn;
    for (size_t i = 0; i < n && i < dn; i++) diff |= pw[i] ^ (uint8_t)CRED_SSH_PASS[i];
    return diff == 0;
}

bool auth_check(const uint8_t *user, size_t user_len, const uint8_t *pw, size_t pw_len)
{
    size_t un = strlen(CRED_SSH_USER);
    uint8_t udiff = user_len != un;
    for (size_t i = 0; i < user_len && i < un; i++) udiff |= user[i] ^ (uint8_t)CRED_SSH_USER[i];
    bool pok = check_pw(pw, pw_len);                  // always evaluate both: no user-enumeration timing
    return udiff == 0 && pok;
}

bool auth_verify_password(const char *pw) { return check_pw((const uint8_t *)pw, strlen(pw)); }

bool auth_set_password(const char *pw)
{
    size_t n = strlen(pw);
    if (n < AUTH_MIN_PW || n > AUTH_MAX_PW) return false;
    uint8_t salt[SALT_LEN], hash[32];
    esp_fill_random(salt, sizeof salt);
    derive(salt, (const uint8_t *)pw, n, hash);
    nvs_handle_t h;
    if (nvs_open("auth", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "salt", salt, sizeof salt) == ESP_OK && nvs_set_blob(h, "hash", hash, sizeof hash) == ESP_OK
              && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool auth_reset_password(void)
{
    nvs_handle_t h;
    if (nvs_open("auth", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return true;
}
