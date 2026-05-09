/*
 * djmt1_io_handler.h
 *
 * IORequestHandler implementation for DJM-T1 AudioServerPlugin.
 * Reads input audio from shared memory IN ring (bridge -> plugin -> CoreAudio)
 * and writes output audio to shared memory OUT ring (CoreAudio -> plugin -> bridge).
 */

#ifndef DJMT1_IO_HANDLER_H
#define DJMT1_IO_HANDLER_H

#include <aspl/IORequestHandler.hpp>
#include "shm_protocol.h"

class DJMT1IOHandler : public aspl::IORequestHandler {
public:
    DJMT1IOHandler();
    ~DJMT1IOHandler() override;

    /* Connect to bridge's shared memory. Returns true on success. */
    bool ConnectToShm();

    /* Disconnect from shared memory. */
    void DisconnectFromShm();

    /* Check if connected to bridge */
    bool IsConnected() const { return m_shm != nullptr; }

    /* ── IORequestHandler overrides (called on realtime thread) ── */

    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
        const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        void* bytes,
        UInt32 bytesCount) override;

    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        const void* bytes,
        UInt32 bytesCount) override;

private:
    struct djmt1_shm *m_shm;
    int m_shm_fd;
};

#endif /* DJMT1_IO_HANDLER_H */
