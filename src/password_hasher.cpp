#include "password_hasher.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <cctype>
#include <cstring>
#include <random>
#include <semaphore>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <crow/logging.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace flapi {

namespace {

constexpr const char* kPbkdf2Prefix = "$pbkdf2-sha256$";
constexpr const char* kRedactionUnused = "";

bool isMd5HexDigest(const std::string& s) {
    if (s.size() != 32) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
}

bool isBcryptPrefix(const std::string& s) {
    if (s.size() < 4 || s[0] != '$' || s[1] != '2' || s[3] != '$') {
        return false;
    }
    const char v = s[2];
    return v == 'a' || v == 'b' || v == 'y';
}

bool startsWith(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Minimal URL-safe base64 (no padding) — sufficient for the salt + hash
// in our MCF string. Avoids pulling another library just for encoding.
std::string base64UrlEncode(const std::vector<unsigned char>& bytes) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(bytes[i]) << 16) |
            (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
            static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
    }
    if (i < bytes.size()) {
        std::uint32_t triple = static_cast<std::uint32_t>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) {
            triple |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;
        }
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        if (i + 1 < bytes.size()) {
            out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        }
    }
    return out;
}

// Decodes every base64 dialect a PBKDF2 MCF string is found in:
//   - URL-safe ('-' '_'), which hashWithDefaults() emits;
//   - passlib's "ab64" ('.' for '+', '/' kept, no padding), which
//     passlib.hash.pbkdf2_sha256 emits - the header promises that format;
//   - standard ('+' '/'), with or without '=' padding.
// The alphabets agree on the first 62 symbols, so accepting all of them is
// unambiguous.
std::vector<unsigned char> base64UrlDecode(std::string s) {
    while (!s.empty() && s.back() == '=') {
        s.pop_back();
    }
    static const std::int8_t kTable[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,62,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    std::vector<unsigned char> out;
    out.reserve((s.size() * 3) / 4);
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        std::int8_t v = (static_cast<unsigned char>(c) < 128) ? kTable[static_cast<unsigned char>(c)] : -1;
        if (v < 0) {
            return {};  // invalid char
        }
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::vector<unsigned char> randomSalt(std::size_t n) {
    std::random_device rd;
    std::vector<unsigned char> salt(n);
    for (std::size_t i = 0; i < n; ++i) {
        salt[i] = static_cast<unsigned char>(rd() & 0xFF);
    }
    return salt;
}

std::vector<unsigned char> pbkdf2(const std::string& password,
                                  const std::vector<unsigned char>& salt,
                                  std::uint32_t iterations,
                                  std::size_t out_bytes) {
    std::vector<unsigned char> out(out_bytes);
    const int ok = PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()),
        salt.data(), static_cast<int>(salt.size()),
        static_cast<int>(iterations),
        EVP_sha256(),
        static_cast<int>(out.size()), out.data());
    if (ok != 1) {
        throw std::runtime_error("PBKDF2: OpenSSL derivation failed");
    }
    return out;
}

bool constantTimeEqual(const std::vector<unsigned char>& a,
                       const std::vector<unsigned char>& b) {
    if (a.size() != b.size() || a.empty()) {
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// Equal-length digests of both sides, compared in constant time: neither the
// position of the first differing byte nor the stored length shows in timing.
bool constantTimeStringEqual(const std::string& a, const std::string& b) {
    unsigned char da[SHA256_DIGEST_LENGTH];
    unsigned char db[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(a.data()), a.size(), da);
    ::SHA256(reinterpret_cast<const unsigned char*>(b.data()), b.size(), db);
    return CRYPTO_memcmp(da, db, SHA256_DIGEST_LENGTH) == 0;
}

bool verifyPbkdf2(const std::string& provided, const std::string& stored) {
    // stored: $pbkdf2-sha256$<iter>$<b64-salt>$<b64-hash>
    auto rest = stored.substr(std::strlen(kPbkdf2Prefix));
    auto dollar1 = rest.find('$');
    auto dollar2 = rest.find('$', dollar1 + 1);
    if (dollar1 == std::string::npos || dollar2 == std::string::npos) {
        return false;
    }
    std::uint32_t iter = 0;
    try {
        iter = static_cast<std::uint32_t>(std::stoul(rest.substr(0, dollar1)));
    } catch (...) {
        return false;
    }
    if (iter == 0 || iter > 10'000'000) {
        // Refuse pathological iteration counts. Upper bound is generous;
        // anything past it is almost certainly a config typo or an attempt
        // to wedge the verify thread.
        return false;
    }
    auto salt = base64UrlDecode(rest.substr(dollar1 + 1, dollar2 - dollar1 - 1));
    auto expected = base64UrlDecode(rest.substr(dollar2 + 1));
    if (salt.empty() || expected.empty()) {
        return false;
    }
    auto actual = pbkdf2(provided, salt, iter, expected.size());
    return constantTimeEqual(actual, expected);
}

// MD5 verification — kept here for upgrade-compat. The Wave 0 startup
// auditor already warns operators that this format is deprecated.
std::string md5Hex(const std::string& input) {
    std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("MD5: cannot create EVP_MD_CTX");
    }
    if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("MD5: hashing failed");
    }
    EVP_MD_CTX_free(ctx);
    std::ostringstream oss;
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::hex;
        oss.width(2);
        oss.fill('0');
        oss << static_cast<int>(digest[i]);
    }
    return oss.str();
}

} // namespace

PasswordFormat PasswordHasher::classifyFormat(const std::string& stored) {
    if (startsWith(stored, kPbkdf2Prefix)) {
        return PasswordFormat::Pbkdf2Sha256;
    }
    if (isBcryptPrefix(stored)) {
        return PasswordFormat::BcryptUnsupported;
    }
    if (isMd5HexDigest(stored)) {
        return PasswordFormat::Md5Deprecated;
    }
    return PasswordFormat::PlaintextDeprecated;
}

std::string PasswordHasher::hashWithDefaults(const std::string& password) const {
    auto salt = randomSalt(kSaltBytes);
    auto derived = pbkdf2(password, salt, kDefaultIterations, kKeyBytes);
    std::ostringstream oss;
    oss << kPbkdf2Prefix << kDefaultIterations << '$'
        << base64UrlEncode(salt) << '$'
        << base64UrlEncode(derived);
    (void) kRedactionUnused;
    return oss.str();
}

bool PasswordHasher::verify(const std::string& provided, const std::string& stored) const {
    switch (classifyFormat(stored)) {
        case PasswordFormat::Pbkdf2Sha256:
            return verifyPbkdf2(provided, stored);
        case PasswordFormat::Md5Deprecated: {
            try {
                return constantTimeStringEqual(md5Hex(provided), stored);
            } catch (...) {
                return false;
            }
        }
        case PasswordFormat::BcryptUnsupported:
            // We refuse to silently fail-open against a bcrypt hash. The
            // operator needs to migrate to PBKDF2 (see SECURITY docs / the
            // Wave 0 auditor warning).
            return false;
        case PasswordFormat::PlaintextDeprecated:
            return constantTimeStringEqual(provided, stored);
    }
    return false;
}

std::string PasswordHasher::dummyHashLike(const std::string& stored) {
    // Never matches anything a caller sends: its password is random and
    // never leaves this function.
    static std::mutex mutex;
    static std::map<std::uint32_t, std::string> pbkdf2_by_iterations;
    static const std::string secret = [] {
        auto bytes = randomSalt(24);
        return base64UrlEncode(bytes);
    }();
    std::uint32_t iterations = kDefaultIterations;
    switch (stored.empty() ? PasswordFormat::Pbkdf2Sha256 : classifyFormat(stored)) {
        case PasswordFormat::Md5Deprecated:
            return md5Hex(secret);
        case PasswordFormat::PlaintextDeprecated:
            return secret;
        case PasswordFormat::BcryptUnsupported:
            return stored;   // refused without any work, as for a real bcrypt user
        case PasswordFormat::Pbkdf2Sha256: {
            if (stored.empty()) {
                break;
            }
            const auto rest = stored.substr(std::strlen(kPbkdf2Prefix));
            try {
                const auto parsed = std::stoul(rest.substr(0, rest.find('$')));
                if (parsed > 0 && parsed <= 10'000'000) {
                    iterations = static_cast<std::uint32_t>(parsed);
                }
            } catch (...) {
            }
            break;
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    auto it = pbkdf2_by_iterations.find(iterations);
    if (it == pbkdf2_by_iterations.end()) {
        auto salt = randomSalt(kSaltBytes);
        auto derived = pbkdf2(secret, salt, iterations, kKeyBytes);
        std::ostringstream oss;
        oss << kPbkdf2Prefix << iterations << '$' << base64UrlEncode(salt) << '$' << base64UrlEncode(derived);
        it = pbkdf2_by_iterations.emplace(iterations, oss.str()).first;
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// PasswordVerificationCache
// ---------------------------------------------------------------------------

namespace {

// How long a caller waits for a derivation slot before it is refused. Long
// enough that a legitimate burst queues instead of failing, short enough that
// a flood cannot park worker threads indefinitely.
constexpr std::chrono::milliseconds kDerivationWait{2000};

std::counting_semaphore<1024>& derivationSlots() {
    static std::counting_semaphore<1024> slots(
        static_cast<std::ptrdiff_t>(PasswordVerificationCache::maxConcurrentDerivations()));
    return slots;
}

void appendLengthPrefixed(std::string& out, const std::string& field) {
    // Length-prefixed, not ':'-joined: a password may contain ':' and the
    // fields must not be able to slide into each other.
    const auto n = static_cast<std::uint64_t>(field.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((n >> shift) & 0xFF));
    }
    out += field;
}

}  // namespace

PasswordVerificationCache& PasswordVerificationCache::instance() {
    static PasswordVerificationCache cache;
    return cache;
}

PasswordVerificationCache::PasswordVerificationCache() {
    unsigned char key[32];
    if (RAND_bytes(key, sizeof(key)) == 1) {
        _hmac_key.assign(reinterpret_cast<const char*>(key), sizeof(key));
    }
    // An empty key disables caching (keyFor returns ""), never weakens it.
}

unsigned PasswordVerificationCache::maxConcurrentDerivations() {
    // Half the cores, at least one: the other half (and the io threads Crow
    // keeps beyond that) stay free to serve requests that need no PBKDF2.
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned half = hw / 2;
    return half > 0 ? (half < 1024 ? half : 1024) : 1;
}

std::string PasswordVerificationCache::keyFor(const std::string& username, const std::string& password,
                                              const std::string& stored) const {
    if (_hmac_key.empty()) {
        return "";
    }
    std::string message;
    message.reserve(username.size() + password.size() + stored.size() + 24);
    appendLengthPrefixed(message, username);
    appendLengthPrefixed(message, password);
    appendLengthPrefixed(message, stored);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    const bool ok = HMAC(EVP_sha256(), _hmac_key.data(), static_cast<int>(_hmac_key.size()),
                         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                         digest, &digest_len) != nullptr && digest_len > 0;
    // The message held the plaintext password; do not leave it on the heap.
    OPENSSL_cleanse(message.data(), message.size());
    if (!ok) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(digest), digest_len);
}

void PasswordVerificationCache::remember(const std::string& key) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(_mutex);
    if (_entries.size() >= kMaxEntries && _entries.find(key) == _entries.end()) {
        for (auto it = _entries.begin(); it != _entries.end();) {
            it = (now - it->second >= kTtl) ? _entries.erase(it) : std::next(it);
        }
        // Still full of live entries: evict an arbitrary one (hash order).
        while (_entries.size() >= kMaxEntries) {
            _entries.erase(_entries.begin());
        }
    }
    _entries[key] = now;
}

bool PasswordVerificationCache::verifyCredentials(const std::string& username, const std::string& password,
                                                  const std::string& stored) {
    if (PasswordHasher::classifyFormat(stored) != PasswordFormat::Pbkdf2Sha256) {
        // MD5 and plaintext are cheap to check and bcrypt never verifies;
        // nothing to save by caching them.
        return PasswordHasher{}.verify(password, stored);
    }

    const std::string key = keyFor(username, password, stored);
    if (!key.empty()) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _entries.find(key);
        if (it != _entries.end()) {
            if (Clock::now() - it->second < kTtl) {
                ++_hits;
                return true;
            }
            _entries.erase(it);
        }
    }

    auto& slots = derivationSlots();
    if (!slots.try_acquire_for(kDerivationWait)) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_refused_busy;
        }
        CROW_LOG_WARNING << "Basic auth: too many password checks in progress; refusing the request "
                            "rather than queueing it (PBKDF2 concurrency limit "
                         << maxConcurrentDerivations() << ")";
        return false;
    }
    bool ok = false;
    try {
        ok = PasswordHasher{}.verify(password, stored);
    } catch (...) {
        ok = false;
    }
    slots.release();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_derivations;
    }
    if (ok && !key.empty()) {
        remember(key);
    }
    return ok;
}

void PasswordVerificationCache::clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    _entries.clear();
}

std::size_t PasswordVerificationCache::size() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _entries.size();
}

std::uint64_t PasswordVerificationCache::hits() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _hits;
}

std::uint64_t PasswordVerificationCache::derivations() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _derivations;
}

std::uint64_t PasswordVerificationCache::refusedBusy() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _refused_busy;
}

void PasswordVerificationCache::ageEntriesForTest(std::chrono::seconds age) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& [key, at] : _entries) {
        at -= age;
    }
}

} // namespace flapi
