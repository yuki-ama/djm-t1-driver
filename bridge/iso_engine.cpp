/*
 * iso_engine.cpp
 *
 * Isochronous transfer engine — manages async IN/OUT transfers and
 * routes audio data through shared memory ring buffers.
 *
 * Derived from the proven pattern in tools/djmt1-iso-test.c and
 * DJMT1AudioDriver.cpp (DriverKit version).
 *
 * IN path:  USB device -> isoch IN completion -> shm IN ring buffer
 * OUT path: shm OUT ring buffer -> isoch OUT buffer -> USB device
 */

#include "iso_engine.h"
#include "usb_device.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

#define LOG(fmt, ...) fprintf(stderr, "[iso] " fmt "\n", ##__VA_ARGS__)

IsoEngine::IsoEngine(libusb_device_handle *dev, struct djmt1_shm *shm)
    : m_dev(dev)
    , m_shm(shm)
    , m_running(false)
    , m_in_completions(0)
    , m_out_completions(0)
    , m_in_errors(0)
    , m_out_errors(0)
{
    memset(m_in_xfers, 0, sizeof(m_in_xfers));
    memset(m_out_xfers, 0, sizeof(m_out_xfers));
    memset(m_in_bufs, 0, sizeof(m_in_bufs));
    memset(m_out_bufs, 0, sizeof(m_out_bufs));
}

IsoEngine::~IsoEngine()
{
    Stop();

    for (int i = 0; i < DJMT1_NUM_XFERS; i++) {
        if (m_in_xfers[i])  libusb_free_transfer(m_in_xfers[i]);
        if (m_out_xfers[i]) libusb_free_transfer(m_out_xfers[i]);
        free(m_in_bufs[i]);
        free(m_out_bufs[i]);
    }
}

/* ── Start: allocate and submit all transfers ── */

bool IsoEngine::Start()
{
    if (m_running) return true;

    int in_pkt_size  = DJMT1_ISOC_MAX_PACKET;
    int out_pkt_size = DJMT1_ISOC_PACKET_SIZE;
    int num_pkts     = DJMT1_ISOC_FRAMES_PER_XFER;

    /* Allocate transfers and buffers */
    for (int i = 0; i < DJMT1_NUM_XFERS; i++) {
        /* IN */
        m_in_bufs[i] = (uint8_t *)calloc(num_pkts, in_pkt_size);
        if (!m_in_bufs[i]) {
            LOG("failed to allocate IN buffer %d", i);
            return false;
        }

        m_in_xfers[i] = libusb_alloc_transfer(num_pkts);
        if (!m_in_xfers[i]) {
            LOG("failed to allocate IN transfer %d", i);
            return false;
        }

        m_in_ctx[i].engine = this;
        m_in_ctx[i].index  = i;

        libusb_fill_iso_transfer(
            m_in_xfers[i], m_dev, DJMT1_EP_IN,
            m_in_bufs[i], num_pkts * in_pkt_size, num_pkts,
            InCallback, &m_in_ctx[i], 0);
        libusb_set_iso_packet_lengths(m_in_xfers[i], in_pkt_size);

        /* OUT */
        m_out_bufs[i] = (uint8_t *)calloc(num_pkts, out_pkt_size);
        if (!m_out_bufs[i]) {
            LOG("failed to allocate OUT buffer %d", i);
            return false;
        }

        m_out_xfers[i] = libusb_alloc_transfer(num_pkts);
        if (!m_out_xfers[i]) {
            LOG("failed to allocate OUT transfer %d", i);
            return false;
        }

        m_out_ctx[i].engine = this;
        m_out_ctx[i].index  = i;

        libusb_fill_iso_transfer(
            m_out_xfers[i], m_dev, DJMT1_EP_OUT,
            m_out_bufs[i], num_pkts * out_pkt_size, num_pkts,
            OutCallback, &m_out_ctx[i], 0);
        libusb_set_iso_packet_lengths(m_out_xfers[i], out_pkt_size);
    }

    m_running = true;

    /*
     * CRITICAL: DJM-T1 requires both IN and OUT endpoints active
     * simultaneously for audio to flow in either direction.
     */
    for (int i = 0; i < DJMT1_NUM_XFERS; i++) {
        SubmitIn(i);
        SubmitOut(i);
    }

    LOG("started: %d IN + %d OUT transfers submitted", DJMT1_NUM_XFERS, DJMT1_NUM_XFERS);
    return true;
}

/* ── Stop: cancel all in-flight transfers ── */

void IsoEngine::Stop()
{
    if (!m_running) return;
    m_running = false;

    for (int i = 0; i < DJMT1_NUM_XFERS; i++) {
        if (m_in_xfers[i])  libusb_cancel_transfer(m_in_xfers[i]);
        if (m_out_xfers[i]) libusb_cancel_transfer(m_out_xfers[i]);
    }

    LOG("stopped: transfers cancelled");
}

/* ── Submit helpers ── */

void IsoEngine::SubmitIn(int idx)
{
    if (!m_running) return;

    /* Reset packet descriptors for next transfer */
    for (int p = 0; p < DJMT1_ISOC_FRAMES_PER_XFER; p++) {
        m_in_xfers[idx]->iso_packet_desc[p].actual_length = 0;
        m_in_xfers[idx]->iso_packet_desc[p].status = LIBUSB_TRANSFER_COMPLETED;
    }

    int ret = libusb_submit_transfer(m_in_xfers[idx]);
    if (ret != 0) {
        /* Retry once after a short delay */
        usleep(500);
        ret = libusb_submit_transfer(m_in_xfers[idx]);
        if (ret != 0) {
            LOG("IN submit[%d] failed: %s", idx, libusb_error_name(ret));
            m_running = false;
        }
    }
}

void IsoEngine::SubmitOut(int idx)
{
    if (!m_running) return;

    /*
     * Fill OUT buffer with audio data from shared memory OUT ring.
     * If plugin hasn't written enough data, pad with silence.
     */
    int pkt_size = DJMT1_ISOC_PACKET_SIZE;
    uint8_t *buf = m_out_bufs[idx];

    for (int p = 0; p < DJMT1_ISOC_FRAMES_PER_XFER; p++) {
        uint8_t *pkt_buf = buf + p * pkt_size;
        uint32_t got = shm_ring_read(
            m_shm->out_ring, DJMT1_SHM_RING_BYTES,
            m_shm->out_read_pos, m_shm->out_write_pos,
            pkt_buf, pkt_size);

        /* Pad remainder with silence if underrun */
        if (got < (uint32_t)pkt_size) {
            memset(pkt_buf + got, 0, pkt_size - got);
        }
    }

    int ret = libusb_submit_transfer(m_out_xfers[idx]);
    if (ret != 0) {
        usleep(500);
        ret = libusb_submit_transfer(m_out_xfers[idx]);
        if (ret != 0) {
            LOG("OUT submit[%d] failed: %s", idx, libusb_error_name(ret));
            m_running = false;
        }
    }
}

/* ── IN completion callback ── */

void LIBUSB_CALL IsoEngine::InCallback(struct libusb_transfer *xfer)
{
    XferContext *ctx = (XferContext *)xfer->user_data;
    ctx->engine->HandleInCompletion(ctx->index, xfer);
}

void IsoEngine::HandleInCompletion(int idx, struct libusb_transfer *xfer)
{
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED ||
        xfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
        m_in_completions++;

        /* Process each isochronous packet */
        for (int p = 0; p < xfer->num_iso_packets; p++) {
            struct libusb_iso_packet_descriptor *pkt = &xfer->iso_packet_desc[p];

            if (pkt->status == LIBUSB_TRANSFER_COMPLETED && pkt->actual_length > 0) {
                uint8_t *data = libusb_get_iso_packet_buffer_simple(xfer, p);
                if (data) {
                    /* Write received audio to shared memory IN ring */
                    shm_ring_write(
                        m_shm->in_ring, DJMT1_SHM_RING_BYTES,
                        m_shm->in_write_pos,
                        data, pkt->actual_length);

                    /* Update sample count */
                    uint32_t frames = pkt->actual_length / DJMT1_SHM_FRAME_SIZE;
                    m_shm->sample_count.fetch_add(frames, std::memory_order_relaxed);
                }
            }
        }
    } else if (xfer->status == LIBUSB_TRANSFER_CANCELLED) {
        return; /* shutting down */
    } else if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        LOG("IN[%d]: device disconnected", idx);
        m_running = false;
        return;
    } else {
        m_in_errors++;
        if (m_in_errors <= 10) {
            LOG("IN[%d] error: %s", idx, libusb_error_name(xfer->status));
        }
    }

    /* Resubmit for continuous streaming */
    if (m_running) {
        SubmitIn(idx);
    }
}

/* ── OUT completion callback ── */

void LIBUSB_CALL IsoEngine::OutCallback(struct libusb_transfer *xfer)
{
    XferContext *ctx = (XferContext *)xfer->user_data;
    ctx->engine->HandleOutCompletion(ctx->index, xfer);
}

void IsoEngine::HandleOutCompletion(int idx, struct libusb_transfer *xfer)
{
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED ||
        xfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
        m_out_completions++;
    } else if (xfer->status == LIBUSB_TRANSFER_CANCELLED) {
        return;
    } else if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        LOG("OUT[%d]: device disconnected", idx);
        m_running = false;
        return;
    } else {
        m_out_errors++;
        if (m_out_errors <= 10) {
            LOG("OUT[%d] error: %s", idx, libusb_error_name(xfer->status));
        }
    }

    /* Resubmit with fresh data from shared memory */
    if (m_running) {
        SubmitOut(idx);
    }
}
