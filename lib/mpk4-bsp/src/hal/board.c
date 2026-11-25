/* OPEN MPK4: board support (matrix scan, ADC + mux, LED chain, LCD, encoder, DIN). Timing and register setup mirror
 * stock v1.41 (docs/hardware/hardware-map.md; peripheral values checked against tools/re/hwtrace.py on the stock
 * image). Structure follows the OPENMPK3 board.c. */
#include "at32f402_405.h"
#include "board.h"
#include <string.h>

volatile uint32_t g_ticks;

void delay_ms(uint32_t ms) { uint32_t t = g_ms; while (g_ms - t < ms) { } }

/* ------------------------------------------------------------------ GPIO helpers (register level) */
enum { IN = 0, OUT = 1, AF = 2, ANALOG = 3, NOPULL = 0, PULLUP = 1, PULLDOWN = 2 };
static void pin_mode(gpio_type *g, int pin, uint32_t mode, uint32_t pull)
{
    g->cfgr = (g->cfgr & ~(3u << (2 * pin))) | (mode << (2 * pin));
    g->pull = (g->pull & ~(3u << (2 * pin))) | (pull << (2 * pin));
    g->omode &= ~(1u << pin);                                       /* push-pull */
    g->odrvr = (g->odrvr & ~(3u << (2 * pin))) | (1u << (2 * pin)); /* stronger drive */
}
static void pin_af(gpio_type *g, int pin, uint32_t af, uint32_t pull)
{
    volatile uint32_t *mux = pin < 8 ? &g->muxl : &g->muxh;
    *mux = (*mux & ~(0xFu << (4 * (pin & 7)))) | (af << (4 * (pin & 7)));
    pin_mode(g, pin, AF, pull);
}
#define HI(g, p) ((g)->scr = 1u << (p))
#define LO(g, p) ((g)->clr = 1u << (p))

/* ------------------------------------------------------------------ matrix: one line per 20 us tick (stock 08016018) */
#define MXQ 64
static mx_event_t mxq[MXQ];
static volatile uint8_t mx_head, mx_tail;
static volatile uint32_t mx_drop;
static volatile uint8_t mx_state[MX_LINES];
static uint8_t mx_line;
/* in RAM (.data), not .rodata: the tick reads it while the flash is busy */
static const struct { gpio_type *g; uint8_t pin; } MX_DRIVE[MX_LINES] __attribute__((section(".data.MX_DRIVE"))) = {
    { GPIOC, 0 }, { GPIOC, 1 }, { GPIOC, 2 }, { GPIOC, 3 }, { GPIOC, 4 }, { GPIOC, 5 }, { GPIOC, 6 }, { GPIOC, 7 },
    { GPIOF, 6 }, { GPIOF, 7 }, { GPIOD, 2 } };

bool matrix_pop(mx_event_t *e)
{
    if (mx_tail == mx_head) return false;
    *e = mxq[mx_tail]; mx_tail = (uint8_t)((mx_tail + 1) % MXQ);
    return true;
}
uint8_t matrix_state(int line) { return line >= 0 && line < MX_LINES ? mx_state[line] : 0; }
uint32_t matrix_dropped(void) { return mx_drop; }

ISR_INLINE void mx_all_high(void)
{
    GPIOC->scr = 0x00FF; HI(GPIOF, 6); HI(GPIOF, 7); HI(GPIOD, 2);
}

ISR_INLINE void matrix_step(void)
{
    uint8_t bits = (uint8_t)~(GPIOC->idt >> 8);          /* sense lines are active low: 1 = closed */
    uint8_t ch = bits ^ mx_state[mx_line];
    if (ch) {
        mx_state[mx_line] = bits;
        uint8_t nh = (uint8_t)((mx_head + 1) % MXQ);
        if (nh != mx_tail) { mxq[mx_head] = (mx_event_t){ mx_line, bits, ch, g_ticks }; mx_head = nh; }
        else mx_drop++;
    }
    mx_all_high();
    mx_line = (uint8_t)((mx_line + 1) % MX_LINES);
    LO(MX_DRIVE[mx_line].g, MX_DRIVE[mx_line].pin);
}

static void matrix_init(void)
{
    mx_all_high();
    for (int i = 0; i < MX_LINES; i++) pin_mode(MX_DRIVE[i].g, MX_DRIVE[i].pin, OUT, PULLUP);
    for (int p = 8; p < 16; p++) pin_mode(GPIOC, p, IN, NOPULL);   /* external pull-ups, as stock */
    LO(MX_DRIVE[0].g, MX_DRIVE[0].pin);
}

/* ------------------------------------------------------------------ ADC1 IN0..5 + mux on PB0..2 (stock 0801dbaa / 0801dde4)
 * One 6-channel scan per mux position, DMA1 ch2, one block per 100 us (five 20 us ticks):
 *   tick 0  the scan started at tick 1 of the previous block is done (it takes 56 us): store it, advance the mux
 *   tick 1  start the next scan, 20 us after the mux switch
 * Why the gap: the mux output node (pin + mux + board capacitance, driven by the high source impedance of an FSR pad
 * divider) still holds the previous position's voltage when the mux switches. Stock and the 40.01 build started the
 * scan right after the switch, so IN0 (PA0, first in the scan) finished sampling 8.9 us later: a saturated pad left
 * about 240/1023 on the next pad of the same mux line (the ghost notes on the unit, all on PA0). With the 20 us gap
 * IN0 stops sampling 28.9 us after the switch. Timing and the decay estimate: docs/hardware/hardware-map.md section 4. */
static volatile uint16_t adc_dma[ADC_CH];
static volatile uint16_t adc_val[ADC_CH][MUX_N];
static uint8_t mux_addr;
static volatile uint32_t adc_nblk;
/* mux slot tables from the stock image (hardware map section 4) */
static const uint8_t KNOB_A_MUX[8] = { 1, 4, 7, 0, 2, 6, 5, 3 };   /* table 0x0802D75C - 8, PA2 */
static const uint8_t KNOB_B_MUX[8] = { 5, 3, 2, 4, 7, 0, 1, 6 };   /* table 0x0802D764 - 16, PA3 */
static const uint8_t PAD_SLOT[8]   = { 3, 2, 4, 0, 1, 6, 5, 7 };   /* table 0x0802D7E4: slot >> 2 = PA0/PA1, & 3 = mux */

uint16_t adc_raw(int ch, int mux) { return adc_val[ch % ADC_CH][mux & 7]; }
uint32_t adc_blocks(void) { return adc_nblk; }
uint16_t knob_a(int k)  { return adc_val[2][KNOB_A_MUX[k & 7]]; }
uint16_t knob_b(int k)  { return adc_val[3][KNOB_B_MUX[k & 7]]; }
uint16_t pad_raw(int p) { uint8_t s = PAD_SLOT[p & 7]; return adc_val[s >> 2][s & 3]; }
uint16_t wheel_raw(int w) { return adc_val[w == WHEEL_PITCH ? 4 : 5][0]; }
bool pedal_level(void) { return (GPIOA->idt >> 7) & 1; }

ISR_INLINE void adc_start_block(void)
{
    DMA1_CHANNEL2->ctrl_bit.chen = 0;
    DMA1->clr = DMA1_FDT2_FLAG | DMA1_GL2_FLAG;
    DMA1_CHANNEL2->dtcnt = ADC_CH;
    DMA1_CHANNEL2->ctrl_bit.chen = 1;
    ADC1->ctrl2_bit.ocswtrg = 1;
}

static bool adc_armed;                                       /* mux switched, next scan due at the next tick */

/* tick 0 of a block: store the finished scan and switch the mux; the scan starts one tick later (adc_trigger) */
ISR_INLINE void adc_collect(void)
{
    if (!(DMA1->sts & DMA1_FDT2_FLAG)) return;
    uint8_t done = mux_addr;
    mux_addr = (uint8_t)((mux_addr + 1) & 7);
    GPIOB->odt = (GPIOB->odt & ~7u) | mux_addr;
    for (int c = 0; c < 4; c++) adc_val[c][done] = adc_dma[c];
    for (int c = 4; c < ADC_CH; c++) for (int m = 0; m < MUX_N; m++) adc_val[c][m] = adc_dma[c];   /* not muxed */
    adc_nblk++;
    adc_armed = true;
}

ISR_INLINE void adc_trigger(void)
{
    if (!adc_armed) return;
    adc_armed = false;
    adc_start_block();
}

static void adc_init(void)
{
    for (int p = 0; p < 6; p++) pin_mode(GPIOA, p, ANALOG, NOPULL);
    for (int p = 0; p < 3; p++) pin_mode(GPIOB, p, OUT, NOPULL);
    GPIOB->clr = 7u;
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);
    adc_clock_div_set(ADC_DIV_8);                               /* stock ADCCOM.cctrl = 0x00060000 */
    adc_base_config_type b;
    adc_base_default_para_init(&b);
    b.sequence_mode = TRUE; b.repeat_mode = FALSE; b.data_align = ADC_RIGHT_ALIGNMENT;
    b.ordinary_channel_length = ADC_CH;
    adc_base_config(ADC1, &b);
    for (int c = 0; c < ADC_CH; c++)
        adc_ordinary_channel_set(ADC1, (adc_channel_select_type)c, (uint8_t)(c + 1), ADC_SAMPLETIME_239_5);
    adc_ordinary_conversion_trigger_set(ADC1, ADC12_ORDINARY_TRIG_SOFTWARE, TRUE);
    adc_dma_mode_enable(ADC1, TRUE);
    adc_enable(ADC1, TRUE);
    adc_calibration_init(ADC1); while (adc_calibration_init_status_get(ADC1)) { }
    adc_calibration_start(ADC1); while (adc_calibration_status_get(ADC1)) { }

    dma_init_type d;
    dma_default_para_init(&d);
    d.peripheral_base_addr = (uint32_t)&ADC1->odt; d.memory_base_addr = (uint32_t)adc_dma;
    d.direction = DMA_DIR_PERIPHERAL_TO_MEMORY; d.buffer_size = ADC_CH;
    d.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD; d.memory_data_width = DMA_MEMORY_DATA_WIDTH_HALFWORD;
    d.memory_inc_enable = TRUE; d.priority = DMA_PRIORITY_MEDIUM;
    dma_init(DMA1_CHANNEL2, &d);
    dmamux_init(DMA1MUX_CHANNEL2, DMAMUX_DMAREQ_ID_ADC1);
    adc_start_block();
}

/* ------------------------------------------------------------------ timed output (clock schedule, DIN, beat flash) */
#define TIMED_TX_READY() (USART1->sts_bit.tdbe)
#define TIMED_TX(b) (USART1->dt = (b))
static inline uint32_t timed_irq_off(void) { uint32_t m = __get_PRIMASK(); __disable_irq(); return m; }
static inline void timed_irq_on(uint32_t m) { __set_PRIMASK(m); }
#include "timed.h"

/* ------------------------------------------------------------------ LED map (hardware map section 6) */
const uint8_t PAD_LEDS[8][3] = {                 /* chain bits in the stock pad-object order: red, green, blue */
    { 0x10, 0x11, 0x12 }, { 0x09, 0x0A, 0x0B }, { 0x13, 0x14, 0x15 }, { 0x16, 0x17, 0x08 },
    { 0x0C, 0x0D, 0x0E }, { 0x0F, 0x00, 0x01 }, { 0x02, 0x03, 0x04 }, { 0x05, 0x06, 0x07 } };
const uint8_t BTN_LEDS[24][2] = {
    { 0x18, NO_LED }, { 0x1F, NO_LED }, { 0x1E, NO_LED }, { 0x1D, NO_LED }, { 0x1C, NO_LED }, { 0x1B, NO_LED },
    { NO_LED, NO_LED }, { NO_LED, NO_LED },
    { 0x1A, 0x19 }, { 0x25, NO_LED }, { 0x20, NO_LED }, { LED_SHIFT, NO_LED }, { 0x27, 0x26 }, { 0x24, NO_LED },
    { NO_LED, NO_LED }, { NO_LED, NO_LED },
    { LED_LOOP, NO_LED }, { 0x23, NO_LED }, { 0x22, NO_LED }, { 0x21, NO_LED },
    { NO_LED, NO_LED }, { NO_LED, NO_LED }, { NO_LED, NO_LED }, { NO_LED, NO_LED } };

/* ------------------------------------------------------------------ LED chain on SPI1 + DMA1 ch3 (stock 0801e218 / 0801bb52)
 * 5 bytes per frame, LED bit n = byte n >> 3, bit n & 7, 0 = on. Software PWM: 32 frames, one frame per 200 us. */
static volatile uint8_t led_level[LED_COUNT], led_flash_level[LED_COUNT];
static volatile bool led_flashes[LED_COUNT];
static uint8_t led_tx[LED_CHAIN_BITS / 8];
static uint8_t led_frame, led_phase;
static bool led_kick;                                     /* a beat restarted the frame: latch it at the next tick */
static uint8_t clamp_led(uint8_t v) { return v > LED_MAX ? LED_MAX : v; }
void led_set(int n, uint8_t level) { if (n >= 0 && n < LED_COUNT) { led_level[n] = clamp_led(level); led_flashes[n] = false; } }
void led_beat(int n, uint8_t level, uint8_t flash)
{
    if (n < 0 || n >= LED_COUNT) return;
    led_level[n] = clamp_led(level); led_flash_level[n] = clamp_led(flash); led_flashes[n] = true;
}
static bool beat_on;                                     /* the beat flash, as of this frame */
ISR_INLINE uint8_t led_now(int n) { return beat_on && led_flashes[n] ? led_flash_level[n] : led_level[n]; }

ISR_INLINE void led_start_frame(void)
{
    beat_on = CK.flash != 0;
    for (int i = 0; i < LED_CHAIN_BITS / 8; i++) {                         /* no memset: this runs from RAM */
        uint8_t b = 0xFF;
        for (int k = 0; k < 8; k++) if (led_frame < led_now(8 * i + k)) b &= (uint8_t)~(1u << k);
        led_tx[i] = b;
    }
    LO(GPIOB, 4);                                                         /* latch low while shifting */
    DMA1_CHANNEL3->ctrl_bit.chen = 0;
    DMA1->clr = DMA1_FDT3_FLAG | DMA1_GL3_FLAG;
    DMA1_CHANNEL3->dtcnt = sizeof led_tx;
    DMA1_CHANNEL3->ctrl_bit.chen = 1;
    if (led_frame < led_now(LED_SHIFT)) HI(GPIOB, 9); else LO(GPIOB, 9);   /* GPIO LEDs: high = on (stock tables) */
    if (led_frame < led_now(LED_LOOP))  HI(GPIOB, 7); else LO(GPIOB, 7);
}

ISR_INLINE void led_step(void)
{
    if (led_phase == 0) { led_start_frame(); led_phase = 1; return; }
    if (!(DMA1->sts & DMA1_FDT3_FLAG) || SPI1->sts_bit.bf) return;
    HI(GPIOB, 4);                                                         /* latch */
    led_frame = (uint8_t)((led_frame + 1) % LED_MAX);
    led_phase = 0;
    led_kick = false;
}

/* a beat: shift a fresh frame out now (aborting the one in flight; the latch is low, so nothing shows) and latch it
 * at the next tick, so the beat LEDs light with the beat's clock byte, not up to a PWM frame later */
ISR_INLINE void led_beat_now(void)
{
    led_frame = 0;
    led_start_frame();
    led_phase = 1;
    led_kick = true;
}

static void led_init(void)
{
    pin_mode(GPIOB, 4, OUT, PULLUP); HI(GPIOB, 4);
    pin_mode(GPIOB, 8, OUT, PULLDOWN); LO(GPIOB, 8);       /* chain enable: low until the first latch */
    pin_mode(GPIOB, 9, OUT, PULLUP); LO(GPIOB, 9);
    pin_mode(GPIOB, 7, OUT, PULLUP); LO(GPIOB, 7);
    pin_af(GPIOB, 3, GPIO_MUX_5, PULLUP); pin_af(GPIOB, 5, GPIO_MUX_5, PULLUP);   /* pulls as stock */
    crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, TRUE);
    spi_init_type s;
    spi_default_para_init(&s);
    s.transmission_mode = SPI_TRANSMIT_HALF_DUPLEX_TX; s.master_slave_mode = SPI_MODE_MASTER;
    s.mclk_freq_division = SPI_MCLK_DIV_64; s.first_bit_transmission = SPI_FIRST_BIT_MSB;
    s.frame_bit_num = SPI_FRAME_8BIT; s.clock_polarity = SPI_CLOCK_POLARITY_LOW; s.clock_phase = SPI_CLOCK_PHASE_1EDGE;
    s.cs_mode_selection = SPI_CS_SOFTWARE_MODE;
    spi_init(SPI1, &s);
    spi_i2s_dma_transmitter_enable(SPI1, TRUE);
    spi_enable(SPI1, TRUE);
    dma_init_type d;
    dma_default_para_init(&d);
    d.peripheral_base_addr = (uint32_t)&SPI1->dt; d.memory_base_addr = (uint32_t)led_tx;
    d.direction = DMA_DIR_MEMORY_TO_PERIPHERAL; d.buffer_size = sizeof led_tx;
    d.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE; d.memory_data_width = DMA_MEMORY_DATA_WIDTH_BYTE;
    d.memory_inc_enable = TRUE; d.priority = DMA_PRIORITY_LOW;
    dma_init(DMA1_CHANNEL3, &d);
    dmamux_init(DMA1MUX_CHANNEL3, DMAMUX_DMAREQ_ID_SPI1_TX);
    led_start_frame();                                      /* latch the levels preloaded by the application */
    while (!(DMA1->sts & DMA1_FDT3_FLAG) || SPI1->sts_bit.bf) { }
    HI(GPIOB, 4);
    HI(GPIOB, 8);
}

/* ------------------------------------------------------------------ LCD on SPI2 + DMA1 ch1 (stock 0801d97a / 0801caa8) */
static uint16_t fb[LCD_W * LCD_H];
static volatile uint8_t lcd_sending;

static void spi2_bytes(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) { while (!SPI2->sts_bit.tdbe) { } SPI2->dt = b[i]; }
    while (SPI2->sts_bit.bf) { }
}
/* one command with its parameters: D/C (PB14) low for the command byte, high for the data */
static void lcd_cmd(const uint8_t *c, int n)
{
    LO(GPIOB, 12);
    LO(GPIOB, 14); spi2_bytes(c, 1);
    HI(GPIOB, 14); if (n > 1) spi2_bytes(c + 1, n - 1);
    HI(GPIOB, 12);
}

bool lcd_busy(void)
{
    if (!lcd_sending) return false;
    if (!(DMA1->sts & DMA1_FDT1_FLAG) || SPI2->sts_bit.bf) return true;
    HI(GPIOB, 12);
    DMA1_CHANNEL1->ctrl_bit.chen = 0;
    lcd_sending = 0;
    return false;
}

uint16_t *lcd_framebuffer(void) { return fb; }

void lcd_flush(void)
{
    if (lcd_busy()) return;
    static const uint8_t caset[] = { 0x2A, 0x00, 0x00, 0x00, LCD_W - 1 };
    static const uint8_t raset[] = { 0x2B, 0x00, 24, 0x00, 24 + LCD_H - 1 };   /* panel sits at row offset 24 */
    static const uint8_t ramwr[] = { 0x2C };
    lcd_cmd(caset, sizeof caset); lcd_cmd(raset, sizeof raset); lcd_cmd(ramwr, sizeof ramwr);
    HI(GPIOB, 14); LO(GPIOB, 12);
    DMA1->clr = DMA1_FDT1_FLAG | DMA1_GL1_FLAG;
    DMA1_CHANNEL1->maddr = (uint32_t)fb;
    DMA1_CHANNEL1->dtcnt = sizeof fb;
    lcd_sending = 1;
    DMA1_CHANNEL1->ctrl_bit.chen = 1;
}

static void lcd_init(void)
{
    pin_mode(GPIOB, 10, OUT, PULLUP); LO(GPIOB, 10);       /* backlight off until the panel is on */
    pin_mode(GPIOB, 11, OUT, PULLUP); HI(GPIOB, 11);
    pin_mode(GPIOB, 12, OUT, PULLUP); HI(GPIOB, 12);
    pin_mode(GPIOB, 14, OUT, PULLUP); HI(GPIOB, 14);
    pin_af(GPIOB, 13, GPIO_MUX_5, PULLUP); pin_af(GPIOB, 15, GPIO_MUX_5, PULLUP);
    crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK, TRUE);
    spi_init_type s;
    spi_default_para_init(&s);
    s.transmission_mode = SPI_TRANSMIT_HALF_DUPLEX_TX; s.master_slave_mode = SPI_MODE_MASTER;
    s.mclk_freq_division = SPI_MCLK_DIV_8; s.first_bit_transmission = SPI_FIRST_BIT_MSB;   /* 108 MHz / 8 */
    s.frame_bit_num = SPI_FRAME_8BIT; s.clock_polarity = SPI_CLOCK_POLARITY_LOW; s.clock_phase = SPI_CLOCK_PHASE_1EDGE;
    s.cs_mode_selection = SPI_CS_SOFTWARE_MODE;
    spi_init(SPI2, &s);
    spi_i2s_dma_transmitter_enable(SPI2, TRUE);
    spi_enable(SPI2, TRUE);
    dma_init_type d;
    dma_default_para_init(&d);
    d.peripheral_base_addr = (uint32_t)&SPI2->dt; d.memory_base_addr = (uint32_t)fb;
    d.direction = DMA_DIR_MEMORY_TO_PERIPHERAL; d.buffer_size = sizeof fb;
    d.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE; d.memory_data_width = DMA_MEMORY_DATA_WIDTH_BYTE;
    d.memory_inc_enable = TRUE; d.priority = DMA_PRIORITY_HIGH;
    dma_init(DMA1_CHANNEL1, &d);
    dmamux_init(DMA1MUX_CHANNEL1, DMAMUX_DMAREQ_ID_SPI2_TX);

    /* reset pulse: stock is high 40 us, low 1 ms, high 40 us; whole ms here (longer is within spec) */
    HI(GPIOB, 11); delay_ms(1); LO(GPIOB, 11); delay_ms(1); HI(GPIOB, 11); delay_ms(1);
    static const uint8_t slpout[] = { 0x11 };
    lcd_cmd(slpout, 1);
    delay_ms(120);
    /* the stock init stream, byte for byte (hardware map section 7): { length, command, parameters... } */
    static const uint8_t seq[] = {
        4, 0xB1, 0x05, 0x3C, 0x3C,
        4, 0xB2, 0x05, 0x3C, 0x3C,
        7, 0xB3, 0x05, 0x3C, 0x3C, 0x05, 0x3C, 0x3C,
        2, 0xB4, 0x07,
        4, 0xC0, 0xE9, 0x09, 0x04,
        2, 0xC1, 0xC5,
        3, 0xC2, 0x0D, 0x00,
        3, 0xC3, 0x8D, 0x6A,
        3, 0xC4, 0x8D, 0xEE,
        2, 0xC5, 0x15,
        2, 0x36, 0x68,
        2, 0x3A, 0x05,
        5, 0x2A, 0x00, 0x18, 0x00, 0x67,
        5, 0x2B, 0x00, 0x00, 0x00, 0x9F,
        17, 0xE0, 0x07, 0x0E, 0x08, 0x07, 0x10, 0x07, 0x02, 0x07, 0x09, 0x0F, 0x25, 0x36, 0x00, 0x08, 0x04, 0x10,
        17, 0xE1, 0x0A, 0x0D, 0x08, 0x07, 0x0F, 0x07, 0x02, 0x07, 0x09, 0x0F, 0x25, 0x35, 0x00, 0x09, 0x04, 0x10,
        2, 0xFC, 0xC0,
        2, 0x3A, 0x05,
        1, 0x13,
        1, 0x29,
        0 };
    for (const uint8_t *p = seq; *p; p += 1 + *p) lcd_cmd(p + 1, *p);
    memset(fb, 0, sizeof fb);
    lcd_flush(); while (lcd_busy()) { }
    HI(GPIOB, 10);                                          /* backlight on */
}

/* ------------------------------------------------------------------ push encoder PF4/PF5 (stock 0802164c) */
static uint8_t enc_prev, enc_moves;
static volatile int32_t enc_cnt;
int32_t enc_count(void) { return enc_cnt; }
uint8_t enc_ab(void) { return (uint8_t)((GPIOF->idt >> 4) & 3); }

ISR_INLINE void enc_step(void)
{
    uint8_t s = (uint8_t)((GPIOF->idt >> 4) & 3);
    if (s == enc_prev) return;
    enc_moves++;
    if (s == 3) {                                           /* detent: direction from the state before it */
        if (enc_moves >= 3) enc_cnt += enc_prev == 1 ? -1 : enc_prev == 2 ? 1 : 0;
        enc_moves = 0;
    }
    enc_prev = s;
}

/* ------------------------------------------------------------------ DIN MIDI out: USART1 TX PB6, driven by the tick */
static void din_init(void)
{
    pin_af(GPIOB, 6, GPIO_MUX_7, NOPULL);
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);
    usart_init(USART1, 31250, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_transmitter_enable(USART1, TRUE);
    usart_enable(USART1, TRUE);
}

/* ------------------------------------------------------------------ timed service (board.h) */
static void (*volatile service_fn)(void);
static volatile bool led_only;
void board_service(void (*fn)(void)) { service_fn = fn; }
void PendSV_Handler(void) { void (*fn)(void) = service_fn; if (fn) fn(); }

/* ------------------------------------------------------------------ 50 kHz tick, in RAM: keeps running during flash erases */
RAMFUNC void TMR3_GLOBAL_IRQHandler(void)
{
    TMR3->ists = 0;
    static uint8_t div;
    if (++div == 5) div = 0;
    if (led_only) {
        if (div == 2) led_step();
        return;
    }
    g_ticks++;
    static uint8_t service_div;
    if (service_fn && ++service_div >= SERVICE_TICKS) { service_div = 0; SCB->ICSR = SCB_ICSR_PENDSVSET_Msk; }
    matrix_step();
    enc_step();
    if (timed_tick(g_ticks)) led_beat_now();
    if (div == 0) adc_collect();
    else if (div == 1) adc_trigger();
    else if (div == 2 || led_kick) led_step();
}

uint32_t cycles(void) { return DWT->CYCCNT; }
uint32_t cycles_us(uint32_t c) { uint32_t mhz = system_core_clock / 1000000u; return c / (mhz ? mhz : 1); }

void board_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;          /* cycle counter for the timing figures */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE); crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE); crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE); crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    dmamux_enable(DMA1, TRUE);
    matrix_init();
    pin_mode(GPIOA, 7, IN, PULLUP);                          /* sustain pedal */
    pin_mode(GPIOF, 4, IN, PULLUP); pin_mode(GPIOF, 5, IN, PULLUP);   /* encoder A/B */
    enc_prev = enc_ab();
    adc_init();
    led_init();
    din_init();
    /* TMR3 update at 50 kHz from the APB1 timer clock (2 x APB1 when APB1 is divided, as here) */
    crm_clocks_freq_type f;
    crm_clocks_freq_get(&f);
    uint32_t tclk = f.ahb_freq == f.apb1_freq ? f.apb1_freq : 2 * f.apb1_freq;
    crm_periph_clock_enable(CRM_TMR3_PERIPH_CLOCK, TRUE);
    tmr_base_init(TMR3, tclk / 50000 - 1, 0);
    tmr_cnt_dir_set(TMR3, TMR_COUNT_UP);
    tmr_interrupt_enable(TMR3, TMR_OVF_INT, TRUE);
    NVIC_SetPriority(PendSV_IRQn, 15);                       /* the timed service: below every interrupt */
    nvic_irq_enable(TMR3_GLOBAL_IRQn, 1, 0);
    led_only = true;
    tmr_counter_enable(TMR3, TRUE);
    lcd_init();
    led_only = false;
}

/* ------------------------------------------------------------------ independent watchdog (WDT, clocked by LICK)
 * Fed by the main loop only while the 50 kHz tick advances, so a stuck main loop or a dead tick both reset the unit.
 * The reset boots straight back into this firmware: startup.c clears the reset flags, so the bootloader sees neither
 * the power-on nor the software-reset flag (update-protocol.md section 3). Timeout from the fastest LICK the
 * datasheet allows (45 kHz; 25 kHz slowest) so it never fires early; the longest legitimate main-loop stall is one
 * 16 ms sector erase (storage.c). */
#define LICK_MAX_HZ    45000u
#define WDT_MIN_MS     250u
#define WDT_PRESCALE   32u                                  /* WDT_CLK_DIV_32 */
#define WDT_RELOAD     (WDT_MIN_MS * LICK_MAX_HZ / WDT_PRESCALE / 1000u)
_Static_assert(WDT_RELOAD <= 0xFFF, "WDT reload is 12 bits");

void wdt_start(void)
{
    crm_clock_source_enable(CRM_CLOCK_SOURCE_LICK, TRUE);
    while (crm_flag_get(CRM_LICK_STABLE_FLAG) != SET) { }
    WDT->cmd = WDT_CMD_UNLOCK;
    WDT->div = WDT_CLK_DIV_32;
    WDT->rld = WDT_RELOAD;
    while (WDT->sts & (WDT_DIVF_UPDATE_FLAG | WDT_RLDF_UPDATE_FLAG)) { }
    WDT->cmd = WDT_CMD_RELOAD;
    WDT->cmd = WDT_CMD_ENABLE;
}

void wdt_feed(void)
{
    static uint32_t fed;
    if (g_ticks == fed) return;
    fed = g_ticks;
    WDT->cmd = WDT_CMD_RELOAD;
}

void unit_serial(char out[17])
{
    int n = 0;
    for (; n < 16; n++) {
        uint8_t c = SETTINGS_PAGE[n];
        if (c < 0x20 || c > 0x7E) break;
        out[n] = (char)c;
    }
    out[n] = 0;
}
