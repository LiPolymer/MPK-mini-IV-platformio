/* OPEN MPK4: input decoding. Algorithms follow stock v1.41 (hardware map sections 3-5), structure the OPENMPK3. */
#include "inputs.h"
#include "board.h"
#include "pad_velocity.h"
#include "mpk4/key_curves.h"
#include <stdlib.h>

const char *const BTN_NAMES[N_BUTTONS] = {
    "OCT-", "OCT+", "ARP", "LATCH", "NOTE RPT", "TAP", "", "",
    "BANK", "-", "+", "SHIFT", "PLUG/DAW", "UNDO", "ENC PUSH", "",
    "LOOP", "STOP/PLAY", "REC", "OVERDUB", "", "", "", "" };

/* ---------------- event queue */
#define EVQ 64
static input_ev_t evq[EVQ]; static uint8_t evh, evt; static uint32_t ev_drop;
static uint32_t t_motion;
static void emit(ev_type_t t, uint8_t a, uint8_t b, int16_t d, uint16_t v)
{
    if (t != EV_MATRIX && t != EV_ADC) t_motion = g_ms;
    uint8_t n = (uint8_t)((evh + 1) % EVQ);
    if (n == evt) { ev_drop++; return; }
    evq[evh] = (input_ev_t){ t, a, b, d, v }; evh = n;
}
bool inputs_pop(input_ev_t *e) { if (evt == evh) return false; *e = evq[evt]; evt = (uint8_t)((evt + 1) % EVQ); return true; }
uint32_t inputs_dropped(void) { return ev_drop; }

/* ---------------- keys: two contacts, velocity from the time between them (stock 08016120) */
/* Contact-time breakpoints from the settings page, in 20 us ticks. */
static uint16_t VEL_T[4] = {4800, 1000, 400, 140};
static unsigned black_key_scale = 9;
static const uint8_t  VEL_V[4] = { 120, 64, 32, 0 };          /* velocity = 128 - V, piecewise linear between */
#define BLACK_KEYS 0x054Au                                    /* semitones C#, D#, F#, G#, A#: thresholds x 0.9 */
static uint8_t key_velocity(int key, uint32_t dt)
{
    uint32_t t[4];
    for (int i = 0; i < 4; i++) t[i] = (BLACK_KEYS >> (key % 12) & 1) ? VEL_T[i] * black_key_scale / 10u : VEL_T[i];
    int v;
    if (dt >= t[0]) v = VEL_V[0];
    else if (dt <= t[3]) v = VEL_V[3];
    else {
        int i = 0;
        while (dt < t[i + 1]) i++;                            /* t[i] > dt >= t[i+1] */
        v = VEL_V[i + 1] + (int)((dt - t[i + 1]) * (uint32_t)(VEL_V[i] - VEL_V[i + 1]) / (t[i] - t[i + 1]));
    }
    v = 128 - v;
    return (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v);
}

static struct { uint32_t t1; uint8_t first, closed; bool down; } keys[N_KEYS];   /* closed: bit0 even, bit1 odd */
bool key_down(int k) { return k >= 0 && k < N_KEYS && keys[k].down; }

/* ---------------- buttons: reported after 10 ms closed (stock), released at once */
static struct { bool closed, down; uint32_t t; } btn[N_BUTTONS];
bool button_down(int b) { return b >= 0 && b < N_BUTTONS && btn[b].down; }

static bool raw;
void inputs_raw(bool on) { raw = on; }

static void handle_matrix(const mx_event_t *e)
{
    if (raw) emit(EV_MATRIX, e->line, e->bits, 0, e->changed);
    for (int bit = 0; bit < 8; bit++) {
        if (!(e->changed >> bit & 1)) continue;
        bool closed = e->bits >> bit & 1;
        if (e->line >= 8) {
            int b = (e->line - 8) * 8 + bit;
            if (closed) { btn[b].closed = true; btn[b].t = g_ms; }
            else { if (btn[b].down) emit(EV_BTN_UP, (uint8_t)b, 0, 0, 0); btn[b].closed = btn[b].down = false; }
            continue;
        }
        int key = 27 - ((e->line >> 1) * 8 + bit);
        if (key < 0 || key >= N_KEYS) continue;
        uint8_t c = (uint8_t)(1u << (e->line & 1)), other = (uint8_t)(c ^ 3);
        if (closed) {
            keys[key].closed |= c;
            if (!(keys[key].closed & other)) { keys[key].t1 = e->ts; keys[key].first = c; }
            else if (!keys[key].down && keys[key].first == other) {
                keys[key].down = true;
                uint32_t dt = e->ts - keys[key].t1;
                emit(EV_KEY_ON, (uint8_t)key, key_velocity(key, dt), keys[key].first == 2,
                     (uint16_t)(dt > 16383 ? 16383 : dt));
            }
        } else {
            keys[key].closed &= (uint8_t)~c;
            if (keys[key].down && (c == 1 || keys[key].closed == 0)) { keys[key].down = false; emit(EV_KEY_OFF, (uint8_t)key, 0, 0, 0); }
            if (keys[key].closed == 0) keys[key].first = 0;
        }
    }
}

static void poll_buttons(void)
{
    for (int b = 0; b < N_BUTTONS; b++)
        if (btn[b].closed && !btn[b].down && g_ms - btn[b].t >= 10) { btn[b].down = true; emit(EV_BTN_DOWN, (uint8_t)b, 0, 0, 0); }
}

/* ---------------- knobs: endless dual-wiper pots, same algorithm as stock 08020800 and the OPENMPK3 */
static struct { uint16_t a, b; uint32_t t_move; bool init; } kn[N_KNOBS];
static void poll_knobs(void)
{
    for (int k = 0; k < N_KNOBS; k++) {
        int a = knob_a(k), b = knob_b(k);
        if (!kn[k].init) { kn[k].a = (uint16_t)a; kn[k].b = (uint16_t)b; kn[k].init = true; continue; }
        int d;
        if (a >= 0xE66)      d = ((int)kn[k].b - b) * 180 / 4095;
        else if (a < 0x19A)  d = (b - (int)kn[k].b) * 180 / 4095;
        else if (b >= 0x801) d = (a - (int)kn[k].a) * 180 / 4095;
        else                 d = ((int)kn[k].a - a) * 180 / 4095;
        uint32_t since = g_ms - kn[k].t_move;
        bool idle = since > KNOB_IDLE_MS;
        int th = idle ? 6 : 2;                                /* at rest a knob needs 6 degrees to wake (no jitter) */
        if (d >= th || d <= -th) {
            int ticks = idle ? (d > 0 ? 1 : -1) : d / 2, n = ticks < 0 ? -ticks : ticks;
            uint32_t per = idle ? KNOB_IDLE_MS : since / (uint32_t)n;
            kn[k].a = (uint16_t)a; kn[k].b = (uint16_t)b; kn[k].t_move = g_ms;
            emit(EV_KNOB, (uint8_t)k, 0, (int16_t)ticks, (uint16_t)(per > KNOB_IDLE_MS ? KNOB_IDLE_MS : per));
        }
    }
}

/* ---------------- encoder: detent counter from the tick, reported as deltas */
static int32_t enc_last;
static void poll_enc(void)
{
    int32_t c = enc_count();
    if (c != enc_last) { emit(EV_ENC, 0, 0, (int16_t)(c - enc_last), 0); enc_last = c; }
}

/* ---------------- pads (FSR): stock thresholds on the 10-bit value, peak over 20 ms (stock 080199e8) */
#define PAD_ON_TH  209
#define PAD_OFF_TH 62
#define PAD_PEAK_TICKS 1001u
#define PAD_RELEASE_TICKS 1001u
static struct { uint8_t st, pressure; uint16_t peak; uint32_t t, pressure_t; } pads[N_PADS];
bool pad_down(int p) { return p >= 0 && p < N_PADS && pads[p].st == 2; }
static void poll_pads(void)
{
    const uint32_t now = g_ticks;
    for (int p = 0; p < N_PADS; p++) {
        uint16_t r = (uint16_t)(pad_raw(p) >> 2);
        switch (pads[p].st) {
        case 0:
            if (r >= PAD_ON_TH) { pads[p].st = 1; pads[p].peak = r; pads[p].t = now; pads[p].pressure = 0; pads[p].pressure_t = now + 10000; }
            break;
        case 1:
            if (r > pads[p].peak) pads[p].peak = r;
            if (now - pads[p].t >= PAD_PEAK_TICKS || r < PAD_OFF_TH) {
                emit(EV_PAD_ON, (uint8_t)p, stock_pad_velocity(pads[p].peak), 0, pads[p].peak);
                pads[p].st = 2;
            }
            break;
        default:
            if (r < PAD_OFF_TH && now - pads[p].t >= PAD_RELEASE_TICKS) {
                emit(EV_PAD_OFF, (uint8_t)p, 0, 0, 0);
                pads[p].st = 0;
            } else if (r > PAD_ON_TH && (int32_t)(now - pads[p].pressure_t) >= 0) {
                uint8_t pressure = stock_pad_velocity(r);
                if (pressure != pads[p].pressure) {
                    pads[p].pressure = pressure;
                    emit(EV_PAD_PRESSURE, (uint8_t)p, pressure, 0, r);
                }
                pads[p].pressure_t = now + 2501;
            }
            break;
        }
    }
}

/* ---------------- wheels: stock sample hysteresis (< 21 ignored, 21..40 +-1, >= 41 jump) and calibration */
wheel_cal_t g_wheel_cal[2];
static uint16_t wh_filt[2]; static int16_t wh_out[2]; static bool wh_init;
static uint16_t cal16(int off) { return (uint16_t)(SETTINGS_PAGE[off] | (SETTINGS_PAGE[off + 1] << 8)); }

static uint16_t hysteresis(uint16_t f, uint16_t r)
{
    int d = (int)r - f;
    if (abs(d) < 21) return f;
    if (abs(d) < 41) return (uint16_t)(f + (d > 0 ? 1 : -1));
    return r;
}

static void poll_wheels(void)
{
    for (int w = 0; w < 2; w++) {
        uint16_t r = wheel_raw(w);
        wh_filt[w] = wh_init ? hysteresis(wh_filt[w], r) : r;
        const wheel_cal_t *c = &g_wheel_cal[w];
        int f = wh_filt[w], out;
        if (w == WHEEL_PITCH) {                                /* dead zone +-300 around the centre, 14-bit */
            if (f > c->centre + 300) out = (f - c->centre - 300) * 8191 / (c->max - c->centre - 300);
            else if (f < c->centre - 300) out = -(c->centre - 300 - f) * 8192 / (c->centre - 300 - c->min);
            else out = 0;
            out = out > 8191 ? 8191 : out < -8192 ? -8192 : out;
        } else {
            out = (f - c->min) * 127 / (c->max - c->min);
            out = out > 127 ? 127 : out < 0 ? 0 : out;
        }
        if (!wh_init || out != wh_out[w]) {
            wh_out[w] = (int16_t)out; emit(EV_WHEEL, (uint8_t)w, 0, (int16_t)out, wh_filt[w]);
        }
    }
    wh_init = true;
}

/* ---------------- pedal: 5 ms debounce (stock 08018fd4); polarity is reported raw */
static bool ped; static uint32_t ped_t;
static void poll_pedal(void)
{
    bool l = pedal_level();
    if (l != ped && g_ms - ped_t >= 5) { ped = l; ped_t = g_ms; emit(EV_PEDAL, 0, l, 0, 0); }
}

/* ---------------- raw ADC slots for the debug stream: every channel x mux position on change */
static uint16_t adc_last[ADC_CH][MUX_N];
static void poll_adc(void)
{
    if (!raw) return;
    for (int c = 0; c < ADC_CH; c++) for (int m = 0; m < MUX_N; m++) {
        uint16_t v = adc_raw(c, m);
        if (abs((int)v - adc_last[c][m]) >= ADC_EVENT_STEP) { adc_last[c][m] = v; emit(EV_ADC, (uint8_t)c, (uint8_t)m, 0, v); }
    }
}

uint32_t inputs_idle_ms(void)
{
    for (int k = 0; k < N_KEYS; k++) if (keys[k].closed) return 0;
    for (int b = 0; b < N_BUTTONS; b++) if (btn[b].closed) return 0;
    for (int p = 0; p < N_PADS; p++) if (pads[p].st) return 0;
    return g_ms - t_motion;
}

void inputs_get_key_curve(unsigned curve, uint16_t values[5])
{
    if (curve > 2) curve = 1;
    const int off = 0x2c + 9 * (int)curve;
    uint16_t from_flash[4];
    for (int i = 0; i < 4; ++i) from_flash[i] = cal16(off + 2 * i);
    unsigned scale = SETTINGS_PAGE[off + 8];
    bool sane = from_flash[0] <= 20000 && from_flash[0] > from_flash[1] &&
        from_flash[1] > from_flash[2] && from_flash[2] > from_flash[3] && from_flash[3] >= 20 &&
        scale >= 5 && scale <= 20;
    for (int i = 0; i < 4; ++i) values[i] = sane ? from_flash[i] : mpk4_key_curve_defaults[curve][i];
    values[4] = sane ? (uint16_t)scale : mpk4_key_curve_defaults[curve][4];
}

void inputs_set_key_curve(unsigned curve)
{
    uint16_t values[5];
    inputs_get_key_curve(curve, values);
    for (int i = 0; i < 4; ++i) VEL_T[i] = values[i];
    black_key_scale = values[4];
}

void inputs_init(void)
{
    inputs_set_key_curve(1);
    /* settings page: wheel 0 (pitch) max/min at 0x20, centres at 0x24/0x26, wheel 1 (mod) max/min at 0x28 */
    const int MAXOFF[2] = { 0x20, 0x28 }, CTROFF[2] = { 0x24, 0x26 };
    for (int w = 0; w < 2; w++) {
        wheel_cal_t c = { cal16(MAXOFF[w] + 2), cal16(CTROFF[w]), cal16(MAXOFF[w]) };
        bool sane = c.min + 300 < c.centre && c.centre + 300 < c.max && c.max < 4096;
        g_wheel_cal[w] = sane ? c : (wheel_cal_t){ 0x0514, 0x0800, 0x0C17 };   /* stock defaults */
    }
    ped = pedal_level();
    enc_last = enc_count();
}

void inputs_poll(void)
{
    mx_event_t e;
    while (matrix_pop(&e)) handle_matrix(&e);
    poll_buttons();
    poll_enc();
    static uint32_t last;
    if (g_ms != last) {                                       /* analog at 1 kHz (a full mux sweep is 0.8 ms) */
        last = g_ms;
        poll_pads(); poll_knobs(); poll_wheels(); poll_pedal(); poll_adc();
    }
}
