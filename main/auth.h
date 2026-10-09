#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_MIN_PW 4
#define AUTH_MAX_PW 64

// Check a login. Uses the password set at runtime (NVS, salted hash) if there is one,
// otherwise the build-time default from .creds.
bool auth_check(const uint8_t *user, size_t user_len, const uint8_t *pw, size_t pw_len);
bool auth_verify_password(const char *pw);          // current password only (for 'passwd')
bool auth_set_password(const char *pw);             // persist a new password
bool auth_reset_password(void);                     // back to the build-time default
bool auth_is_custom(void);
