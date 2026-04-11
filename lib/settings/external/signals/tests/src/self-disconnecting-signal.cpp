#include <pajlada/signals/signal.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace pajlada::Signals;

TEST(SelfDisconnectingSignal, MultipleConnects)
{
    NoArgSelfDisconnectingSignal signal;

    int a = 0;
    int b = 0;

    signal.connect([&] {
        a++;
        return a == 1;
    });

    signal.connect([&] {
        b++;
        return b == 2;
    });

    signal.invoke();

    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);

    signal.invoke();

    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);

    signal.invoke();

    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
}

TEST(SelfDisconnectingSignal, InvokeOwned)
{
    SelfDisconnectingSignal<std::string> signal;

    bool called = false;
    auto consumerOwned = [&](std::string s) {
        EXPECT_EQ(s, "Yes, this is a really long long string!");
        called = true;
        return false;
    };
    auto consumerConstRef = [&](const std::string &s) {
        EXPECT_EQ(s, "Yes, this is a really long long string!");
        called = true;
        return false;
    };

    signal.connect(consumerOwned);
    signal.connect(consumerConstRef);
    signal.connect(consumerOwned);
    signal.connect(consumerConstRef);
    signal.invoke("Yes, this is a really long long string!");
    EXPECT_TRUE(called);
    called = false;

    std::string owned = "Yes, this is a really long long string!";
    signal.invoke(owned);
    EXPECT_TRUE(called);
}
