// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * USB Descriptors for UAC1 Speaker with Feedback
 */

#include "tusb.h"
#include "usb_descriptors.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,

    // Use Interface Association Descriptor (IAD) for Audio
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x1209,  // pid.codes VID
    .idProduct          = 0xDA15,  // pid.codes PID
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

// Total length of configuration descriptor
// 1 sample rate: 48kHz only
#define TUD_AUDIO_DESC_IAD_LEN  8
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DESC_IAD_LEN + TUD_AUDIO10_SPEAKER_STEREO_FB_DESC_LEN(1) + TUD_DFU_RT_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Audio Interface Association Descriptor — groups Audio Control + Audio Streaming
    TUD_AUDIO_DESC_IAD_LEN, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_AUDIO_CONTROL, 2, TUSB_CLASS_AUDIO, 0x00, 0x00, 4,

    // Interface number, string index, byte per sample, bit per sample, EP Out, EP size, EP feedback, sample rates
    TUD_AUDIO10_SPEAKER_STEREO_FB_DESCRIPTOR(
        ITF_NUM_AUDIO_CONTROL,
        4,  // String index for interface name
        CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,
        CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX,
        EPNUM_AUDIO_OUT,
        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_FS,
        EPNUM_AUDIO_FB,
        48000  // Supported sample rate
    ),

    // DFU Runtime Interface
    TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU, 5, DFU_ATTR_WILL_DETACH, 1000, 0),

    // CDC Interface (for EQ profile management)
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 6, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

// Verify descriptor size
TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "Incorrect configuration descriptor size");

// Invoked when received GET CONFIGURATION DESCRIPTOR
uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return desc_configuration;
}

//--------------------------------------------------------------------+
// BOS & MS OS 2.0 Descriptors (Windows driver binding)
//--------------------------------------------------------------------+

#define MS_OS_20_DESC_LEN  42

static uint8_t const desc_ms_os_20[] = {
    // Microsoft OS 2.0 Descriptor Set Header (10 bytes)
    U16_TO_U8S_LE(0x000A),                         // wLength
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),  // wDescriptorType
    U32_TO_U8S_LE(0x06030000),                      // dwWindowsVersion (Windows 8.1+)
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),               // wTotalLength

    // Microsoft OS 2.0 CCGP Device Feature Descriptor (4 bytes)
    // Tells Windows to use composite device driver (usbccgp.sys)
    U16_TO_U8S_LE(0x0004),                          // wLength
    U16_TO_U8S_LE(MS_OS_20_FEATURE_CCGP_DEVICE),    // wDescriptorType

    // Microsoft OS 2.0 Function Subset Header - DFU (8 bytes)
    U16_TO_U8S_LE(0x0008),                          // wLength
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), // wDescriptorType
    ITF_NUM_DFU,                                     // bFirstInterface
    0x00,                                            // bReserved
    U16_TO_U8S_LE(0x0008 + 0x0014),                 // wSubsetLength (header + compatible ID)

    // Microsoft OS 2.0 Compatible ID Descriptor for DFU - WINUSB (20 bytes)
    U16_TO_U8S_LE(0x0014),                          // wLength
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),   // wDescriptorType
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,      // CompatibleID
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SubCompatibleID

    // CDC ACM (ITF_NUM_CDC) is left without a function subset —
    // Windows 10 1703+ auto-detects CDC ACM by class/subclass/protocol
    // and loads usbser.sys via built-in class driver matching.
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect MS OS 2.0 descriptor size");

#define BOS_TOTAL_LEN  (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

TU_VERIFY_STATIC(sizeof(desc_bos) == BOS_TOTAL_LEN, "Incorrect BOS descriptor size");

// Invoked when received GET BOS DESCRIPTOR request
uint8_t const* tud_descriptor_bos_cb(void) {
    return desc_bos;
}

// Invoked when received control request with VENDOR TYPE
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    if (request->bRequest == VENDOR_REQUEST_MICROSOFT && request->wIndex == 7) {
        // MS OS 2.0 descriptor request
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_xfer(rhport, request, (void*)(uintptr_t)desc_ms_os_20, sizeof(desc_ms_os_20));
        }
        return true;
    }
    return false;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// String descriptor index
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_AUDIO_ITF,
    STRID_DFU_RT,
    STRID_CDC,
};

// Array of pointer to string descriptors
static char const* string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  // 0: Supported language is English (0x0409)
    "Elia Chiarucci",               // 1: Manufacturer
    "DA15",                         // 2: Product
    "000000000001",                 // 3: Serial number
    "DA15",                         // 4: Audio Interface
    "DFU Runtime",                  // 5: DFU Runtime Interface
    "DA15 EQ Config",               // 6: CDC Interface
};

static uint16_t _desc_str[32 + 1];

// Invoked when received GET STRING DESCRIPTOR request
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    size_t chr_count;

    switch (index) {
        case STRID_LANGID:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

        default:
            if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
                return NULL;
            }

            const char* str = string_desc_arr[index];

            // Cap at max char
            chr_count = strlen(str);
            size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
            if (chr_count > max_count) {
                chr_count = max_count;
            }

            // Convert ASCII string into UTF-16
            for (size_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
            break;
    }

    // First byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
