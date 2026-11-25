#!/usr/bin/env python3
"""OPEN MPK4 flasher for the Akai MPK mini IV (AT32F405, USB 09E8:005D) via Akai's HID bootloader.

  python3 tools/mpk4_flash.py flash IMAGE.bin            # DRY RUN (default): gates + write plan, no USB access
  python3 tools/mpk4_flash.py flash APP_PACKAGE.bin      # the same for an app package: the full image is assembled
                                                         # in memory from your stock image (tools/fetch_stock.py)
  python3 tools/mpk4_flash.py flash stock                # the same for Akai's stock image itself (back to stock)
  python3 tools/mpk4_flash.py probe                      # read-only: C4 asks which mode the device is in
  python3 tools/mpk4_flash.py enter-update               # prints the SysEx; add --send to send it
  python3 tools/mpk4_flash.py flash IMAGE.bin --write    # really flash: enters update mode itself

Update mode: unplug, hold SHIFT + UNDO, plug USB in (screen shows "Upgrade"), or send the SysEx
F0 47 00 5D 74 00 00 F7 to the running stock firmware (it resets into the bootloader).
Protocol, safety analysis and the rules applied here: docs/hardware/update-protocol.md.
Port of the OPENMPK3 mpk_flash.py; layout and gates live in mpk4_layout.py.
"""
import argparse, hashlib, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mpk4_layout as L  # noqa: E402
import mkimage  # noqa: E402

TIMEOUT_MS = 2000          # the Akai updater uses 2000 ms for every command
ERASE_TIMEOUT_MS = 30000   # C1 erases 114 sectors before it answers


def load_image(path, stock):
    """The full image to flash for `path`: a full image, an app package, or 'stock' (the stock image itself)."""
    return load_image_bytes(None if path == 'stock' else open(path, 'rb').read(), stock)


def load_image_bytes(data, stock):
    """The full image to flash: `data` itself, the image assembled from `data` when it is an app package (with the
    stock image), or the stock image when data is None. Every image passes the same gates, with the stock layout byte."""
    img = stock if data is None else data
    if data is not None and L.is_app_package(data):
        try:
            app, version, storage = L.unpack_app(data)
            img = mkimage.build(stock, app, version, storage=storage)
        except ValueError as e:
            sys.exit(f'REFUSED: {e}')
    errs = L.validate(img, allow_layout=stock[L.LAYOUT_BYTE])
    if errs: sys.exit('REFUSED:\n  ' + '\n  '.join(errs))
    return img


def describe_plan(img):
    plan = L.flash_plan(img)
    body = [o for o in plan if o < L.LAST_PAGE]
    return (f'{len(plan)} packets: {len(body) - 1} programmed directly (0x{min(body):05X}-0x{max(body):05X}, '
            f'erased 0xFF packets skipped), {len([o for o in plan if o >= L.LAST_PAGE])} to the staged last '
            f'page (commit at 0x{L.TAIL_PKT:05X}), app-valid packet 0x{plan[-1]:05X} last')


def select_hid(devices):
    """Pick the vendor HID interface of an MPK mini IV from hid.enumerate() output; refuse anything else."""
    akai = [d for d in devices if d.get('vendor_id') == L.VID]
    ours = [d for d in akai if d.get('product_id') == L.PID]
    if not ours:
        other = sorted({f"{d.get('product_id'):04x}" for d in akai})
        sys.exit(f'REFUSED: no MPK mini IV (PID {L.PID:04x}) found' +
                 (f'; other Akai devices present (PID {", ".join(other)}) are not supported' if other else ''))
    hid_if = [d for d in ours if d.get('usage_page') == 0xFFA0 or d.get('interface_number') == 0]
    if not hid_if: sys.exit('REFUSED: MPK mini IV found but not its vendor HID interface (usage page 0xFFA0)')
    if len({d.get('serial_number') or d.get('path') for d in hid_if}) > 1:
        sys.exit('REFUSED: more than one MPK mini IV connected; plug in only the one to flash')
    return hid_if[0]


class Dev:
    def __init__(self):
        import hid  # pip install hidapi
        info = select_hid(hid.enumerate(L.VID, 0))   # 0 = every PID of this vendor, so wrong PIDs are seen
        self.h = hid.device(); self.h.open_path(info['path'])
        self.drain()

    def drain(self):
        self.h.set_nonblocking(1)
        try:
            for _ in range(512):
                if not self.h.read(64): break
        finally:
            self.h.set_nonblocking(0)

    def xfer(self, pkt, timeout_ms=TIMEOUT_MS):
        assert len(pkt) == 32
        if self.h.write(b'\x00' + pkt) < 0: raise IOError('HID write failed')
        r = bytes(self.h.read(32, timeout_ms=timeout_ms))
        if len(r) < 3: raise TimeoutError('no reply from device')
        if r[:2] != b'UI': raise IOError(f'unexpected reply {r.hex()}')
        return r


def probe(d):
    r = d.xfer(L.command(0xC4))
    if r[2] == 0xE4: return 'app', None
    if r[2] == 0xF4: return 'bootloader', f'{r[3]:02X}.{r[4]:02X}'
    return 'unknown', r.hex()


def flash(path, write, stock_path=None):
    try:
        _, stock = L.require_stock(stock_path)
    except L.StockError as e:
        sys.exit(f'REFUSED: {e}')
    img = load_image(path, stock)
    print(f'image OK: {path}  version {img[L.VERSION[0]]:02X}.{img[L.VERSION[1]]:02X}  '
          f'bootloader {L.KNOWN_STOCK_BOOT[L.boot_hash(img)]}  sha256 {hashlib.sha256(img).hexdigest()[:16]}')
    print('plan: C1 erase 0x06000-0x3EFFF; ' + describe_plan(img) + '; C4 version check; C3')
    if not write:
        print('dry run only: nothing was sent. Add --write to flash.'); return
    d = Dev(); mode, ver = probe(d)
    if mode == 'app':
        d.h.close()
        enter_update(send=True)
        d = wait_for_bootloader()
        mode, ver = probe(d)
    if mode != 'bootloader':
        sys.exit(f'device did not reach update mode ({mode}). Unplug, hold SHIFT + UNDO, plug back in, retry.')
    print(f'bootloader ready (installed firmware v{ver})')
    t0 = time.time()
    r = d.xfer(L.command(0xC1), ERASE_TIMEOUT_MS)
    if r[2] != 0xF1: sys.exit(f'erase failed: {r.hex()}')
    print(f'erased in {time.time() - t0:.1f}s')
    plan = L.flash_plan(img)
    for i, off in enumerate(plan):
        r = d.xfer(L.c2_packet(off, img[off:off + L.PKT]))
        if r[2] != 0xF2:
            sys.exit(f'\nwrite failed at {off:#07x}: {r.hex()}\nThe app-valid bytes are not written yet, so the '
                     'device stays in update mode: replug and re-run.')
        if i % 512 == 0 or i == len(plan) - 1:
            print(f'\r  writing {100 * (i + 1) // len(plan):3d}%  ({off:#07x})', end='', flush=True)
    print()
    mode, ver = probe(d)
    want = f'{img[L.VERSION[0]]:02X}.{img[L.VERSION[1]]:02X}'
    if ver != want: sys.exit(f'version readback mismatch: got {ver}, want {want}')
    r = d.xfer(L.command(0xC3))
    if r[2] != 0xF3: sys.exit(f'finish failed: {r.hex()}')
    print(f'done in {time.time() - t0:.1f}s - the bootloader now starts v{ver}')


def wait_for_bootloader(timeout_s=120):
    """After the reset the unit re-enumerates as HID only; return it once it answers C4 with F4.

    The bootloader is a new USB identity, so macOS may hold it behind its "Allow accessory to connect?" prompt
    until the user clicks Allow: the wait is long enough for that, and the prompt is named while waiting."""
    print('waiting for the bootloader (if macOS asks "Allow accessory to connect?", click Allow)')
    t, last = time.time(), 'not seen yet'
    while time.time() - t < timeout_s:
        time.sleep(0.2)
        try:
            d = Dev()
            if probe(d)[0] == 'bootloader': return d
            d.h.close()
            last = 'device answered but is not in the bootloader'
        except (SystemExit, OSError, TimeoutError) as e:
            last = str(e) or type(e).__name__
    sys.exit(f'no bootloader after the reset ({last}). Unplug, hold SHIFT + UNDO, plug back in, retry.')


def enter_update(send):
    msg = L.SYSEX_ENTER_UPDATE
    print('SysEx:', msg.hex(' ').upper(), '(stock firmware resets into the HID bootloader)')
    if not send:
        print('not sent: add --send'); return
    import mido  # pip install mido python-rtmidi
    name = pick_midi_port(mido.get_output_names())
    with mido.open_output(name) as o:
        o.send(mido.Message('sysex', data=list(msg[1:-1])))
    print('sent to', name)


def pick_midi_port(names):
    """The unit's main MIDI port (stock or OPEN MPK4 firmware) among the host's port names; refuse unless exactly one."""
    unit = [n for n in names if any(p in n for p in L.PRODUCT_NAMES)]
    main = [n for n in unit if 'MIDI Port' in n] or unit
    if len(main) != 1:
        sys.exit(f'REFUSED: need exactly one MIDI port of an MPK mini IV ({" / ".join(L.PRODUCT_NAMES)}), found {main}')
    return main[0]


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    sp.add_parser('probe')
    e = sp.add_parser('enter-update'); e.add_argument('--send', action='store_true')
    f = sp.add_parser('flash'); f.add_argument('image', help='full image, app package, or "stock"'); f.add_argument('--write', action='store_true')
    f.add_argument('--stock', help='path to a verified Akai reference image')
    a = ap.parse_args()
    if a.cmd == 'probe':
        mode, ver = probe(Dev()); print(mode, ver or '')
    elif a.cmd == 'enter-update':
        enter_update(a.send)
    else:
        flash(a.image, a.write, a.stock)
