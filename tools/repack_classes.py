"""repack_classes.py - rebuild java/classes.zip from our own recompiled class
containers and prove the packaging path byte-for-byte.

For every entry of the shipped zip:
  1. unpack the FLZD wrapper                       (game's flzd_unpack)
  2. re-pack the *same* TUFA container              (game's flzd_pack)
  3. compare with the shipped entry byte-for-byte
Then write a zip built from those re-packed entries (stored, same order) and
report its md5 next to the original.

If every entry reproduces exactly, a rebuilt zip is indistinguishable from the
shipped one -- which is the strongest possible "artefact is identical" result and
also the artifact you can drop into the game to test packaging acceptance.

Usage: ./.capenv/Scripts/python.exe tools/repack_classes.py [--out scratch/classes_rebuilt.zip]
"""
import argparse
import hashlib
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from lasr_flzd import FlzdCodec, ADDR_PACK, HEAP_BASE, BUFFER_OFFSET   # noqa: E402

EXE = r"C:\Games\LASR\LASR.exe"
ZIP = Path(r"C:\Games\LASR\java\classes.zip")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="scratch/classes_rebuilt.zip")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    codec = FlzdCodec(EXE)
    zin = zipfile.ZipFile(ZIP)
    names = zin.namelist()
    if a.limit:
        names = names[:a.limit]

    src_ptr = HEAP_BASE + BUFFER_OFFSET + 0x400000
    dst_ptr = HEAP_BASE + BUFFER_OFFSET + 0x800000

    same = 0
    diff = []
    out_entries = {}
    for i, name in enumerate(names, 1):
        blob = zin.read(name)
        try:
            tufa = codec.unpack(blob)
        except Exception as e:                                   # noqa: BLE001
            diff.append((name, "unpack: %s" % e))
            continue
        level = blob[12] if len(blob) > 12 else 11
        cap = len(tufa) + 0x10000
        codec.uc.mem_write(src_ptr, tufa)
        codec.uc.mem_write(dst_ptr, b"\xcc" * cap)
        size = codec._call(ADDR_PACK, [src_ptr, len(tufa), dst_ptr, cap, level])
        if size <= 0:
            diff.append((name, "pack ret=%d" % size))
            continue
        blob2 = bytes(codec.uc.mem_read(dst_ptr, size))
        out_entries[name] = blob2
        if blob2 == blob:
            same += 1
        else:
            diff.append((name, "bytes differ (%d vs %d)" % (len(blob2), len(blob))))
        if i % 25 == 0:
            print("   ... %d/%d" % (i, len(names)))

    print("entries:            %d" % len(names))
    print("byte-identical:     %d" % same)
    print("differing:          %d" % len(diff))
    for n, why in diff[:8]:
        print("   %-40s %s" % (n, why))

    out = ROOT / a.out
    out.parent.mkdir(parents=True, exist_ok=True)
    # preserve the shipped ZipInfo (date_time / attrs / compression) so the
    # rebuilt archive can be compared as a whole file, not just entry by entry
    infos = {i.filename: i for i in zin.infolist()}
    with zipfile.ZipFile(out, "w") as z:
        for name in names:
            if name not in out_entries:
                continue
            zi = zipfile.ZipInfo(filename=name, date_time=infos[name].date_time)
            zi.compress_type = infos[name].compress_type
            zi.external_attr = infos[name].external_attr
            zi.internal_attr = infos[name].internal_attr
            zi.create_system = infos[name].create_system
            # shipped: "version made by" = 20 (central dir only),
            #          "version needed"  = 10 (local + central)
            zi.create_version = 20
            zi.extract_version = 10
            z.writestr(zi, out_entries[name])
    md5 = lambda p: hashlib.md5(Path(p).read_bytes()).hexdigest()
    print("rebuilt: %s  %d B  md5=%s" % (out, out.stat().st_size, md5(out)))
    print("shipped: %s  %d B  md5=%s" % (ZIP, ZIP.stat().st_size, md5(ZIP)))
    print("whole-file identical:", md5(out) == md5(ZIP))
    return 0 if not diff else 1


if __name__ == "__main__":
    sys.exit(main())
