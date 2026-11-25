/* OPEN MPK4: TinyUSB configuration. OTGFS1 (rhport 0, PA11/PA12, 48 MHz from PLLU), full speed, like stock. */
#pragma once
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64
#define CFG_TUD_HID             1      /* vendor HID: answers the flasher's C4 mode probe like the stock app */
#define CFG_TUD_HID_EP_BUFSIZE  32
#define CFG_TUD_MIDI            1
#define CFG_TUD_MIDI_RX_BUFSIZE 256
#define CFG_TUD_MIDI_TX_BUFSIZE 2048   /* the debug SysEx stream can burst (every control on the bring-up build) */
#define CFG_TUD_MIDI_EP_BUFSIZE 64
#define USB_MIDI_EP_OUT 0x02            /* host to unit */
#define USB_MIDI_EP_IN  0x82            /* unit to host (midi.c watches it to coalesce controllers) */
