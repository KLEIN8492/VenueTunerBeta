#pragma once
#include <atomic>
#include <cstdint>

namespace vt {

// Lock-free "latest value" exchange between one writer (control thread) and one
// reader (audio thread). Neither side ever blocks or allocates; the reader always
// gets the most recent *complete* value.
template <class T>
class TripleBuffer
{
public:
    TripleBuffer() = default;

    // Writer side: copy into the back slot, then publish it as the middle slot.
    void write(const T& value)
    {
        slots[back] = value;
        const uint8_t prev = middle.exchange(static_cast<uint8_t>(back | kDirty), std::memory_order_acq_rel);
        back = prev & kIndexMask;
    }

    // Reader side: if something new was published, take it; else keep the current.
    const T& read()
    {
        if (middle.load(std::memory_order_relaxed) & kDirty) {
            const uint8_t prev = middle.exchange(static_cast<uint8_t>(front), std::memory_order_acq_rel);
            front = prev & kIndexMask;
        }
        return slots[front];
    }

private:
    static constexpr uint8_t kDirty = 0x4;
    static constexpr uint8_t kIndexMask = 0x3;

    T slots[3] {};
    std::atomic<uint8_t> middle { 1 };
    uint8_t front = 0; // owned by reader
    uint8_t back = 2;  // owned by writer
};

} // namespace vt
