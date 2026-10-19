"""TUFA class-file IR: lossless parse / serialize / text-roundtrip.

Milestone 1 of the full-decompilation line (docs/64): prove that every
extracted .class can be decoded into a structured IR (and into an editable
text form, .tasm) and re-emitted BYTE-IDENTICAL. Byte-identical recompile ==
provably identical behaviour under the game's own VM.

Container grammar (all facts from docs/02, 08, 10, 11 + probe_container.py):

  file    = "TUFA" u32 ver u32 build  chunk*        (chunks tile the file exactly)
  chunk   = tag(4cc) u32 size body(size)
  TUFA    = inner-class marker, size==4, body=u32 build   (7 files carry 18 inner classes)
  CONS    = u32 count; count x entry
            entry: tag0 = NUL-terminated bytes | tag4 = u32 | tag5 = u32,u32 | tag7 = u32,u32
  FILD    = u32 n_static  n_static x 4*u32 (flags,name,type,extra)
            u32 n_inst    n_inst   x 4*u32
  MTHD    = u32 n_static  n_static x 5*u32 (flags,name,desc,tree_slot,n_locals)
            u32 n_inst    n_inst   x 5*u32      (two groups, like FILD)
  CLSS    = (size/4) x u32   (24/28/36 B observed)
  TREE    = u32 n; n x (u32 rec_size; rec_size bytes of instruction stream)
  instr   = u8 opcode (+ u32 payload iff width 5); widths from docs/08 §5.4
            (100% of records tile exactly; opcode 0x00/0x0a/>0x55 never occur)

Usage:
  python tools/tufa_ir.py --roundtrip         # IR parse+serialize == original, all classes
  python tools/tufa_ir.py --text-roundtrip    # dis -> .tasm -> asm == original bytes
  python tools/tufa_ir.py --dis PATH [-o OUT] # dump one class as .tasm
  python tools/tufa_ir.py --asm PATH -o OUT   # assemble .tasm -> .class bytes
  python tools/tufa_ir.py --stats             # corpus statistics + opcode histogram
"""
import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EX = ROOT / "extracted"

# ------------------------------------------------------------ opcode table
# docs/08_VM_OPCODES.md §5.4: op -> (name, width). 0x0a reserved (never emitted).
OPS = {
    0x01: ("CAST", 5), 0x02: ("INSTANCEOF", 5), 0x03: ("NULL_LITERAL", 1),
    0x04: ("BOOL_LITERAL", 5), 0x05: ("INT_LITERAL", 5), 0x06: ("FLOAT_LITERAL", 5),
    0x07: ("CHAR_LITERAL", 5), 0x08: ("STRING_LITERAL", 5), 0x09: ("RID_LITERAL", 5),
    0x0B: ("LOCAL_LOAD", 5), 0x0C: ("LOCAL_CREATE", 1), 0x0D: ("LOCAL_STORE", 5),
    0x0E: ("LOCAL_CLEAR", 1), 0x0F: ("LOCAL_CLEARN", 5), 0x10: ("INVOKE", 5),
    0x11: ("INVOKESPECIAL", 5), 0x12: ("INVOKESTATIC", 5),
    0x13: ("FIELD_REF_INSTANCE", 5), 0x14: ("FIELD_REF_STATIC", 5),
    0x15: ("FIELD_REF_QUICK", 5), 0x16: ("RETURN", 1), 0x17: ("JMP", 5),
    0x18: ("JMP_NE", 5), 0x19: ("JMP_EQ", 5), 0x1A: ("JMP_EQ2", 5),
    0x1B: ("F2I", 1), 0x1C: ("I2F", 1), 0x1D: ("I2S", 1), 0x1E: ("F2S", 1),
    0x1F: ("PUTFIELD_INSTANCE", 5), 0x20: ("PUTFIELD_STATIC", 5),
    0x21: ("PUTFIELD_QUICK", 5), 0x22: ("NEWARRAY", 5), 0x23: ("ARRAY_STORE", 1),
    0x24: ("ARRAY_INIT", 5), 0x25: ("EMPTYDIMS", 5), 0x26: ("ARRAY_ACCESS", 1),
    0x27: ("NEW", 5), 0x28: ("DELETE", 5), 0x29: ("POP", 1), 0x2A: ("DUP", 1),
    0x2B: ("DUP_X1", 1), 0x2C: ("DUP_X2", 1), 0x2D: ("DUP2", 1),
    0x2E: ("SADD", 1), 0x2F: ("IADD", 1), 0x30: ("FADD", 1), 0x31: ("ISUB", 1),
    0x32: ("FSUB", 1), 0x33: ("IDIV", 1), 0x34: ("FDIV", 1), 0x35: ("IMUL", 1),
    0x36: ("FMUL", 1), 0x37: ("INEG", 1), 0x38: ("FNEG", 1), 0x39: ("IINC", 1),
    0x3A: ("FINC", 1), 0x3B: ("IDEC", 1), 0x3C: ("FDEC", 1), 0x3D: ("MOD", 1),
    0x3E: ("SHORTCUT_AND", 1), 0x3F: ("SHORTCUT_OR", 1), 0x40: ("ANDAND", 1),
    0x41: ("OROR", 1), 0x42: ("AND", 1), 0x43: ("OR", 1), 0x44: ("XOR", 1),
    0x45: ("NOT", 1), 0x46: ("EXCLAMATION", 1), 0x47: ("ASR", 1), 0x48: ("ASL", 1),
    0x49: ("LSR", 1), 0x4A: ("IEQ", 1), 0x4B: ("INE", 1), 0x4C: ("IGT", 1),
    0x4D: ("IGE", 1), 0x4E: ("ILT", 1), 0x4F: ("ILE", 1), 0x50: ("FEQ", 1),
    0x51: ("FNE", 1), 0x52: ("FGT", 1), 0x53: ("FGE", 1), 0x54: ("FLT", 1),
    0x55: ("FLE", 1),
}
BY_NAME = {v[0]: (k, v[1]) for k, v in OPS.items()}
JUMP_OPS = {0x17, 0x18, 0x19, 0x1A}          # payload = target - instr_addr


class TufaError(Exception):
    pass


# ------------------------------------------------------------------ parsing
def parse_instrs(body, where):
    out, pos, n = [], 0, len(body)
    while pos < n:
        op = body[pos]
        if op not in OPS:
            raise TufaError(f"{where}: illegal opcode 0x{op:02x} at {pos}")
        name, w = OPS[op]
        if pos + w > n:
            raise TufaError(f"{where}: truncated instr at {pos}")
        pay = struct.unpack_from("<I", body, pos + 1)[0] if w == 5 else None
        out.append((pos, op, pay))
        pos += w
    if pos != n:
        raise TufaError(f"{where}: stream overran ({pos} != {n})")
    return out


def parse_cons(body, where):
    count, = struct.unpack_from("<I", body, 0)
    ents, pos = [], 4
    for i in range(count):
        if pos >= len(body):
            raise TufaError(f"{where}: CONS truncated at entry {i}")
        tag = body[pos]
        pos += 1
        if tag == 0:
            end = body.index(b"\x00", pos)
            ents.append((0, body[pos:end]))
            pos = end + 1
        elif tag == 4:
            ents.append((4, struct.unpack_from("<I", body, pos)[0]))
            pos += 4
        elif tag in (5, 7):
            a, b = struct.unpack_from("<II", body, pos)
            ents.append((tag, a, b))
            pos += 8
        else:
            raise TufaError(f"{where}: CONS entry {i} unknown tag {tag}")
    if pos != len(body):
        raise TufaError(f"{where}: CONS blob not tiled ({pos} != {len(body)})")
    return count, ents


def parse_u32_rows(body, where, per_row, what):
    if len(body) % (4 * per_row):
        pass  # caller validates against explicit counts
    return body


def parse_fild(body, where):
    n1, = struct.unpack_from("<I", body, 0)
    pos = 4 + 16 * n1
    if pos + 4 > len(body):
        raise TufaError(f"{where}: FILD truncated (static)")
    n2, = struct.unpack_from("<I", body, pos)
    pos += 4 + 16 * n2
    if pos != len(body):
        raise TufaError(f"{where}: FILD not tiled ({pos} != {len(body)})")
    g1 = [struct.unpack_from("<IIII", body, 4 + 16 * i) for i in range(n1)]
    g2 = [struct.unpack_from("<IIII", body, 8 + 16 * n1 + 16 * i) for i in range(n2)]
    return n1, g1, n2, g2


def parse_mthd(body, where):
    # Two groups of 20-byte rows with a second count in between (like FILD):
    #   u32 n_static | n_static x 5*u32 | u32 n_inst | n_inst x 5*u32
    # Semantics verified corpus-wide: every group-1 row carries ACC_STATIC (flags&8)
    # and no group-2 row does (2242/2242 MTHD chunks tile exactly).
    n1, = struct.unpack_from("<I", body, 0)
    pos = 4 + 20 * n1
    if pos + 4 > len(body):
        raise TufaError(f"{where}: MTHD truncated (static group)")
    n2, = struct.unpack_from("<I", body, pos)
    if pos + 4 + 20 * n2 != len(body):
        raise TufaError(f"{where}: MTHD not tiled ({pos + 4 + 20 * n2} != {len(body)})")
    g1 = [struct.unpack_from("<IIIII", body, 4 + 20 * i) for i in range(n1)]
    g2 = [struct.unpack_from("<IIIII", body, pos + 4 + 20 * i) for i in range(n2)]
    return n1, g1, n2, g2


def parse_tree(body, where):
    n, = struct.unpack_from("<I", body, 0)
    recs, pos = [], 4
    for i in range(n):
        if pos + 4 > len(body):
            raise TufaError(f"{where}: TREE truncated at record {i}")
        sz, = struct.unpack_from("<I", body, pos)
        pos += 4
        rb = body[pos:pos + sz]
        if len(rb) != sz:
            raise TufaError(f"{where}: TREE record {i} runs past body")
        pos += sz
        recs.append(parse_instrs(rb, f"{where}/rec{i}"))
    if pos != len(body):
        raise TufaError(f"{where}: TREE not tiled ({pos} != {len(body)})")
    return n, recs


def parse(data, name="<mem>"):
    """-> IR dict. Raises TufaError on any format violation."""
    if data[:4] != b"TUFA":
        raise TufaError(f"{name}: bad magic {data[:4]!r}")
    ver, build = struct.unpack_from("<II", data, 4)
    chunks, off = [], 12
    while off < len(data):
        if off + 8 > len(data):
            raise TufaError(f"{name}: dangling chunk header at {off}")
        tag = data[off:off + 4].decode("latin1")
        size, = struct.unpack_from("<I", data, off + 4)
        body = data[off + 8:off + 8 + size]
        if len(body) != size:
            raise TufaError(f"{name}: chunk {tag} runs past EOF")
        where = f"{name}@{off}:{tag}"
        if tag == "TUFA":
            if size != 4:
                raise TufaError(f"{where}: inner marker size {size} != 4")
            chunks.append(("TUFA", struct.unpack("<I", body)[0]))
        elif tag == "CONS":
            chunks.append(("CONS",) + parse_cons(body, where))
        elif tag == "FILD":
            chunks.append(("FILD",) + parse_fild(body, where))
        elif tag == "MTHD":
            chunks.append(("MTHD",) + parse_mthd(body, where))
        elif tag == "CLSS":
            if size % 4:
                raise TufaError(f"{where}: CLSS size {size} not u32-aligned")
            chunks.append(("CLSS", list(struct.unpack(f"<{size // 4}I", body))))
        elif tag == "TREE":
            chunks.append(("TREE",) + parse_tree(body, where))
        else:
            chunks.append(("RAW", tag.encode("latin1"), body))  # never observed
        off += 8 + size
    if off != len(data):
        raise TufaError(f"{name}: chunk stream overran ({off} != {len(data)})")
    return {"ver": ver, "build": build, "chunks": chunks}


# ---------------------------------------------------------------- emitting
def ser_cons(count, ents):
    out = [struct.pack("<I", count)]
    for e in ents:
        if e[0] == 0:
            out.append(b"\x00" + e[1] + b"\x00")
        elif e[0] == 4:
            out.append(b"\x04" + struct.pack("<I", e[1]))
        else:
            out.append(bytes([e[0]]) + struct.pack("<II", e[1], e[2]))
    return b"".join(out)


def ser_fild(n1, g1, n2, g2):
    out = [struct.pack("<I", n1)]
    out += [struct.pack("<IIII", *r) for r in g1]
    out.append(struct.pack("<I", n2))
    out += [struct.pack("<IIII", *r) for r in g2]
    return b"".join(out)


def ser_mthd(n1, g1, n2, g2):
    out = [struct.pack("<I", n1)]
    out += [struct.pack("<IIIII", *r) for r in g1]
    out.append(struct.pack("<I", n2))
    out += [struct.pack("<IIIII", *r) for r in g2]
    return b"".join(out)


def ser_tree(n, recs):
    out = [struct.pack("<I", n)]
    for r in recs:
        code = bytearray()
        for _pos, op, pay in r:
            code.append(op)
            if OPS[op][1] == 5:
                code += struct.pack("<I", pay)
        out.append(struct.pack("<I", len(code)))
        out.append(bytes(code))
    return b"".join(out)


def serialize(ir):
    out = [b"TUFA", struct.pack("<II", ir["ver"], ir["build"])]
    for ch in ir["chunks"]:
        kind = ch[0]
        if kind == "TUFA":
            tag, body = b"TUFA", struct.pack("<I", ch[1])
        elif kind == "CONS":
            tag, body = b"CONS", ser_cons(ch[1], ch[2])
        elif kind == "FILD":
            tag, body = b"FILD", ser_fild(*ch[1:])
        elif kind == "MTHD":
            tag, body = b"MTHD", ser_mthd(*ch[1:])
        elif kind == "CLSS":
            tag, body = b"CLSS", struct.pack(f"<{len(ch[1])}I", *ch[1])
        elif kind == "TREE":
            tag, body = b"TREE", ser_tree(ch[1], ch[2])
        elif kind == "RAW":
            tag, body = ch[1], ch[2]
        else:
            raise TufaError(f"unknown chunk kind {kind}")
        out.append(tag + struct.pack("<I", len(body)) + body)
    return b"".join(out)


# -------------------------------------------------------------- text (.tasm)
def esc(b):
    s = []
    for c in b:
        if 0x20 <= c < 0x7F and c not in (0x22, 0x5C):
            s.append(chr(c))
        else:
            s.append(f"\\x{c:02x}")
    return '"' + "".join(s) + '"'


def unesc(tok, name):
    if len(tok) < 2 or tok[0] != '"' or tok[-1] != '"':
        raise TufaError(f"{name}: bad string token {tok!r}")
    out, i = bytearray(), 1
    while i < len(tok) - 1:
        c = tok[i]
        if c == "\\" and i + 3 < len(tok) and tok[i + 1] == "x":
            out.append(int(tok[i + 2:i + 4], 16))
            i += 4
        elif c == "\\" and i + 1 < len(tok) - 1:
            nxt = tok[i + 1]
            out.append({"n": 0x0A, "t": 0x09, "r": 0x0D}.get(nxt, ord(nxt)))
            i += 2
        else:
            out.extend(c.encode("utf-8"))
            i += 1
    return bytes(out)


def num(tok, name):
    return int(tok, 16) if tok.lower().startswith("0x") else int(tok)


def flat(s):
    """Flatten decoded string text for use inside a `;` comment (must stay one line)."""
    return (s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")
             .replace("\t", "\\t"))


def to_text(ir, comments=True, tree_hook=None):
    """IR -> .tasm lines. Comments (; ...) are stripped by the assembler.
    tree_hook(n_records, recs, cons) -> list[str] may replace the per-record
    rendering of TREE chunks (used by tufa_src.py to emit high-level bodies)."""
    L = [f"TUFA {ir['ver']} {ir['build']}"]
    cons = None
    for ch in ir["chunks"]:
        k = ch[0]
        if k == "TUFA":
            L += ["", f"TUFA_INNER {ch[1]}"]
            cons = None
        elif k == "CONS":
            _, count, ents = ch
            cons = ents
            L += ["", f"CONS {count}"]
            for i, e in enumerate(ents):
                c = ""
                if e[0] == 0:
                    L.append(f"S {esc(e[1])}")
                elif e[0] == 4:
                    tgt = ""
                    if comments and 0 <= e[1] < len(ents) and ents[e[1]][0] == 0:
                        tgt = " ; " + flat(ents[e[1]][1].decode("utf-8", "replace"))
                    L.append(f"C1 {e[1]}{tgt}")
                else:
                    nm = ds = ""
                    if comments:
                        if 0 <= e[1] < len(ents) and ents[e[1]][0] == 0:
                            nm = flat(ents[e[1]][1].decode("utf-8", "replace"))
                        if 0 <= e[2] < len(ents) and ents[e[2]][0] == 0:
                            ds = flat(ents[e[2]][1].decode("utf-8", "replace"))
                    tagname = "C2" if e[0] == 5 else "NT"
                    cmt = f" ; {nm} {ds}" if (nm or ds) else ""
                    L.append(f"{tagname} {e[1]} {e[2]}{cmt}")
        elif k == "FILD":
            _, n1, g1, n2, g2 = ch
            L += ["", "FILD", f"STATIC {n1}"]
            for r in g1:
                L.append(row_comment("F", r, cons, comments, (1, 2)))
            L.append(f"INSTANCE {n2}")
            for r in g2:
                L.append(row_comment("F", r, cons, comments, (1, 2)))
        elif k == "MTHD":
            _, n1, g1, n2, g2 = ch
            L += ["", "MTHD", f"STATIC {n1}"]
            for r in g1:
                L.append(row_comment("M", r, cons, comments, (1, 2)))
            L.append(f"INSTANCE {n2}")
            for r in g2:
                L.append(row_comment("M", r, cons, comments, (1, 2)))
        elif k == "CLSS":
            L += ["", f"CLSS {len(ch[1])}"]
            for v in ch[1]:
                L.append(f"W {v}")
        elif k == "TREE":
            _, n, recs = ch
            L += ["", f"TREE {n}"]
            if tree_hook is not None:
                L += tree_hook(n, recs, cons)
                continue
            for i, r in enumerate(recs):
                L.append(f"REC {i} {len(r)}")
                for pos, op, pay in r:
                    nm, w = OPS[op]
                    if w == 5:
                        c = ""
                        if comments and op in JUMP_OPS:
                            c = f" ; -> {pos + pay}"
                        elif comments and op == 0x06:  # FLOAT_LITERAL
                            f, = struct.unpack("<f", struct.pack("<I", pay))
                            c = f" ; {f!r}"
                        elif comments and op == 0x08 and cons:  # STRING_LITERAL
                            if 0 <= pay < len(cons) and cons[pay][0] == 0:
                                c = " ; " + flat(cons[pay][1].decode("utf-8", "replace"))
                        L.append(f"  {nm} {pay}{c}")
                    else:
                        L.append(f"  {nm}")
                L.append("END")
        elif k == "RAW":
            L += ["", f"RAW {ch[1].decode('latin1')} {len(ch[2])}", esc(ch[2])]
    return "\n".join(L) + "\n"


def row_comment(prefix, r, cons, comments, idx_fields):
    base = f"{prefix} " + " ".join(str(v) for v in r)
    if comments and cons:
        names = []
        for j in idx_fields:
            v = r[j]
            if 0 <= v < len(cons) and cons[v][0] == 0:
                names.append(flat(cons[v][1].decode("utf-8", "replace")))
        if names:
            base += " ; " + " ".join(names)
    return base


def strip_comment(line):
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c == '"':  # skip quoted string
            out.append(c)
            i += 1
            while i < n:
                out.append(line[i])
                if line[i] == "\\":
                    i += 1
                    if i < n:
                        out.append(line[i])
                elif line[i] == '"':
                    break
                i += 1
            i += 1
        elif c == ";":
            break
        else:
            out.append(c)
            i += 1
    return "".join(out).strip()


def from_text(text, name="<tasm>"):
    lines = [strip_comment(l) for l in text.splitlines()]
    lines = [l for l in lines if l]
    if not lines or not lines[0].startswith("TUFA "):
        raise TufaError(f"{name}: missing TUFA header")
    _, ver, build = lines[0].split()
    ir = {"ver": num(ver, name), "build": num(build, name), "chunks": []}
    i = 1

    def toks(j):
        return lines[j].split()

    while i < len(lines):
        ln = lines[i]
        head = ln.split()[0]
        if head == "TUFA_INNER":
            ir["chunks"].append(("TUFA", num(toks(i)[1], name)))
            i += 1
        elif head == "CONS":
            count = num(toks(i)[1], name)
            ents = []
            i += 1
            while len(ents) < count:
                t = lines[i]
                k = t.split()[0]
                if k == "S":
                    ents.append((0, unesc(t[2:].strip(), name)))
                elif k == "C1":
                    ents.append((4, num(t.split()[1], name)))
                elif k in ("C2", "NT"):
                    p = t.split()
                    ents.append((5 if k == "C2" else 7, num(p[1], name), num(p[2], name)))
                else:
                    raise TufaError(f"{name}: in CONS, bad line {t!r}")
                i += 1
            ir["chunks"].append(("CONS", count, ents))
        elif head == "FILD":
            i += 1
            if toks(i)[0] != "STATIC":
                raise TufaError(f"{name}: FILD expects STATIC")
            n1 = num(toks(i)[1], name)
            g1 = []
            i += 1
            for _ in range(n1):
                p = toks(i)
                assert p[0] == "F", f"{name}: FILD row"
                g1.append(tuple(num(x, name) for x in p[1:5]))
                i += 1
            if toks(i)[0] != "INSTANCE":
                raise TufaError(f"{name}: FILD expects INSTANCE")
            n2 = num(toks(i)[1], name)
            g2 = []
            i += 1
            for _ in range(n2):
                p = toks(i)
                assert p[0] == "F", f"{name}: FILD row"
                g2.append(tuple(num(x, name) for x in p[1:5]))
                i += 1
            ir["chunks"].append(("FILD", n1, g1, n2, g2))
        elif head == "MTHD":
            i += 1
            if toks(i)[0] != "STATIC":
                raise TufaError(f"{name}: MTHD expects STATIC")
            n1 = num(toks(i)[1], name)
            g1 = []
            i += 1
            for _ in range(n1):
                q = toks(i)
                assert q[0] == "M", f"{name}: MTHD row"
                g1.append(tuple(num(x, name) for x in q[1:6]))
                i += 1
            if toks(i)[0] != "INSTANCE":
                raise TufaError(f"{name}: MTHD expects INSTANCE")
            n2 = num(toks(i)[1], name)
            g2 = []
            i += 1
            for _ in range(n2):
                q = toks(i)
                assert q[0] == "M", f"{name}: MTHD row"
                g2.append(tuple(num(x, name) for x in q[1:6]))
                i += 1
            ir["chunks"].append(("MTHD", n1, g1, n2, g2))
        elif head == "CLSS":
            p = toks(i)
            cnt = num(p[1], name)
            vals = []
            i += 1
            for _ in range(cnt):
                q = toks(i)
                assert q[0] == "W", f"{name}: CLSS word"
                vals.append(num(q[1], name))
                i += 1
            ir["chunks"].append(("CLSS", vals))
        elif head == "TREE":
            n = num(toks(i)[1], name)
            recs = []
            i += 1
            for _ in range(n):
                p = toks(i)
                assert p[0] == "REC", f"{name}: TREE expects REC"
                k = num(p[2], name)
                i += 1
                instrs, pos = [], 0
                for _ in range(k):
                    q = toks(i)
                    nm = q[0]
                    if nm not in BY_NAME:
                        raise TufaError(f"{name}: bad mnemonic {nm!r}")
                    op, w = BY_NAME[nm]
                    pay = num(q[1], name) if w == 5 else None
                    if w == 5 and len(q) < 2:
                        raise TufaError(f"{name}: {nm} missing payload")
                    instrs.append((pos, op, pay))
                    pos += w
                    i += 1
                if toks(i)[0] != "END":
                    raise TufaError(f"{name}: REC not closed with END")
                i += 1
                recs.append(instrs)
            ir["chunks"].append(("TREE", n, recs))
        elif head == "RAW":
            p = toks(i)
            tag = p[1].encode("latin1")
            i += 1
            body = unesc(lines[i], name)
            i += 1
            ir["chunks"].append(("RAW", tag, body))
        else:
            raise TufaError(f"{name}: unknown line {ln!r}")
    return ir


# ------------------------------------------------------------------- driver
def all_classes():
    return sorted(EX.rglob("*.class"))


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n if len(a) != len(b) else -1


def cmd_roundtrip(text_mode):
    files = all_classes()
    ok = bad = 0
    fails = []
    for f in files:
        data = f.read_bytes()
        rel = str(f.relative_to(EX))
        try:
            ir = parse(data, rel)
            if text_mode:
                t = to_text(ir)
                ir2 = from_text(t, rel)
                out = serialize(ir2)
            else:
                out = serialize(ir)
            if out == data:
                ok += 1
            else:
                bad += 1
                fails.append((rel, f"len {len(out)} vs {len(data)}, diff@{first_diff(out, data)}"))
        except Exception as e:
            bad += 1
            fails.append((rel, f"{type(e).__name__}: {e}"))
    mode = "TEXT (dis->asm)" if text_mode else "IR (parse->serialize)"
    print(f"{mode} roundtrip: {ok}/{len(files)} byte-identical, {bad} failed")
    for rel, why in fails[:20]:
        print("  FAIL", rel, "|", why)
    return 0 if bad == 0 else 1


def cmd_emit_all(outdir):
    """Disassemble every class to OUTDIR (as a .tasm source tree), then re-assemble
    each file FROM DISK and compare with the original bytes. End-to-end proof."""
    out = Path(outdir)
    files = all_classes()
    ok = bad = 0
    fails = []
    tmp = out / "_reasm"
    tmp.mkdir(parents=True, exist_ok=True)
    for f in files:
        data = f.read_bytes()
        rel = f.relative_to(EX)
        tasm = out / rel.with_suffix(rel.suffix + ".tasm")
        tasm.parent.mkdir(parents=True, exist_ok=True)
        try:
            ir = parse(data, str(rel))
            tasm.write_text(to_text(ir), encoding="utf-8")
            ir2 = from_text(tasm.read_text(encoding="utf-8"), str(rel))
            back = serialize(ir2)
            chk = tmp / rel
            chk.parent.mkdir(parents=True, exist_ok=True)
            chk.write_bytes(back)
            if back == data and chk.read_bytes() == data:
                ok += 1
            else:
                bad += 1
                fails.append((str(rel), f"len {len(back)} vs {len(data)} diff@{first_diff(back, data)}"))
        except Exception as e:
            bad += 1
            fails.append((str(rel), f"{type(e).__name__}: {e}"))
    print(f"emit-all: {ok}/{len(files)} disk-roundtrip byte-identical, {bad} failed")
    for rel, why in fails[:20]:
        print("  FAIL", rel, "|", why)
    return 0 if bad == 0 else 1


def cmd_stats():
    files = all_classes()
    n_inner = n_instr = n_rec = 0
    ops = Counter()
    ncons = Counter()
    clss_sizes = Counter()
    nchunks = Counter()
    for f in files:
        ir = parse(f.read_bytes(), str(f))
        for ch in ir["chunks"]:
            nchunks[ch[0]] += 1
            if ch[0] == "TUFA":
                n_inner += 1
            elif ch[0] == "CONS":
                for e in ch[2]:
                    ncons[e[0]] += 1
            elif ch[0] == "CLSS":
                clss_sizes[4 * len(ch[1])] += 1
            elif ch[0] == "TREE":
                n_rec += ch[1]
                for r in ch[2]:
                    for _pos, op, _pay in r:
                        n_instr += 1
                        ops[OPS[op][0]] += 1
    print("files:", len(files), " inner classes:", n_inner)
    print("chunks:", dict(nchunks))
    print("TREE records:", n_rec, " instructions:", n_instr)
    print("CONS entry tags:", dict(ncons))
    print("CLSS sizes:", dict(clss_sizes))
    print("top opcodes:", ops.most_common(15))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roundtrip", action="store_true")
    ap.add_argument("--text-roundtrip", action="store_true")
    ap.add_argument("--emit-all", metavar="OUTDIR")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--dis")
    ap.add_argument("--asm")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    if a.roundtrip:
        sys.exit(cmd_roundtrip(False))
    if a.text_roundtrip:
        sys.exit(cmd_roundtrip(True))
    if a.emit_all:
        sys.exit(cmd_emit_all(a.emit_all))
    if a.stats:
        cmd_stats()
        return
    if a.dis:
        data = Path(a.dis).read_bytes()
        ir = parse(data, a.dis)
        txt = to_text(ir)
        if a.out:
            Path(a.out).write_text(txt, encoding="utf-8")
            print(f"wrote {a.out} ({len(txt):,} chars)")
        else:
            sys.stdout.write(txt)
        return
    if a.asm:
        ir = from_text(Path(a.asm).read_text(encoding="utf-8"), a.asm)
        out = serialize(ir)
        if not a.out:
            ap.error("--asm needs -o")
        Path(a.out).write_bytes(out)
        print(f"wrote {a.out} ({len(out):,} bytes)")
        return
    ap.print_help()


if __name__ == "__main__":
    main()
