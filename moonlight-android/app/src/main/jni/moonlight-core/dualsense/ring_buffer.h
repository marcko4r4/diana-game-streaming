#ifndef DUALSENSE_RING_BUFFER_H
#define DUALSENSE_RING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

// Capacity in frames (each frame = 4 channels of int16_t = 8 bytes)
// 4096 frames = ~85.3 ms at 48 kHz (power of 2, mask = 4095)
#define HAPTICS_RING_BUFFER_CAPACITY 4096
#define HAPTICS_RING_BUFFER_MASK     (HAPTICS_RING_BUFFER_CAPACITY - 1)

typedef struct {
    int16_t ch[4]; // 0: L audio, 1: R audio, 2: L haptic, 3: R haptic
} HapticsFrame;

typedef struct {
    HapticsFrame buffer[HAPTICS_RING_BUFFER_CAPACITY];
    atomic_size_t head; // Written by producer
    atomic_size_t tail; // Written by consumer
} HapticsRingBuffer;

static inline void HapticsRingBuffer_Init(HapticsRingBuffer* rb) {
    atomic_store_explicit(&rb->head, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);
    memset(rb->buffer, 0, sizeof(rb->buffer));
}

static inline size_t HapticsRingBuffer_AvailableRead(const HapticsRingBuffer* rb) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    return head - tail;
}

static inline size_t HapticsRingBuffer_AvailableWrite(const HapticsRingBuffer* rb) {
    return HAPTICS_RING_BUFFER_CAPACITY - HapticsRingBuffer_AvailableRead(rb);
}

// Push 2-channel stereo S16LE samples from Vibepollo, converting to 4-channel DualSense frames
static inline size_t HapticsRingBuffer_PushStereo(HapticsRingBuffer* rb, const int16_t* stereoPcm, size_t frameCount) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    size_t current = head - tail;

    // Latency clamping: if buffer has accumulated more than 35 ms (~1680 frames),
    // skip oldest frames to keep latency tightly bounded under ~15 ms.
    if (current > 1680) {
        size_t drop = current - 720; // reset to 15 ms
        tail += drop;
        atomic_store_explicit(&rb->tail, tail, memory_order_release);
    }

    size_t toWrite = HAPTICS_RING_BUFFER_CAPACITY - (head - tail);
    if (frameCount < toWrite) {
        toWrite = frameCount;
    }

    for (size_t i = 0; i < toWrite; i++) {
        size_t idx = (head + i) & HAPTICS_RING_BUFFER_MASK;
        rb->buffer[idx].ch[0] = 0;                     // Left ear headphone: silent
        rb->buffer[idx].ch[1] = 0;                     // Right ear headphone: silent
        rb->buffer[idx].ch[2] = stereoPcm[i * 2];     // Left haptic actuator
        rb->buffer[idx].ch[3] = stereoPcm[i * 2 + 1]; // Right haptic actuator
    }

    atomic_store_explicit(&rb->head, head + toWrite, memory_order_release);
    return toWrite;
}

// Pop 4-channel frames for USB Isochronous transmission (returns frames read, pads with 0 on underrun)
static inline size_t HapticsRingBuffer_PopFrames(HapticsRingBuffer* rb, HapticsFrame* outFrames, size_t requestedCount) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t available = head - tail;

    size_t readCount = requestedCount < available ? requestedCount : available;

    for (size_t i = 0; i < readCount; i++) {
        size_t idx = (tail + i) & HAPTICS_RING_BUFFER_MASK;
        outFrames[i] = rb->buffer[idx];
    }

    // Pad remaining requested frames with silence (underrun click protection)
    if (readCount < requestedCount) {
        memset(&outFrames[readCount], 0, (requestedCount - readCount) * sizeof(HapticsFrame));
    }

    atomic_store_explicit(&rb->tail, tail + readCount, memory_order_release);
    return readCount;
}

#endif // DUALSENSE_RING_BUFFER_H
