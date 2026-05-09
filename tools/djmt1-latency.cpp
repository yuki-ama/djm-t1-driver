/*
 * djmt1-latency.cpp
 *
 * Latency measurement tool for DJM-T1 audio driver.
 *
 * Mode 1 (default): Query CoreAudio-reported latency + shared memory stats
 * Mode 2 (--loopback): Round-trip impulse loopback test
 *   Requires physical cable from DJM-T1 output back to an input channel.
 *
 * Usage:
 *   ./djmt1-latency                  # report latency values
 *   ./djmt1-latency --loopback       # round-trip impulse test
 *   ./djmt1-latency --bufsize 128    # set CoreAudio buffer size (frames)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <mach/mach_time.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include "shm_protocol.h"

#define DEVICE_UID "DJMT1-AudioPlugin-UID"
#define CHANNELS   6
#define BYTES_PER_SAMPLE 3
#define FRAME_SIZE (CHANNELS * BYTES_PER_SAMPLE)

/* ── 24-bit PCM helpers ── */

static int32_t read_int24(const uint8_t *p)
{
    int32_t val = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    if (val & 0x800000) val |= (int32_t)0xFF000000;
    return val;
}

static void write_int24(uint8_t *p, int32_t val)
{
    p[0] = val & 0xFF;
    p[1] = (val >> 8) & 0xFF;
    p[2] = (val >> 16) & 0xFF;
}

/* ── Find DJM-T1 audio device by UID ── */

static AudioDeviceID find_device(void)
{
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    CFStringRef uid = CFStringCreateWithCString(NULL, DEVICE_UID, kCFStringEncodingUTF8);
    AudioDeviceID deviceID = kAudioObjectUnknown;
    UInt32 size = sizeof(deviceID);

    OSStatus err = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &addr, sizeof(uid), &uid, &size, &deviceID);

    CFRelease(uid);

    if (err != noErr || deviceID == kAudioObjectUnknown) {
        return kAudioObjectUnknown;
    }
    return deviceID;
}

/* ── Query a UInt32 device property ── */

static UInt32 get_uint32_prop(AudioDeviceID dev, AudioObjectPropertySelector sel,
                               AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress addr = { sel, scope, kAudioObjectPropertyElementMain };
    UInt32 val = 0, size = sizeof(val);
    AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &val);
    return val;
}

/* ── Query a Float64 device property ── */

static Float64 get_float64_prop(AudioDeviceID dev, AudioObjectPropertySelector sel,
                                 AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress addr = { sel, scope, kAudioObjectPropertyElementMain };
    Float64 val = 0;
    UInt32 size = sizeof(val);
    AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &val);
    return val;
}

/* ── Set buffer frame size ── */

static bool set_buffer_size(AudioDeviceID dev, UInt32 frames)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    OSStatus err = AudioObjectSetPropertyData(dev, &addr, 0, NULL, sizeof(frames), &frames);
    return err == noErr;
}

/* ── Get stream latency ── */

static UInt32 get_stream_latency(AudioDeviceID dev, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size);
    if (size == 0) return 0;

    AudioStreamID streamID;
    size = sizeof(streamID);
    OSStatus err = AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &streamID);
    if (err != noErr) return 0;

    AudioObjectPropertyAddress sAddr = {
        kAudioStreamPropertyLatency,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 lat = 0;
    size = sizeof(lat);
    AudioObjectGetPropertyData(streamID, &sAddr, 0, NULL, &size, &lat);
    return lat;
}

/* ── Print CoreAudio latency report ── */

static void print_latency_report(AudioDeviceID dev)
{
    Float64 sampleRate = get_float64_prop(dev,
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    UInt32 bufSize = get_uint32_prop(dev,
        kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);

    UInt32 inDevLat = get_uint32_prop(dev,
        kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
    UInt32 outDevLat = get_uint32_prop(dev,
        kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);

    UInt32 inSafety = get_uint32_prop(dev,
        kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);
    UInt32 outSafety = get_uint32_prop(dev,
        kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);

    UInt32 inStreamLat  = get_stream_latency(dev, kAudioObjectPropertyScopeInput);
    UInt32 outStreamLat = get_stream_latency(dev, kAudioObjectPropertyScopeOutput);

    printf("=== CoreAudio Reported Latency ===\n");
    printf("Sample rate:          %.0f Hz\n", sampleRate);
    printf("Buffer size:          %u frames (%.2f ms)\n",
           bufSize, bufSize / sampleRate * 1000.0);
    printf("\n");
    printf("                      Frames     ms\n");
    printf("Input device latency:  %4u     %6.2f\n",
           inDevLat, inDevLat / sampleRate * 1000.0);
    printf("Input stream latency:  %4u     %6.2f\n",
           inStreamLat, inStreamLat / sampleRate * 1000.0);
    printf("Input safety offset:   %4u     %6.2f\n",
           inSafety, inSafety / sampleRate * 1000.0);
    printf("Output device latency: %4u     %6.2f\n",
           outDevLat, outDevLat / sampleRate * 1000.0);
    printf("Output stream latency: %4u     %6.2f\n",
           outStreamLat, outStreamLat / sampleRate * 1000.0);
    printf("Output safety offset:  %4u     %6.2f\n",
           outSafety, outSafety / sampleRate * 1000.0);

    UInt32 totalIn  = bufSize + inDevLat + inStreamLat + inSafety;
    UInt32 totalOut = bufSize + outDevLat + outStreamLat + outSafety;
    UInt32 roundTrip = totalIn + totalOut;

    printf("\n");
    printf("Total input latency:   %4u     %6.2f  (buf + dev + stream + safety)\n",
           totalIn, totalIn / sampleRate * 1000.0);
    printf("Total output latency:  %4u     %6.2f  (buf + dev + stream + safety)\n",
           totalOut, totalOut / sampleRate * 1000.0);
    printf("Round-trip (estimate):  %4u     %6.2f\n",
           roundTrip, roundTrip / sampleRate * 1000.0);
}

/* ── Print shared memory stats ── */

static void print_shm_stats(void)
{
    int fd = shm_open(DJMT1_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) {
        printf("\n=== Shared Memory ===\n");
        printf("Not available (bridge not running?)\n");
        return;
    }

    void *ptr = mmap(NULL, DJMT1_SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) return;

    struct djmt1_shm *shm = (struct djmt1_shm *)ptr;
    if (!shm_validate(shm)) {
        printf("\n=== Shared Memory ===\n");
        printf("Invalid (magic/version mismatch)\n");
        munmap(ptr, DJMT1_SHM_SIZE);
        return;
    }

    uint64_t in_wp = shm->in_write_pos.load(std::memory_order_relaxed);
    uint64_t in_rp = shm->in_read_pos.load(std::memory_order_relaxed);
    uint64_t out_wp = shm->out_write_pos.load(std::memory_order_relaxed);
    uint64_t out_rp = shm->out_read_pos.load(std::memory_order_relaxed);
    uint64_t samples = shm->sample_count.load(std::memory_order_relaxed);
    uint64_t hb = shm->heartbeat.load(std::memory_order_relaxed);
    uint32_t flags = shm->flags.load(std::memory_order_relaxed);

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t now = mach_absolute_time();
    double hb_age_ms = (double)(now - hb) * tb.numer / tb.denom / 1e6;

    uint64_t in_avail = in_wp - in_rp;
    uint64_t out_avail = out_wp - out_rp;
    uint32_t in_frames = (uint32_t)(in_avail / DJMT1_SHM_FRAME_SIZE);
    uint32_t out_frames = (uint32_t)(out_avail / DJMT1_SHM_FRAME_SIZE);

    printf("\n=== Shared Memory (Bridge IPC) ===\n");
    printf("Flags:                %s %s %s\n",
           (flags & DJMT1_SHM_FLAG_RUNNING)   ? "RUNNING"   : "",
           (flags & DJMT1_SHM_FLAG_STREAMING) ? "STREAMING" : "",
           (flags & DJMT1_SHM_FLAG_DEVICE_OK) ? "DEVICE_OK" : "");
    printf("Heartbeat age:        %.1f ms\n", hb_age_ms);
    printf("Total samples IN:     %llu (%.1f sec)\n",
           (unsigned long long)samples, (double)samples / DJMT1_SHM_SAMPLE_RATE);
    printf("IN ring buffer:       %u/%u frames (%u/%u bytes) = %.1f ms buffered\n",
           in_frames, DJMT1_SHM_RING_FRAMES,
           (uint32_t)in_avail, DJMT1_SHM_RING_BYTES,
           (double)in_frames / DJMT1_SHM_SAMPLE_RATE * 1000.0);
    printf("OUT ring buffer:      %u/%u frames (%u/%u bytes) = %.1f ms buffered\n",
           out_frames, DJMT1_SHM_RING_FRAMES,
           (uint32_t)out_avail, DJMT1_SHM_RING_BYTES,
           (double)out_frames / DJMT1_SHM_SAMPLE_RATE * 1000.0);

    /* Measure IN write rate over 500ms */
    uint64_t s1 = shm->sample_count.load(std::memory_order_acquire);
    uint64_t t1 = mach_absolute_time();
    usleep(500000);
    uint64_t s2 = shm->sample_count.load(std::memory_order_acquire);
    uint64_t t2 = mach_absolute_time();

    double dt_sec = (double)(t2 - t1) * tb.numer / tb.denom / 1e9;
    double rate = (double)(s2 - s1) / dt_sec;

    printf("Measured sample rate:  %.1f Hz (over %.0f ms)\n", rate, dt_sec * 1000.0);
    printf("Clock drift:          %+.1f ppm\n",
           (rate - DJMT1_SHM_SAMPLE_RATE) / DJMT1_SHM_SAMPLE_RATE * 1e6);

    munmap(ptr, DJMT1_SHM_SIZE);
}

/* ── Direct shared-memory loopback test ── */
/*
 * Bypasses CoreAudio entirely. Writes an impulse directly into the
 * OUT ring buffer, then monitors the IN ring buffer for the signal
 * coming back through the USB isochronous loop.
 *
 * This measures the true USB round-trip latency:
 *   OUT ring → bridge → USB OUT → DJM-T1 → cable → DJM-T1 → USB IN → bridge → IN ring
 */

static void run_loopback_test(AudioDeviceID dev, int /*bufSize*/)
{
    Float64 sampleRate = get_float64_prop(dev,
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);

    int fd = shm_open(DJMT1_SHM_NAME, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "Cannot open shared memory (bridge not running?)\n");
        return;
    }

    void *ptr = mmap(NULL, DJMT1_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap failed\n");
        return;
    }

    struct djmt1_shm *shm = (struct djmt1_shm *)ptr;
    if (!shm_validate(shm)) {
        fprintf(stderr, "Shared memory invalid\n");
        munmap(ptr, DJMT1_SHM_SIZE);
        return;
    }

    printf("\n=== USB Round-Trip Loopback Test (via shared memory) ===\n");
    printf("Sending impulse via OUT ring, listening on IN ring ch1...\n");
    printf("(Connect DJM-T1 MASTER OUT → LINE IN with cable)\n\n");

    /* Drain any old data in the IN ring by advancing read_pos to write_pos */
    uint64_t in_wp = shm->in_write_pos.load(std::memory_order_acquire);
    shm->in_read_pos.store(in_wp, std::memory_order_release);

    /* Wait a bit for ring to stabilize */
    usleep(50000);  /* 50ms */

    /* Record timestamp and write impulse into OUT ring */
    uint8_t impulse[FRAME_SIZE * 4];  /* 4 frames of impulse for robustness */
    for (int f = 0; f < 4; f++) {
        for (int ch = 0; ch < CHANNELS; ch++) {
            write_int24(impulse + f * FRAME_SIZE + ch * BYTES_PER_SAMPLE, 0x7FFFFF);
        }
    }

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);

    uint64_t send_time = mach_absolute_time();
    shm_ring_write(shm->out_ring, DJMT1_SHM_RING_BYTES,
                   shm->out_write_pos, impulse, sizeof(impulse));

    /* Monitor IN ring for the impulse */
    int32_t threshold = 0x010000;  /* ~0.8% of full scale — low threshold */
    int timeout_us = 3000000;  /* 3 seconds */
    int elapsed_us = 0;
    bool detected = false;
    uint64_t detect_time = 0;
    int detect_samples = 0;
    int32_t max_sample = 0;  /* track max for debugging */

    uint8_t buf[FRAME_SIZE * 48];  /* read 1ms worth at a time */

    while (elapsed_us < timeout_us) {
        uint64_t avail = shm_ring_available(shm->in_write_pos, shm->in_read_pos);
        if (avail >= FRAME_SIZE) {
            uint32_t to_read = (avail < sizeof(buf)) ? (uint32_t)avail : sizeof(buf);
            /* align to frame boundary */
            to_read = (to_read / FRAME_SIZE) * FRAME_SIZE;
            if (to_read == 0) { usleep(100); elapsed_us += 100; continue; }

            uint32_t got = shm_ring_read(shm->in_ring, DJMT1_SHM_RING_BYTES,
                                         shm->in_read_pos, shm->in_write_pos,
                                         buf, to_read);
            int frames_read = (int)(got / FRAME_SIZE);
            for (int f = 0; f < frames_read; f++) {
                /* Check all 6 channels */
                for (int ch = 0; ch < CHANNELS; ch++) {
                    int32_t sample = read_int24(buf + f * FRAME_SIZE + ch * BYTES_PER_SAMPLE);
                    int32_t mag = abs(sample);
                    if (mag > max_sample) max_sample = mag;
                    if (mag > threshold) {
                        detect_time = mach_absolute_time();
                        detected = true;
                        printf("  Detected on ch%d, value=0x%06X\n", ch + 1, mag);
                        break;
                    }
                }
                detect_samples++;
                if (detected) break;
            }
            if (detected) break;
        } else {
            usleep(100);  /* 0.1ms poll interval */
            elapsed_us += 100;
        }
    }

    if (detected) {
        double rtt_ms = (double)(detect_time - send_time) * tb.numer / tb.denom / 1e6;
        double rtt_frames = rtt_ms * sampleRate / 1000.0;

        printf("Impulse detected!\n");
        printf("Wall-clock round-trip:    %.2f ms\n", rtt_ms);
        printf("Equivalent frames:       %.0f (at %.0f Hz)\n", rtt_frames, sampleRate);
        printf("Samples scanned:         %d\n", detect_samples);
        printf("\nOne-way estimate:        %.2f ms  (round-trip / 2)\n", rtt_ms / 2.0);
        printf("\nBreakdown (estimated):\n");
        printf("  USB OUT transfer:      ~%.1f ms  (8 packets × 1ms)\n", 8.0);
        printf("  DJM-T1 internal:       ~%.1f ms  (analog path)\n", 0.5);
        printf("  USB IN transfer:       ~%.1f ms  (8 packets × 1ms)\n", 8.0);
        printf("  Ring buffer overhead:  ~%.2f ms  (remainder)\n",
               rtt_ms - 16.5 > 0 ? rtt_ms - 16.5 : 0.0);
    } else {
        printf("TIMEOUT: No impulse detected after %d ms.\n", timeout_us / 1000);
        printf("Checked %d frames (all %d channels).\n", detect_samples, CHANNELS);
        printf("Max sample magnitude:    0x%06X (%d)\n", max_sample, max_sample);
        printf("\nTroubleshooting:\n");
        printf("  1. Cable: DJM-T1 MASTER OUT (RCA) → CH1 LINE IN (RCA)\n");
        printf("  2. MASTER LEVEL: turn up to a reasonable level\n");
        printf("  3. Input selector: set CH1 to LINE\n");
        printf("  4. Verify bridge is streaming: check 'Flags' above\n");
    }

    munmap(ptr, DJMT1_SHM_SIZE);
}

/* ── Main ── */

int main(int argc, char *argv[])
{
    bool do_loopback = false;
    int bufSize = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--loopback") == 0) {
            do_loopback = true;
        } else if (strcmp(argv[i], "--bufsize") == 0 && i + 1 < argc) {
            bufSize = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: djmt1-latency [--loopback] [--bufsize FRAMES]\n");
            printf("  --loopback     Run round-trip impulse test\n");
            printf("  --bufsize N    Set CoreAudio buffer size (32-4096)\n");
            return 0;
        }
    }

    printf("DJM-T1 Latency Measurement Tool\n");
    printf("================================\n\n");

    AudioDeviceID dev = find_device();
    if (dev == kAudioObjectUnknown) {
        fprintf(stderr, "DJM-T1 audio device not found.\n");
        fprintf(stderr, "Make sure the plugin is installed and coreaudiod is running.\n");
        return 1;
    }
    printf("Device found (ID: %u)\n\n", dev);

    if (bufSize > 0) {
        if (set_buffer_size(dev, bufSize)) {
            printf("Buffer size set to %d frames\n\n", bufSize);
        } else {
            fprintf(stderr, "Warning: could not set buffer size to %d\n\n", bufSize);
        }
    }

    print_latency_report(dev);
    print_shm_stats();

    if (do_loopback) {
        run_loopback_test(dev, bufSize);
    }

    return 0;
}
