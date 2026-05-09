/*
 * plugin_entry.cpp
 *
 * AudioServerPlugin entry point for DJM-T1.
 * Creates a libASPL Driver with a single device (6ch x 24bit x 48kHz, in+out).
 *
 * The factory function is registered in Info.plist and called by coreaudiod
 * when the plugin bundle is loaded from /Library/Audio/Plug-Ins/HAL/.
 */

#include <aspl/Context.hpp>
#include <aspl/Driver.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Device.hpp>
#include <aspl/Stream.hpp>
#include <aspl/ControlRequestHandler.hpp>

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

#include <memory>
#include <os/log.h>

#include "djmt1_io_handler.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "DJMT1Plugin: " fmt, ##__VA_ARGS__)

/* ── Audio format: 6ch x 24bit x 48kHz interleaved ── */
static const AudioStreamBasicDescription kDJMT1Format = {
    .mSampleRate       = 48000,
    .mFormatID         = kAudioFormatLinearPCM,
    .mFormatFlags      = kAudioFormatFlagIsSignedInteger |
                         kAudioFormatFlagsNativeEndian |
                         kAudioFormatFlagIsPacked,
    .mBitsPerChannel   = 24,
    .mChannelsPerFrame = 6,
    .mBytesPerFrame    = 18,   /* 6 * 3 */
    .mFramesPerPacket  = 1,
    .mBytesPerPacket   = 18,
};

/*
 * Control request handler — connects/disconnects shared memory on IO start/stop.
 * OnStartIO/OnStopIO are called on non-realtime thread, so shm_open is safe.
 */
class DJMT1ControlHandler : public aspl::ControlRequestHandler {
public:
    explicit DJMT1ControlHandler(std::shared_ptr<DJMT1IOHandler> ioHandler)
        : m_ioHandler(std::move(ioHandler))
    {
    }

    OSStatus OnStartIO() override
    {
        LOG("OnStartIO: connecting to bridge");
        if (!m_ioHandler->IsConnected()) {
            m_ioHandler->ConnectToShm();
        }
        if (!m_ioHandler->IsConnected()) {
            LOG("OnStartIO: bridge not available, will output silence");
        }
        return kAudioHardwareNoError;
    }

    void OnStopIO() override
    {
        LOG("OnStopIO");
        /* Keep shm connected — bridge may still be running for next start */
    }

private:
    std::shared_ptr<DJMT1IOHandler> m_ioHandler;
};

/*
 * Plugin factory function.
 * Called by coreaudiod when loading the plugin bundle.
 * Must return an AudioServerPlugInDriverRef.
 */
extern "C" void* DJMT1AudioPlugin_Create(
    CFAllocatorRef /*allocator*/,
    CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    LOG("factory: creating DJM-T1 audio plugin");

    /* Create shared context */
    auto context = std::make_shared<aspl::Context>();

    /* Create plugin */
    aspl::PluginParameters pluginParams;
    pluginParams.Manufacturer = "Pioneer DJ";
    auto plugin = std::make_shared<aspl::Plugin>(context, pluginParams);

    /* Create device */
    aspl::DeviceParameters devParams;
    devParams.Name         = "PIONEER DJM-T1";
    devParams.Manufacturer = "Pioneer DJ";
    devParams.DeviceUID    = "DJMT1-AudioPlugin-UID";
    devParams.ModelUID     = "DJM-T1";
    devParams.SampleRate   = 48000;
    devParams.ChannelCount = 6;
    devParams.Latency      = 32;    /* 0.67ms — DJ low-latency */
    devParams.SafetyOffset = 0;     /* no safety margin for DJ monitoring */
    devParams.EnableMixing = true;
    devParams.ClockIsStable = true;

    auto device = std::make_shared<aspl::Device>(context, devParams);

    /* Create input stream (device -> app, recording) */
    aspl::StreamParameters inputParams;
    inputParams.Direction       = aspl::Direction::Input;
    inputParams.StartingChannel = 1;
    inputParams.Format          = kDJMT1Format;
    device->AddStreamAsync(inputParams);

    /* Create output stream (app -> device, playback) */
    aspl::StreamParameters outputParams;
    outputParams.Direction       = aspl::Direction::Output;
    outputParams.StartingChannel = 1;
    outputParams.Format          = kDJMT1Format;
    device->AddStreamAsync(outputParams);

    /* Create IO handler (shared memory bridge) */
    auto ioHandler = std::make_shared<DJMT1IOHandler>();

    /* Try initial connection to bridge */
    ioHandler->ConnectToShm();

    /* Create control handler (manages shm lifecycle) */
    auto controlHandler = std::make_shared<DJMT1ControlHandler>(ioHandler);

    device->SetIOHandler(ioHandler);
    device->SetControlHandler(controlHandler);

    /* Add device to plugin */
    plugin->AddDevice(device);

    /* Create driver (the top-level COM object returned to HAL) */
    static auto driver = std::make_shared<aspl::Driver>(context, plugin);

    LOG("factory: DJM-T1 audio plugin created successfully");
    return driver->GetReference();
}
