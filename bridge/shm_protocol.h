/*
 * shm_protocol.h
 *
 * Shared memory layout for djmt1-bridge <-> AudioServerPlugin IPC.
 * This header is included by both the bridge process and the HAL plugin.
 *
 * IPC mechanism: POSIX shared memory (shm_open + mmap) with lock-free
 * SPSC (Single-Producer/Single-Consumer) ring buffers using std::atomic.
 *
 * Ring buffer protocol:
 *   - IN ring:  bridge produces (USB IN data), plugin consumes (CoreAudio input)
 *   - OUT ring: plugin produces (CoreAudio output), bridge consumes (USB OUT data)
 *
 * Position counters are monotonically increasing byte offsets (never wrap).
 * Actual ring index = position % ring_bytes.
 * Available data   = write_pos - read_pos.
 * Free space       = ring_bytes - (write_pos - read_pos).
 *
 * Requirements:
 *   - std::atomic<uint64_t> must be lock-free (guaranteed on macOS ARM64/x86_64)
 *   - Both processes must include this same header
 */

#ifndef DJMT1_SHM_PROTOCOL_H
#define DJMT1_SHM_PROTOCOL_H

#include <atomic>
#include <cstdint>
#include <cstring>

/* ── POSIX shared memory name ── */
#define DJMT1_SHM_NAME  "/djmt1-audio"

/* ── Audio format constants (matching DJMT1IsoEngine.h) ── */
#define DJMT1_SHM_CHANNELS          6
#define DJMT1_SHM_BITS_PER_SAMPLE   24
#define DJMT1_SHM_BYTES_PER_SAMPLE  3
#define DJMT1_SHM_SAMPLE_RATE       48000
#define DJMT1_SHM_FRAME_SIZE        (DJMT1_SHM_CHANNELS * DJMT1_SHM_BYTES_PER_SAMPLE) /* 18 */

/* ── Ring buffer capacity ── */
#define DJMT1_SHM_RING_FRAMES  256   /* ~5.3ms at 48kHz — optimized for DJ low-latency */
#define DJMT1_SHM_RING_BYTES   (DJMT1_SHM_RING_FRAMES * DJMT1_SHM_FRAME_SIZE) /* 4608 */

/* ── Protocol identification ── */
#define DJMT1_SHM_MAGIC    0x444A4D31u  /* "DJM1" */
#define DJMT1_SHM_VERSION  1u

/* ── Status flags (atomic, bitfield) ── */
#define DJMT1_SHM_FLAG_RUNNING   (1u << 0)  /* bridge process is alive */
#define DJMT1_SHM_FLAG_STREAMING (1u << 1)  /* isoch transfers active */
#define DJMT1_SHM_FLAG_DEVICE_OK (1u << 2)  /* USB device connected and claimed */

/* ── USB isochronous parameters ── */
#define DJMT1_ISOC_PACKET_SIZE      864   /* nominal bytes/ms (48 frames x 18) */
#define DJMT1_ISOC_PACKET_SIZE_MAX  882   /* max bytes/packet (49 frames x 18) */
#define DJMT1_ISOC_MAX_PACKET       1024  /* USB descriptor maxPacketSize */
#define DJMT1_ISOC_FRAMES_PER_XFER  4     /* isochronous packets per transfer (4ms/URB) */
#define DJMT1_NUM_XFERS             4     /* in-flight transfers per direction (16ms total) */

/*
 * Shared memory structure.
 *
 * Layout:
 *   Offset  0: header (32 bytes: 8 x uint32_t)
 *   Offset 32: atomic counters (64 bytes: 6 x uint64_t + 1 x uint32_t + pad)
 *   Offset 96: IN ring buffer  (4608 bytes)
 *   Offset 96+4608: OUT ring buffer (4608 bytes)
 *   Total: 9312 bytes (~9 KB)
 */
struct djmt1_shm {
    /* ── Fixed header (initialized once by bridge) ── */
    uint32_t magic;
    uint32_t version;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t frame_size;
    uint32_t ring_frames;
    uint32_t ring_bytes;
    /* 32 bytes */

    /* ── Atomic counters ── */
    std::atomic<uint64_t> in_write_pos;   /* bridge writes (byte offset, monotonic) */
    std::atomic<uint64_t> in_read_pos;    /* plugin reads */
    std::atomic<uint64_t> out_write_pos;  /* plugin writes */
    std::atomic<uint64_t> out_read_pos;   /* bridge reads */
    std::atomic<uint64_t> sample_count;   /* total IN sample frames received */
    std::atomic<uint64_t> heartbeat;      /* mach_absolute_time from bridge */
    std::atomic<uint32_t> flags;          /* DJMT1_SHM_FLAG_* */
    uint32_t _reserved[3];
    /* 64 bytes (32 + 32 padding from _reserved) */

    /* ── Ring buffers ── */
    uint8_t in_ring[DJMT1_SHM_RING_BYTES];
    uint8_t out_ring[DJMT1_SHM_RING_BYTES];
};

/* Total shared memory size */
#define DJMT1_SHM_SIZE  sizeof(struct djmt1_shm)

/* ── Ring buffer helper functions ── */

/*
 * Write data into a ring buffer (producer side).
 * Positions are monotonically increasing byte counters.
 */
static inline void shm_ring_write(uint8_t *ring, uint32_t cap,
                                  std::atomic<uint64_t> &write_pos,
                                  const uint8_t *data, uint32_t len)
{
    uint64_t wp = write_pos.load(std::memory_order_relaxed);
    uint32_t offset = (uint32_t)(wp % cap);
    uint32_t chunk1 = cap - offset;

    if (chunk1 >= len) {
        memcpy(ring + offset, data, len);
    } else {
        memcpy(ring + offset, data, chunk1);
        memcpy(ring, data + chunk1, len - chunk1);
    }

    write_pos.store(wp + len, std::memory_order_release);
}

/*
 * Read data from a ring buffer (consumer side).
 * Returns the number of bytes actually read (may be less than requested on underrun).
 */
static inline uint32_t shm_ring_read(const uint8_t *ring, uint32_t cap,
                                     std::atomic<uint64_t> &read_pos,
                                     const std::atomic<uint64_t> &write_pos,
                                     uint8_t *dst, uint32_t max_len)
{
    uint64_t wp = write_pos.load(std::memory_order_acquire);
    uint64_t rp = read_pos.load(std::memory_order_relaxed);
    uint64_t available = wp - rp;

    /* Overflow: producer has lapped consumer — data is corrupted.
       Skip to current write position and return silence for this cycle. */
    if (available > cap) {
        read_pos.store(wp, std::memory_order_release);
        return 0;
    }

    if (available == 0) return 0;

    uint32_t to_read = (available < max_len) ? (uint32_t)available : max_len;
    uint32_t offset = (uint32_t)(rp % cap);
    uint32_t chunk1 = cap - offset;

    if (chunk1 >= to_read) {
        memcpy(dst, ring + offset, to_read);
    } else {
        memcpy(dst, ring + offset, chunk1);
        memcpy(dst + chunk1, ring, to_read - chunk1);
    }

    read_pos.store(rp + to_read, std::memory_order_release);
    return to_read;
}

/*
 * Query available data in a ring buffer (bytes).
 */
static inline uint64_t shm_ring_available(const std::atomic<uint64_t> &write_pos,
                                          const std::atomic<uint64_t> &read_pos)
{
    uint64_t wp = write_pos.load(std::memory_order_acquire);
    uint64_t rp = read_pos.load(std::memory_order_relaxed);
    return wp - rp;
}

/*
 * Initialize shared memory header (called by bridge on creation).
 */
static inline void shm_init(struct djmt1_shm *shm)
{
    memset(shm, 0, sizeof(*shm));
    shm->magic           = DJMT1_SHM_MAGIC;
    shm->version         = DJMT1_SHM_VERSION;
    shm->sample_rate     = DJMT1_SHM_SAMPLE_RATE;
    shm->channels        = DJMT1_SHM_CHANNELS;
    shm->bits_per_sample = DJMT1_SHM_BITS_PER_SAMPLE;
    shm->frame_size      = DJMT1_SHM_FRAME_SIZE;
    shm->ring_frames     = DJMT1_SHM_RING_FRAMES;
    shm->ring_bytes      = DJMT1_SHM_RING_BYTES;

    shm->in_write_pos.store(0, std::memory_order_relaxed);
    shm->in_read_pos.store(0, std::memory_order_relaxed);
    shm->out_write_pos.store(0, std::memory_order_relaxed);
    shm->out_read_pos.store(0, std::memory_order_relaxed);
    shm->sample_count.store(0, std::memory_order_relaxed);
    shm->heartbeat.store(0, std::memory_order_relaxed);
    shm->flags.store(0, std::memory_order_relaxed);
}

/*
 * Validate shared memory header (called by plugin on open).
 */
static inline bool shm_validate(const struct djmt1_shm *shm)
{
    return shm->magic   == DJMT1_SHM_MAGIC &&
           shm->version == DJMT1_SHM_VERSION &&
           shm->frame_size == DJMT1_SHM_FRAME_SIZE &&
           shm->ring_bytes == DJMT1_SHM_RING_BYTES;
}

#endif /* DJMT1_SHM_PROTOCOL_H */
