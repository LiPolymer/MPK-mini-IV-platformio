#!/usr/bin/env python3
"""Get Akai's stock MPK mini IV firmware image (v1.41) onto this computer, verified. Touches no device.

  python3 tools/fetch_stock.py                   # download only the firmware piece of Akai's public installer
  python3 tools/fetch_stock.py --from-file FILE  # use a file you already have (no network)
  python3 tools/fetch_stock.py --check           # say whether a verified stock image is already here

OPEN MPK4 images keep Akai's bootloader, its staged copy and the settings page byte for byte, so they are assembled
on your computer from Akai's own image; this project never distributes it. Akai publishes it inside the free Studio
Instrument Collection installer (linked from akaipro.com/mpk-download, no account needed). That installer is an 8 GB
xar archive, so this tool reads only its table of contents and the ~80 KB firmware member with HTTP range requests,
checks the archive's own sha1 of each, unpacks the gzip cpio inside and keeps the one file whose sha256 is the pinned
stock image (mpk4_layout.KNOWN_STOCK_IMAGE; its bootloader must also be a known genuine one).

FILE may be the installer .pkg, its firmware member (FirmwareContent.pkg/Payload: gzip cpio), a cpio archive, or the
stock .bin itself. The verified image is stored as mpk4_layout.cache_dir()/MPKminiIV_v1.41_stock.bin (~/.openmpk4,
or $OPENMPK4_HOME), where mkimage, the flasher and the tests find it.
"""
import argparse, gzip, hashlib, os, ssl, struct, subprocess, sys, tempfile, urllib.error, urllib.request, zlib
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mpk4_layout as L  # noqa: E402

INSTALLER_URL = ('https://cdn.inmusicbrands.com/Software/SIC/'
                 'Studio%20Instrument%20Collection%201.2.0.9-release%20Installer.pkg')
FIRMWARE_MEMBER = 'FirmwareContent.pkg'   # the installer component holding the firmware (its name ends with this)
XAR_HEADER = struct.Struct('>4sHHQQI')    # magic, header size, version, TOC length compressed / uncompressed, cksum
MAX_TOC, MAX_MEMBER = 16 << 20, 64 << 20  # refuse anything far larger than the real pieces (~150 KB, ~80 KB)


class FetchError(Exception):
    pass


# ------------------------------------------------------------------ readers: the installer over HTTP or on disk
def http_reader(url):
    """Range reads over HTTPS. Python builds that ship without root certificates (python.org's macOS installer
    until its "Install Certificates" step is run) fail certificate verification; then the same request goes
    through the system's curl, which uses the operating system's trust store. Either way the bytes are checked
    afterwards against the installer's own sha1 and the pinned sha256, so the transport is not what we trust."""
    use_curl = []

    def read_urllib(offset, length):
        req = urllib.request.Request(url, headers={'Range': f'bytes={offset}-{offset + length - 1}',
                                                   'User-Agent': 'openmpk4-fetch-stock'})
        with urllib.request.urlopen(req, timeout=60) as r:
            if r.status != 206: raise FetchError(f'the server ignored the range request (HTTP {r.status})')
            return r.read(length + 1)

    def read_curl(offset, length):
        try:
            p = subprocess.run(['curl', '-fsSL', '--max-time', '60', '-A', 'openmpk4-fetch-stock',
                                '-r', f'{offset}-{offset + length - 1}', url], capture_output=True, check=False)
        except FileNotFoundError:
            raise FetchError('certificate verification failed and curl is not installed: install your Python\'s '
                             'certificates (macOS: run "Install Certificates.command" in the Python folder)')
        if p.returncode: raise FetchError(f'curl failed: {p.stderr.decode(errors="replace").strip()}')
        return p.stdout

    def read(offset, length):
        if use_curl: data = read_curl(offset, length)
        else:
            try: data = read_urllib(offset, length)
            except urllib.error.URLError as e:
                if not isinstance(e.reason, ssl.SSLCertVerificationError): raise
                use_curl.append(True)
                data = read_curl(offset, length)
        if len(data) != length: raise FetchError(f'asked for {length} bytes at {offset}, got {len(data)}')
        return data
    return read


def bytes_reader(blob):
    def read(offset, length):
        if offset + length > len(blob): raise FetchError('the installer file is truncated')
        return blob[offset:offset + length]
    return read


# ------------------------------------------------------------------ xar (the macOS .pkg container)
def xar_member(read, suffix=FIRMWARE_MEMBER, name='Payload'):
    """The decoded bytes of member `name` inside the directory whose name ends with `suffix`, read through
    read(offset, length). The TOC's own sha1 and the member's archived and extracted sha1 are checked."""
    magic, hsize, _, toc_c, toc_u, cksum = XAR_HEADER.unpack(read(0, XAR_HEADER.size))
    if magic != b'xar!': raise FetchError('not a xar archive (.pkg installer)')
    if not 0 < toc_c <= MAX_TOC or not 0 < toc_u <= MAX_TOC: raise FetchError('xar table of contents size is implausible')
    toc_z = read(hsize, toc_c)
    try:
        toc = ET.fromstring(zlib.decompress(toc_z))
    except (zlib.error, ET.ParseError) as e:
        raise FetchError(f'xar table of contents unreadable: {e}')
    heap = hsize + toc_c
    ck = toc.find('toc/checksum')
    if cksum != 1 or ck is None or ck.get('style') != 'sha1':
        raise FetchError('xar table of contents is not sha1-checksummed')
    if read(heap + int(ck.findtext('offset')), int(ck.findtext('size'))) != hashlib.sha1(toc_z).digest():
        raise FetchError('xar table of contents sha1 mismatch')
    dirs = [d for d in toc.iter('file') if d.findtext('type') == 'directory' and (d.findtext('name') or '').endswith(suffix)]
    hits = [f for d in dirs for f in d.findall('file') if f.findtext('name') == name and f.find('data') is not None]
    if len(hits) != 1:
        raise FetchError(f'expected one {name} in a *{suffix} component, found {len(hits)} '
                         f'(components: {", ".join(d.findtext("name") for d in dirs) or "none"})')
    data = hits[0].find('data')
    off, length, size = (int(data.findtext(k)) for k in ('offset', 'length', 'size'))
    if not 0 < length <= MAX_MEMBER or not 0 < size <= MAX_MEMBER: raise FetchError(f'{name} size is implausible')
    raw = read(heap + off, length)
    sums = {k: data.find(k) for k in ('archived-checksum', 'extracted-checksum')}
    for k, el in sums.items():
        if el is None or el.get('style') != 'sha1': raise FetchError(f'{name}: no sha1 {k} in the table of contents')
    if hashlib.sha1(raw).hexdigest() != sums['archived-checksum'].text.strip():
        raise FetchError(f'{name}: archived sha1 mismatch (download damaged)')
    enc = data.find('encoding').get('style') if data.find('encoding') is not None else 'application/octet-stream'
    if enc == 'application/x-gzip': out = zlib.decompress(raw)
    elif enc == 'application/octet-stream': out = raw
    else: raise FetchError(f'{name}: encoding {enc} not supported')
    if hashlib.sha1(out).hexdigest() != sums['extracted-checksum'].text.strip():
        raise FetchError(f'{name}: extracted sha1 mismatch')
    return out


# ------------------------------------------------------------------ cpio (the component's Payload)
def cpio_files(blob):
    """{name: bytes} of the regular files in an odc (070707) or newc (070701/070702) cpio archive."""
    files, o = {}, 0
    while o < len(blob):
        magic = blob[o:o + 6]
        if magic == b'070707':
            h = blob[o:o + 76]
            mode, namesize, filesize = int(h[18:24], 8), int(h[59:65], 8), int(h[65:76], 8)
            name_at, pad = o + 76, lambda n: n
        elif magic in (b'070701', b'070702'):
            h = blob[o:o + 110]
            mode, filesize, namesize = int(h[14:22], 16), int(h[54:62], 16), int(h[94:102], 16)
            name_at, pad = o + 110, lambda n: (n + 3) & ~3
        else:
            raise FetchError(f'not a cpio archive (bad header at {o})')
        name = blob[name_at:name_at + namesize - 1].decode('utf8', 'replace')
        data_at = pad(name_at + namesize)
        if name == 'TRAILER!!!': break
        if data_at + filesize > len(blob): raise FetchError('cpio archive truncated')
        if mode & 0o170000 == 0o100000: files[name] = blob[data_at:data_at + filesize]
        o = pad(data_at + filesize)
    return files


# ------------------------------------------------------------------ any of the above -> the verified stock image
def stock_from(blob, depth=0):
    """The verified stock image found in `blob` (installer, gzip, cpio or the image itself)."""
    if depth > 4: raise FetchError('nested too deep')
    if blob[:4] == b'xar!': return stock_from(xar_member(bytes_reader(blob)), depth + 1)
    if blob[:2] == b'\x1f\x8b': return stock_from(gzip.decompress(blob), depth + 1)
    if blob[:5] == b'07070':
        found = [d for d in cpio_files(blob).values() if hashlib.sha256(d).hexdigest() in L.KNOWN_STOCK_IMAGE]
        if not found: raise FetchError('no file in the archive is the pinned stock image')
        blob = found[0]
    errs = L.stock_errors(blob)
    if errs: raise FetchError('not the genuine stock image: ' + '; '.join(errs))
    return blob


def store(img, folder=None):
    """Write the verified image into the cache (atomically) and return its path."""
    errs = L.stock_errors(img)
    if errs: raise FetchError('refusing to store: ' + '; '.join(errs))
    folder = folder or L.cache_dir()
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, L.STOCK_NAME)
    fd, tmp = tempfile.mkstemp(dir=folder, prefix='.stock-')
    with os.fdopen(fd, 'wb') as f: f.write(img)
    os.replace(tmp, path)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--from-file', metavar='FILE', help='installer .pkg, its firmware Payload, a cpio, or the stock .bin')
    ap.add_argument('--url', default=INSTALLER_URL, help='the installer to range-read (default: Akai\'s, see above)')
    ap.add_argument('--check', action='store_true', help='only report whether a verified stock image is present')
    a = ap.parse_args()
    try:
        if a.check:
            found = L.find_stock()
            print(f'stock image OK: {found[0]}' if found else 'no stock image yet: run python3 tools/fetch_stock.py')
            return 0 if found else 1
        if a.from_file:
            with open(a.from_file, 'rb') as f: img = stock_from(f.read())
        else:
            print(f'reading the firmware piece of {a.url} (range requests: table of contents + ~80 KB)')
            img = stock_from(xar_member(http_reader(a.url)))
        path = store(img)
    except (FetchError, L.StockError, OSError, ValueError) as e:
        sys.exit(f'FAILED: {e}')
    print(f'stock image OK ({L.KNOWN_STOCK_IMAGE[hashlib.sha256(img).hexdigest()]}, sha256 '
          f'{hashlib.sha256(img).hexdigest()[:16]}...): {path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
