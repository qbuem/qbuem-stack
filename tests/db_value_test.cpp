/**
 * @file db_value_test.cpp
 * @brief db::Value heap-free variant unit tests.
 *
 * Coverage:
 * - Each type constructor and type tag check
 * - get<T>() value extraction
 * - Size constraint (≤32B)
 * - BoundParams stack binding
 * - Equality comparison operator
 */

#include <qbuem/db/value.hpp>
#include <gtest/gtest.h>

using namespace qbuem::db;

// ─── Type construction and tags ──────────────────────────────────────────────

TEST(DbValue, NullDefault) {
    Value v;
    EXPECT_TRUE(v.is_null());
    EXPECT_EQ(v.type(), Value::Type::Null);
    EXPECT_TRUE(v.is<Null>());
}

TEST(DbValue, NullSentinel) {
    Value v = null;
    EXPECT_TRUE(v.is_null());
}

TEST(DbValue, Int64) {
    Value v = int64_t{42};
    EXPECT_EQ(v.type(), Value::Type::Int64);
    EXPECT_TRUE(v.is<int64_t>());
    EXPECT_FALSE(v.is<double>());
    EXPECT_EQ(v.get<int64_t>(), 42);
}

TEST(DbValue, Int32ConvertedToInt64) {
    Value v = int32_t{-7};
    EXPECT_EQ(v.type(), Value::Type::Int64);
    EXPECT_EQ(v.get<int64_t>(), -7);
}

TEST(DbValue, Float64) {
    Value v = 3.14;
    EXPECT_EQ(v.type(), Value::Type::Float64);
    EXPECT_TRUE(v.is<double>());
    EXPECT_DOUBLE_EQ(v.get<double>(), 3.14);
}

TEST(DbValue, FloatConvertedToDouble) {
    Value v = 1.5f;
    EXPECT_EQ(v.type(), Value::Type::Float64);
    EXPECT_FLOAT_EQ(static_cast<float>(v.get<double>()), 1.5f);
}

TEST(DbValue, Bool) {
    Value vt = true;
    Value vf = false;
    EXPECT_EQ(vt.type(), Value::Type::Bool);
    EXPECT_TRUE(vt.get<bool>());
    EXPECT_FALSE(vf.get<bool>());
}

TEST(DbValue, Text) {
    std::string_view sv = "hello, qbuem";
    Value v = sv;
    EXPECT_EQ(v.type(), Value::Type::Text);
    EXPECT_TRUE(v.is<std::string_view>());
    EXPECT_EQ(v.get<std::string_view>(), sv);
}

TEST(DbValue, Blob) {
    static const uint8_t data[] = {0x01, 0x02, 0x03};
    qbuem::BufferView bv{data, sizeof(data)};
    Value v = bv;
    EXPECT_EQ(v.type(), Value::Type::Blob);
    EXPECT_TRUE(v.is<qbuem::BufferView>());
    auto out = v.get<qbuem::BufferView>();
    EXPECT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], 0x01);
}

// ─── Size constraint ─────────────────────────────────────────────────────────

TEST(DbValue, SizeConstraint) {
    EXPECT_LE(sizeof(Value), 32u);
}

// ─── Equality comparison ─────────────────────────────────────────────────────

TEST(DbValue, EqualityNull) {
    EXPECT_EQ(Value{}, Value{null});
}

TEST(DbValue, EqualityInt) {
    EXPECT_EQ(Value{int64_t{100}}, Value{int64_t{100}});
    EXPECT_NE(Value{int64_t{1}},   Value{int64_t{2}});
}

TEST(DbValue, EqualityText) {
    EXPECT_EQ(Value{std::string_view{"abc"}}, Value{std::string_view{"abc"}});
    EXPECT_NE(Value{std::string_view{"abc"}}, Value{std::string_view{"xyz"}});
}

TEST(DbValue, TypeMismatchNotEqual) {
    EXPECT_NE(Value{int64_t{0}}, Value{false});
}

// ─── BoundParams ─────────────────────────────────────────────────────────────

TEST(DbValue, BoundParamsStack) {
    BoundParams<4> params;
    params.bind(int64_t{1});
    params.bind(std::string_view{"user"});
    params.bind(3.14);
    params.bind(null);

    auto sp = params.span();
    EXPECT_EQ(sp.size(), 4u);
    EXPECT_EQ(sp[0].get<int64_t>(), 1);
    EXPECT_EQ(sp[1].get<std::string_view>(), "user");
    EXPECT_DOUBLE_EQ(sp[2].get<double>(), 3.14);
    EXPECT_TRUE(sp[3].is_null());
}

TEST(DbValue, BoundParamsOverflowSafe) {
    BoundParams<2> params;
    params.bind(int64_t{1});
    params.bind(int64_t{2});
    params.bind(int64_t{3}); // overflow — silently dropped
    EXPECT_EQ(params.span().size(), 2u);
}

TEST(DbValue, BoundParamsDefaultCapacity) {
    BoundParams<> params; // default N=8
    for (int i = 0; i < 8; ++i) params.bind(int64_t{i});
    EXPECT_EQ(params.span().size(), 8u);
}
