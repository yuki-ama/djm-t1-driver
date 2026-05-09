/*
 * usb_device.cpp
 *
 * libusb device management for DJM-T1.
 * C++ port of the USB lifecycle from USBDeviceManager.swift and
 * USBControlTransfer.swift.
 */

#include "usb_device.h"
#include <cstdio>

#define LOG(fmt, ...) fprintf(stderr, "[usb] " fmt "\n", ##__VA_ARGS__)

USBDevice::USBDevice()
    : m_ctx(nullptr)
    , m_dev(nullptr)
    , m_interface_claimed(false)
    , m_kernel_detached(false)
{
}

USBDevice::~USBDevice()
{
    Close();
}

bool USBDevice::Open()
{
    if (m_dev) {
        LOG("already open");
        return true;
    }

    /* Initialize libusb */
    int ret = libusb_init(&m_ctx);
    if (ret != 0) {
        LOG("libusb_init failed: %s", libusb_error_name(ret));
        return false;
    }

#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(m_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#else
    libusb_set_debug(m_ctx, LIBUSB_LOG_LEVEL_WARNING);
#endif

    /* Find and open device */
    m_dev = libusb_open_device_with_vid_pid(m_ctx, DJMT1_VID, DJMT1_PID);
    if (!m_dev) {
        LOG("DJM-T1 not found (VID=0x%04X PID=0x%04X)", DJMT1_VID, DJMT1_PID);
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }
    LOG("DJM-T1 found");

    /* Detach kernel driver if attached */
    if (libusb_kernel_driver_active(m_dev, DJMT1_INTERFACE) == 1) {
        ret = libusb_detach_kernel_driver(m_dev, DJMT1_INTERFACE);
        if (ret != 0) {
            LOG("detach kernel driver failed: %s", libusb_error_name(ret));
            Close();
            return false;
        }
        m_kernel_detached = true;
        LOG("kernel driver detached");
    }

    /* Claim interface */
    ret = libusb_claim_interface(m_dev, DJMT1_INTERFACE);
    if (ret != 0) {
        LOG("claim interface %d failed: %s", DJMT1_INTERFACE, libusb_error_name(ret));
        Close();
        return false;
    }
    m_interface_claimed = true;
    LOG("interface %d claimed", DJMT1_INTERFACE);

    /* Set alt setting 1 (streaming mode) */
    ret = libusb_set_interface_alt_setting(m_dev, DJMT1_INTERFACE, DJMT1_ALT_SETTING);
    if (ret != 0) {
        LOG("set alt setting %d failed: %s", DJMT1_ALT_SETTING, libusb_error_name(ret));
        Close();
        return false;
    }
    LOG("alt setting %d active", DJMT1_ALT_SETTING);

    return true;
}

void USBDevice::Close()
{
    if (m_dev) {
        if (m_interface_claimed) {
            libusb_set_interface_alt_setting(m_dev, DJMT1_INTERFACE, 0);
            libusb_release_interface(m_dev, DJMT1_INTERFACE);
            m_interface_claimed = false;
        }

        if (m_kernel_detached) {
            libusb_attach_kernel_driver(m_dev, DJMT1_INTERFACE);
            m_kernel_detached = false;
        }

        libusb_close(m_dev);
        m_dev = nullptr;
        LOG("device closed");
    }

    if (m_ctx) {
        libusb_exit(m_ctx);
        m_ctx = nullptr;
    }
}

bool USBDevice::SetBoostLevel(int level)
{
    if (!m_dev) return false;

    /*
     * SET request: wValue = value << 8 (high byte), wIndex = command selector
     * From USBControlTransfer.swift: sendSetRequest encodes value in high byte.
     */
    uint16_t wValue = (uint16_t)(level & 0xFF) << 8;
    uint16_t wIndex = 0x8003; /* usbOutputLevel */

    int ret = libusb_control_transfer(m_dev,
        DJMT1_SET_REQUEST_TYPE, DJMT1_SET_REQUEST,
        wValue, wIndex, nullptr, 0, 2000);

    if (ret < 0) {
        LOG("set boost level failed: %s", libusb_error_name(ret));
        return false;
    }

    LOG("boost level set to %d", level);
    return true;
}

bool USBDevice::SetMIDIChannel(int channel)
{
    if (!m_dev) return false;

    uint16_t wValue = (uint16_t)(channel & 0xFF) << 8;
    uint16_t wIndex = 0x8004; /* midiChannel */

    int ret = libusb_control_transfer(m_dev,
        DJMT1_SET_REQUEST_TYPE, DJMT1_SET_REQUEST,
        wValue, wIndex, nullptr, 0, 2000);

    if (ret < 0) {
        LOG("set MIDI channel failed: %s", libusb_error_name(ret));
        return false;
    }

    LOG("MIDI channel set to %d", channel);
    return true;
}

bool USBDevice::GetInputSelector(uint8_t out[3])
{
    if (!m_dev) return false;

    uint16_t wIndex = 0x8002; /* inputSelector */

    int ret = libusb_control_transfer(m_dev,
        DJMT1_GET_REQUEST_TYPE, DJMT1_GET_REQUEST,
        0, wIndex, out, 3, 2000);

    if (ret < 0) {
        LOG("get input selector failed: %s", libusb_error_name(ret));
        return false;
    }

    return true;
}
