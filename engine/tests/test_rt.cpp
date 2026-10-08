#include <catch2/catch_test_macros.hpp>
#include "rt/TripleBuffer.h"
#include "rt/SpscFifo.h"
#include <thread>
#include <vector>

TEST_CASE("TripleBuffer returns the latest written value")
{
    vt::TripleBuffer<int> tb;
    tb.write(1);
    tb.write(2);
    CHECK(tb.read() == 2);
    CHECK(tb.read() == 2); // no new write: same value again
    tb.write(3);
    CHECK(tb.read() == 3);
}

TEST_CASE("SpscFifo preserves order across wraparound and reports full")
{
    vt::SpscFifo<int> f(8);
    int in[6] = {1, 2, 3, 4, 5, 6}, out[8] = {};
    CHECK(f.push(in, 6) == 6);
    CHECK(f.pop(out, 4) == 4);
    CHECK(out[3] == 4);
    CHECK(f.push(in, 6) == 6);       // wraps
    CHECK(f.push(in, 6) == 0);       // full (8 stored)
    CHECK(f.available() == 8);
    CHECK(f.pop(out, 8) == 8);
    CHECK(out[0] == 5);
    CHECK(out[2] == 1);
    CHECK(out[7] == 6);
}

TEST_CASE("SpscFifo is ordered under concurrent producer/consumer")
{
    vt::SpscFifo<int> f(1024);
    constexpr int N = 1'000'000;
    std::thread producer([&] {
        int i = 0;
        while (i < N) { int v = i; if (f.push(&v, 1) == 1) ++i; }
    });
    int expected = 0;
    bool ordered = true;
    while (expected < N) {
        int buf[64];
        auto n = f.pop(buf, 64);
        for (size_t k = 0; k < n; ++k) ordered &= (buf[k] == expected++);
    }
    producer.join();
    CHECK(ordered);
}
