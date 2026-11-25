"""Akai MPK mini IV (AT32F405, USB 09E8:005D) flash layout and image safety gates.

One place for every number the image builder and the flasher depend on. Each constant is a fact about the
genuine Akai bootloader (pinned below by sha256), with the bootloader instruction that proves it in
EVIDENCE; `derive_from_bootloader()` re-reads those instructions from a stock image so the self-test can
prove the table matches the code. Full analysis: docs/hardware/update-protocol.md.
"""
import hashlib, os, struct

BASE = 0x08000000
IMG_SIZE = 0x40000            # 256 KiB: whole internal flash
PKT = 16                      # payload bytes per C2 packet
SECTOR = 0x800                # erase granularity used by the bootloader (2 KiB)

BOOT = (0x0000, 0x6000)       # Akai bootloader, never written by the bootloader itself
STAGE = (0x6000, 0xC000)      # staged bootloader copy; the stock app copies it over BOOT when markers differ
BOOT_MARKER = BOOT[1] - 1     # last byte of each half is the bootloader "version marker"
STAGE_MARKER = STAGE[1] - 1
APP = 0xC000                  # application vector table; the bootloader jumps to *(BASE + APP + 4)
ERASE = (0x6000, 0x3F000)     # C1 erases these sectors
WRITE_MIN, WRITE_MAX = 0x6000, 0x40000   # C2 below WRITE_MIN: acked F2, ignored; at/above WRITE_MAX: E2
PROTECTED = (0x3F000, 0x3F800)           # C2 acked F2 and ignored; C1 never erases it (app's settings backup)
LAST_PAGE = 0x3F800           # C2 here goes to a RAM staging page, committed by the packet at TAIL_PKT
TAIL_PKT = 0x3FFF0
APP_VALID = (0x38FFE, 0x38FFF)  # if either byte is 0xFF the bootloader stays in update mode
APP_VALID_SECTOR = (APP_VALID[0] & ~(SECTOR - 1), (APP_VALID[1] | (SECTOR - 1)) + 1)   # 0x38800-0x38FFF
PRESERVE_ALWAYS = 0x2C        # last-page bytes [0, 0x2C) always kept from the device (serials, wheel cal)
PRESERVE_IF_LAYOUT = (0x2C, 0x7F0)  # kept too when the image's layout byte equals the device's
LAYOUT_BYTE = 0x3FFF5
VIDPID = 0x3FFF0              # 09 E8 00 5D, big-endian
VERSION = (0x3FFF6, 0x3FFF7)  # BCD major, minor; returned by C4
CHECKSUM = 0x3FFFE            # big-endian 16-bit byte sum of [0, CHECKSUM); checked by the host only
VID, PID = 0x09E8, 0x005D
# USB product strings of the firmwares that run on the unit (both use VID/PID above): stock Akai, OPEN MPK4.
# Hosts name the MIDI ports after them, e.g. "MPK mini IV MIDI Port" / "OPEN MPK4 MIDI Port".
PRODUCT_NAMES = ('MPK mini IV', 'OPEN MPK4')
SRAM = (0x20000000, 0x20018000)  # AT32F405: 96 KiB SRAM [M]; the stock app uses up to 0x200115F0

# SHA-256 of image[0:STAGE[1]] (bootloader and staged copy). Stock v1.41 and v1.43
# use the same flash/reset layout, checked at their respective instruction offsets.
KNOWN_STOCK_BOOT = {
    'c8bb122f6d91697a24aac027281515d2b2305fb57c442a7a2f987311e0b77966': 'v1.43',
    '709d90ac544a9f5c4f1aced836b363d7d292be2582f54c37e75dc5aa6f8e64d0': 'v1.41',
    '92a9955a46aa72c9570edb3b607d6f23ed1e2857b2c2b129e0b389b216a360c7': 'v1.27',
}

# sha256 of the whole genuine Akai stock image every OPEN MPK4 image is assembled from (its bootloader + staged copy,
# app-valid packet and settings pages are copied from it). It is Akai's own file: never distributed by this project;
# tools/fetch_stock.py takes it from Akai's public installer (or a file the user has) and checks it against this pin.
STOCK_NAME = 'MPKminiIV_v1.41_stock.bin'
KNOWN_STOCK_IMAGE = {
    'd196bc38d1f55b53c6ecc49c2401db0f94fc682b3317aa4f0af501c9c9db4c19': 'v1.41',
    '4425879b3ec23f5febd6e2134844dc4c6aaa220600602d5c3803b79ac7cdab97': 'v1.43',
}

SYSEX_ENTER_UPDATE = bytes([0xF0, 0x47, 0x00, 0x5D, 0x74, 0x00, 0x00, 0xF7])

# Bootloader instructions (v1.41, absolute addresses) whose literal-pool value is the constant.
EVIDENCE = {
    'erase_start': (0x08004512, BASE + ERASE[0]),
    'erase_end': (0x08004516, BASE + ERASE[1]),
    'write_min': (0x0800458E, BASE + WRITE_MIN),
    'write_max': (0x080045AE, BASE + WRITE_MAX),
    'direct_write_end': (0x08004644, BASE + PROTECTED[0]),   # C2 below this: programmed at once
    'stage_page': (0x08004688, BASE + LAST_PAGE),            # C2 at/above this: RAM staging page
    'tail_packet': (0x080046C2, BASE + TAIL_PKT),
    'preserve_tail': (0x08004712, BASE + LAST_PAGE + PRESERVE_ALWAYS),
    'layout_byte': (0x080046F4, BASE + LAYOUT_BYTE),
    'version_major': (0x080048AA, BASE + VERSION[0]),
    'version_minor': (0x080048B2, BASE + VERSION[1]),
    'app_valid_hi': (0x08004D34, BASE + APP_VALID[1]),
    'app_valid_lo': (0x08004D3E, BASE + APP_VALID[0]),
    'app_reset_vector': (0x08004DBE, BASE + APP + 4),
    'marker_live': (0x080047F2, BASE + BOOT_MARKER),
    'marker_staged': (0x080047F6, BASE + STAGE_MARKER),
}

# v1.43 moved most literals by 0x20–0x24; the sector and bounds are unchanged.
EVIDENCE_V143 = {
    'erase_start': 0x080044EE, 'erase_end': 0x080044F2,
    'write_min': 0x0800456A, 'write_max': 0x0800458A,
    'direct_write_end': 0x08004620, 'stage_page': 0x08004664, 'tail_packet': 0x080046A0,
    'preserve_tail': 0x080046F0, 'layout_byte': 0x080046D2,
    'version_major': 0x0800488A, 'version_minor': 0x08004892,
    'app_valid_hi': 0x08004D14, 'app_valid_lo': 0x08004D1E, 'app_reset_vector': 0x08004D9E,
    'marker_live': 0x080047D2, 'marker_staged': 0x080047D6,
}


def sum16(b):
    return sum(b) & 0xFFFF


def _ldr_literal(img, addr):
    """Value loaded by a Thumb PC-relative LDR at absolute `addr` (T1 0x48xx-0x4Fxx or T2 F8DF)."""
    o = addr - BASE
    hw = struct.unpack_from('<H', img, o)[0]
    if hw & 0xF800 == 0x4800:
        la = ((addr + 4) & ~3) + (hw & 0xFF) * 4
    elif hw == 0xF8DF:
        hw2 = struct.unpack_from('<H', img, o + 2)[0]
        la = ((addr + 4) & ~3) + (hw2 & 0xFFF)
    else:
        raise ValueError(f'no PC-relative LDR at {addr:#010x} ({hw:#06x})')
    return struct.unpack_from('<I', img, la - BASE)[0]


def derive_from_bootloader(img):
    """{name: (read, expected)} for every EVIDENCE entry, read from the image's bootloader code."""
    adjusted = EVIDENCE_V143 if KNOWN_STOCK_BOOT.get(boot_hash(img)) == 'v1.43' else {}
    return {k: (_ldr_literal(img, adjusted.get(k, a)), v) for k, (a, v) in EVIDENCE.items()}


def boot_hash(img):
    return hashlib.sha256(img[:STAGE[1]]).hexdigest()


def validate(img, allow_layout=None):
    """Return a list of refusal reasons (empty = the image is safe to flash).

    allow_layout: if given, the image's layout byte must equal it (pass the stock value to make sure the
    bootloader keeps the user's settings instead of overwriting them with the image's defaults).
    """
    errs = []
    if len(img) != IMG_SIZE:
        return [f'image must be exactly {IMG_SIZE} bytes, got {len(img)}']
    h = boot_hash(img)
    if h not in KNOWN_STOCK_BOOT:
        errs.append('0x0000-0xBFFF is not a genuine Akai bootloader + staged copy (sha256 mismatch)')
    if img[BOOT[0]:BOOT[1]] != img[STAGE[0]:STAGE[1]]:
        errs.append('staged slot 0x6000-0xBFFF differs from the bootloader: the stock app would copy it over '
                    'the bootloader (brick)')
    if img[VIDPID:VIDPID + 4] != struct.pack('>HH', VID, PID):
        errs.append(f'trailer VID/PID is {img[VIDPID:VIDPID + 4].hex()}, expected {VID:04x}{PID:04x}')
    if 0xFF in img[VERSION[0]:VERSION[1] + 1]:
        errs.append('version bytes 0x3FFF6/7 are 0xFF (the app treats the settings page as blank, C4 readback fails)')
    if 0xFF in (img[APP_VALID[0]], img[APP_VALID[1]]):
        errs.append('app-valid bytes 0x38FFE/0x38FFF are 0xFF: the bootloader would never start the app')
    if allow_layout is not None and img[LAYOUT_BYTE] != allow_layout:
        errs.append(f'layout byte {img[LAYOUT_BYTE]:#04x} != {allow_layout:#04x}: the device settings at '
                    f'0x3F82C-0x3FFEF would be replaced by the image defaults')
    sp, rv = struct.unpack_from('<II', img, APP)
    if not (SRAM[0] < sp <= SRAM[1] and sp % 8 == 0):
        errs.append(f'app initial SP {sp:#x} outside SRAM')
    if not (rv & 1 and BASE + APP + 0x40 <= (rv & ~1) < BASE + APP_VALID[0]):
        errs.append(f'app reset vector {rv:#x} is not a Thumb address inside the app region')
    if struct.unpack_from('>H', img, CHECKSUM)[0] != sum16(img[:CHECKSUM]):
        errs.append('trailer checksum (BE sum16 of 0x00000-0x3FFFD) mismatch')
    return errs


def storage_errors(lo, hi, app_end):
    """Reasons the firmware's storage region [lo, hi) (image offsets) breaks the rules; [] = fine. The same rules as
    the ASSERTs in firmware/ld/app.ld: whole sectors; inside the range C1 erases on every update (so an update always
    starts it empty and nothing of stock lives there); clear of the bootloader and its staged copy, the sector holding
    the app-valid bytes, the settings backup / settings page, and the application [APP, app_end)."""
    errs = []
    overlaps = lambda a, b: lo < b and a < hi
    if lo % SECTOR or hi % SECTOR: errs.append(f'storage {lo:#x}-{hi:#x} is not whole {SECTOR:#x}-byte sectors')
    if hi <= lo: errs.append('storage is empty')
    if lo < ERASE[0] or hi > ERASE[1]:
        errs.append(f'storage {lo:#x}-{hi:#x} outside the range every update erases {ERASE[0]:#x}-{ERASE[1]:#x}')
    if overlaps(BOOT[0], STAGE[1]): errs.append('storage overlaps the bootloader or its staged copy')
    if overlaps(*APP_VALID_SECTOR): errs.append('storage overlaps the sector holding the app-valid bytes')
    if overlaps(PROTECTED[0], IMG_SIZE): errs.append('storage overlaps the settings backup / settings page')
    if overlaps(APP, app_end): errs.append(f'storage overlaps the application {APP:#x}-{app_end:#x}')
    return errs


def flash_plan(img):
    """Ordered list of C2 offsets that leaves the device recoverable if interrupted at any point.

    * nothing below WRITE_MIN or inside PROTECTED (the bootloader ignores them anyway)
    * erased-range packets that are all 0xFF are skipped (C1 already left 0xFF there)
    * every LAST_PAGE packet is sent (staging RAM is not initialised), TAIL_PKT last of them (commit)
    * the packet holding the app-valid bytes goes last: until it lands the bootloader keeps update mode
    """
    marker_pkt = APP_VALID[0] & ~(PKT - 1)
    body = [o for o in range(ERASE[0], ERASE[1], PKT)
            if o != marker_pkt and not PROTECTED[0] <= o < PROTECTED[1] and img[o:o + PKT] != b'\xff' * PKT]
    tail = [o for o in range(LAST_PAGE, IMG_SIZE, PKT) if o != TAIL_PKT] + [TAIL_PKT]
    return body + tail + [marker_pkt]


def c2_packet(off, data):
    """55 4F C2, BE offset, 00, (off>>4)&0x3F counter (as the Akai updater), 16 data, 0 pad, BE sum16."""
    assert len(data) == PKT and off % PKT == 0
    p = bytearray(32)
    p[0:3] = b'\x55\x4F\xC2'
    p[3:7] = struct.pack('>I', off)
    p[8] = (off >> 4) & 0x3F
    p[9:25] = data
    p[30:32] = struct.pack('>H', sum16(data))
    return bytes(p)


def command(c):
    return bytes([0x55, 0x4F, c]) + bytes(29)


# ------------------------------------------------------------------ the stock image: where it is, whether it is genuine
class StockError(Exception):
    pass


def cache_dir():
    """The user-local folder holding the stock image fetched by tools/fetch_stock.py (outside any checkout)."""
    return os.environ.get('OPENMPK4_HOME') or os.path.join(os.path.expanduser('~'), '.openmpk4')


def stock_candidates():
    """Where the stock image is looked for, in order: the repo's own backup copy (private checkouts), then the cache."""
    here = os.path.dirname(os.path.abspath(__file__))
    return [os.path.normpath(os.path.join(here, '..', 'backup', 'firmware', STOCK_NAME)),
            os.path.join(cache_dir(), STOCK_NAME)]


def stock_errors(img):
    """Reasons `img` is not the pinned genuine stock image ([] = it is)."""
    if len(img) != IMG_SIZE: return [f'stock image must be exactly {IMG_SIZE} bytes, got {len(img)}']
    errs = []
    if hashlib.sha256(img).hexdigest() not in KNOWN_STOCK_IMAGE:
        errs.append('sha256 is not the pinned Akai stock image ' + ', '.join(KNOWN_STOCK_IMAGE))
    if boot_hash(img) not in KNOWN_STOCK_BOOT: errs.append('bootloader + staged copy is not a known genuine one')
    errs += validate(img)
    return errs


def find_stock(path=None):
    """(path, bytes) of the verified stock image: `path` if given, else the first of stock_candidates() that exists;
    None when there is none. Raises StockError when the file found is missing (explicit path) or not genuine."""
    if path is not None and not os.path.exists(path): raise StockError(f'{path}: no such file')
    for p in [path] if path is not None else stock_candidates():
        if os.path.exists(p):
            with open(p, 'rb') as f: img = f.read()
            errs = stock_errors(img)
            if errs: raise StockError(f'{p} is not the genuine stock image: ' + '; '.join(errs))
            return p, img
    return None


def require_stock(path=None):
    """find_stock(), but no stock image is an error that says how to get one."""
    found = find_stock(path)
    if found is None:
        raise StockError('no stock image found (looked in ' + ', '.join(stock_candidates()) + '). '
                         'Get it with: python3 tools/fetch_stock.py')
    return found


# ------------------------------------------------------------------ app package: an app binary that carries what
# mkimage needs (version, storage region), so a build can be published without any Akai bytes and assembled into a full
# image on the user's computer from their own stock image. Layout: app bytes, then a footer (little-endian):
#   magic 12 | format 1 | version major, minor 2 | reserved 1 | app length 4 | storage lo, hi 4+4 (image offsets) |
#   sha256 of the app bytes 32
PKG_MAGIC = b'OPENMPK4APP\x00'
PKG_FORMAT = 1
PKG_FOOTER = struct.Struct('<12sBBBBIII32s')


def is_app_package(data):
    return len(data) > PKG_FOOTER.size and data[-PKG_FOOTER.size:][:len(PKG_MAGIC)] == PKG_MAGIC


def pack_app(app, version, storage):
    """App package bytes for `app` (the linked binary), BCD (major, minor) and the storage region (lo, hi offsets)."""
    return bytes(app) + PKG_FOOTER.pack(PKG_MAGIC, PKG_FORMAT, version[0], version[1], 0, len(app), storage[0],
                                        storage[1], hashlib.sha256(app).digest())


def unpack_app(data):
    """(app, (major, minor), (lo, hi)) from an app package; ValueError if it is not one or it is damaged."""
    if not is_app_package(data): raise ValueError('not an OPEN MPK4 app package (footer magic missing)')
    magic, fmt, vmaj, vmin, _, n, lo, hi, digest = PKG_FOOTER.unpack(data[-PKG_FOOTER.size:])
    if fmt != PKG_FORMAT: raise ValueError(f'app package format {fmt} is not supported (this tool reads {PKG_FORMAT})')
    app = data[:-PKG_FOOTER.size]
    if len(app) != n: raise ValueError(f'app package truncated or padded: {len(app)} app bytes, footer says {n}')
    if hashlib.sha256(app).digest() != digest: raise ValueError('app package damaged: sha256 of the app does not match')
    return app, (vmaj, vmin), (lo, hi)
