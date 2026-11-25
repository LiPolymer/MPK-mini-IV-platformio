/* OPEN MPK4: input decoding (keys with velocity, buttons, knobs, encoder, pads, wheels, pedal) plus the raw events
 * (matrix changes, ADC slot changes) the bring-up build streams to the host. Same shape as the OPENMPK3 inputs. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define N_KEYS    25
#define N_BUTTONS 24                 /* idx = (line - 8) * 8 + bit, lines 8..10 */
#define N_KNOBS   8
#define N_PADS    8
/* button idx (hardware map section 3, every one confirmed on the unit) */
enum { BTN_OCT_DN = 0, BTN_OCT_UP = 1, BTN_ARP = 2, BTN_LATCH = 3, BTN_REPEAT = 4, BTN_TAP = 5, BTN_BANK = 8,
       BTN_MINUS = 9, BTN_PLUS = 10, BTN_SHIFT = 11, BTN_DAW = 12, BTN_UNDO = 13, BTN_ENC_PUSH = 14, BTN_LOOP = 16,
       BTN_PLAY = 17, BTN_REC = 18, BTN_OVERDUB = 19 };
extern const char *const BTN_NAMES[N_BUTTONS];   /* stock-map names; "" = no button at that position */

typedef enum {
    EV_MATRIX,      /* raw (inputs_raw): a = line, b = bits now closed, v = bits that changed */
    EV_KEY_ON,      /* a = key 0..24 (0 = lowest C), b = velocity, v = contact time dt (20 us ticks), d = 1 if the
                       odd (second) contact closed first */
    EV_KEY_OFF,     /* a = key */
    EV_BTN_DOWN,    /* a = button idx */
    EV_BTN_UP,
    EV_KNOB,        /* a = knob 0..7, d = signed ticks (2 degrees of turn each), v = ms per tick since the knob's last
                       event (KNOB_IDLE_MS or more: the first tick of a gesture); the app turns these into steps */
    EV_ENC,         /* d = signed detents */
    EV_PAD_ON,      /* a = pad 0..7, b = velocity, v = peak (10-bit) */
    EV_PAD_OFF,     /* a = pad */
    EV_WHEEL,       /* a = WHEEL_PITCH / WHEEL_MOD, v = filtered raw, d = output (bend -8192..8191 / CC 0..127) */
    EV_PEDAL,       /* b = pin level (1 = high) */
    EV_ADC,         /* raw (inputs_raw): a = ADC channel 0..5, b = mux 0..7, v = 12-bit value (changed by >= ADC_EVENT_STEP) */
    EV_PAD_PRESSURE, /* a = pad, b = pressure, v = sample (10-bit) */
} ev_type_t;

typedef struct { ev_type_t type; uint8_t a, b; int16_t d; uint16_t v; } input_ev_t;

#define ADC_EVENT_STEP 32            /* raw ADC debug events: 32 of 4096 counts, below that is noise */

void inputs_init(void);
void inputs_set_key_curve(unsigned curve);
void inputs_get_key_curve(unsigned curve, uint16_t values[5]);
void inputs_poll(void);              /* every main-loop pass */
bool inputs_pop(input_ev_t *e);
uint32_t inputs_dropped(void);
bool button_down(int idx);
bool key_down(int key);
bool pad_down(int p);
void inputs_raw(bool on);            /* also queue the raw EV_MATRIX / EV_ADC events (hardware test); off at boot */
/* ms since the player last touched the unit (a key, pad or button pressed or released, a knob, the encoder or a wheel
 * moved); 0 while anything is held. KNOB_IDLE_MS or more: a short main-loop stall is not felt, a burst has ended */
uint32_t inputs_idle_ms(void);
#define KNOB_IDLE_MS 200             /* a knob at rest this long starts a new gesture (and needs a firmer turn to wake) */

/* wheel calibration in use (from the settings page if it is sane, else the stock defaults) */
typedef struct { uint16_t min, centre, max; } wheel_cal_t;
extern wheel_cal_t g_wheel_cal[2];
