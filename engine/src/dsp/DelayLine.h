#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace vt {

// Integer-sample delay line. A delay change crossfades from the old tap to the new
// one over 50 ms so live edits never click. Allocates only in prepare().
class DelayLine
{
public:
    void prepare(double sampleRate, int maxDelaySamples)
    {
        size_t size = 1;
        while (size < size_t(maxDelaySamples) + 2) size <<= 1;
        buffer.assign(size, 0.0f);
        mask = size - 1;
        maxDelay = maxDelaySamples;
        fadeLen = std::max(1, int(std::lround(0.05 * sampleRate)));
        current = target = pending = 0;
        fadePos = -1;
        writePos = 0;
    }

    void setDelaySamples(int d)
    {
        pending = std::clamp(d, 0, maxDelay);
    }

    int delaySamples() const { return pending; }
    void clear() { std::fill(buffer.begin(), buffer.end(), 0.0f); current = target = pending; fadePos = -1; }

    float process(float x)
    {
        if (fadePos < 0 && pending != current) {
            target = pending;
            fadePos = 0;
        }
        buffer[writePos & mask] = x;
        float y = buffer[(writePos - size_t(current)) & mask];
        if (fadePos >= 0) {
            const float a = float(fadePos) / float(fadeLen);
            const float yNew = buffer[(writePos - size_t(target)) & mask];
            y = (1.0f - a) * y + a * yNew;
            if (++fadePos > fadeLen) { current = target; fadePos = -1; }
        }
        ++writePos;
        return y;
    }

private:
    std::vector<float> buffer;
    size_t mask = 0, writePos = 0;
    int maxDelay = 0, fadeLen = 1;
    int current = 0, target = 0, pending = 0, fadePos = -1;
};

} // namespace vt
