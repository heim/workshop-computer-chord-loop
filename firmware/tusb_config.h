// TinyUSB-konfig for Euclid4 — device-modus, én CDC-klasse.
// CFG_TUSB_MCU (=OPT_MCU_RP2040) og CFG_TUSB_OS (=OPT_OS_PICO) settes av
// Pico SDK sitt tinyusb_device-bibliotek via compile-defines.

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// Board / RHPort
//--------------------------------------------------------------------
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

//--------------------------------------------------------------------
// Common
//--------------------------------------------------------------------
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (settes normalt av tinyusb_device)
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

// Bare device-stacken
#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// Device
//--------------------------------------------------------------------
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

// Aktive klasser
#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

// CDC FIFO-størrelser (TX/RX). Rikelig for korte JSON-/kommandolinjer.
#define CFG_TUD_CDC_RX_BUFSIZE    256
#define CFG_TUD_CDC_TX_BUFSIZE    256

// CDC endpoint transfer-buffer (full-speed = 64)
#define CFG_TUD_CDC_EP_BUFSIZE    64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
