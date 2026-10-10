#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <vector>

namespace vt {

// Wait-free single-producer / single-consumer FIFO. Capacity must be a power of two.
// Allocation happens only in the constructor.
template <class T>
class SpscFifo
{
public:
    explicit SpscFifo(size_t capacityPow2)
        : buffer(capacityPow2), mask(capacityPow2 - 1)
    {
        assert(capacityPow2 >= 2 && (capacityPow2 & mask) == 0);
    }

    size_t push(const T* data, size_t n)
    {
        const size_t w = writePos.load(std::memory_order_relaxed);
        const size_t r = readPos.load(std::memory_order_acquire);
        const size_t space = buffer.size() - (w - r);
        if (n > space) n = space;
        for (size_t i = 0; i < n; ++i) buffer[(w + i) & mask] = data[i];
        writePos.store(w + n, std::memory_order_release);
        return n;
    }

    size_t pop(T* data, size_t n)
    {
        const size_t r = readPos.load(std::memory_order_relaxed);
        const size_t w = writePos.load(std::memory_order_acquire);
        const size_t avail = w - r;
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; ++i) data[i] = buffer[(r + i) & mask];
        readPos.store(r + n, std::memory_order_release);
        return n;
    }

    size_t available() const
    {
        return writePos.load(std::memory_order_acquire) - readPos.load(std::memory_order_acquire);
    }

    size_t space() const
    {
        const size_t w = writePos.load(std::memory_order_relaxed);
        const size_t r = readPos.load(std::memory_order_acquire);
        return buffer.size() - (w - r);
    }

    void clear() { readPos.store(writePos.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<T> buffer;
    const size_t mask;
    alignas(64) std::atomic<size_t> writePos { 0 };
    alignas(64) std::atomic<size_t> readPos { 0 };
};

} // namespace vt
