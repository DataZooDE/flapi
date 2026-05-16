#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace flapi {

// Recognised storage formats for credentials in the inline `auth.users[]`
// block. New deployments should always use Pbkdf2Sha256; the others exist
// for upgrade compatibility and surface as warnings via the Wave 0
// startup auditor.
enum class PasswordFormat {
    Pbkdf2Sha256,         // $pbkdf2-sha256$<iter>$<b64-salt>$<b64-hash>
    Md5Deprecated,        // 32 lowercase hex chars
    BcryptUnsupported,    // $2a$ | $2b$ | $2y$  — recognised but not verifiable
    PlaintextDeprecated,  // anything else, including empty
};

// W1.1: Modern password hashing for Basic-auth users.
//
// Hash output uses the standard Modular Crypt Format
//   $pbkdf2-sha256$<iter>$<b64-salt>$<b64-hash>
// produced via OpenSSL's `PKCS5_PBKDF2_HMAC` with SHA-256. This is the
// same shape passlib emits and is FIPS-approved (NIST SP 800-132).
//
// We deliberately do NOT ship a bcrypt implementation. A stored bcrypt
// hash is recognised (so configs that paste one don't silently fail) but
// `verify()` returns false — better than a slow-failing or partially-correct
// drop-in.
class PasswordHasher {
public:
    // Defaults: 600,000 iterations (OWASP 2023 minimum), 16-byte salt,
    // 32-byte derived key. Caller-tunable variants can be added later
    // without breaking the storage format.
    static constexpr std::uint32_t kDefaultIterations = 600'000;
    static constexpr std::size_t kSaltBytes = 16;
    static constexpr std::size_t kKeyBytes = 32;

    // Generate a fresh hash for `password` using a random salt.
    std::string hashWithDefaults(const std::string& password) const;

    // Verify `provided` against `stored`. Format auto-detected by prefix.
    bool verify(const std::string& provided, const std::string& stored) const;

    // Inspect what shape `stored` is. Pure function, exported for tests
    // and for the startup auditor's deprecation warnings.
    static PasswordFormat classifyFormat(const std::string& stored);
};

} // namespace flapi
