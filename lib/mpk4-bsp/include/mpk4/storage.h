/* OPEN MPK4: persistent slots in internal flash (patterns, settings, undo/scratch). Blobs are opaque bytes; their
 * format belongs to the caller. Region, power-loss scheme and timing:
 * docs/hardware/hardware-map.md section 9 (OPEN MPK4 storage).
 *
 * Region: [__storage_start, __storage_end) from ld/app.ld, whose ASSERTs state the rules it obeys.
 *
 * Usage: storage_init() once at boot (reads only). storage_write() queues one blob and returns at once; the main loop
 * calls storage_poll(idle) every pass, which programs a few words per call and erases one sector per call only when
 * `idle` is true. A write is durable once storage_busy() is false and storage_last() == STORAGE_OK; until then the
 * slot still holds its previous blob, also across a power cut. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_SLOTS 19
#define STORAGE_SECTOR      0x800u                          /* AT32F405xC erase unit, 2 KiB */
#define STORAGE_HEADER      24u                             /* per-sector header, see storage.c */
#define STORAGE_PART_BYTES  (STORAGE_SECTOR - STORAGE_HEADER) /* blob bytes per sector */
#define STORAGE_MAX_PARTS   6u                              /* a full pattern (blob.h PATTERN_BLOB_MAX) fits */
#define STORAGE_MAX_BLOB    (STORAGE_MAX_PARTS * STORAGE_PART_BYTES)

typedef enum {
    STORAGE_OK = 0,
    STORAGE_BUSY = -1,      /* a write is in progress (storage_write / storage_erase) */
    STORAGE_FULL = -2,      /* not enough sectors for this blob next to the other slots' blobs */
    STORAGE_ARG = -3,       /* bad slot or length */
    STORAGE_FLASH = -4,     /* the flash reported an error or a programmed word did not read back */
    STORAGE_CHANGED = -5,   /* the caller's buffer changed while it was being written: nothing committed */
    STORAGE_EMPTY = -6,     /* storage_read: no blob in this slot */
} storage_result_t;

void storage_init(void);
/* Queue a blob. `data` must stay unchanged until storage_busy() is false (it is checksummed at the start and
 * compared at the end; a change aborts the write and keeps the old blob). len 0 stores an empty blob. */
storage_result_t storage_write(int slot, const void *data, uint32_t len);
storage_result_t storage_erase(int slot);         /* the slot reads empty at once; durable once storage_pending() is false */
int storage_len(int slot);                        /* blob length, or STORAGE_EMPTY */
int storage_read(int slot, void *buf, uint32_t max);   /* copies the blob, returns its length; STORAGE_ARG if max is short */
/* copies bytes [off, off + n) of the slot's blob (the caller keeps inside storage_len); no RAM copy of the whole blob */
void storage_read_at(int slot, uint32_t off, void *out, uint32_t n);
void storage_poll(bool idle);                     /* every main-loop pass; idle = a 16 ms stall of the main loop is fine now */
bool storage_busy(void);                          /* a write is in progress */
bool storage_pending(void);                       /* a write is in progress or sectors wait to be erased */
storage_result_t storage_last(void);              /* result of the last finished write */

typedef struct { uint8_t sectors, free, live, dirty, bad; } storage_stats_t;
storage_stats_t storage_stats(void);

#ifdef __cplusplus
}
#endif
