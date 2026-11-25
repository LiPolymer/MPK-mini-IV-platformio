/* OPEN MPK4: TinyUSB glue: OTGFS1 (rhport 0) IRQ, clock, VBUS sensing off, millisecond time base. */
#include "at32f402_405.h"
#include "tusb.h"
#include "board.h"

void OTGFS1_IRQHandler(void) { tusb_int_handler(0, true); }
uint32_t tusb_time_millis_api(void) { return g_ms; }

#define OTGFS1_GCCFG (*(volatile uint32_t *)(OTGFS1_BASE + 0x38))
#define GCCFG_VBUSIG (1u << 21)                  /* no VBUS pin on this board (PA9 unused): ignore VBUS sensing */

void usb_init(void)
{
    crm_periph_clock_enable(CRM_OTGFS1_PERIPH_CLOCK, TRUE);
    /* startup.c reset the core (D+ pull-up off). After a flash the host still holds the bootloader's HID-only
     * enumeration: keep detached long enough for it to notice before connecting again. */
    while (g_ms < 100) { }
    OTGFS1_GCCFG |= GCCFG_VBUSIG;
    NVIC_SetPriority(OTGFS1_IRQn, 2);
    tusb_rhport_init_t dev = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    tusb_init(0, &dev);
}
