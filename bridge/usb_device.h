/*
 * usb_device.h
 *
 * libusb device management for Pioneer DJM-T1.
 * Handles device open/close, interface claim, alt setting, and control transfers.
 */

#ifndef DJMT1_USB_DEVICE_H
#define DJMT1_USB_DEVICE_H

#include <cstdint>
#include <libusb.h>

/* USB device identifiers */
#define DJMT1_VID  0x08E4   /* Pioneer */
#define DJMT1_PID  0x015E   /* DJM-T1 */

/* USB interface/endpoints */
#define DJMT1_INTERFACE    0
#define DJMT1_ALT_SETTING  1
#define DJMT1_EP_IN        0x82
#define DJMT1_EP_OUT       0x01

/* Vendor control request constants */
#define DJMT1_SET_REQUEST_TYPE  0x40
#define DJMT1_SET_REQUEST       0x03
#define DJMT1_GET_REQUEST_TYPE  0xC0
#define DJMT1_GET_REQUEST       0x00

class USBDevice {
public:
    USBDevice();
    ~USBDevice();

    /* Open DJM-T1: init libusb, find device, claim interface, set alt setting */
    bool Open();

    /* Close device and release all resources */
    void Close();

    bool IsOpen() const { return m_dev != nullptr; }

    /* Get the device handle (for isoch transfers) */
    libusb_device_handle *Handle() const { return m_dev; }

    /* Get the libusb context (for event handling) */
    libusb_context *Context() const { return m_ctx; }

    /* Vendor control: set boost level (0=+18dB, 1=+12dB, 2=+6dB, 3=0dB) */
    bool SetBoostLevel(int level);

    /* Vendor control: set MIDI channel (0-15) */
    bool SetMIDIChannel(int channel);

    /* Vendor control: get input selector state (3 bytes for CH1/CH2/AUX) */
    bool GetInputSelector(uint8_t out[3]);

private:
    libusb_context *m_ctx;
    libusb_device_handle *m_dev;
    bool m_interface_claimed;
    bool m_kernel_detached;
};

#endif /* DJMT1_USB_DEVICE_H */
