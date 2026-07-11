// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/BoostJsonWrap.hpp"

#include "Test.hpp"

#include <boost/json/parse.hpp>

using namespace chatterino;

TEST(BoostJsonWrap, arrayIntegerIndex)
{
    auto jv = boost::json::parse(R"([10, "twenty", true, 40])");
    BoostJsonValue root(jv);

    ASSERT_TRUE(root.isArray());

    // Integer-index each element through the wrapper and confirm the value at
    // that position (not some out-of-bounds neighbour) comes back.
    EXPECT_EQ(root[static_cast<size_t>(0)].toInt64(-1), 10);
    EXPECT_EQ(root[static_cast<size_t>(1)].toStdString(), "twenty");
    EXPECT_TRUE(root[static_cast<size_t>(2)].toBool());
    EXPECT_EQ(root[static_cast<size_t>(3)].toInt64(-1), 40);
}

TEST(BoostJsonWrap, arrayIndexOutOfRange)
{
    auto jv = boost::json::parse(R"([1, 2, 3])");
    BoostJsonValue root(jv);

    ASSERT_TRUE(root.isArray());

    // Out-of-range indexing must not crash; the wrapper contract is to return
    // an undefined value.
    EXPECT_TRUE(root[static_cast<size_t>(3)].isUndefined());
    EXPECT_TRUE(root[static_cast<size_t>(100)].isUndefined());
}

TEST(BoostJsonWrap, indexNonArray)
{
    auto jv = boost::json::parse(R"({"a": 1})");
    BoostJsonValue root(jv);

    // Integer-indexing a non-array value returns an undefined value.
    EXPECT_TRUE(root[static_cast<size_t>(0)].isUndefined());
}
