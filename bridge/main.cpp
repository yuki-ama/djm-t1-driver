/*
 * main.cpp — djmt1-bridge
 *
 * User-space bridge daemon for Pioneer DJM-T1.
 * Opens the USB device via libusb, runs isochronous IN/OUT transfers,
 * and exchanges audio data with the AudioServerPlugin via POSIX shared memory.
 *
 * Usage:
 *   djmt1-bridge [--boost LEVEL]
 *
 * Options:
 *   --boost LEVEL   Set USB output boost (0=+18dB, 1=+12dB, 2=+6dB, 3=0dB)
 *                   Default: 0 (+18dB)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <mach/mach_time.h>

#include "shm_protocol.h"
#include "usb_device.h"
#include "iso_engine.h"

#define LOG(fmt, ...) fprintf(stderr, "[bridge] " fmt "\n", ##__VA_ARGS__)

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── Create and map POSIX shared memory ── */
static struct djmt1_shm *create_shared_memory()
{
    /* Remove stale segment if it exists */
    shm_unlink(DJMT1_SHM_NAME);

    int fd = shm_open(DJMT1_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        return nullptr;
    }

    if (ftruncate(fd, DJMT1_SHM_SIZE) != 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(DJMT1_SHM_NAME);
        return nullptr;
    }

    void *ptr = mmap(nullptr, DJMT1_SHM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        perror("mmap");
        shm_unlink(DJMT1_SHM_NAME);
        return nullptr;
    }

    struct djmt1_shm *shm = (struct djmt1_shm *)ptr;
    shm_init(shm);

    LOG("shared memory created: %s (%zu bytes)", DJMT1_SHM_NAME, (size_t)DJMT1_SHM_SIZE);
    return shm;
}

static void destroy_shared_memory(struct djmt1_shm *shm)
{
    if (shm) {
        shm->flags.store(0, std::memory_order_release);
        munmap(shm, DJMT1_SHM_SIZE);
    }
    shm_unlink(DJMT1_SHM_NAME);
    LOG("shared memory destroyed");
}

/* ── Parse command-line arguments ── */
struct Options {
    int boost_level = 0;  /* +18dB default */
};

static Options parse_args(int argc, char *argv[])
{
    Options opts;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--boost") == 0 && i + 1 < argc) {
            opts.boost_level = atoi(argv[++i]);
            if (opts.boost_level < 0 || opts.boost_level > 3) {
                fprintf(stderr, "Invalid boost level %d, using 0 (+18dB)\n",
                        opts.boost_level);
                opts.boost_level = 0;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: djmt1-bridge [--boost LEVEL]\n");
            printf("  --boost LEVEL  USB output boost (0=+18dB, 1=+12dB, 2=+6dB, 3=0dB)\n");
            exit(0);
        }
    }

    return opts;
}

/* ── Main ── */
int main(int argc, char *argv[])
{
    Options opts = parse_args(argc, argv);

    static const char *boost_labels[] = {"+18dB", "+12dB", "+6dB", "0dB"};
    LOG("DJM-T1 Bridge starting");
    LOG("Audio: %dch x %dbit x %dHz", DJMT1_SHM_CHANNELS,
        DJMT1_SHM_BITS_PER_SAMPLE, DJMT1_SHM_SAMPLE_RATE);
    LOG("Ring buffer: %d frames (~%dms)",
        DJMT1_SHM_RING_FRAMES, DJMT1_SHM_RING_FRAMES * 1000 / DJMT1_SHM_SAMPLE_RATE);
    LOG("Boost: %s", boost_labels[opts.boost_level]);

    /* Install signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Step 1: Create shared memory */
    struct djmt1_shm *shm = create_shared_memory();
    if (!shm) {
        LOG("failed to create shared memory");
        return 1;
    }

    /* Step 2: Open USB device */
    USBDevice usb;
    if (!usb.Open()) {
        LOG("failed to open USB device");
        destroy_shared_memory(shm);
        return 1;
    }

    shm->flags.fetch_or(DJMT1_SHM_FLAG_RUNNING | DJMT1_SHM_FLAG_DEVICE_OK,
                        std::memory_order_release);

    /* Step 3: Set boost level */
    usb.SetBoostLevel(opts.boost_level);

    /* Step 4: Start isochronous engine */
    IsoEngine engine(usb.Handle(), shm);
    if (!engine.Start()) {
        LOG("failed to start isoch engine");
        destroy_shared_memory(shm);
        return 1;
    }

    shm->flags.fetch_or(DJMT1_SHM_FLAG_STREAMING, std::memory_order_release);
    LOG("streaming started");

    /* Step 5: Event loop */
    struct timeval tv;
    uint64_t last_log = 0;
    uint64_t loop_count = 0;
    (void)loop_count;
    mach_timebase_info_data_t tb_info;
    mach_timebase_info(&tb_info);

    while (g_running && engine.IsRunning()) {
        /* Update heartbeat */
        shm->heartbeat.store(mach_absolute_time(), std::memory_order_release);

        /* Handle libusb events (1ms timeout — low latency) */
        tv.tv_sec = 0;
        tv.tv_usec = 1000;
        int ret = libusb_handle_events_timeout_completed(usb.Context(), &tv, nullptr);
        if (ret != 0 && ret != LIBUSB_ERROR_TIMEOUT && ret != LIBUSB_ERROR_INTERRUPTED) {
            LOG("event handling error: %s", libusb_error_name(ret));
            break;
        }

        loop_count++;

        /* Periodic status log every 10 seconds */
        uint64_t now = mach_absolute_time();
        uint64_t ns_elapsed = (now - last_log) * tb_info.numer / tb_info.denom;
        if (last_log == 0 || ns_elapsed >= 10000000000ULL) {
            uint64_t samples = shm->sample_count.load(std::memory_order_relaxed);
            uint64_t in_avail = shm_ring_available(shm->in_write_pos, shm->in_read_pos);
            uint64_t out_avail = shm_ring_available(shm->out_write_pos, shm->out_read_pos);

            LOG("status: samples=%llu, IN=%llu/%llu, OUT=%llu/%llu, "
                "completions(IN=%llu OUT=%llu), errors(IN=%llu OUT=%llu)",
                (unsigned long long)samples,
                (unsigned long long)in_avail,
                (unsigned long long)DJMT1_SHM_RING_BYTES,
                (unsigned long long)out_avail,
                (unsigned long long)DJMT1_SHM_RING_BYTES,
                (unsigned long long)engine.InCompletions(),
                (unsigned long long)engine.OutCompletions(),
                (unsigned long long)engine.InErrors(),
                (unsigned long long)engine.OutErrors());
            last_log = now;
        }
    }

    /* Step 6: Shutdown */
    LOG("shutting down...");

    shm->flags.fetch_and(~(uint32_t)DJMT1_SHM_FLAG_STREAMING, std::memory_order_release);
    engine.Stop();

    /* Drain remaining libusb events */
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    libusb_handle_events_timeout_completed(usb.Context(), &tv, nullptr);
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    libusb_handle_events_timeout_completed(usb.Context(), &tv, nullptr);

    LOG("final: IN completions=%llu, OUT completions=%llu, IN errors=%llu, OUT errors=%llu",
        (unsigned long long)engine.InCompletions(),
        (unsigned long long)engine.OutCompletions(),
        (unsigned long long)engine.InErrors(),
        (unsigned long long)engine.OutErrors());

    usb.Close();
    destroy_shared_memory(shm);

    LOG("done");
    return 0;
}
