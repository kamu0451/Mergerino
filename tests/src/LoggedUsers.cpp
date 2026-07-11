// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "controllers/logging/LoggedUsers.hpp"

#include "Test.hpp"

#include <QString>

using namespace chatterino;

namespace {

struct TestCase {
    QString input;
    QString expected;
};

}  // namespace

TEST(LoggedUsers, SimplePassthroughAndLowercase)
{
    std::vector<TestCase> tests{
        {"testuser", "testuser"},
        {"TestUser", "testuser"},
        {"  TestUser  ", "testuser"},
        {"PAJLADA", "pajlada"},
    };

    for (const auto &c : tests)
    {
        EXPECT_EQ(logging::normalizeLoggedUserName(c.input), c.expected)
            << "input: " << c.input;
    }
}

TEST(LoggedUsers, AtSignStripping)
{
    std::vector<TestCase> tests{
        {"@TestUser", "testuser"},
        {"@@TestUser", "testuser"},
        {"@@@foo", "foo"},
        // '@' in the middle is not a leading marker and is left alone.
        {"foo@bar", "foo@bar"},
    };

    for (const auto &c : tests)
    {
        EXPECT_EQ(logging::normalizeLoggedUserName(c.input), c.expected)
            << "input: " << c.input;
    }
}

TEST(LoggedUsers, PathSeparatorsAreStripped)
{
    std::vector<TestCase> tests{
        {"a/b", "ab"},
        {"a\\b", "ab"},
        {"a/b\\c", "abc"},
    };

    for (const auto &c : tests)
    {
        EXPECT_EQ(logging::normalizeLoggedUserName(c.input), c.expected)
            << "input: " << c.input;
    }
}

// Dot-only and dot-terminated names are rejected outright (empty result)
// since Windows resolves "." / ".." as path segments and silently drops
// trailing dots from file names. A traversal payload that still contains a
// path separator has that separator stripped first, so it may survive as a
// dot-containing (but otherwise harmless) filename - pinning that actual
// behavior below rather than assuming that "../x" gets fully sanitized to "x".
TEST(LoggedUsers, TraversalPayloads)
{
    EXPECT_EQ(logging::normalizeLoggedUserName("."), QString());
    EXPECT_EQ(logging::normalizeLoggedUserName(".."), QString());
    EXPECT_EQ(logging::normalizeLoggedUserName("..."), QString());
    EXPECT_EQ(logging::normalizeLoggedUserName("x.."), QString());
    EXPECT_EQ(logging::normalizeLoggedUserName("..x"), QString("..x"));

    // The slashes are removed before the trailing-dot check runs, so this
    // collapses to "..x" instead of being rejected.
    EXPECT_EQ(logging::normalizeLoggedUserName("../x"), QString("..x"));
    EXPECT_EQ(logging::normalizeLoggedUserName("..\\x"), QString("..x"));
}

TEST(LoggedUsers, WindowsIllegalCharsAreStripped)
{
    std::vector<TestCase> tests{
        {"a:b", "ab"},
        {"a*b", "ab"},
        {"a?b", "ab"},
        {"a\"b", "ab"},
        {"a<b", "ab"},
        {"a>b", "ab"},
        {"a|b", "ab"},
        {"a:b*c?d\"e<f>g|h", "abcdefgh"},
        // Control characters (here: a tab, U+0009) are stripped too.
        {QStringLiteral("foo\tbar"), "foobar"},
    };

    for (const auto &c : tests)
    {
        EXPECT_EQ(logging::normalizeLoggedUserName(c.input), c.expected)
            << "input: " << c.input;
    }
}

// normalizeLoggedUserName does not special-case Windows reserved device
// names (CON, PRN, AUX, NUL, COM1, LPT1, ...) - they pass through unchanged
// (aside from lowercasing). This pins the current behavior; it is not an
// endorsement that it's safe to write a file literally named "con".
TEST(LoggedUsers, ReservedNamesPassThroughUnchanged)
{
    std::vector<TestCase> tests{
        {"CON", "con"},
        {"PRN", "prn"},
        {"AUX", "aux"},
        {"NUL", "nul"},
        {"COM1", "com1"},
        {"LPT1", "lpt1"},
    };

    for (const auto &c : tests)
    {
        EXPECT_EQ(logging::normalizeLoggedUserName(c.input), c.expected)
            << "input: " << c.input;
    }
}
