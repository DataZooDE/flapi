#include <catch2/catch_test_macros.hpp>

#include "password_hasher.hpp"

namespace flapi {
namespace test {

TEST_CASE("PasswordHasher: hash+verify roundtrip on the same password",
          "[security][password]") {
    PasswordHasher hasher;
    auto stored = hasher.hashWithDefaults("hunter2");
    REQUIRE_FALSE(stored.empty());
    REQUIRE(hasher.verify("hunter2", stored));
}

TEST_CASE("PasswordHasher: verify rejects the wrong password",
          "[security][password]") {
    PasswordHasher hasher;
    auto stored = hasher.hashWithDefaults("hunter2");
    REQUIRE_FALSE(hasher.verify("hunter3", stored));
    REQUIRE_FALSE(hasher.verify("", stored));
}

TEST_CASE("PasswordHasher: hash output uses the documented $pbkdf2-sha256$ format",
          "[security][password]") {
    // The format string is part of the operator-facing contract — admins
    // copy it into YAML, so it must be stable and self-describing.
    PasswordHasher hasher;
    auto stored = hasher.hashWithDefaults("any");
    REQUIRE(stored.find("$pbkdf2-sha256$") == 0);
}

TEST_CASE("PasswordHasher: two hashes of the same password are different (random salt)",
          "[security][password]") {
    PasswordHasher hasher;
    auto a = hasher.hashWithDefaults("hunter2");
    auto b = hasher.hashWithDefaults("hunter2");
    REQUIRE(a != b);
    REQUIRE(hasher.verify("hunter2", a));
    REQUIRE(hasher.verify("hunter2", b));
}

TEST_CASE("PasswordHasher: classifyFormat tags every supported and legacy form",
          "[security][password]") {
    REQUIRE(PasswordHasher::classifyFormat("$pbkdf2-sha256$600000$salt$hash") ==
            PasswordFormat::Pbkdf2Sha256);
    REQUIRE(PasswordHasher::classifyFormat("2ab96390c7dbe3439de74d0c9b0b1767") ==
            PasswordFormat::Md5Deprecated);
    REQUIRE(PasswordHasher::classifyFormat("hunter2") ==
            PasswordFormat::PlaintextDeprecated);
    REQUIRE(PasswordHasher::classifyFormat("$2b$12$abcdefghij...") ==
            PasswordFormat::BcryptUnsupported);
    REQUIRE(PasswordHasher::classifyFormat("") ==
            PasswordFormat::PlaintextDeprecated);
}

TEST_CASE("PasswordHasher: MD5-hashed password still verifies (deprecated path)",
          "[security][password]") {
    // The Wave 0 startup auditor warns about MD5; the runtime continues to
    // accept it so existing configs do not break on upgrade.
    PasswordHasher hasher;
    // MD5("hunter2")
    REQUIRE(hasher.verify("hunter2", "2ab96390c7dbe3439de74d0c9b0b1767"));
}

TEST_CASE("PasswordHasher: plaintext password still verifies (deprecated path)",
          "[security][password]") {
    PasswordHasher hasher;
    REQUIRE(hasher.verify("hunter2", "hunter2"));
    REQUIRE_FALSE(hasher.verify("hunter3", "hunter2"));
}

TEST_CASE("PasswordHasher: bcrypt-prefixed stored value is recognised but rejected",
          "[security][password]") {
    // Until a bcrypt verifier ships, we deliberately do NOT attempt a
    // best-effort match — silently failing a bcrypt verify would be the
    // worst possible UX (admin thinks their hash works but it doesn't).
    PasswordHasher hasher;
    REQUIRE_FALSE(hasher.verify("hunter2", "$2b$12$abcdefghijklmnopqrstuv"));
    REQUIRE_FALSE(hasher.verify("anything", "$2a$10$shortbcrypt"));
    REQUIRE_FALSE(hasher.verify("anything", "$2y$10$shortbcrypt"));
}

TEST_CASE("PasswordHasher: verify is constant-time-ish across stored format",
          "[security][password]") {
    // Smoke test: the same wrong-password verify path must work the same
    // way regardless of format. (Real timing-attack hardening lives in
    // OpenSSL's PKCS5_PBKDF2_HMAC and CRYPTO_memcmp.)
    PasswordHasher hasher;
    REQUIRE_FALSE(hasher.verify("wrong", "plaintext"));
    REQUIRE_FALSE(hasher.verify("wrong", "2ab96390c7dbe3439de74d0c9b0b1767"));
    auto pbkdf2 = hasher.hashWithDefaults("right");
    REQUIRE_FALSE(hasher.verify("wrong", pbkdf2));
}

} // namespace test
} // namespace flapi
