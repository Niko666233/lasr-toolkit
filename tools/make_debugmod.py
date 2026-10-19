"""Build the "retail debug build" patch for L.A. Street Racing.

Produces a drop-in replacement for `<game>/java/classes.zip` that exposes the
developer menus which ship disabled in the retail build, and moves the debug
console hotkey to a key your keyboard actually has.

Two independent edits:

  1. java.util.Config.majomParade : false -> true
       Config.<clinit>  `BOOL_LITERAL 0` + `PUTFIELD_STATIC 224`
       Config.init()    `BOOL_LITERAL 0` + `PUTFIELD_STATIC 224`
       `Config.master` is 1 in the shipped build, so init() re-clears the flag
       every launch -> both sites must be patched.

  2. console hotkey : java.io.Input.RCDIK_RMENU value 184 -> <key>
       Input.<clinit>   `INT_LITERAL 184` + `PUTFIELD_STATIC 651`
       The shipped value 184 (0xB8) is DIK_RALT (right Alt), not the Menu key
       the constant name suggests.  RCDIK_RMENU is read by exactly one site
       (Init's console hotkey), so repointing its value is safe.

Nothing game-related is stored in this repository: the script reads the two
class containers out of *your own* java/classes.zip, patches the two literals
in the lossless TUFA text form, re-packs with the game's own FLZD routine and
writes a new zip.  Entries that were not targeted are re-packed and checked to
reproduce the shipped bytes exactly.

Usage:
  python tools/make_debugmod.py --game "C:\\Games\\LASR" --out classes_debugmod.zip
  python tools/make_debugmod.py --game ... --console-key 68      # F10 instead of ~
  python tools/make_debugmod.py --game ... --console-key 0       # keep the original key
"""
import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import tufa_ir as T                                              # noqa: E402
from lasr_flzd import FlzdCodec, ADDR_PACK, HEAP_BASE, BUFFER_OFFSET  # noqa: E402

MAJOM = ("  BOOL_LITERAL 0\n  PUTFIELD_STATIC 224\n",
         "  BOOL_LITERAL 1\n  PUTFIELD_STATIC 224\n", (0x00, 0x01), 2)
CONSOLE = "  INT_LITERAL 184\n  PUTFIELD_STATIC 651\n"

KEYS = {41: "grave / ~ (backtick)", 43: "backslash", 57: "space",
        59: "F1", 68: "F10", 88: "F12"}


def patch_text(raw, needle, repl, expect, count, label):
    text = T.to_text(T.parse(raw))
    if needle not in text and text.count(repl) == count:
        return None, []          # already patched
    if T.serialize(T.from_text(text)) != raw:
        raise SystemExit("[%s] unpatched round-trip is not byte-identical" % label)
    n = text.count(needle)
    if n != count:
        raise SystemExit("[%s] expected %d site(s), found %d" % (label, count, n))
    out = T.serialize(T.from_text(text.replace(needle, repl)))
    if len(out) != len(raw):
        raise SystemExit("[%s] length changed %d -> %d" % (label, len(raw), len(out)))
    diffs = [i for i in range(len(raw)) if raw[i] != out[i]]
    if len(diffs) != count:
        raise SystemExit("[%s] %d byte(s) changed, expected %d" % (label, len(diffs), count))
    for i in diffs:
        if (raw[i], out[i]) != expect:
            raise SystemExit("[%s] diff at 0x%x is %02x->%02x, expected %02x->%02x"
                             % (label, i, raw[i], out[i], expect[0], expect[1]))
    return out, diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=r"C:\Games\LASR")
    ap.add_argument("--zip", default="", help="classes.zip to read (default <game>/java/classes.zip)")
    ap.add_argument("--out", default="classes_debugmod.zip")
    ap.add_argument("--console-key", type=int, default=41,
                    help="DirectInput scancode for the console hotkey; 0 = keep the shipped one")
    ap.add_argument("--no-majom", action="store_true", help="skip the debug-menu flag patch")
    a = ap.parse_args()

    game = Path(a.game)
    exe = game / "LASR.exe"
    zip_path = Path(a.zip) if a.zip else (game / "java" / "classes.zip")
    for p in (exe, zip_path):
        if not p.exists():
            raise SystemExit("missing: %s" % p)

    codec = FlzdCodec(str(exe))
    src_ptr = HEAP_BASE + BUFFER_OFFSET + 0x400000
    dst_ptr = HEAP_BASE + BUFFER_OFFSET + 0x800000

    zin = zipfile.ZipFile(zip_path)
    names = zin.namelist()
    jobs = {}

    if not a.no_majom:
        raw = codec.unpack(zin.read("util/Config.class"))
        patched, diffs = patch_text(raw, *MAJOM, label="Config")
        if patched is None:
            print("Config.class  already patched, skipped")
        else:
            jobs["util/Config.class"] = patched
        if patched is not None:
            print("Config.class  %5d B  md5 %s -> %s  (%d byte(s) at %s)"
              % (len(raw), hashlib.md5(raw).hexdigest()[:12], hashlib.md5(patched).hexdigest()[:12],
                 len(diffs), ", ".join("0x%x" % i for i in diffs)))

    if a.console_key:
        k = a.console_key
        raw = codec.unpack(zin.read("io/Input.class"))
        patched, diffs = patch_text(
            raw, CONSOLE, CONSOLE.replace("184", str(k)),
            (184 & 0xFF, k & 0xFF), 1, "Input")
        if patched is None:
            print("Input.class   already at the requested key, skipped")
        else:
            jobs["io/Input.class"] = patched
        if patched is not None:
            print("Input.class   %5d B  md5 %s -> %s  (%d byte at %s)  key=%d (%s)"
              % (len(raw), hashlib.md5(raw).hexdigest()[:12], hashlib.md5(patched).hexdigest()[:12],
                 len(diffs), ", ".join("0x%x" % i for i in diffs), k, KEYS.get(k, "custom")))

    if not jobs:
        raise SystemExit("nothing to do: both patches disabled")

    out_entries = {}
    same = 0
    for name in names:
        blob = zin.read(name)
        tufa = codec.unpack(blob)
        level = blob[12] if len(blob) > 12 else 11
        if name in jobs:
            tufa = jobs[name]
        cap = len(tufa) + 0x10000
        codec.uc.mem_write(src_ptr, tufa)
        codec.uc.mem_write(dst_ptr, b"\xcc" * cap)
        size = codec._call(ADDR_PACK, [src_ptr, len(tufa), dst_ptr, cap, level])
        if size <= 0:
            raise SystemExit("FLZD pack failed on %s (ret=%d)" % (name, size))
        blob2 = bytes(codec.uc.mem_read(dst_ptr, size))
        if name not in jobs:
            if blob2 != blob:
                raise SystemExit("repack of untouched entry %s differs from the shipped bytes" % name)
            same += 1
        out_entries[name] = blob2

    print("re-packed: %d/%d untouched entries byte-identical" % (same, len(names) - len(jobs)))

    outp = Path(a.out)
    with zipfile.ZipFile(outp, "w") as zo:
        for name in names:
            zi = zipfile.ZipInfo(name, date_time=(2006, 7, 4, 14, 31, 0))
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0
            zo.writestr(zi, out_entries[name])
    print("wrote %s (%d bytes, md5 %s)"
          % (outp, outp.stat().st_size, hashlib.md5(outp.read_bytes()).hexdigest()))
    print("\ninstall:  copy this zip over %s (keep a backup of the original first)"
          % (game / "java" / "classes.zip"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
