/*
 * djmt1_io_handler.cpp
 *
 * Shared memory I/O handler for DJM-T1 AudioServerPlugin.
 *
 * All IO methods run on CoreAudio's realtime thread and must be lock-free.
 * The shared memory ring buffers use atomic positions for SPSC synchronization.
 *
 * IN path:  shm IN ring (written by bridge) -> OnReadClientInput -> CoreAudio
 * OUT path: CoreAudio -> OnWriteMixedOutput -> shm OUT ring (read by bridge)
 */

#include "djmt1_io_handler.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <os/log.h>

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "DJMT1Plugin: " fmt, ##__VA_ARGS__)

DJMT1IOHandler::DJMT1IOHandler()
    : m_shm(nullptr)
    , m_shm_fd(-1)
{
}

DJMT1IOHandler::~DJMT1IOHandler()
{
    DisconnectFromShm();
}

bool DJMT1IOHandler::ConnectToShm()
{
    if (m_shm) return true;

    m_shm_fd = shm_open(DJMT1_SHM_NAME, O_RDWR, 0666);
    if (m_shm_fd < 0) {
        LOG("ConnectToShm: shm_open failed (bridge not running?)");
        return false;
    }

    void *ptr = mmap(nullptr, DJMT1_SHM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, m_shm_fd, 0);

    if (ptr == MAP_FAILED) {
        LOG("ConnectToShm: mmap failed");
        close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }

    struct djmt1_shm *shm = (struct djmt1_shm *)ptr;

    if (!shm_validate(shm)) {
        LOG("ConnectToShm: invalid shared memory (magic/version mismatch)");
        munmap(ptr, DJMT1_SHM_SIZE);
        close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }

    m_shm = shm;
    LOG("ConnectToShm: connected to bridge shared memory");
    return true;
}

void DJMT1IOHandler::DisconnectFromShm()
{
    if (m_shm) {
        munmap(m_shm, DJMT1_SHM_SIZE);
        m_shm = nullptr;
    }
    if (m_shm_fd >= 0) {
        close(m_shm_fd);
        m_shm_fd = -1;
    }
}

/*
 * OnReadClientInput — Provide input audio to CoreAudio.
 *
 * Reads from the shared memory IN ring buffer (produced by bridge from USB IN).
 * If not connected or insufficient data, fills with silence.
 *
 * Called on realtime thread — must be lock-free.
 */
void DJMT1IOHandler::OnReadClientInput(
    const std::shared_ptr<aspl::Client>& /*client*/,
    const std::shared_ptr<aspl::Stream>& /*stream*/,
    Float64 /*zeroTimestamp*/,
    Float64 /*timestamp*/,
    void* bytes,
    UInt32 bytesCount)
{
    if (!m_shm) {
        memset(bytes, 0, bytesCount);
        return;
    }

    uint32_t got = shm_ring_read(
        m_shm->in_ring, DJMT1_SHM_RING_BYTES,
        m_shm->in_read_pos, m_shm->in_write_pos,
        reinterpret_cast<uint8_t*>(bytes), bytesCount);

    /* Pad with silence on underrun */
    if (got < bytesCount) {
        memset(reinterpret_cast<uint8_t*>(bytes) + got, 0, bytesCount - got);
    }
}

/*
 * OnWriteMixedOutput — Receive mixed output from CoreAudio.
 *
 * Writes to the shared memory OUT ring buffer (consumed by bridge for USB OUT).
 * If not connected, silently discards.
 *
 * Called on realtime thread — must be lock-free.
 */
void DJMT1IOHandler::OnWriteMixedOutput(
    const std::shared_ptr<aspl::Stream>& /*stream*/,
    Float64 /*zeroTimestamp*/,
    Float64 /*timestamp*/,
    const void* bytes,
    UInt32 bytesCount)
{
    if (!m_shm) return;

    shm_ring_write(
        m_shm->out_ring, DJMT1_SHM_RING_BYTES,
        m_shm->out_write_pos,
        reinterpret_cast<const uint8_t*>(bytes), bytesCount);
}
