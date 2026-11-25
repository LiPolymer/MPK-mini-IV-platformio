/* OPEN MPK4: the timed output stage of the 50 kHz tick (board.h, "timed output"). Included by exactly one file per
 * binary: board.c (the tick runs it from RAM) and, on the computer, the app tests' fake board, so the tests run this
 * very code. The includer defines TIMED_TX_READY() (the DIN UART can take a byte) and TIMED_TX(b) (send it).
 *
 * Clock: the internal 24 PPQN schedule lives here. Tick k is due at next_q8 + (k - n) * period_q8 (g_ticks << 8, so
 * every tempo keeps its exact period); when it is due the tick emits it at once: the DIN clock byte, the beat flash,
 * the count the USB side and the engine read. Nothing the main loop does (frames, flash erases) can delay it.
 *
 * DIN, by priority, one byte per free UART slot:
 *   1. the realtime queue, in the order its bytes came: clock bytes this tick emitted, and Start / Continue / Stop /
 *      followed clocks from the main loop. Real-time messages may go between any two bytes (MIDI 1.0), so they
 *      never wait for a message
 *   2. the rest of the message on the wire (a message is never split by anything but 1.)
 *   3. Song Position Pointer (it must precede its Continue, so it shares the realtime queue, sent whole)
 *   4. staged messages whose time has come: what the engine computed for a clock tick, released by the tick that
 *      emits it, right behind its clock byte
 *   5. the main queue (live playing, everything else)
 * Beat flash: on for BEAT_FLASH_TICKS clock ticks from every 24th (the beat), switched in the tick that emits the
 * beat's clock byte; the LED chain shows a flashing LED's beat level while it is on. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "board.h"

static volatile struct {
    bool run, din;                     /* internal schedule on; clock bytes to DIN */
    uint32_t next_q8, period_q8;       /* when tick n is due (g_ticks << 8), and the period */
    uint32_t n;                        /* index of the next tick to emit */
    uint32_t emitted;                  /* ticks emitted, ever (internal and followed) */
    uint8_t flash;                     /* clock ticks the beat flash stays on */
    bool flash_new;                    /* the flash just started (the LED chain restarts its frame) */
} CK;

#define DINQ 256
static uint8_t dinq[DINQ];
static volatile uint16_t din_head, din_tail;
#define RTQ 16
static uint8_t rtq[RTQ];
static volatile uint8_t rt_head, rt_tail;
#define SQ 32
static struct { uint32_t t; uint8_t n, b[3]; } sq[SQ];
static volatile uint8_t sq_head, sq_tail;
static uint8_t din_rem, din_src, sq_i;          /* bytes left of the message on the wire, where it comes from */
enum { SRC_Q, SRC_RT, SRC_SQ };
#define SYSEX_OPEN 0xFF                         /* din_rem while a SysEx runs (until F7) */

ISR_INLINE uint8_t midi_msg_len(uint8_t s)      /* bytes of a message from its status; 0 = SysEx (to F7) */
{
    if (s < 0xF0) return (s & 0xE0) == 0xC0 ? 2 : 3;   /* program change / channel pressure: 2 */
    if (s == 0xF0) return 0;
    if (s == 0xF2) return 3;
    if (s == 0xF1 || s == 0xF3) return 2;
    return 1;
}

ISR_INLINE bool rt_push(uint8_t b)              /* the tick, or the main loop with the tick held off */
{
    uint8_t h = rt_head, nh = (uint8_t)((h + 1) % RTQ);
    if (nh == rt_tail) return false;
    rtq[h] = b; rt_head = nh;
    return true;
}

ISR_INLINE void beat_tick(uint32_t k)
{
    if (k % 24 == 0) { CK.flash = BEAT_FLASH_TICKS; CK.flash_new = true; }
    else if (CK.flash) CK.flash--;
}

ISR_INLINE bool din_pop(int src, uint8_t *b)
{
    if (src == SRC_Q) {
        if (din_tail == din_head) return false;
        *b = dinq[din_tail]; din_tail = (uint16_t)((din_tail + 1) % DINQ);
    } else if (src == SRC_RT) {
        if (rt_tail == rt_head) return false;
        *b = rtq[rt_tail]; rt_tail = (uint8_t)((rt_tail + 1) % RTQ);
    } else {
        if (sq_tail == sq_head) return false;
        *b = sq[sq_tail].b[sq_i++];
        if (sq_i >= sq[sq_tail].n) { sq_i = 0; sq_tail = (uint8_t)((sq_tail + 1) % SQ); }
    }
    return true;
}

ISR_INLINE void din_step(uint32_t now)
{
    if (!TIMED_TX_READY()) return;
    uint8_t b = 0;
    if (rt_tail != rt_head && rtq[rt_tail] >= 0xF8) { din_pop(SRC_RT, &b); TIMED_TX(b); return; }
    if (din_rem) {
        if (!din_pop(din_src, &b)) return;                   /* the rest is still being written */
        if (din_rem == SYSEX_OPEN) { if (b == 0xF7) din_rem = 0; }
        else din_rem--;
        TIMED_TX(b);
        return;
    }
    if (rt_tail != rt_head) din_src = SRC_RT;
    else if (sq_tail != sq_head && (int32_t)(now - sq[sq_tail].t) >= 0) din_src = SRC_SQ;
    else if (din_tail != din_head) din_src = SRC_Q;
    else return;
    din_pop(din_src, &b);
    uint8_t len = midi_msg_len(b);
    din_rem = len ? (uint8_t)(len - 1) : SYSEX_OPEN;
    TIMED_TX(b);
}

/* every 20 us tick, from the timer interrupt; returns true when this tick started a beat flash */
ISR_INLINE bool timed_tick(uint32_t now)
{
    bool beat = false;
    if (CK.run && (int32_t)((now << 8) - CK.next_q8) >= 0) {
        uint32_t k = CK.n++;
        CK.next_q8 += CK.period_q8;
        CK.emitted++;
        if (CK.din) rt_push(0xF8);
        beat_tick(k);
        beat = k % 24 == 0;
    }
    din_step(now);
    return beat;
}

/* ------------------------------------------------------------------ main-loop side (board.h) */
#define TIMED_ATOMIC(stmt) do { uint32_t pm_ = timed_irq_off(); stmt; timed_irq_on(pm_); } while (0)

void clk_run(uint32_t first_q8, uint32_t period_q8, uint32_t n, bool din)
{
    TIMED_ATOMIC((CK.next_q8 = first_q8, CK.period_q8 = period_q8, CK.n = n, CK.din = din, CK.run = true));
}
void clk_period(uint32_t period_q8, bool din) { TIMED_ATOMIC((CK.period_q8 = period_q8, CK.din = din)); }
void clk_stop(void) { CK.run = false; }
bool clk_running(void) { return CK.run; }
void clk_next(uint32_t *q8, uint32_t *n) { TIMED_ATOMIC((*q8 = CK.next_q8, *n = CK.n)); }
uint32_t clk_emitted(void) { return CK.emitted; }
void clk_followed(uint32_t k, bool din)            /* a followed clock, handled by the main loop as it arrives */
{
    TIMED_ATOMIC((CK.emitted++, beat_tick(k)));
    if (din) din_realtime(0xF8);
}

bool din_write(const uint8_t *b, int n)
{
    uint16_t h = din_head, used = (uint16_t)((h + DINQ - din_tail) % DINQ);
    if (DINQ - 1 - used < n) {                         /* full (31250 baud is slow): the message is dropped */
#ifdef TIMED_DIN_DROP
        TIMED_DIN_DROP();                               /* the app tests count them */
#endif
        return false;
    }
    for (int i = 0; i < n; i++) { dinq[h] = b[i]; h = (uint16_t)((h + 1) % DINQ); }
    din_head = h;                                       /* the whole message at once */
    return true;
}
bool din_realtime(uint8_t b)                           /* also the 3 bytes of a Song Position Pointer, in order */
{
    bool ok; TIMED_ATOMIC(ok = rt_push(b));
    return ok;
}
bool din_stage(uint32_t t, const uint8_t *b, int n)
{
    uint8_t h = sq_head, nh = (uint8_t)((h + 1) % SQ);
    if (nh == sq_tail || n > 3) return false;
    sq[h].t = t; sq[h].n = (uint8_t)n;
    for (int i = 0; i < n; i++) sq[h].b[i] = b[i];
    sq_head = nh;
    return true;
}
bool din_backlog(void) { return din_tail != din_head; }
uint32_t din_queued(void)
{
    return (uint32_t)((din_head + DINQ - din_tail) % DINQ) + (uint32_t)((rt_head + RTQ - rt_tail) % RTQ)
         + (uint32_t)((sq_head + SQ - sq_tail) % SQ) + (din_rem != 0);
}
