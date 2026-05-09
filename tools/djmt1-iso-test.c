/*
 * djmt1-iso-test.c
 *
 * Standalone isochronous transfer test for Pioneer DJM-T1.
 * Validates that libusb can successfully perform async isoch IN+OUT
 * transfers with the DJM-T1 hardware.
 *
 * Usage:
 *   cc -o djmt1-iso-test djmt1-iso-test.c $(pkg-config --cflags --libs libusb-1.0)
 *   ./djmt1-iso-test [capture_seconds]
 *
 * Output:
 *   capture.raw — raw interleaved 6ch/24bit/48kHz PCM (little-endian)
 *
 * Verify with:
 *   sox -t raw -r 48000 -c 6 -b 24 -e signed-integer -L capture.raw output.wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <libusb.h>

/* ── DJM-T1 USB identifiers ── */
#define DJMT1_VID              0x08E4   /* Pioneer */
#define DJMT1_PID              0x015E   /* DJM-T1 */

/* ── Audio format (confirmed by Phase 0 testing) ── */
#define DJMT1_CHANNELS         6
#define DJMT1_BYTES_PER_SAMPLE 3        /* 24-bit packed */
#define DJMT1_SAMPLE_RATE      48000
#define DJMT1_FRAME_SIZE       (DJMT1_CHANNELS * DJMT1_BYTES_PER_SAMPLE) /* 18 bytes */

/* ── USB isochronous parameters ── */
#define DJMT1_EP_IN            0x82
#define DJMT1_EP_OUT           0x01
#define DJMT1_INTERFACE        0
#define DJMT1_ALT_SETTING      1

#define DJMT1_ISOC_PACKET_SIZE     864  /* nominal bytes/ms (48 frames x 18) */
#define DJMT1_ISOC_PACKET_SIZE_MAX 882  /* max bytes/packet (49 frames x 18) */
#define DJMT1_ISOC_MAX_PACKET      1024 /* USB descriptor maxPacketSize */
#define DJMT1_FRAMES_PER_XFER      8    /* isochronous packets per transfer */
#define DJMT1_NUM_XFERS            8    /* in-flight transfers (double+ buffering) */

/* ── Capture settings ── */
#define DEFAULT_CAPTURE_SECONDS  5
#define OUTPUT_FILENAME          "capture.raw"

/* ── Vendor control request (boost level) ── */
#define DJMT1_SET_REQUEST_TYPE   0x40
#define DJMT1_SET_REQUEST        0x03
#define DJMT1_BOOST_INDEX        0x8003
#define DJMT1_BOOST_18DB         0x0000

/* ── Global state ── */
static volatile int g_running = 1;
static FILE *g_capture_file = NULL;
static uint64_t g_bytes_captured = 0;
static uint64_t g_in_completions = 0;
static uint64_t g_out_completions = 0;
static uint64_t g_in_errors = 0;
static uint64_t g_out_errors = 0;
static uint64_t g_zero_length = 0;
static struct timespec g_start_time;
static int g_capture_seconds = DEFAULT_CAPTURE_SECONDS;

/* Transfer context */
struct xfer_ctx {
    int index;
    int is_input;  /* 1 = IN, 0 = OUT */
};

static struct libusb_transfer *g_in_xfers[DJMT1_NUM_XFERS];
static struct libusb_transfer *g_out_xfers[DJMT1_NUM_XFERS];
static uint8_t *g_in_bufs[DJMT1_NUM_XFERS];
static uint8_t *g_out_bufs[DJMT1_NUM_XFERS];
static struct xfer_ctx g_in_ctx[DJMT1_NUM_XFERS];
static struct xfer_ctx g_out_ctx[DJMT1_NUM_XFERS];

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static double elapsed_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_start_time.tv_sec) +
           (now.tv_nsec - g_start_time.tv_nsec) / 1e9;
}

/* ── IN completion callback ── */
static void LIBUSB_CALL in_callback(struct libusb_transfer *xfer)
{
    struct xfer_ctx *ctx = (struct xfer_ctx *)xfer->user_data;

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED ||
        xfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
        g_in_completions++;

        /* Process each isochronous packet */
        for (int i = 0; i < xfer->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *pkt = &xfer->iso_packet_desc[i];

            if (pkt->status == LIBUSB_TRANSFER_COMPLETED && pkt->actual_length > 0) {
                uint8_t *data = libusb_get_iso_packet_buffer_simple(xfer, i);
                if (data && g_capture_file) {
                    fwrite(data, 1, pkt->actual_length, g_capture_file);
                    g_bytes_captured += pkt->actual_length;
                }
            } else if (pkt->actual_length == 0) {
                g_zero_length++;
            }
        }
    } else if (xfer->status == LIBUSB_TRANSFER_CANCELLED) {
        return; /* shutting down, don't resubmit */
    } else {
        g_in_errors++;
        if (g_in_errors <= 10) {
            fprintf(stderr, "IN xfer[%d] error: %s\n",
                    ctx->index, libusb_error_name(xfer->status));
        }
    }

    /* Check time limit */
    if (!g_running || elapsed_seconds() >= g_capture_seconds) {
        g_running = 0;
        return;
    }

    /* Resubmit */
    int ret = libusb_submit_transfer(xfer);
    if (ret != 0) {
        fprintf(stderr, "IN resubmit[%d] failed: %s\n",
                ctx->index, libusb_error_name(ret));
        g_running = 0;
    }
}

/* ── OUT completion callback ── */
static void LIBUSB_CALL out_callback(struct libusb_transfer *xfer)
{
    struct xfer_ctx *ctx = (struct xfer_ctx *)xfer->user_data;

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED ||
        xfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
        g_out_completions++;
    } else if (xfer->status == LIBUSB_TRANSFER_CANCELLED) {
        return;
    } else {
        g_out_errors++;
        if (g_out_errors <= 10) {
            fprintf(stderr, "OUT xfer[%d] error: %s\n",
                    ctx->index, libusb_error_name(xfer->status));
        }
    }

    if (!g_running || elapsed_seconds() >= g_capture_seconds) {
        g_running = 0;
        return;
    }

    /* Fill with silence (all zeros) and resubmit */
    int buf_size = DJMT1_FRAMES_PER_XFER * DJMT1_ISOC_PACKET_SIZE;
    memset(xfer->buffer, 0, buf_size);

    int ret = libusb_submit_transfer(xfer);
    if (ret != 0) {
        fprintf(stderr, "OUT resubmit[%d] failed: %s\n",
                ctx->index, libusb_error_name(ret));
        g_running = 0;
    }
}

/* ── Allocate and configure an isochronous transfer ── */
static struct libusb_transfer *create_isoc_transfer(
    libusb_device_handle *dev,
    uint8_t endpoint,
    uint8_t *buffer,
    int packet_size,
    int num_packets,
    libusb_transfer_cb_fn callback,
    void *user_data)
{
    struct libusb_transfer *xfer = libusb_alloc_transfer(num_packets);
    if (!xfer) return NULL;

    int total_size = num_packets * packet_size;

    libusb_fill_iso_transfer(
        xfer,
        dev,
        endpoint,
        buffer,
        total_size,
        num_packets,
        callback,
        user_data,
        0  /* no timeout for isoch */
    );

    libusb_set_iso_packet_lengths(xfer, packet_size);

    return xfer;
}

int main(int argc, char *argv[])
{
    libusb_context *ctx = NULL;
    libusb_device_handle *dev = NULL;
    int ret;
    int kernel_detached = 0;

    /* Parse arguments */
    if (argc > 1) {
        g_capture_seconds = atoi(argv[1]);
        if (g_capture_seconds <= 0) g_capture_seconds = DEFAULT_CAPTURE_SECONDS;
    }

    printf("DJM-T1 Isochronous Transfer Test\n");
    printf("================================\n");
    printf("Capture duration: %d seconds\n", g_capture_seconds);
    printf("Audio format: %dch x %dbit x %dHz\n",
           DJMT1_CHANNELS, DJMT1_BYTES_PER_SAMPLE * 8, DJMT1_SAMPLE_RATE);
    printf("Nominal packet size: %d bytes/ms\n", DJMT1_ISOC_PACKET_SIZE);
    printf("Transfers: %d in-flight x %d packets each\n\n",
           DJMT1_NUM_XFERS, DJMT1_FRAMES_PER_XFER);

    /* Signal handler for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize libusb */
    ret = libusb_init(&ctx);
    if (ret != 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(ret));
        return 1;
    }

#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#else
    libusb_set_debug(ctx, LIBUSB_LOG_LEVEL_WARNING);
#endif

    /* Open DJM-T1 */
    dev = libusb_open_device_with_vid_pid(ctx, DJMT1_VID, DJMT1_PID);
    if (!dev) {
        fprintf(stderr, "DJM-T1 not found (VID=0x%04X PID=0x%04X)\n",
                DJMT1_VID, DJMT1_PID);
        fprintf(stderr, "Make sure the device is connected and you have permission.\n");
        libusb_exit(ctx);
        return 1;
    }
    printf("DJM-T1 found and opened.\n");

    /* Detach kernel driver if attached */
    if (libusb_kernel_driver_active(dev, DJMT1_INTERFACE) == 1) {
        ret = libusb_detach_kernel_driver(dev, DJMT1_INTERFACE);
        if (ret != 0) {
            fprintf(stderr, "Failed to detach kernel driver: %s\n",
                    libusb_error_name(ret));
            fprintf(stderr, "Try running with sudo.\n");
            goto cleanup;
        }
        kernel_detached = 1;
        printf("Kernel driver detached.\n");
    }

    /* Claim interface */
    ret = libusb_claim_interface(dev, DJMT1_INTERFACE);
    if (ret != 0) {
        fprintf(stderr, "Failed to claim interface %d: %s\n",
                DJMT1_INTERFACE, libusb_error_name(ret));
        goto cleanup;
    }
    printf("Interface %d claimed.\n", DJMT1_INTERFACE);

    /* Set alternate setting 1 (streaming mode) */
    ret = libusb_set_interface_alt_setting(dev, DJMT1_INTERFACE, DJMT1_ALT_SETTING);
    if (ret != 0) {
        fprintf(stderr, "Failed to set alt setting %d: %s\n",
                DJMT1_ALT_SETTING, libusb_error_name(ret));
        goto release;
    }
    printf("Alt setting %d (streaming) active.\n", DJMT1_ALT_SETTING);

    /* Set boost level to +18dB */
    ret = libusb_control_transfer(dev,
        DJMT1_SET_REQUEST_TYPE, DJMT1_SET_REQUEST,
        DJMT1_BOOST_18DB, DJMT1_BOOST_INDEX,
        NULL, 0, 1000);
    if (ret < 0) {
        fprintf(stderr, "Warning: boost set failed: %s (continuing)\n",
                libusb_error_name(ret));
    } else {
        printf("Boost level set to +18dB.\n");
    }

    /* Open capture file */
    g_capture_file = fopen(OUTPUT_FILENAME, "wb");
    if (!g_capture_file) {
        perror("Failed to open capture file");
        goto release;
    }
    printf("Capture file: %s\n\n", OUTPUT_FILENAME);

    /* ── Allocate transfers ── */
    int in_pkt_size = DJMT1_ISOC_MAX_PACKET;  /* request max for IN */
    int out_pkt_size = DJMT1_ISOC_PACKET_SIZE; /* nominal for OUT */

    for (int i = 0; i < DJMT1_NUM_XFERS; i++) {
        /* IN buffers and transfers */
        g_in_bufs[i] = calloc(DJMT1_FRAMES_PER_XFER, in_pkt_size);
        if (!g_in_bufs[i]) {
            fprintf(stderr, "Failed to allocate IN buffer %d\n", i);
            goto cleanup_xfers;
        }

        g_in_ctx[i].index = i;
        g_in_ctx[i].is_input = 1;

        g_in_xfers[i] = create_isoc_transfer(
            dev, DJMT1_EP_IN, g_in_bufs[i],
            in_pkt_size, DJMT1_FRAMES_PER_XFER,
            in_callback, &g_in_ctx[i]);
        if (!g_in_xfers[i]) {
            fprintf(stderr, "Failed to allocate IN transfer %d\n", i);
            goto cleanup_xfers;
        }

        /* OUT buffers and transfers (filled with silence) */
        g_out_bufs[i] = calloc(DJMT1_FRAMES_PER_XFER, out_pkt_size);
        if (!g_out_bufs[i]) {
            fprintf(stderr, "Failed to allocate OUT buffer %d\n", i);
            goto cleanup_xfers;
        }

        g_out_ctx[i].index = i;
        g_out_ctx[i].is_input = 0;

        g_out_xfers[i] = create_isoc_transfer(
            dev, DJMT1_EP_OUT, g_out_bufs[i],
            out_pkt_size, DJMT1_FRAMES_PER_XFER,
            out_callback, &g_out_ctx[i]);
        if (!g_out_xfers[i]) {
            fprintf(stderr, "Failed to allocate OUT transfer %d\n", i);
            goto cleanup_xfers;
        }
    }

    /* ── Submit all transfers ── */
    /*
     * CRITICAL: DJM-T1 requires both IN and OUT endpoints active
     * simultaneously for audio to flow in either direction.
     */
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);

    printf("Submitting %d IN + %d OUT transfers...\n",
           DJMT1_NUM_XFERS, DJMT1_NUM_XFERS);

    for (int i = 0; i < DJMT1_NUM_XFERS; i++) {
        ret = libusb_submit_transfer(g_in_xfers[i]);
        if (ret != 0) {
            fprintf(stderr, "IN submit[%d] failed: %s\n",
                    i, libusb_error_name(ret));
            g_running = 0;
            goto cleanup_xfers;
        }

        ret = libusb_submit_transfer(g_out_xfers[i]);
        if (ret != 0) {
            fprintf(stderr, "OUT submit[%d] failed: %s\n",
                    i, libusb_error_name(ret));
            g_running = 0;
            goto cleanup_xfers;
        }
    }

    printf("Capturing audio for %d seconds... (Ctrl+C to stop early)\n\n",
           g_capture_seconds);

    /* ── Event loop ── */
    struct timeval tv;
    double last_report = 0;

    while (g_running && elapsed_seconds() < g_capture_seconds) {
        tv.tv_sec = 0;
        tv.tv_usec = 100000; /* 100ms timeout */
        ret = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        if (ret != 0 && ret != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "Event handling error: %s\n", libusb_error_name(ret));
            break;
        }

        /* Periodic progress report every second */
        double now = elapsed_seconds();
        if (now - last_report >= 1.0) {
            printf("  [%.0fs] captured: %.1f KB, IN: %llu completions, "
                   "OUT: %llu completions, errors: IN=%llu OUT=%llu, "
                   "zero-len: %llu\n",
                   now,
                   g_bytes_captured / 1024.0,
                   (unsigned long long)g_in_completions,
                   (unsigned long long)g_out_completions,
                   (unsigned long long)g_in_errors,
                   (unsigned long long)g_out_errors,
                   (unsigned long long)g_zero_length);
            last_report = now;
        }
    }

    g_running = 0;
    printf("\nStopping capture...\n");

    /* Cancel all in-flight transfers */
    for (int i = 0; i < DJMT1_NUM_XFERS; i++) {
        if (g_in_xfers[i])  libusb_cancel_transfer(g_in_xfers[i]);
        if (g_out_xfers[i]) libusb_cancel_transfer(g_out_xfers[i]);
    }

    /* Drain remaining events (wait for cancellations to complete) */
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    while (1) {
        ret = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        if (ret != 0) break;
        /* Short timeout to check if all transfers are done */
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        ret = libusb_handle_events_timeout_completed(ctx, &tv, NULL);
        break;
    }

    /* ── Final report ── */
    double total_time = elapsed_seconds();
    uint64_t expected = (uint64_t)(DJMT1_ISOC_PACKET_SIZE * total_time * 1000);

    printf("\n=== Capture Results ===\n");
    printf("Duration:         %.2f seconds\n", total_time);
    printf("Bytes captured:   %llu (%.2f MB)\n",
           (unsigned long long)g_bytes_captured,
           g_bytes_captured / (1024.0 * 1024.0));
    printf("Expected bytes:   %llu (%.2f MB)\n",
           (unsigned long long)expected,
           expected / (1024.0 * 1024.0));
    printf("Capture ratio:    %.1f%%\n",
           expected > 0 ? (g_bytes_captured * 100.0 / expected) : 0.0);
    printf("IN completions:   %llu\n", (unsigned long long)g_in_completions);
    printf("OUT completions:  %llu\n", (unsigned long long)g_out_completions);
    printf("IN errors:        %llu\n", (unsigned long long)g_in_errors);
    printf("OUT errors:       %llu\n", (unsigned long long)g_out_errors);
    printf("Zero-length pkts: %llu\n", (unsigned long long)g_zero_length);
    printf("Output file:      %s\n", OUTPUT_FILENAME);

    if (g_bytes_captured > 0) {
        uint64_t sample_frames = g_bytes_captured / DJMT1_FRAME_SIZE;
        double audio_seconds = (double)sample_frames / DJMT1_SAMPLE_RATE;
        printf("Audio duration:   %.2f seconds (%llu sample frames)\n",
               audio_seconds, (unsigned long long)sample_frames);
        printf("\nTo convert to WAV:\n");
        printf("  sox -t raw -r %d -c %d -b %d -e signed-integer -L %s output.wav\n",
               DJMT1_SAMPLE_RATE, DJMT1_CHANNELS,
               DJMT1_BYTES_PER_SAMPLE * 8, OUTPUT_FILENAME);
    } else {
        printf("\nWARNING: No audio data captured!\n");
        printf("Check that the DJM-T1 is powered on and an input source is active.\n");
    }

cleanup_xfers:
    for (int i = 0; i < DJMT1_NUM_XFERS; i++) {
        if (g_in_xfers[i])  libusb_free_transfer(g_in_xfers[i]);
        if (g_out_xfers[i]) libusb_free_transfer(g_out_xfers[i]);
        free(g_in_bufs[i]);
        free(g_out_bufs[i]);
    }

    if (g_capture_file) fclose(g_capture_file);

release:
    /* Return to alt setting 0 before releasing */
    libusb_set_interface_alt_setting(dev, DJMT1_INTERFACE, 0);
    libusb_release_interface(dev, DJMT1_INTERFACE);

    if (kernel_detached) {
        libusb_attach_kernel_driver(dev, DJMT1_INTERFACE);
    }

cleanup:
    if (dev) libusb_close(dev);
    libusb_exit(ctx);

    return (g_bytes_captured > 0) ? 0 : 1;
}
