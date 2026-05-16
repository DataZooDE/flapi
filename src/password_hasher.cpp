#include "password_hasher.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>

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

std::vector<unsigned char> base64UrlDecode(const std::string& s) {
    static const std::int8_t kTable[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
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
                return md5Hex(provided) == stored;
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
            return provided == stored;
    }
    return false;
}

} // namespace flapi
