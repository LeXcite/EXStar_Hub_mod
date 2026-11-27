#!/usr/bin/env python3
"""Read the EXStar Hub installer payload without running it.

Why this exists
---------------
The setup is an NSIS build the vendor forked: the archive after the first
header is not a 7-Zip/zip/cab container (no archiver on the market opens it,
and innounp does not recognize it either), so an update cannot be inspected
with off-the-shelf tools.  Everything needed is derived from the bytes:

  * ``firstheader`` at a 512-byte aligned offset: flags, 0xDEADBEEF,
    "Null" "soft" "Inst", length_of_header, length_of all following data.
  * every payload object is a record:  a little-endian u64 whose bit 63 means
    "deflated"; the remaining bits are the stored length (deflated records
    declare 3 bytes more than their content, because each compressed block is
    preceded by a 3-byte length), followed by that many bytes.  A u64 of 0 is
    an 8-byte separator.  The next record is always at ``frame + 8 + value``.
  * a deflated record is a sequence of ``[u24 length][raw-deflate]`` blocks,
    written with Z_FULL_FLUSH, each holding at most 1 MiB of plaintext.  A
    record is therefore inflatable on its own: random access to one file
    without touching the other 1.6 GB.
  * a record is one file.  Files bigger than 1 MiB are several blocks in one
    record; the plaintext length is the sum of the block outputs.
  * the header stream (the first record) inflates to exactly length_of_header
    and carries the block table: pages, sections, entries, strings, lang
    tables.  Instruction entries are 36 bytes: opcode, then 8 operands.  The
    file instruction (opcode 20 in this build) keeps its destination name as a
    character index into the UTF-16 string block (byte offset =
    strings + 2 * index) and the record's frame offset relative to the start
    of the data area in the next operand.

Validation, not guesswork
-------------------------
``verify`` compares every record against the installed application: identical
size plus identical sha256.  Against a 2.1.0-10 installation 2992 of the 3020
installed files matched a record (2594 MB); the 28 that did not match are files
the mod set itself puts there.  A reader that reproduces an installed tree
byte for byte is trusted for a tree that is not installed yet.

Usage
-----
    python tools\\nsis_payload.py index   <setup.exe> <index.json>
    python tools\\nsis_payload.py list    <index.json> [substring]
    python tools\\nsis_payload.py extract <index.json> <name> <outdir>
    python tools\\nsis_payload.py verify  <index.json> <installed_dir>

`index` walks the whole payload and hashes it (about 30 s for 1.6 GB); the
other commands work from the resulting index.  Nothing is written except the
index and the files that are explicitly extracted.
"""

import hashlib
import json
import os
import struct
import sys
import zlib

MAGIC = (0xDEADBEEF, 0x6C6C754E, 0x74666F73, 0x74736E49)   # beef, "NullsoftInst"
BLOCK_NAMES = ['pages', 'sections', 'entries', 'strings',
               'langtables', 'ctlcolors', 'bgfont', 'data']
OP_FILE = 20            # File "source" "destination" in this NSIS build
ENTRY_SIZE = 36
MI = 1 << 20


def find_first_header(f):
    """The firstheader record: NSIS magic at a sector boundary."""
    size = f.seek(0, 2)
    for off in range(0, size - 28, 512):
        f.seek(off)
        d = struct.unpack('<7I', f.read(28))
        if (d[1], d[2], d[3], d[4]) == MAGIC:
            return off, d[5], d[6]
    raise SystemExit('no NSIS firstheader found - not a setup we understand')


def read_header(f, fhoff, hdr_len):
    """Inflate the header stream and find where the file records begin.

    The header is itself a record of the same framing, so its u64 sits 11 bytes
    in front of the first compressed block (8 bytes of u64 plus the 3-byte block
    length) and declares where the data area starts.
    """
    f.seek(fhoff + 28)
    probe = f.read(200000)
    for skip in range(0, 64):
        try:
            dd = zlib.decompressobj(-15)
            out = dd.decompress(probe[skip:])
        except Exception:
            continue
        if len(out) == hdr_len:
            hstart = fhoff + 28 + skip
            f.seek(hstart - 11)
            hv, = struct.unpack('<Q', f.read(8))
            if not (hv >> 63):
                raise SystemExit('header record is not marked compressed')
            return out, hstart - 11 + 8 + (hv & ~(1 << 63))
    raise SystemExit('header block does not inflate to length_of_header')


def block_table(hdr):
    blocks = {}
    for i, name in enumerate(BLOCK_NAMES):
        off, num = struct.unpack_from('<2i', hdr, 4 + i * 8)
        blocks[name] = (off, num)
    return blocks


def strings_at(hdr, blocks):
    """Decode a string from an entry operand (a UTF-16 character index)."""
    base, end = blocks['strings'][0], blocks['langtables'][0]

    def get(idx):
        if idx is None or idx < 0:
            return None
        p = base + 2 * idx
        if p >= end:
            return None
        out = []
        while p + 1 < end:
            u = hdr[p:p + 2]
            if u == b'\x00\x00':
                break
            out.append(u)
            p += 2
            if len(out) > 1024:
                break
        return b''.join(out).decode('utf-16-le', 'replace')
    return get


def walk(f, data_start, data_end, hash_it=True, report=None):
    """Every record of the data area, in payload order."""
    recs = []
    pos = data_start
    i = 0
    while pos < data_end - 8:
        f.seek(pos)
        raw = f.read(8)
        if len(raw) < 8:
            break
        v, = struct.unpack('<Q', raw)
        if v == 0:
            pos += 8
            continue
        deflated = bool(v >> 63)
        n = v & ~(1 << 63)
        if n <= 0 or n > (1 << 32) or pos + 8 + n > data_end + 16:
            # the payload's own trailer: not a record, and the walk is over
            print('  not a record at %d: u64=0x%x - stopping here' % (pos, v))
            break
        rec = {'i': i, 'frame': pos, 'comp': 1 if deflated else 0, 'compd': n}
        if deflated:
            p, end = pos + 8, pos + 8 + n - 3
            plain = 0
            groups = 0
            h = hashlib.sha256() if hash_it else None
            while p < end:
                f.seek(p)
                g = f.read(3)
                if len(g) < 3:
                    raise SystemExit('record %d: truncated block header at %d' % (i, p))
                glen = g[0] | (g[1] << 8) | (g[2] << 16)
                f.seek(p + 3)
                data = zlib.decompressobj(-15).decompress(f.read(glen))
                plain += len(data)
                groups += 1
                if h:
                    h.update(data)
                p += 3 + glen
            rec['plain'] = plain
            rec['groups'] = groups
            if h:
                rec['sha'] = h.hexdigest()
        else:
            h = hashlib.sha256()
            left, p = n, pos + 8
            while left > 0:
                f.seek(p)
                piece = f.read(min(MI, left))
                if not piece:
                    break
                h.update(piece)
                p += len(piece)
                left -= len(piece)
            rec['plain'] = n
            rec['groups'] = 1
            if hash_it:
                rec['sha'] = h.hexdigest()
        recs.append(rec)
        pos += 8 + n
        i += 1
        if report and i % 400 == 0:
            report(i, pos)
    return recs, pos


def name_table(f, fhoff, hdr_len, blocks):
    """record frame offset -> the destination names the script gives it."""
    hdr, _ = read_header(f, fhoff, hdr_len)
    get = strings_at(hdr, blocks)
    ent, enum = blocks['entries']
    by_rel = {}
    for k in range(enum):
        e = struct.unpack_from('<9i', hdr, ent + k * ENTRY_SIZE)
        if e[0] != OP_FILE or e[3] < 0:
            continue
        nm = get(e[2])
        if not nm or '.' not in nm:
            continue
        by_rel.setdefault(e[3], []).append(nm)
    return by_rel


def cmd_index(argv):
    src, out = argv[0], argv[1]
    with open(src, 'rb') as f:
        fhoff, hdr_len, all_after = find_first_header(f)
        hdr, data_start = read_header(f, fhoff, hdr_len)
        blocks = block_table(hdr)
        by_rel = name_table(f, fhoff, hdr_len, blocks)
        data_end = fhoff + 28 + all_after
        recs, ended = walk(f, data_start, data_end,
                           report=lambda i, p: print('  %d records, %.0f MB'
                                                     % (i, (p - data_start) / 1e6)))
    for r in recs:
        names = by_rel.get(r['frame'] - data_start)
        if names:
            r['names'] = names
    json.dump({'source': os.path.abspath(src), 'firstheader': fhoff,
               'header_len': hdr_len, 'data_start': data_start,
               'data_end': data_end, 'ended_at': ended, 'records': recs},
              open(out, 'w', encoding='utf-8'))
    named = sum(1 for r in recs if 'names' in r)
    print('%d records, %d named, %.1f MB plaintext, walk ended %d B before data_end'
          % (len(recs), named, sum(r['plain'] for r in recs) / 1e6,
             data_end - ended))
    print('index -> %s' % out)


def cmd_list(argv):
    idx = json.load(open(argv[0]))
    want = argv[1].lower() if len(argv) > 1 else ''
    for r in idx['records']:
        for nm in r.get('names', []):
            if want in nm.lower():
                print('rec %-5d %10d  %s' % (r['i'], r['plain'], nm))


def read_record(idx, rec):
    with open(idx['source'], 'rb') as f:
        f.seek(rec['frame'])
        v, = struct.unpack('<Q', f.read(8))
        n = v & ~(1 << 63)
        out = bytearray()
        if v >> 63:
            p, end = rec['frame'] + 8, rec['frame'] + 8 + n - 3
            while p < end:
                f.seek(p)
                g = f.read(3)
                glen = g[0] | (g[1] << 8) | (g[2] << 16)
                f.seek(p + 3)
                out += zlib.decompressobj(-15).decompress(f.read(glen))
                p += 3 + glen
        else:
            left, p = n, rec['frame'] + 8
            while left > 0:
                f.seek(p)
                piece = f.read(min(MI, left))
                if not piece:
                    break
                out += piece
                p += len(piece)
                left -= len(piece)
    return bytes(out)


def cmd_extract(argv):
    idx = json.load(open(argv[0]))
    want, outdir = argv[1].lower(), argv[2]
    os.makedirs(outdir, exist_ok=True)
    done = 0
    for r in idx['records']:
        names = [n for n in r.get('names', []) if n.lower().endswith(want)]
        if not names:
            continue
        data = read_record(idx, r)
        got = hashlib.sha256(data).hexdigest()
        dst = os.path.join(outdir, os.path.basename(names[0]))
        if os.path.exists(dst) and r.get('sha') != hashlib.sha256(
                open(dst, 'rb').read()).hexdigest():
            stem, ext = os.path.splitext(os.path.basename(names[0]))
            dst = os.path.join(outdir, '%s_rec%d%s' % (stem, r['i'], ext))
        with open(dst, 'wb') as w:
            w.write(data)
        print('%-42s %10d  rec %-5d %s'
              % (os.path.basename(dst), len(data), r['i'],
                 'ok' if got == r.get('sha') else 'HASH MISMATCH'))
        done += 1
    print('%d files -> %s' % (done, outdir))


def cmd_verify(argv):
    idx = json.load(open(argv[0]))
    root = argv[1]
    by_hash = {}
    for r in idx['records']:
        if r.get('sha'):
            by_hash.setdefault((r['plain'], r['sha']), []).append(r)
    total = matched = 0
    unmatched = []
    for dirpath, dirs, files in os.walk(root):
        for fn in files:
            p = os.path.join(dirpath, fn)
            try:
                st = os.stat(p)
                h = hashlib.sha256(open(p, 'rb').read()).hexdigest()
            except OSError:
                continue
            total += 1
            hit = by_hash.get((st.st_size, h))
            if hit:
                matched += 1
            else:
                unmatched.append(os.path.relpath(p, root))
    print('%d of %d installed files are payload records (%.1f MB)'
          % (matched, total, sum(r['plain'] for r in idx['records']) / 1e6))
    print('%d not in the payload (our own files, or dirs unreadable here):'
          % len(unmatched))
    for u in unmatched[:40]:
        print('   ' + u)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd, argv = sys.argv[1], sys.argv[2:]
    {'index': cmd_index, 'list': cmd_list,
     'extract': cmd_extract, 'verify': cmd_verify}[cmd](argv)


if __name__ == '__main__':
    main()
