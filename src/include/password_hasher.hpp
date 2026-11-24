#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

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

    // A stored hash no password matches, costing what checking `stored` costs:
    // PBKDF2 with the same iteration count (the default when `stored` is empty
    // or unparsable), MD5 for MD5, a random plaintext for plaintext. Checked
    // when a username is unknown, so the response time does not tell whether
    // the user exists.
    static std::string dummyHashLike(const std::string& stored);
};

// Basic auth sends the password on EVERY request, and a PBKDF2 hash at the
// default 600,000 iterations costs ~165 ms of CPU to check. Without a cache
// every request paid it, and a flood of wrong passwords was a cheap CPU
// denial of service.
//
// verifyCredentials() is PasswordHasher::verify() plus two guards:
//   - SUCCESSFUL PBKDF2 verifications are remembered for a short TTL, keyed by
//     HMAC-SHA256(process-random key, user, password, stored hash). The
//     plaintext password is never stored; the key dies with the process. The
//     stored hash is part of the key, so a changed password can never match
//     an old entry, and clear() (called whenever auth config is parsed, i.e.
//     on every load and reload) drops everything. Failures are never cached:
//     a wrong password is checked in full every time.
//   - At most maxConcurrentDerivations() PBKDF2 derivations run at once. A
//     caller that cannot get a slot within the wait budget is REFUSED (fail
//     closed), so wrong-password traffic cannot occupy every worker thread.
// Bounded: at most kMaxEntries entries; expired ones are swept first, then
// arbitrary ones are evicted.
class PasswordVerificationCache {
public:
    static constexpr std::size_t kMaxEntries = 10'000;
    static constexpr std::chrono::seconds kTtl{300};

    static PasswordVerificationCache& instance();

    // True when `password` verifies against `stored` for `username`.
    bool verifyCredentials(const std::string& username, const std::string& password,
                           const std::string& stored);

    // Forget every remembered verification (config load/reload).
    void clear();

    // Test/observability seams.
    std::size_t size() const;
    std::uint64_t hits() const;
    std::uint64_t derivations() const;
    std::uint64_t refusedBusy() const;
    static unsigned maxConcurrentDerivations();
    // Tests only: pretend entries were verified `age` ago.
    void ageEntriesForTest(std::chrono::seconds age);

private:
    PasswordVerificationCache();
    std::string keyFor(const std::string& username, const std::string& password,
                       const std::string& stored) const;
    void remember(const std::string& key);

    using Clock = std::chrono::steady_clock;
    std::string _hmac_key;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, Clock::time_point> _entries;
    std::uint64_t _hits = 0;
    std::uint64_t _derivations = 0;
    std::uint64_t _refused_busy = 0;
};

} // namespace flapi
