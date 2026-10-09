#include <catch2/catch_test_macros.hpp>
#include "measure/LevelLogger.h"
#include "core/Features.h"

TEST_CASE("Level logger writes daily CSV files and prunes old ones")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("vt_logger_test");
    dir.deleteRecursively();
    vt::LevelLogger log(dir, 183);
    juce::String err;
    const juce::Time day1(2026, 9, 9, 21, 30, 0); // month is 0-based: 9 October 2026
    REQUIRE(log.append({ day1, 98.24, 110.0, 97.5, 112.3, 128.9, "calibrator" }, err));
    REQUIRE(log.append({ day1 + juce::RelativeTime::seconds(1), 99.0, -200, 97.6, 112.3, 129.0, "calibrator" }, err));
    const auto f = log.fileFor(day1);
    CHECK(f.getFileName() == "2026-10-09.csv");
    auto lines = juce::StringArray::fromLines(f.loadFileAsString().trim());
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "time,LAeq_1s,LCeq_1s,LAeq_15min,LCeq_15min,LCpeak,calibration");
    CHECK(lines[1].contains(",98.2,110.0,97.5,112.3,128.9,calibrator"));
    CHECK(lines[2].contains(",99.0,,97.6,")); // missing value left empty, not -200

    // An old file is removed when a new day starts.
    auto old = dir.getChildFile("2026-01-01.csv");
    old.replaceWithText("x");
    old.setLastModificationTime(day1 - juce::RelativeTime::days(200));
    REQUIRE(log.append({ day1 + juce::RelativeTime::days(1), 90, 100, 90, 100, 110, "estimate" }, err));
    CHECK_FALSE(old.existsAsFile());
    CHECK(log.fileFor(day1 + juce::RelativeTime::days(1)).existsAsFile());
    dir.deleteRecursively();
}

TEST_CASE("The level logger is asleep in this version")
{
    STATIC_CHECK_FALSE(vt::features::kLevelLogger);
}
