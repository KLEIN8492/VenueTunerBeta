#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "measure/Align.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("Align to latest arrival")
{
    // Main arrives at 20 ms (no delay), fill at 12 ms (no delay), delay tower at 25 ms (5 ms set).
    auto r = vt::computeAlignment({ { 0, 20, 0 }, { 1, 12, 0 }, { 2, 25, 5 } }, -1);
    REQUIRE(r.size() == 3);
    CHECK_THAT(r[0].delayMs, WithinAbs(5.0, 1e-9));
    CHECK_THAT(r[1].delayMs, WithinAbs(13.0, 1e-9));
    CHECK_THAT(r[2].delayMs, WithinAbs(5.0, 1e-9)); // latest keeps its delay
}

TEST_CASE("Align to a reference output, shifting if needed")
{
    auto r = vt::computeAlignment({ { 0, 20, 0 }, { 1, 12, 0 } }, 0);
    CHECK_THAT(r[0].delayMs, WithinAbs(0.0, 1e-9));
    CHECK_THAT(r[1].delayMs, WithinAbs(8.0, 1e-9));
    // Reference arrives first: the later one can't go negative, so the reference gets delayed.
    r = vt::computeAlignment({ { 0, 10, 0 }, { 1, 14, 0 } }, 0);
    CHECK_THAT(r[0].delayMs, WithinAbs(4.0, 1e-9));
    CHECK_THAT(r[1].delayMs, WithinAbs(0.0, 1e-9));
}
