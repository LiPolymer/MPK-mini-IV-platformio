/* OPEN MPK4: slot storage in internal flash. The only code in the firmware that erases or programs flash.
 *
 * Region: [__storage_start, __storage_end) from ld/app.ld (asserted there: whole sectors inside the range the
 * bootloader erases on update, clear of the bootloader, the app, the sector holding the app-valid bytes and the
 * settings pages). flash_op() refuses any address outside it before it touches a FLASH register.
 *
 * Layout: every 2 KiB sector holds one part of one blob: a 24-byte header, then up to STORAGE_PART_BYTES of data.
 *   header { u32 magic, u32 seq, u8 slot, u8 part, u8 nparts, u8 format, u16 len, u16 blob_len, u32 data_crc,
 *            u32 hdr_crc }   seq: one number per blob, higher = newer; CRC-32 (zlib) over the part's data / the header
 * A blob is valid when all its parts are present with matching slot/seq/nparts/blob_len and correct CRCs. Per slot
 * the valid blob with the highest seq is current.
 *
 * Power loss: a write programs the data of every part into erased sectors, reads each word back, then programs the
 * headers (the commit marker, written last). The previous blob is not touched until the new one is complete, and only
 * then becomes garbage, so an interrupted write leaves the slot exactly as it was. Garbage sectors are erased lowest
 * seq first: an older copy of a slot is always gone before a newer one, so a deleted or superseded blob can never come
 * back after a power cut. A sector whose header is not valid is garbage too (torn write or interrupted erase).
 *
 * Timing (datasheet: word program 40-42 us, 2 KiB sector erase 13.2-16 ms; one flash bank, so the CPU cannot fetch
 * from flash while it is busy): flash_op() runs from RAM, and while the flash is busy it keeps only interrupts whose
 * handler is in RAM enabled (the 50 kHz tick: matrix, ADC, LEDs, DIN; SysTick). Others (USB) wait at most one erase;
 * the USB core NAKs meanwhile, nothing is lost. storage_poll programs STORAGE_WORDS_PER_POLL words per call (about
 * 0.17 ms, so a main-loop pass stays short enough for MIDI timing) and erases at most one sector per call, and only
 * when the caller says idle. */
#include "mpk4/storage.h"
#include "board.h"
#include "at32f402_405.h"
#include <stddef.h>
#include <string.h>

extern const uint8_t __storage_start[], __storage_end[];   /* ld/app.ld */
extern uint32_t _estack;

#define MAGIC 0x31534D4Fu                                 /* "OMS1" */
#define FORMAT 1
#define MAX_SECTORS (0x40000u / STORAGE_SECTOR)            /* the whole flash: bounds the RAM tables for any region */
#define STORAGE_WORDS_PER_POLL 4

typedef struct {
    uint32_t magic, seq;
    uint8_t slot, part, nparts, format;
    uint16_t len, blob_len;
    uint32_t data_crc, hdr_crc;
} hdr_t;
_Static_assert(sizeof(hdr_t) == STORAGE_HEADER, "header size");
_Static_assert(STORAGE_SLOTS <= 255 && STORAGE_MAX_PARTS <= 255, "slot and part numbers are bytes");

/* ------------------------------------------------------------------ lowest level: RAM, guarded */
#define FLASH_STS_ERRORS (FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG)

/* The only function that starts a flash erase or program. Refuses (returns -1, no register touched) any address
 * outside the storage region or not aligned to its unit. erase: sector erase at addr; else program `word` at addr. */
__attribute__((section(".ramfunc"), noipa))
static int flash_op(uint32_t addr, uint32_t word, bool erase)
{
    const uint32_t lo = (uint32_t)__storage_start, hi = (uint32_t)__storage_end;
    if (addr < lo || addr >= hi || (addr & (erase ? STORAGE_SECTOR - 1 : 3u))) return -1;
    /* while the flash is busy only handlers in RAM may run: mask every enabled IRQ whose vector points elsewhere */
    const uint32_t *vt = (const uint32_t *)SCB->VTOR;
    uint32_t masked[sizeof NVIC->ISER / sizeof NVIC->ISER[0]];
    for (unsigned i = 0; i < sizeof masked / sizeof masked[0]; i++) {
        masked[i] = 0;
        for (uint32_t en = NVIC->ISER[i]; en; en &= en - 1) {
            uint32_t b = 31u - __CLZ(en & -en), h = vt[16 + 32 * i + b] & ~1u;
            if (h < SRAM_BASE || h >= (uint32_t)&_estack) masked[i] |= 1u << b;
        }
        NVIC->ICER[i] = masked[i];
    }
    __DSB(); __ISB();
    FLASH->unlock = FLASH_UNLOCK_KEY1;
    FLASH->unlock = FLASH_UNLOCK_KEY2;
    FLASH->sts = FLASH_ODF_FLAG | FLASH_STS_ERRORS;          /* write 1 to clear */
    if (erase) {
        FLASH->ctrl_bit.secers = 1;
        FLASH->addr = addr;
        FLASH->ctrl_bit.erstr = 1;
    } else {
        FLASH->ctrl_bit.fprgm = 1;
        *(volatile uint32_t *)addr = word;
    }
    while (FLASH->sts & FLASH_OBF_FLAG) { }
    uint32_t st = FLASH->sts;
    FLASH->ctrl_bit.secers = 0;
    FLASH->ctrl_bit.fprgm = 0;
    FLASH->ctrl_bit.oplk = 1;
    for (unsigned i = 0; i < sizeof masked / sizeof masked[0]; i++) NVIC->ISER[i] = masked[i];
    return st & FLASH_STS_ERRORS ? -2 : 0;
}

/* ------------------------------------------------------------------ CRC-32 (zlib polynomial, nibble table) */
static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n)
{
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
    c = ~c;
    while (n--) { c ^= *p++; c = (c >> 4) ^ T[c & 15]; c = (c >> 4) ^ T[c & 15]; }
    return ~c;
}

/* ------------------------------------------------------------------ state (all rebuilt from flash by storage_init) */
enum { S_FREE, S_LIVE, S_DIRTY, S_BAD };
static struct {
    uint32_t base, nsec, next_seq;
    uint32_t next_alloc;                          /* wear levelling: allocate round-robin, after the newest sector */
    uint8_t state[MAX_SECTORS];
    uint32_t seq[MAX_SECTORS];                    /* DIRTY: header seq if the header is valid, else 0 (erase first) */
    struct { uint8_t nparts; int8_t sec[STORAGE_MAX_PARTS]; uint16_t len; } slot[STORAGE_SLOTS];
    struct {
        bool active, headers;                     /* headers: data of all parts programmed, now the commit markers */
        uint8_t slot, nparts, part;
        int8_t sec[STORAGE_MAX_PARTS];
        const uint8_t *src;
        uint32_t len, seq, off, crc, part_crc;    /* off: byte offset in the current part (data) or header */
    } job;
    storage_result_t last;
} st;

static const hdr_t *hdr_at(int s) { return (const hdr_t *)(st.base + (uint32_t)s * STORAGE_SECTOR); }
static const uint8_t *data_at(int s) { return (const uint8_t *)hdr_at(s) + STORAGE_HEADER; }
static uint32_t part_len(uint32_t blob_len, int part)
{
    uint32_t done = (uint32_t)part * STORAGE_PART_BYTES;
    return blob_len - done < STORAGE_PART_BYTES ? blob_len - done : STORAGE_PART_BYTES;
}
static uint32_t nparts_for(uint32_t len) { return len ? (len + STORAGE_PART_BYTES - 1) / STORAGE_PART_BYTES : 1; }

static bool header_ok(const hdr_t *h)
{
    return h->magic == MAGIC && h->format == FORMAT && h->hdr_crc == crc32(0, (const uint8_t *)h, offsetof(hdr_t, hdr_crc))
        && h->slot < STORAGE_SLOTS && h->nparts >= 1 && h->nparts <= STORAGE_MAX_PARTS && h->part < h->nparts
        && h->nparts == nparts_for(h->blob_len) && h->len == part_len(h->blob_len, h->part);
}

static bool erased(int s)
{
    const uint32_t *w = (const uint32_t *)hdr_at(s);
    for (uint32_t i = 0; i < STORAGE_SECTOR / 4; i++) if (w[i] != 0xFFFFFFFFu) return false;
    return true;
}

void storage_init(void)
{
    memset(&st, 0, sizeof st);
    st.base = (uint32_t)__storage_start;
    st.nsec = ((uint32_t)__storage_end - st.base) / STORAGE_SECTOR;
    if (st.nsec > MAX_SECTORS) st.nsec = 0;       /* cannot happen with ld/app.ld; never index past the tables */
    uint32_t best[STORAGE_SLOTS] = { 0 };
    bool part_ok[MAX_SECTORS];
    for (uint32_t s = 0; s < st.nsec; s++) {
        const hdr_t *h = hdr_at((int)s);
        part_ok[s] = header_ok(h) && h->data_crc == crc32(0, data_at((int)s), h->len);
        if (part_ok[s] && h->seq >= st.next_seq) { st.next_seq = h->seq + 1; st.next_alloc = s + 1; }
        st.state[s] = erased((int)s) ? S_FREE : S_DIRTY;
        st.seq[s] = header_ok(h) ? h->seq : 0;
    }
    /* per slot, the newest blob whose parts are all present and valid */
    for (uint32_t s = 0; s < st.nsec; s++) {
        if (!part_ok[s]) continue;
        const hdr_t *h = hdr_at((int)s);
        if (h->part != 0 || (st.slot[h->slot].nparts && h->seq <= best[h->slot])) continue;
        int8_t sec[STORAGE_MAX_PARTS]; uint32_t found = 0;
        for (uint32_t t = 0; t < st.nsec; t++) {
            const hdr_t *g = hdr_at((int)t);
            if (part_ok[t] && g->seq == h->seq && g->slot == h->slot && g->nparts == h->nparts && g->blob_len == h->blob_len
                && !(found >> g->part & 1)) { sec[g->part] = (int8_t)t; found |= 1u << g->part; }
        }
        if (found != (1u << h->nparts) - 1) continue;
        best[h->slot] = h->seq;
        st.slot[h->slot].nparts = h->nparts; st.slot[h->slot].len = h->blob_len;
        memcpy(st.slot[h->slot].sec, sec, sizeof sec);
    }
    for (int k = 0; k < STORAGE_SLOTS; k++)
        for (int p = 0; p < st.slot[k].nparts; p++) st.state[st.slot[k].sec[p]] = S_LIVE;
    if (!st.next_seq) st.next_seq = 1;
    st.last = STORAGE_OK;
}

/* ------------------------------------------------------------------ queries */
int storage_len(int slot)
{
    if (slot < 0 || slot >= STORAGE_SLOTS) return STORAGE_ARG;
    return st.slot[slot].nparts ? st.slot[slot].len : STORAGE_EMPTY;
}

int storage_read(int slot, void *buf, uint32_t max)
{
    int n = storage_len(slot);
    if (n < 0) return n;
    if ((uint32_t)n > max) return STORAGE_ARG;
    for (int p = 0; p < st.slot[slot].nparts; p++)
        memcpy((uint8_t *)buf + p * STORAGE_PART_BYTES, data_at(st.slot[slot].sec[p]), part_len((uint32_t)n, p));
    return n;
}

void storage_read_at(int slot, uint32_t off, void *out, uint32_t n)
{
    uint8_t *o = out;
    while (n) {
        uint32_t p = off / STORAGE_PART_BYTES, in = off % STORAGE_PART_BYTES, k = STORAGE_PART_BYTES - in;
        if (k > n) k = n;
        if (slot < 0 || slot >= STORAGE_SLOTS || p >= st.slot[slot].nparts) { memset(o, 0, n); return; }
        memcpy(o, data_at(st.slot[slot].sec[p]) + in, k);
        o += k; off += k; n -= k;
    }
}

bool storage_busy(void) { return st.job.active; }
storage_result_t storage_last(void) { return st.last; }

static int count(int state)
{
    int n = 0;
    for (uint32_t s = 0; s < st.nsec; s++) n += st.state[s] == state;
    return n;
}
bool storage_pending(void) { return st.job.active || count(S_DIRTY) > 0; }
storage_stats_t storage_stats(void)
{
    return (storage_stats_t){ (uint8_t)st.nsec, (uint8_t)count(S_FREE), (uint8_t)count(S_LIVE), (uint8_t)count(S_DIRTY),
                              (uint8_t)count(S_BAD) };
}

/* ------------------------------------------------------------------ writes */
storage_result_t storage_write(int slot, const void *data, uint32_t len)
{
    if (slot < 0 || slot >= STORAGE_SLOTS || len > STORAGE_MAX_BLOB || (len && !data)) return STORAGE_ARG;
    if (st.job.active) return STORAGE_BUSY;
    uint32_t np = nparts_for(len);
    if ((uint32_t)(count(S_FREE) + count(S_DIRTY)) < np) return STORAGE_FULL;   /* the old blob stays until the commit */
    st.job = (typeof(st.job)){ .active = true, .slot = (uint8_t)slot, .nparts = (uint8_t)np, .src = data, .len = len,
                               .seq = st.next_seq++, .crc = crc32(0, data, len) };
    for (unsigned p = 0; p < STORAGE_MAX_PARTS; p++) st.job.sec[p] = -1;
    return STORAGE_OK;
}

storage_result_t storage_erase(int slot)
{
    if (slot < 0 || slot >= STORAGE_SLOTS) return STORAGE_ARG;
    if (st.job.active) return STORAGE_BUSY;
    for (int p = 0; p < st.slot[slot].nparts; p++) st.state[st.slot[slot].sec[p]] = S_DIRTY;
    st.slot[slot].nparts = 0;
    return STORAGE_OK;
}

static void job_end(storage_result_t r)
{
    st.job.active = false;
    st.last = r;
}

/* the job's sectors hold partial data: garbage unless the job commits */
static int alloc_sector(void)
{
    for (uint32_t i = 0; i < st.nsec; i++) {
        uint32_t s = (st.next_alloc + i) % st.nsec;
        if (st.state[s] != S_FREE) continue;
        st.next_alloc = s + 1;
        st.state[s] = S_DIRTY; st.seq[s] = st.job.seq;
        return (int)s;
    }
    return -1;
}

/* erase the garbage sector with the lowest seq (never one the running job is writing); false if there is none */
static bool erase_one(void)
{
    int pick = -1;
    for (uint32_t s = 0; s < st.nsec; s++) {
        if (st.state[s] != S_DIRTY) continue;
        bool mine = false;
        for (int p = 0; st.job.active && p < st.job.nparts; p++) mine |= st.job.sec[p] == (int8_t)s;
        if (!mine && (pick < 0 || st.seq[s] < st.seq[pick])) pick = (int)s;
    }
    if (pick < 0) return false;
    bool ok = flash_op(st.base + (uint32_t)pick * STORAGE_SECTOR, 0, true) == 0 && erased(pick);
    st.state[pick] = ok ? S_FREE : S_BAD;
    st.seq[pick] = 0;
    return true;
}

static bool program(uint32_t addr, uint32_t w)
{
    return flash_op(addr, w, false) == 0 && *(const volatile uint32_t *)addr == w;
}

/* one word of the job: data of the current part, then (all data done) the headers. false = finished or waiting */
static bool job_step(bool idle)
{
    int p = st.job.part;
    if (!st.job.headers) {
        if (st.job.sec[p] < 0) {
            int s = alloc_sector();
            if (s < 0) { if (idle) erase_one(); return false; }   /* no erased sector yet: wait for an idle poll */
            st.job.sec[p] = (int8_t)s;
        }
        uint32_t n = part_len(st.job.len, p);
        if (st.job.off < n) {
            uint32_t w = 0xFFFFFFFFu;
            uint32_t k = n - st.job.off < 4 ? n - st.job.off : 4;
            memcpy(&w, st.job.src + p * STORAGE_PART_BYTES + st.job.off, k);
            if (!program((uint32_t)data_at(st.job.sec[p]) + st.job.off, w)) { job_end(STORAGE_FLASH); return false; }
            st.job.off += 4;
            return true;
        }
        st.job.off = 0;
        if (++st.job.part < st.job.nparts) return true;
        /* all data written: commit only if the source did not change underneath */
        if (crc32(0, st.job.src, st.job.len) != st.job.crc) { job_end(STORAGE_CHANGED); return false; }
        st.job.headers = true; st.job.part = 0;
        return true;
    }
    int s = st.job.sec[p];
    uint32_t n = part_len(st.job.len, p);
    if (st.job.off == 0) st.job.part_crc = crc32(0, data_at(s), n);   /* of the verified flash copy */
    hdr_t h = { MAGIC, st.job.seq, st.job.slot, (uint8_t)p, st.job.nparts, FORMAT, (uint16_t)n, (uint16_t)st.job.len,
                st.job.part_crc, 0 };
    h.hdr_crc = crc32(0, (const uint8_t *)&h, offsetof(hdr_t, hdr_crc));
    uint32_t w; memcpy(&w, (const uint8_t *)&h + st.job.off, 4);
    if (!program((uint32_t)hdr_at(s) + st.job.off, w)) { job_end(STORAGE_FLASH); return false; }
    st.job.off += 4;
    if (st.job.off < STORAGE_HEADER) return true;
    st.job.off = 0;
    if (++st.job.part < st.job.nparts) return true;
    /* committed: the new blob is current, the old one is garbage */
    int k = st.job.slot;
    for (int q = 0; q < st.slot[k].nparts; q++) st.state[st.slot[k].sec[q]] = S_DIRTY;
    st.slot[k].nparts = st.job.nparts; st.slot[k].len = (uint16_t)st.job.len;
    for (int q = 0; q < st.job.nparts; q++) { st.slot[k].sec[q] = st.job.sec[q]; st.state[st.job.sec[q]] = S_LIVE; }
    job_end(STORAGE_OK);
    return false;
}

void storage_poll(bool idle)
{
    if (st.job.active) {
        for (int i = 0; i < STORAGE_WORDS_PER_POLL && job_step(idle); i++) { }
        return;
    }
    if (idle) erase_one();
}
