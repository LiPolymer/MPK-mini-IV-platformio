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
import argparse, hashlib, importlib, os, subprocess, sys, time

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


def ensure_import(module_name, packages):
    """Import `module_name`, installing `packages` into the active Python with pip if missing."""
    try:
        return importlib.import_module(module_name)
    except ImportError:
        print(f'missing dependency ({", ".join(packages)}), installing...')
        try:
            subprocess.check_call([sys.executable, '-m', 'pip', 'install', *packages])
        except (OSError, subprocess.CalledProcessError) as e:
            sys.exit(f'FAILED to install {" ".join(packages)}: {e}\n'
                     f'Install manually with: "{sys.executable}" -m pip install {" ".join(packages)}')
        importlib.invalidate_caches()
        return importlib.import_module(module_name)


class Dev:
    def __init__(self):
        hid = ensure_import('hid', ['hidapi'])
        if not hasattr(hid, 'device'):
            sys.exit('REFUSED: wrong "hid" package installed; run '
                     f'"{sys.executable}" -m pip uninstall -y hid && "{sys.executable}" -m pip install hidapi')
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


def _ks_midi_out_ports():
    """Return [(filter_path, pin_id, port_name)] for host-to-device MIDI pins on MPK mini IV via Windows KS."""
    import ctypes, struct
    from ctypes import wintypes

    class GUID(ctypes.Structure):
        _fields_ = [('Data1', wintypes.DWORD), ('Data2', wintypes.WORD),
                    ('Data3', wintypes.WORD), ('Data4', ctypes.c_ubyte * 8)]

    class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
        _fields_ = [('cbSize', wintypes.DWORD), ('InterfaceClassGuid', GUID),
                    ('Flags', wintypes.DWORD), ('Reserved', ctypes.c_size_t)]

    setupapi = ctypes.WinDLL('setupapi', use_last_error=True)
    setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
    setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
    setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), wintypes.DWORD,
        ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
    setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p,
        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]

    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                    ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]

    kscat_audio = GUID(0x6994AD04, 0x93EF, 0x11D0, (ctypes.c_ubyte * 8)(0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96))
    kspropset_pin = struct.pack('<IHH8B', 0x8C134960, 0x51AD, 0x11CF, 0x87, 0x8A, 0x94, 0xF8, 0x01, 0xC1, 0x00, 0x00)
    ioctl_ks_property = 0x002F0003
    invalid_handle = ctypes.c_void_p(-1).value
    detail_cb = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
    vidpid_tag = f'vid_{L.VID:04x}&pid_{L.PID:04x}'

    dev_info = setupapi.SetupDiGetClassDevsW(ctypes.byref(kscat_audio), None, None, 0x12)  # PRESENT | DEVICEINTERFACE
    if not dev_info or dev_info == invalid_handle:
        return []
    ports = []
    try:
        idx = 0
        while True:
            did = SP_DEVICE_INTERFACE_DATA(cbSize=ctypes.sizeof(SP_DEVICE_INTERFACE_DATA))
            if not setupapi.SetupDiEnumDeviceInterfaces(dev_info, None, ctypes.byref(kscat_audio), idx, ctypes.byref(did)):
                break
            idx += 1
            req = wintypes.DWORD()
            setupapi.SetupDiGetDeviceInterfaceDetailW(dev_info, ctypes.byref(did), None, 0, ctypes.byref(req), None)
            if not req.value: continue
            buf = ctypes.create_string_buffer(req.value)
            ctypes.cast(buf, ctypes.POINTER(wintypes.DWORD))[0] = detail_cb
            if not setupapi.SetupDiGetDeviceInterfaceDetailW(dev_info, ctypes.byref(did), buf, req.value, None, None):
                continue
            path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
            if vidpid_tag not in path.lower(): continue
            h = k32.CreateFileW(path, 0xC0000000, 3, None, 3, 0, None)
            if h == invalid_handle: continue
            try:
                br = wintypes.DWORD()
                prop = ctypes.create_string_buffer(kspropset_pin + struct.pack('<II', 1, 1), 24)  # PIN_CTYPES
                cnt = ctypes.c_uint32()
                if not k32.DeviceIoControl(h, ioctl_ks_property, prop, 24, ctypes.byref(cnt), 4, ctypes.byref(br), None):
                    continue
                for pin in range(cnt.value):
                    kdf = ctypes.create_string_buffer(kspropset_pin + struct.pack('<IIII', 2, 1, pin, 0), 32)  # DATAFLOW
                    df = ctypes.c_uint32()
                    if not k32.DeviceIoControl(h, ioctl_ks_property, kdf, 32, ctypes.byref(df), 4, ctypes.byref(br), None):
                        continue
                    if df.value != 1: continue  # KSPIN_DATAFLOW_IN (host -> device)
                    kn = ctypes.create_string_buffer(kspropset_pin + struct.pack('<IIII', 12, 1, pin, 0), 32)  # PIN_NAME
                    nbuf = ctypes.create_unicode_buffer(128)
                    if k32.DeviceIoControl(h, ioctl_ks_property, kn, 32, nbuf, ctypes.sizeof(nbuf), ctypes.byref(br), None):
                        ports.append((path, pin, nbuf.value))
            finally:
                k32.CloseHandle(h)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(dev_info)
    return ports


def _send_sysex_ks(msg):
    """Send raw SysEx bytes directly via Windows Kernel Streaming (usbaudio.sys), bypassing WinMM/midisrv."""
    import ctypes, struct
    from ctypes import wintypes

    ports = _ks_midi_out_ports()
    name = pick_midi_port([n for _, _, n in ports])
    path, pin_id, _ = next(p for p in ports if p[2] == name)

    class KSIDENTIFIER(ctypes.Structure):
        _fields_ = [('Set', ctypes.c_byte * 16), ('Id', wintypes.DWORD), ('Flags', wintypes.DWORD)]

    class KSPRIORITY(ctypes.Structure):
        _fields_ = [('PriorityClass', wintypes.DWORD), ('PrioritySubClass', wintypes.DWORD)]

    class KSDATAFORMAT(ctypes.Structure):
        _fields_ = [('FormatSize', wintypes.DWORD), ('Flags', wintypes.DWORD),
                    ('SampleSize', wintypes.DWORD), ('Reserved', wintypes.DWORD),
                    ('MajorFormat', ctypes.c_byte * 16), ('SubFormat', ctypes.c_byte * 16),
                    ('Specifier', ctypes.c_byte * 16)]

    class KSPIN_CONNECT_FORMAT(ctypes.Structure):
        _fields_ = [('Interface', KSIDENTIFIER), ('Medium', KSIDENTIFIER), ('PinId', wintypes.DWORD),
                    ('PinToHandle', wintypes.HANDLE), ('Priority', KSPRIORITY), ('Format', KSDATAFORMAT)]

    class KSTIME(ctypes.Structure):
        _fields_ = [('Time', ctypes.c_int64), ('Numerator', wintypes.DWORD), ('Denominator', wintypes.DWORD)]

    hdr_fields = [('Size', wintypes.DWORD), ('TypeSpecificFlags', wintypes.DWORD),
                  ('PresentationTime', KSTIME), ('Duration', ctypes.c_int64),
                  ('FrameExtent', wintypes.DWORD), ('DataUsed', wintypes.DWORD),
                  ('Data', ctypes.c_void_p), ('OptionsFlags', wintypes.DWORD)]
    if ctypes.sizeof(ctypes.c_void_p) == 8:
        hdr_fields.append(('Reserved', wintypes.DWORD))

    class KSSTREAM_HEADER(ctypes.Structure):
        _fields_ = hdr_fields

    def guid_bytes(d1, d2, d3, *d4):
        return (ctypes.c_byte * 16).from_buffer_copy(struct.pack('<IHH8B', d1, d2, d3, *d4))

    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                    ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]

    ksuser = ctypes.WinDLL('ksuser', use_last_error=True)
    ksuser.KsCreatePin.argtypes = [wintypes.HANDLE, ctypes.POINTER(KSPIN_CONNECT_FORMAT),
                                   wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    ksuser.KsCreatePin.restype = wintypes.DWORD

    invalid_handle = ctypes.c_void_p(-1).value
    ioctl_ks_property = 0x002F0003
    ioctl_ks_write_stream = 0x002F8013
    kspropset_conn = struct.pack('<IHH8B', 0x1D58C920, 0xAC9B, 0x11CF, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00)

    conn = KSPIN_CONNECT_FORMAT()
    conn.Interface.Set = guid_bytes(0x1A8766A0, 0x62CE, 0x11CF, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00)
    conn.Medium.Set = guid_bytes(0x4747B320, 0x62CE, 0x11CF, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00)
    conn.PinId = pin_id
    conn.Priority.PriorityClass = 0x40000000  # KSPRIORITY_NORMAL
    conn.Priority.PrioritySubClass = 1
    conn.Format.FormatSize = ctypes.sizeof(KSDATAFORMAT)
    conn.Format.MajorFormat = guid_bytes(0xE725D360, 0x62CC, 0x11CF, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00)
    conn.Format.SubFormat = guid_bytes(0x1D262760, 0xE957, 0x11CF, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00)
    conn.Format.Specifier = guid_bytes(0x0F6417D6, 0xC318, 0x11D0, 0xA4, 0x3F, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96)

    filter_h = k32.CreateFileW(path, 0xC0000000, 3, None, 3, 0, None)
    if filter_h == invalid_handle:
        raise OSError(f'CreateFileW({path}) failed: winerror {ctypes.get_last_error()}')
    pin_h = wintypes.HANDLE(invalid_handle)
    try:
        rc = ksuser.KsCreatePin(filter_h, ctypes.byref(conn), 0x40000000, ctypes.byref(pin_h))  # GENERIC_WRITE
        if rc != 0 or pin_h.value == invalid_handle:
            raise OSError(f'KsCreatePin({name!r}, pin {pin_id}) failed: error {rc} '
                          '(close any DAW/MIDI app using the port and retry)')
        br = wintypes.DWORD()
        prop_state = ctypes.create_string_buffer(kspropset_conn + struct.pack('<II', 0, 2), 24)  # CONNECTION_STATE SET
        for st in (1, 2, 3):  # ACQUIRE -> PAUSE -> RUN
            val = ctypes.c_uint32(st)
            if not k32.DeviceIoControl(pin_h, ioctl_ks_property, prop_state, 24, ctypes.byref(val), 4, ctypes.byref(br), None):
                raise OSError(f'KSSTATE({st}) failed: winerror {ctypes.get_last_error()}')
        padded = msg + bytes((-len(msg)) & 3)
        payload = ctypes.create_string_buffer(struct.pack('<II', 0, len(msg)) + padded, 8 + len(padded))
        hdr = KSSTREAM_HEADER(Size=ctypes.sizeof(KSSTREAM_HEADER),
                              PresentationTime=KSTIME(0, 1, 1),
                              FrameExtent=len(payload), DataUsed=len(payload),
                              Data=ctypes.addressof(payload))
        if not k32.DeviceIoControl(pin_h, ioctl_ks_write_stream, None, 0,
                                   ctypes.byref(hdr), ctypes.sizeof(hdr), ctypes.byref(br), None):
            raise OSError(f'IOCTL_KS_WRITE_STREAM failed: winerror {ctypes.get_last_error()}')
    finally:
        if pin_h.value != invalid_handle:
            val = ctypes.c_uint32(0)  # KSSTATE_STOP
            br = wintypes.DWORD()
            prop_state = ctypes.create_string_buffer(kspropset_conn + struct.pack('<II', 0, 2), 24)
            k32.DeviceIoControl(pin_h, ioctl_ks_property, prop_state, 24, ctypes.byref(val), 4, ctypes.byref(br), None)
            k32.CloseHandle(pin_h)
        k32.CloseHandle(filter_h)
    return name


def enter_update(send):
    msg = L.SYSEX_ENTER_UPDATE
    print('SysEx:', msg.hex(' ').upper(), '(stock firmware resets into the HID bootloader)')
    if not send:
        print('not sent: add --send'); return
    if sys.platform == 'win32':
        name = _send_sysex_ks(msg)
    else:
        mido = ensure_import('mido', ['mido', 'python-rtmidi'])
        ensure_import('rtmidi', ['python-rtmidi'])
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
