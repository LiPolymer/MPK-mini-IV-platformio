#!/usr/bin/env python3
"""Pack an OPEN MPK4 app binary into a full 256 KiB image the Akai MPK mini IV bootloader accepts.

  python3 tools/mkimage.py APP.bin OUT.bin [--version 02.00] [--storage LO HI] [--data DATA.bin] [--stock STOCK.bin]
  python3 tools/mkimage.py APP.bin OUT.bin --version 02.00 --storage LO HI --app-package
  python3 tools/mkimage.py PACKAGE.bin OUT.bin                # an app package: version and storage come from it
  python3 tools/mkimage.py --find-stock                       # exit 0 found, 1 none, 2 a file that is not genuine

--app-package writes the app plus a footer naming its version and storage region (mpk4_layout.pack_app): a build that
holds no Akai bytes and needs no stock image. Everything else assembles the full image from the stock image, found by
mpk4_layout.find_stock() (backup/firmware/ in this checkout, else the cache tools/fetch_stock.py fills) unless --stock
names one; it must be the pinned genuine image.

  0x00000-0x0BFFF  bootloader + staged copy, byte-for-byte from the stock image (sha256-pinned)
  0x0C000-         app (linked at 0x0800C000, vector table first)
  --storage LO HI  the firmware's storage region (its linker symbols): checked against mpk4_layout.storage_errors(),
                   the app must end below it, and it ships erased (0xFF)
  0x38FF0-0x38FFF  packet holding the app-valid bytes 0x38FFE/F, taken from stock (must not be 0xFF, or the
                   bootloader never starts the app)
  0x39000-0x3EFFF  app data pages: --data, else erased (0xFF)
  0x3F000-0x3F7FF  stock (the bootloader never writes this page; kept so a stock repack is byte-exact)
  0x3F800-0x3FFFF  stock settings page with the trailer: VID PID FF layout VER(2) ... BE sum16

Port of the OPENMPK3 mkimage.py; the layout and every gate live in mpk4_layout.py.
"""
import argparse, hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mpk4_layout as L  # noqa: E402

DATA = (L.APP_VALID[1] + 1, L.PROTECTED[0])   # 0x39000-0x3EFFF


def build(stock, app, version, data=None, storage=None):
    errs = L.validate(stock)
    if errs: raise ValueError('stock image rejected: ' + '; '.join(errs))
    app_max = (L.APP_VALID[0] & ~(L.PKT - 1)) - L.APP     # keep the packet holding the app-valid bytes free
    if storage is not None:
        errs = L.storage_errors(storage[0], storage[1], L.APP + len(app))
        if errs: raise ValueError('storage region rejected: ' + '; '.join(errs))
        if storage[0] > L.APP: app_max = min(app_max, storage[0] - L.APP)
        if data is not None and DATA[0] < storage[1] and storage[0] < DATA[0] + len(data):
            raise ValueError('data would land in the storage region (it must ship erased)')
    if len(app) > app_max: raise ValueError(f'app too big: {len(app)} > {app_max}')
    sp, rv = struct.unpack_from('<II', app, 0)
    if not (L.SRAM[0] < sp <= L.SRAM[1] and rv & 1 and L.BASE + L.APP < (rv & ~1) < L.BASE + L.APP + len(app)):
        raise ValueError(f'app vector table looks wrong: sp={sp:#x} reset={rv:#x} (link at {L.BASE + L.APP:#x})')
    if data is not None and len(data) > DATA[1] - DATA[0]:
        raise ValueError(f'data too big: {len(data)} > {DATA[1] - DATA[0]}')
    img = bytearray(b'\xff' * L.IMG_SIZE)
    img[:L.STAGE[1]] = stock[:L.STAGE[1]]                        # Akai bootloader + staged copy, untouched
    img[L.APP:L.APP + len(app)] = app
    mk = L.APP_VALID[0] & ~(L.PKT - 1)
    img[mk:mk + L.PKT] = stock[mk:mk + L.PKT]                    # packet holding the app-valid bytes
    if data is not None: img[DATA[0]:DATA[0] + len(data)] = data
    img[L.PROTECTED[0]:] = stock[L.PROTECTED[0]:]                # settings backup (never flashed) + settings page
    img[L.VERSION[0]:L.VERSION[1] + 1] = bytes(version)
    img[L.CHECKSUM:] = struct.pack('>H', L.sum16(img[:L.CHECKSUM]))
    errs = L.validate(bytes(img), allow_layout=stock[L.LAYOUT_BYTE])
    if errs: raise ValueError('built image failed the safety gates: ' + '; '.join(errs))
    return bytes(img)


def parse_version(s):
    maj, mnr = (int(x, 16) for x in s.split('.'))
    if not (0 <= maj < 0xFF and 0 <= mnr < 0xFF): raise ValueError('version bytes must not be 0xFF')
    return maj, mnr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('app', nargs='?'); ap.add_argument('out', nargs='?')
    ap.add_argument('--stock', help='the stock image (default: mpk4_layout.find_stock())')
    ap.add_argument('--version', help='BCD major.minor shown by the bootloader, e.g. 02.00 (default 02.00)')
    ap.add_argument('--data', help='optional contents for the app data pages 0x39000-0x3EFFF')
    ap.add_argument('--storage', nargs=2, metavar=('LO', 'HI'), type=lambda v: int(v, 16) - L.BASE,
                    help='the firmware storage region, absolute hex addresses (linker symbols __storage_start/end)')
    ap.add_argument('--app-package', action='store_true', help='write an app package (no stock image needed)')
    ap.add_argument('--find-stock', action='store_true', help='report the stock image the lookup finds and exit')
    a = ap.parse_args()
    try:
        if a.find_stock:
            found = L.find_stock(a.stock)
            if found is None:
                print('no stock image: ' + ', '.join(L.stock_candidates())); return 1
            print(f'stock image OK: {found[0]}'); return 0
        if not (a.app and a.out): ap.error('APP and OUT are required')
        app = open(a.app, 'rb').read()
        version = parse_version(a.version) if a.version else None
        if L.is_app_package(app):
            if a.app_package or version or a.storage: ap.error('an app package already names its version and storage')
            app, version, a.storage = L.unpack_app(app)
        version = version or parse_version('02.00')
        if a.app_package:
            if a.storage is None: ap.error('--app-package needs --storage (the full image must ship the storage erased)')
            if a.data: ap.error('--data goes into a full image, not an app package')
            errs = L.storage_errors(a.storage[0], a.storage[1], L.APP + len(app))
            if errs: raise ValueError('storage region rejected: ' + '; '.join(errs))
            pkg = L.pack_app(app, version, a.storage)
            open(a.out, 'wb').write(pkg)
            print(f'{a.out}: app package, app {len(app)} bytes, version {version[0]:02x}.{version[1]:02x}, storage '
                  f'{a.storage[0]:#x}-{a.storage[1]:#x}, sha256 {hashlib.sha256(pkg).hexdigest()[:16]}')
            return 0
        _, stock = L.require_stock(a.stock)
        img = build(stock, app, version, open(a.data, 'rb').read() if a.data else None, a.storage)
    except (ValueError, L.StockError) as e:
        sys.exit(f'REFUSED: {e}')
    open(a.out, 'wb').write(img)
    print(f'{a.out}: app {len(app)} bytes, version {img[L.VERSION[0]]:02x}.{img[L.VERSION[1]]:02x}, '
          f'checksum {L.sum16(img[:L.CHECKSUM]):#06x}, sha256 {hashlib.sha256(img).hexdigest()[:16]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
