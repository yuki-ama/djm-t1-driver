/*
 * iso_engine.h
 *
 * Isochronous transfer engine for DJM-T1 bridge.
 * Manages async IN/OUT isoch transfers via libusb, exchanging audio data
 * with the AudioServerPlugin through the shared memory ring buffers.
 */

#ifndef DJMT1_ISO_ENGINE_H
#define DJMT1_ISO_ENGINE_H

#include <cstdint>
#include <atomic>
#include <libusb.h>
#include "shm_protocol.h"

class IsoEngine {
public:
    IsoEngine(libusb_device_handle *dev, struct djmt1_shm *shm);
    ~IsoEngine();

    /* Start isochronous streaming (submit all transfers) */
    bool Start();

    /* Stop streaming (cancel all transfers, must drain events after) */
    void Stop();

    bool IsRunning() const { return m_running; }

    /* Statistics */
    uint64_t InCompletions() const { return m_in_completions; }
    uint64_t OutCompletions() const { return m_out_completions; }
    uint64_t InErrors() const { return m_in_errors; }
    uint64_t OutErrors() const { return m_out_errors; }

private:
    /* libusb callbacks (C linkage) */
    static void LIBUSB_CALL InCallback(struct libusb_transfer *xfer);
    static void LIBUSB_CALL OutCallback(struct libusb_transfer *xfer);

    /* Internal handlers */
    void HandleInCompletion(int idx, struct libusb_transfer *xfer);
    void HandleOutCompletion(int idx, struct libusb_transfer *xfer);
    void SubmitIn(int idx);
    void SubmitOut(int idx);

    /* Transfer context passed via user_data */
    struct XferContext {
        IsoEngine *engine;
        int index;
    };

    libusb_device_handle *m_dev;
    struct djmt1_shm *m_shm;
    std::atomic<bool> m_running;

    /* Transfer arrays */
    struct libusb_transfer *m_in_xfers[DJMT1_NUM_XFERS];
    struct libusb_transfer *m_out_xfers[DJMT1_NUM_XFERS];
    uint8_t *m_in_bufs[DJMT1_NUM_XFERS];
    uint8_t *m_out_bufs[DJMT1_NUM_XFERS];
    XferContext m_in_ctx[DJMT1_NUM_XFERS];
    XferContext m_out_ctx[DJMT1_NUM_XFERS];

    /* Statistics */
    uint64_t m_in_completions;
    uint64_t m_out_completions;
    uint64_t m_in_errors;
    uint64_t m_out_errors;
};

#endif /* DJMT1_ISO_ENGINE_H */
