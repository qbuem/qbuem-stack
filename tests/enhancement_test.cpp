/**
 * @file tests/enhancement_test.cpp
 * @brief Enhancement: LockFreeConnectionPool, FutexSync, JwtAuthAction layout/basic tests.
 */

#include <gtest/gtest.h>

// ── Enhancement headers ───────────────────────────────────────────────────────
#include <qbuem/db/connection_pool.hpp>
#include <qbuem/shm/futex_sync.hpp>
#include <qbuem/security/jwt_action.hpp>

#include <cstring>
#include <type_traits>
#include <atomic>

// ─── LockFreeConnectionPool ──────────────────────────────────────────────────

TEST(ConnectionPool, SlotStateValues) {
    using namespace qbuem::db;
    EXPECT_EQ(static_cast<uint8_t>(SlotState::Free),      0u);
    EXPECT_EQ(static_cast<uint8_t>(SlotState::Active),    1u);
    EXPECT_EQ(static_cast<uint8_t>(SlotState::Recycling), 2u);
    EXPECT_EQ(static_cast<uint8_t>(SlotState::Dead),      3u);
}

TEST(ConnectionPool, LockFreeConnectionPoolIsNotCopyable) {
    using T = qbuem::db::LockFreeConnectionPool;
    EXPECT_FALSE(std::is_copy_constructible_v<T>);
    EXPECT_FALSE(std::is_copy_assignable_v<T>);
}

TEST(ConnectionPool, PooledConnectionDefault) {
    qbuem::db::PooledConnection pc{};
    EXPECT_FALSE(pc.valid());
}

TEST(ConnectionPool, PooledConnectionMoveOnly) {
    using T = qbuem::db::PooledConnection;
    EXPECT_FALSE(std::is_copy_constructible_v<T>);
    EXPECT_FALSE(std::is_copy_assignable_v<T>);
    EXPECT_TRUE(std::is_move_constructible_v<T>);
    EXPECT_TRUE(std::is_move_assignable_v<T>);
}

// ─── FutexSync ───────────────────────────────────────────────────────────────

TEST(FutexSync, FutexWordSize) {
    using namespace qbuem::shm;
    static_assert(sizeof(FutexWord) == 4,  "FutexWord must be 4 bytes");
    static_assert(alignof(FutexWord) == 4, "FutexWord must be 4-byte aligned");
    EXPECT_EQ(sizeof(FutexWord),  4u);
    EXPECT_EQ(alignof(FutexWord), 4u);
}

TEST(FutexSync, FutexWordInit) {
    using namespace qbuem::shm;
    FutexWord fw{42};
    EXPECT_EQ(fw.load(), 42u);
}

TEST(FutexSync, FutexWordStore) {
    using namespace qbuem::shm;
    FutexWord fw{0};
    fw.store(99);
    EXPECT_EQ(fw.load(), 99u);
}

TEST(FutexSync, FutexWordCAS) {
    using namespace qbuem::shm;
    FutexWord fw{10};
    uint32_t expected = 10;
    bool ok = fw.compare_exchange(expected, 20);
    EXPECT_TRUE(ok);
    EXPECT_EQ(fw.load(), 20u);

    // Verify CAS failure when already modified
    expected = 10;
    ok = fw.compare_exchange(expected, 30);
    EXPECT_FALSE(ok);
    EXPECT_EQ(expected, 20u); // on failure, expected is updated to current value
}

TEST(FutexSync, FutexWordFetchAdd) {
    using namespace qbuem::shm;
    FutexWord fw{5};
    uint32_t prev = fw.fetch_add(3);
    EXPECT_EQ(prev,     5u);
    EXPECT_EQ(fw.load(),8u);
}

TEST(FutexSync, FutexWordFetchSub) {
    using namespace qbuem::shm;
    FutexWord fw{10};
    uint32_t prev = fw.fetch_sub(4);
    EXPECT_EQ(prev,     10u);
    EXPECT_EQ(fw.load(), 6u);
}

TEST(FutexSync, FutexMutexDefaultUnlocked) {
    using namespace qbuem::shm;
    FutexMutex mtx;
    EXPECT_FALSE(mtx.is_locked());
}

TEST(FutexSync, FutexMutexTryLock) {
    using namespace qbuem::shm;
    FutexMutex mtx;
    auto guard = mtx.try_lock();
    EXPECT_TRUE(guard.has_value());
    EXPECT_TRUE(mtx.is_locked());
    // guard destruction → unlock
}

TEST(FutexSync, FutexMutexTryLockTwiceFails) {
    using namespace qbuem::shm;
    FutexMutex mtx;
    auto g1 = mtx.try_lock();
    EXPECT_TRUE(g1.has_value());

    auto g2 = mtx.try_lock();
    EXPECT_FALSE(g2.has_value()); // already locked
}

TEST(FutexSync, FutexMutexUnlockAfterGuard) {
    using namespace qbuem::shm;
    FutexMutex mtx;
    {
        auto guard = mtx.try_lock();
        EXPECT_TRUE(mtx.is_locked());
    }
    // unlocked after guard destruction
    EXPECT_FALSE(mtx.is_locked());
}

TEST(FutexSync, FutexSemaphoreInitValue) {
    using namespace qbuem::shm;
    FutexSemaphore sem{5};
    EXPECT_EQ(sem.value(), 5u);
}

TEST(FutexSync, FutexSemaphoreTryAcquireSuccess) {
    using namespace qbuem::shm;
    FutexSemaphore sem{3};
    bool ok = sem.try_acquire();
    EXPECT_TRUE(ok);
    EXPECT_EQ(sem.value(), 2u);
}

TEST(FutexSync, FutexSemaphoreTryAcquireFailsAtZero) {
    using namespace qbuem::shm;
    FutexSemaphore sem{0};
    bool ok = sem.try_acquire();
    EXPECT_FALSE(ok);
    EXPECT_EQ(sem.value(), 0u);
}

TEST(FutexSync, FutexSemaphoreRelease) {
    using namespace qbuem::shm;
    FutexSemaphore sem{0};
    sem.release(3);
    EXPECT_EQ(sem.value(), 3u);
}

// ─── JwtAuthAction ───────────────────────────────────────────────────────────

TEST(JwtAction, JwtClaimsDefault) {
    using namespace qbuem::security;
    JwtClaims c{};
    EXPECT_TRUE(c.sub.empty());
    EXPECT_TRUE(c.iss.empty());
    EXPECT_EQ(c.exp, -1);
    EXPECT_EQ(c.iat, -1);
    EXPECT_EQ(c.nbf, -1);
}

TEST(JwtAction, JwtClaimsIsValidAt) {
    using namespace qbuem::security;
    JwtClaims c{};
    c.exp = 2000000000LL; // future expiry
    int64_t now = 1700000000LL;
    EXPECT_TRUE(c.is_valid_at(now));
}

TEST(JwtAction, JwtClaimsExpired) {
    using namespace qbuem::security;
    JwtClaims c{};
    c.exp = 1000000000LL; // past expiry
    int64_t now = 1700000000LL;
    EXPECT_FALSE(c.is_valid_at(now));
}

TEST(JwtAction, JwtClaimsLeeway) {
    using namespace qbuem::security;
    JwtClaims c{};
    c.exp = 1700000000LL;
    int64_t now = 1700000005LL; // 5 seconds past expiry
    EXPECT_FALSE(c.is_valid_at(now, 0));
    EXPECT_TRUE(c.is_valid_at(now, 10));  // leeway of 10 seconds allowed
}

TEST(JwtAction, JwtClaimsNbf) {
    using namespace qbuem::security;
    JwtClaims c{};
    c.nbf = 1700000010LL;
    int64_t now = 1700000000LL;  // before nbf
    EXPECT_FALSE(c.is_valid_at(now));
    EXPECT_TRUE(c.is_valid_at(now, 15));  // leeway allowed
}

TEST(JwtAction, JwtAuthConfigDefaults) {
    using namespace qbuem::security;
    JwtAuthConfig cfg{};
    EXPECT_EQ(cfg.leeway_sec,   0);
    EXPECT_EQ(cfg.cache_size, 256u);
    EXPECT_TRUE(cfg.require_exp);
    EXPECT_FALSE(cfg.require_sub);
    EXPECT_EQ(cfg.auth_header, "authorization");
    EXPECT_EQ(JwtAuthConfig::kBearerPrefixLen, 7u);
}

TEST(JwtAction, JwtAuthResultValues) {
    using namespace qbuem::security;
    EXPECT_EQ(static_cast<uint8_t>(JwtAuthResult::OK),              0u);
    EXPECT_EQ(static_cast<uint8_t>(JwtAuthResult::NoToken),         1u);
    EXPECT_EQ(static_cast<uint8_t>(JwtAuthResult::InvalidFormat),   2u);
    EXPECT_EQ(static_cast<uint8_t>(JwtAuthResult::Expired),         3u);
    EXPECT_EQ(static_cast<uint8_t>(JwtAuthResult::NotYetValid),     4u);
    EXPECT_EQ(static_cast<uint8_t>(JwtAuthResult::SignatureInvalid),5u);
    EXPECT_EQ(static_cast<uint8_t>(JwtAuthResult::MissingClaim),    6u);
    EXPECT_EQ(static_cast<uint8_t>(JwtAuthResult::CacheHit),        7u);
}
