#pragma once
#include <juce_core/juce_core.h>

namespace vt {

// Records sound levels once per second to a daily CSV file (<dir>/YYYY-MM-DD.csv) and deletes
// files older than `keepDays` (the decree asks for 6 months). It only records: it never acts on
// the audio. Message thread only.
class LevelLogger
{
public:
    struct Row
    {
        juce::Time time;
        double laeq1s, lceq1s, laeq15, lceq15, lcPeak;
        juce::String calibration; // calibrator | reference | estimate | default
    };

    explicit LevelLogger(juce::File dir, int keepDays = 183) : dir(std::move(dir)), keepDays(keepDays) {}

    bool append(const Row& r, juce::String& error)
    {
        if (!dir.isDirectory() && !dir.createDirectory()) { error = "cannot create " + dir.getFullPathName(); return false; }
        const auto file = fileFor(r.time);
        const bool fresh = !file.existsAsFile();
        juce::FileOutputStream out(file);
        if (!out.openedOk()) { error = "cannot write " + file.getFullPathName(); return false; }
        out.setPosition(out.getFile().getSize());
        if (fresh) {
            out << "time,LAeq_1s,LCeq_1s,LAeq_15min,LCeq_15min,LCpeak,calibration\n";
            pruneOld(r.time);
        }
        auto f = [](double v) { return v > -150 ? juce::String(v, 1) : juce::String(); };
        out << r.time.toISO8601(true) << "," << f(r.laeq1s) << "," << f(r.lceq1s) << "," << f(r.laeq15) << ","
            << f(r.lceq15) << "," << f(r.lcPeak) << "," << r.calibration << "\n";
        return true;
    }

    juce::File fileFor(juce::Time t) const { return dir.getChildFile(t.formatted("%Y-%m-%d") + ".csv"); }

private:
    void pruneOld(juce::Time now)
    {
        const auto cutoff = now - juce::RelativeTime::days(keepDays);
        for (auto& f : dir.findChildFiles(juce::File::findFiles, false, "????-??-??.csv"))
            if (f.getLastModificationTime() < cutoff) f.deleteFile();
    }

    juce::File dir;
    int keepDays;
};

} // namespace vt
