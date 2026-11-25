/* OPEN MPK4: board support for the MPK mini IV (pin map: docs/hardware/hardware-map.md). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

extern volatile uint32_t g_ms;       /* 1 kHz, SysTick */
extern volatile uint32_t g_ticks;    /* 50 kHz (20 us), TMR3: matrix step; every 5th tick LEDs + ADC block */
bool clock_init(void);               /* 216 MHz from the 12 MHz crystal; false = crystal did not start (HICK) */
extern uint32_t g_reset_flags;       /* CRM ctrlsts as the bootloader left it; startup.c clears the flags (watchdog) */

/* Code that must keep running while the flash is busy (storage.c: a sector erase stalls flash reads for 13-16 ms):
 * linked into SRAM (ld/app.ld .ramfunc). Everything such code calls is ISR_INLINE, so no copy of it stays in flash.
 * tools/fw_smoke.py fails if any flash fetch or read happens while the flash is busy. */
#define RAMFUNC    __attribute__((section(".ramfunc"), noinline))
#define ISR_INLINE static inline __attribute__((always_inline))

/* independent watchdog, fed from the main loop (board.c) */
void wdt_start(void);
void wdt_feed(void);
void board_init(void);
void delay_ms(uint32_t ms);

/* ---- key/button matrix: 11 drive lines (PC0-7, PF6, PF7, PD2) x 8 sense lines (PC8-15), raw events ---- */
#define MX_LINES 11
typedef struct { uint8_t line, bits, changed; uint32_t ts; } mx_event_t;   /* bits: 1 = closed */
bool    matrix_pop(mx_event_t *e);
uint8_t matrix_state(int line);
uint32_t matrix_dropped(void);       /* events lost because the queue was full */

/* ---- analog: ADC1 IN0..IN5 (PA0..PA5) x mux address 0..7 (PB0..PB2), 12-bit ---- */
#define ADC_CH  6
#define MUX_N   8
uint16_t adc_raw(int ch, int mux);
uint32_t adc_blocks(void);           /* finished conversion blocks (one per mux position, nominal 10 kHz) */
uint16_t knob_a(int k);              /* k = 0..7 (K1..K8): wiper A on PA2, wiper B on PA3 */
uint16_t knob_b(int k);
uint16_t pad_raw(int p);             /* p = 0..7 (pad 1..8) */
enum { WHEEL_PITCH = 0, WHEEL_MOD = 1 };
uint16_t wheel_raw(int w);           /* PA4 pitch, PA5 mod */
bool     pedal_level(void);          /* PA7 pin level (pull-up: 1 = open) */

/* ---- push encoder: PF4/PF5 quadrature, one count per detent (stock algorithm) ---- */
int32_t enc_count(void);
uint8_t enc_ab(void);                /* raw PF5:PF4 */

/* ---- LEDs: 40-bit shift-register chain (SPI1, active low) + SHIFT (PB9) + LOOP (PB7), 0..32 brightness ---- */
enum { LED_CHAIN_BITS = 40, LED_SHIFT = 40, LED_LOOP = 41, LED_COUNT = 42, LED_MAX = 32, NO_LED = 0xFF };
void led_set(int n, uint8_t level);
void led_beat(int n, uint8_t level, uint8_t flash);   /* `flash` while the beat flash is on (timed.h), else `level` */
/* hardware map section 6: each pad's red, green, blue chain bits; each button's LED (bicolour buttons: second colour
 * in [1]), indexed by button idx (inputs.h), NO_LED where there is none */
extern const uint8_t PAD_LEDS[8][3];
extern const uint8_t BTN_LEDS[24][2];

/* ---- timed service: while set, the 50 kHz tick requests `fn` every SERVICE_TICKS ticks through PendSV, the lowest
 * priority exception: it preempts the main loop, never an interrupt. The main loop sets it around long work that
 * shares no state with `fn` (the frame render), so `fn` (the engine that sends MIDI) keeps its timing meanwhile. ---- */
#define SERVICE_TICKS 10             /* 200 us */
void board_service(void (*fn)(void));   /* NULL = off */

/* ---- CPU cycle counter (DWT), for the timing figures in the debug STATUS ---- */
uint32_t cycles(void);
uint32_t cycles_us(uint32_t c);      /* cycles -> microseconds at the running core clock */

/* ---- LCD: ST7735S-class 160 x 80, RGB565 big-endian, SPI2 + DMA, ONE framebuffer (ui/ui.h says why) ---- */
#define LCD_W 160
#define LCD_H 80
#define LCD_FLUSH_US (LCD_W * LCD_H * 2u * 8u * 1000u / 13500u)   /* one frame over SPI at 13.5 MHz: 15.2 ms */
uint16_t *lcd_framebuffer(void);     /* draw here only while !lcd_busy(); lcd_flush() hands it to the DMA */
bool lcd_busy(void);
void lcd_flush(void);

/* ---- timed output (timed.h): the 50 kHz tick runs the internal 24 PPQN clock and the DIN output, so neither the
 * main loop nor a flash erase can delay a clock byte or a note staged for a tick ---- */
void clk_run(uint32_t first_q8, uint32_t period_q8, uint32_t n, bool din);   /* tick n due at first_q8 (g_ticks << 8),
                                                                                   then one per period_q8 */
void clk_period(uint32_t period_q8, bool din);   /* new tempo from the tick after the next; clock bytes to DIN or not */
void clk_stop(void);                            /* the internal schedule stops (following a MIDI clock) */
bool clk_running(void);
void clk_next(uint32_t *q8, uint32_t *n);       /* the next tick the schedule emits: its due time and index */
uint32_t clk_emitted(void);                     /* clock ticks emitted so far (internal and followed), wraps */
void clk_followed(uint32_t k, bool din);        /* a followed clock tick k: the beat flash, and the byte on to DIN */
#define BEAT_FLASH_TICKS 4                       /* the beat flash lasts 4 clock ticks (1/6 beat) */

/* ---- DIN MIDI out: USART1 TX on PB6, 31250 baud, driven by the tick (timed.h gives the order) ---- */
bool din_write(const uint8_t *b, int n);   /* a whole message or nothing (a part would garble the stream) */
bool din_realtime(uint8_t b);              /* a realtime byte (Start, Stop, ...), ahead of the queue; also F2 + 2 */
bool din_stage(uint32_t t, const uint8_t *b, int n);   /* a message (<= 3 bytes) to send once g_ticks reaches t */
uint32_t din_queued(void);                 /* bytes and messages waiting to go out */
bool din_backlog(void);                    /* messages wait in the queue (a staged one would overtake them) */

/* ---- settings page 0x0803F800 (read-only here): serial number, wheel calibration (hardware map section 9) ---- */
#define SETTINGS_PAGE ((const volatile uint8_t *)0x0803F800)
void unit_serial(char out[17]);      /* ProductSN, printable ASCII, NUL-terminated (empty if the page is blank) */
