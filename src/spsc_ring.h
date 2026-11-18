// spsc_ring.h - A lock-free buffer between Winamp's decoder and the audio thread.
//
// "SPSC" means single producer, single consumer:
//   * The producer is Winamp's decoding thread, which calls write().
//   * The consumer is our real-time render thread, which calls read().
//
// Why lock-free? The render thread has only a few milliseconds to deliver audio.
// If it had to wait on a lock held by a lower-priority thread, the sound could
// drop out. Here neither side ever waits for the other.
//
// How it works: readPos_ and writePos_ count forward through the range
// 0..(2 x capacity), then wrap. Using twice the capacity distinguishes "empty"
// (positions equal) from "full" (positions exactly one capacity apart). Each side
// changes only its own position, and publishes it after the bytes are in place.
//
// Seeking: a seek must throw away old audio, but only the consumer may move the
// read position. requestDiscard() therefore leaves a marker saying "everything
// written so far is stale", and the consumer skips to that marker. The producer
// keeps counting the stale bytes as occupied until then, so it can never overwrite
// memory the consumer might still be copying.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

class SpscRing {
public:
    // Sets the capacity and empties the buffer. No other thread may be using the
    // ring during this call.
    void allocate(size_t capacityBytes) {
        storage_.assign(capacityBytes ? capacityBytes : 1, 0);
        readPos_.store(0, std::memory_order_relaxed);
        writePos_.store(0, std::memory_order_relaxed);
        discardTo_.store(kNoDiscard, std::memory_order_relaxed);
    }

    size_t capacity() const { return storage_.size(); }

    // Producer: how many bytes can be written right now.
    size_t freeBytes() const {
        const size_t read = readPos_.load(std::memory_order_acquire);
        const size_t write = writePos_.load(std::memory_order_relaxed);
        return capacity() - distance(read, write);
    }

    // Producer: copies up to `bytes` into the ring and returns how many were copied.
    size_t write(const unsigned char* source, size_t bytes) {
        const size_t room = freeBytes();
        if (bytes > room) bytes = room;
        if (!bytes) return 0;

        const size_t position = writePos_.load(std::memory_order_relaxed);
        const size_t index = position % capacity();
        const size_t untilEnd = capacity() - index;
        const size_t first = bytes < untilEnd ? bytes : untilEnd;
        memcpy(&storage_[index], source, first);
        if (bytes > first) memcpy(&storage_[0], source + first, bytes - first);

        // "release" makes the copied bytes visible before the new position.
        writePos_.store(advance(position, bytes), std::memory_order_release);
        return bytes;
    }

    // Consumer: copies up to `bytes` out of the ring and returns how many were copied.
    size_t read(unsigned char* destination, size_t bytes) {
        applyDiscard();
        const size_t position = readPos_.load(std::memory_order_relaxed);
        const size_t available = distance(position, writePos_.load(std::memory_order_acquire));
        if (bytes > available) bytes = available;
        if (!bytes) return 0;

        const size_t index = position % capacity();
        const size_t untilEnd = capacity() - index;
        const size_t first = bytes < untilEnd ? bytes : untilEnd;
        memcpy(destination, &storage_[index], first);
        if (bytes > first) memcpy(destination + first, &storage_[0], bytes - first);

        // "release" tells the producer this space may now be reused.
        readPos_.store(advance(position, bytes), std::memory_order_release);
        return bytes;
    }

    // Any thread: marks everything written so far as stale.
    void requestDiscard() {
        discardTo_.store(writePos_.load(std::memory_order_acquire), std::memory_order_release);
    }

    // Consumer: acts on a pending discard, freeing that space for the producer.
    void applyDiscard() {
        const size_t target = discardTo_.exchange(kNoDiscard, std::memory_order_acq_rel);
        if (target == kNoDiscard) return;

        // A read may have already passed the marker. Only ever move forward, so audio
        // that has been played is never played a second time.
        const size_t position = readPos_.load(std::memory_order_relaxed);
        const size_t available = distance(position, writePos_.load(std::memory_order_acquire));
        if (distance(position, target) <= available) readPos_.store(target, std::memory_order_release);
    }

    // Any thread: bytes still waiting to be played, not counting stale audio.
    size_t pendingBytes() const {
        const size_t discard = discardTo_.load(std::memory_order_acquire);
        const size_t start = discard != kNoDiscard ? discard : readPos_.load(std::memory_order_acquire);
        return distance(start, writePos_.load(std::memory_order_acquire));
    }

private:
    static constexpr size_t kNoDiscard = static_cast<size_t>(-1);

    // Positions wrap at twice the capacity; see the explanation at the top of the file.
    size_t wrapLimit() const { return capacity() * 2; }
    size_t advance(size_t position, size_t bytes) const { return (position + bytes) % wrapLimit(); }
    size_t distance(size_t from, size_t to) const { return (to + wrapLimit() - from) % wrapLimit(); }

    std::vector<unsigned char> storage_ = std::vector<unsigned char>(1, 0);

    // Kept on separate CPU cache lines so the two threads do not slow each other down.
    alignas(64) std::atomic<size_t> readPos_{0};
    alignas(64) std::atomic<size_t> writePos_{0};
    alignas(64) std::atomic<size_t> discardTo_{kNoDiscard};
};
