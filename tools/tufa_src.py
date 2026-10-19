"""M3 finale: strict, losslessly-compilable source format `.lasr.src`.

Pipeline (all steps verified):
    .class --tufa_lift--> AST --strict render--> .lasr.src
    .lasr.src --this parser--> AST --emit--> instructions --tasm--> tufa_ir
              --assemble--> .class'   (must equal .class byte-for-byte)

Design:
  * The pretty tree `out_src/*.lasr.java` (tufa_lift --emit-src) stays ONE-WAY
    for humans. `.lasr.src` is the COMPILABLE source: prefix-form expressions
    with every pool reference as an explicit integer, jumps as instruction
    indices (Goto/IfJump/SwitchCase carry them literally; structured nodes
    IfThen/IfElse/While/DoWhile/Ternary/ShortCircuit DERIVE their indices from
    layout while parsing -- the parser tracks the absolute instruction cursor).
  * Skeleton chunks (CONS/FILD/MTHD/CLSS/TUFA_INNER) reuse the proven .tasm
    line syntax; compiled method bodies are re-rendered as .tasm REC blocks and
    assembled by tufa_ir.from_text -- so the byte-identity guarantee chains
    through the already-proven assembler.
  * Records the lifter cannot structure (107 of 14,879) dump as RAWREC blocks
    (mnemonic + payload per line) -- still source, still byte-exact.

Usage:
  python tools/tufa_src.py --roundtrip           # corpus: dump->compile->compare
  python tools/tufa_src.py --emit-all OUTDIR     # write .lasr.src tree
  python tools/tufa_src.py --dump CLASSFILE      # one class to stdout
  python tools/tufa_src.py --compile SRC -o OUT  # .lasr.src -> .class bytes
"""
import argparse
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import tufa_ir as TI   # noqa: E402
import tufa_lift as TL  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
EX = ROOT / "extracted"


# ================================================================ renderer
def sx(e, dupreg, dupctr):
    """Strict expression rendering -> single-line token string."""
    if isinstance(e, TL.Lit):
        return f"LIT {e.op} {'-' if e.pay is None else e.pay}"
    if isinstance(e, TL.Un):
        return f"UN {e.op} {'-' if e.pay is None else e.pay} {sx(e.x, dupreg, dupctr)}"
    if isinstance(e, TL.Bin):
        return f"BIN {e.op} {sx(e.a, dupreg, dupctr)} {sx(e.b, dupreg, dupctr)}"
    if isinstance(e, TL.FieldGet):
        o = "-" if e.obj is None else sx(e.obj, dupreg, dupctr)
        return f"FIELD {e.op} {e.ref} {o}"
    if isinstance(e, TL.Call):
        r = "-" if e.recv is None else sx(e.recv, dupreg, dupctr)
        args = " ".join(sx(a, dupreg, dupctr) for a in e.args)
        return f"CALL {e.op} {e.ref} {int(e.pushes)} {r} {len(e.args)} {args}".rstrip()
    if isinstance(e, TL.New):
        args = " ".join(sx(a, dupreg, dupctr) for a in (e.args or []))
        return f"NEW {e.ref} {int(e.had_dup)} {e.initref} {len(e.args or [])} {args}".rstrip()
    if isinstance(e, TL.ArrayLit):
        elems = " ".join(sx(x, dupreg, dupctr) for x in e.elems)
        return (f"ARRLIT {e.ref} {len(e.elems)} {sx(e.len, dupreg, dupctr)} {elems}").rstrip()
    if isinstance(e, TL.ArrayNew):
        return f"ARRNEW {e.ref} {sx(e.len, dupreg, dupctr)}"
    if isinstance(e, TL.ArrayAccess):
        return f"ARRGET {sx(e.idx, dupreg, dupctr)} {sx(e.arr, dupreg, dupctr)}"
    if isinstance(e, TL.EmptyDims):
        dims = " ".join(sx(d, dupreg, dupctr) for d in e.dims)
        return f"EMPTYDIMS {e.n} {dims}".rstrip()
    if isinstance(e, TL.Ternary):
        return (f"TERN {e.op} {sx(e.cond, dupreg, dupctr)} {sx(e.fall, dupreg, dupctr)} "
                f"{sx(e.jump, dupreg, dupctr)}")
    if isinstance(e, TL.ShortCircuit):
        return f"SC {e.op} {sx(e.lhs, dupreg, dupctr)} {sx(e.R, dupreg, dupctr)}"
    if isinstance(e, TL.DupRef):
        k = dupreg.get(id(e.src))
        if k is None:
            raise TL.Abort("DupRef src not registered")
        return f"DUPREF {k}"
    raise TL.Abort(f"cannot render expr {type(e).__name__}")


def sstmt(s, dupreg, dupctr, out, ind=1):
    pad = "  " * ind
    if isinstance(s, TL.LocalMgmt):
        out.append(f"{pad}LOCMGMT {s.op} {'-' if s.pay is None else s.pay}")
    elif isinstance(s, TL.Assign):
        out.append(f"{pad}ASSIGN {s.local} {sx(s.e, dupreg, dupctr)}")
    elif isinstance(s, TL.AssignDup):
        k = dupctr[0]; dupctr[0] += 1
        dupreg[id(s.e)] = k
        out.append(f"{pad}ASSIGNDUP {k} {s.local} {sx(s.e, dupreg, dupctr)}")
    elif isinstance(s, TL.PutFieldDup):
        k = dupctr[0]; dupctr[0] += 1
        dupreg[id(s.v)] = k
        o = "-" if s.obj is None else sx(s.obj, dupreg, dupctr)
        out.append(f"{pad}PUTFIELDDUP {k} {s.op} {s.ref} {o} {sx(s.v, dupreg, dupctr)}")
    elif isinstance(s, TL.PutField):
        o = "-" if s.obj is None else sx(s.obj, dupreg, dupctr)
        out.append(f"{pad}PUTFIELD {s.op} {s.ref} {o} {sx(s.v, dupreg, dupctr)}")
    elif isinstance(s, TL.ArrayStore):
        out.append(f"{pad}ARRAYSTORE {sx(s.arr, dupreg, dupctr)} {sx(s.idx, dupreg, dupctr)} {sx(s.v, dupreg, dupctr)}")
    elif isinstance(s, TL.Discard):
        out.append(f"{pad}DISCARD {sx(s.e, dupreg, dupctr)}")
    elif isinstance(s, TL.Delete):
        out.append(f"{pad}DELETE {sx(s.e, dupreg, dupctr)}")
    elif isinstance(s, TL.ExprStmt):
        out.append(f"{pad}EXPRSTMT {sx(s.e, dupreg, dupctr)}")
    elif isinstance(s, TL.Return):
        out.append(f"{pad}RETURN {'-' if s.e is None else sx(s.e, dupreg, dupctr)}")
    elif isinstance(s, TL.Goto):
        out.append(f"{pad}GOTO {s.op} {s.tidx}")
    elif isinstance(s, TL.IfJump):
        out.append(f"{pad}IFJUMP {s.op} {s.tidx} {sx(s.cond, dupreg, dupctr)}")
    elif isinstance(s, TL.SwitchStmt):
        out.append(f"{pad}SWITCH {len(s.cases)} {sx(s.value, dupreg, dupctr)} {{")
        for kexpr, t in s.cases:
            out.append(f"{pad}  CASE {sx(kexpr, dupreg, dupctr)} {t}")
        out.append(f"{pad}}}")
    elif isinstance(s, TL.IfThen):
        out.append(f"{pad}IFTHEN {s.op} {sx(s.cond, dupreg, dupctr)} {{")
        for t in s.then_stmts:
            sstmt(t, dupreg, dupctr, out, ind + 1)
        out.append(f"{pad}}}")
    elif isinstance(s, TL.IfElse):
        out.append(f"{pad}IFELSE {s.op} {sx(s.cond, dupreg, dupctr)} {{")
        for t in s.then_stmts:
            sstmt(t, dupreg, dupctr, out, ind + 1)
        out.append(f"{pad}}} ELSE {{")
        for t in s.else_stmts:
            sstmt(t, dupreg, dupctr, out, ind + 1)
        out.append(f"{pad}}}")
    elif isinstance(s, TL.While):
        out.append(f"{pad}WHILE {s.op} {s.header_idx} {sx(s.cond, dupreg, dupctr)} {{")
        for t in s.body:
            sstmt(t, dupreg, dupctr, out, ind + 1)
        out.append(f"{pad}}}")
    elif isinstance(s, TL.DoWhile):
        out.append(f"{pad}DOWHILE {s.op} {s.header_idx} {{")
        for t in s.body:
            sstmt(t, dupreg, dupctr, out, ind + 1)
        out.append(f"{pad}}} WHILE {sx(s.cond, dupreg, dupctr)}")
    else:
        raise TL.Abort(f"cannot render stmt {type(s).__name__}")


RAW_REASONS = []   # diagnostic sink: (where, ri, reason) for every RAWREC


def dump_record_lines(ri, rec, pool, where="?"):
    """SRCREC (structured) or RAWREC (fallback) lines for one TREE record."""
    try:
        stmts = TL.lift_record(rec, pool)
        if TL.emit_stmts(stmts) == [(op, pay) for _p, op, pay in rec]:
            out = [f"SRCREC {ri}"]
            dupreg, dupctr = {}, [0]
            for s in stmts:
                sstmt(s, dupreg, dupctr, out)
            out.append("ENDREC")
            return out, True
        RAW_REASONS.append((where, ri, "emit mismatch"))
    except TL.Abort as e:
        RAW_REASONS.append((where, ri, f"Abort: {e}"))
    out = [f"RAWREC {ri}"]
    for _pos, op, pay in rec:
        out.append(f"  {TI.OPS[op][0]} {'' if pay is None else pay}")
    out.append("ENDREC")
    return out, False


def dump_src(data, name="<mem>"):
    ir = TI.parse(data, name)
    stats = {"src": 0, "raw": 0}

    def hook(n, recs, cons):
        pool = TL.Pool(cons)
        lines = []
        for ri, rec in enumerate(recs):
            rl, ok = dump_record_lines(ri, rec, pool, where=f"{name}:T{n}")
            lines += rl
            stats["src" if ok else "raw"] += 1
        return lines

    txt = TI.to_text(ir, comments=True, tree_hook=hook)
    return txt, stats


# ================================================================== parser
class SrcError(Exception):
    pass


class Tok:
    def __init__(self, toks, where):
        self.t = toks
        self.i = 0
        self.where = where

    def peek(self):
        if self.i >= len(self.t):
            raise SrcError(f"{self.where}: unexpected end")
        return self.t[self.i]

    def next(self):
        v = self.peek()
        self.i += 1
        return v

    def expect(self, s):
        v = self.next()
        if v != s:
            raise SrcError(f"{self.where}: expected {s!r} got {v!r}")

    def num(self):
        v = self.next()
        return int(v, 16) if v.lower().startswith("0x") else int(v)

    def numdash(self):
        v = self.next()
        return None if v == "-" else (int(v, 16) if v.lower().startswith("0x") else int(v))


class RecParser:
    """Parses one SRCREC body; tracks the absolute instruction cursor so that
    structured nodes can derive their jump-target indices from layout."""

    def __init__(self, toks, where):
        self.tk = Tok(toks, where)
        self.ctr = [0]           # absolute instruction index cursor
        self.dupreg = {}         # k -> expr node

    # ---- expressions ----
    def expr(self):
        t = self.tk
        tag = t.next()
        ctr = self.ctr
        start = ctr[0]
        if tag == "LIT":
            op = t.num(); pay = t.numdash()
            n = TL.Lit(op, pay, at=start)
        elif tag == "UN":
            op = t.num(); pay = t.numdash(); x = self.expr()
            n = TL.Un(op, x, pay)
        elif tag == "BIN":
            op = t.num(); a = self.expr(); b = self.expr()
            n = TL.Bin(op, a, b)
        elif tag == "FIELD":
            op = t.num(); ref = t.num()
            tok = t.peek()
            obj = None if tok == "-" and (t.next() or True) else self.expr()
            n = TL.FieldGet(op, ref, obj, at=start)
        elif tag == "CALL":
            op = t.num(); ref = t.num(); pushes = bool(t.num())
            tok = t.peek()
            recv = None if tok == "-" and (t.next() or True) else self.expr()
            k = t.num()
            args = [self.expr() for _ in range(k)]
            n = TL.Call(op, ref, recv, args, pushes, at=start)
        elif tag == "NEW":
            ref = t.num(); had = bool(t.num()); initref = t.num(); k = t.num()
            ctr[0] += 2 if had else 1        # NEW (+DUP) precede the args
            args = [self.expr() for _ in range(k)]
            n = TL.New(ref, at=start)
            n.initref, n.args, n.done, n.had_dup = initref, args, True, had
        elif tag == "ARRNEW":
            ref = t.num(); ln = self.expr()
            n = TL.ArrayNew(ref, ln, at=start)
        elif tag == "ARRLIT":
            ref = t.num(); cnt = t.num()
            save = ctr[0]
            ln = self.expr()                 # layout: elems FIRST, then INT n
            ctr[0] = save
            elems = [self.expr() for _ in range(cnt)]
            n = TL.ArrayLit(ref, ln, elems)
        elif tag == "ARRGET":
            i2 = self.expr(); a = self.expr()   # layout: idx first, then arr
            n = TL.ArrayAccess(a, i2)
        elif tag == "EMPTYDIMS":
            cnt = t.num(); dims = [self.expr() for _ in range(cnt)]
            n = TL.EmptyDims(cnt, dims, at=start)
        elif tag == "TERN":
            # layout: [cond][jmp][fall][goto][jumparm]; text order matches
            op = t.num()
            cond = self.expr()
            ctr[0] += 1                      # the cond-jump instruction
            fall = self.expr()
            else_idx = ctr[0] + 1            # jump arm starts after fall's goto
            ctr[0] = else_idx
            jump = self.expr()
            end_idx = ctr[0]
            n = TL.Ternary(op, cond, fall, jump, else_idx, end_idx)
        elif tag == "SC":
            # layout: [lhs][SC][jmp][tail]; R shares lhs as its emit prefix
            op = t.num()
            lhs = self.expr()
            l = ctr[0] - start
            ctr[0] = start                   # R re-emits lhs first
            R = self.expr()
            le = lhs.emit()
            re_ = R.emit()
            if re_[:len(le)] != le:
                raise SrcError("SC: lhs not a prefix of R")
            _fix_tail_indices(R, start, l, start)
            end_idx = start + l + 2 + (len(re_) - l)
            n = TL.ShortCircuit(op, lhs, R, end_idx, re_[len(le):])
        elif tag == "DUPREF":
            k = t.num()
            if k not in self.dupreg:
                raise SrcError(f"DUPREF {k} not registered")
            n = TL.DupRef(self.dupreg[k])
        else:
            raise SrcError(f"bad expr tag {tag!r}")
        ctr[0] = start + len(n.emit())
        return n

    # ---- statements ----
    def stmt(self):
        t = self.tk
        ctr = self.ctr
        tag = t.next()
        start = ctr[0]
        if tag == "LOCMGMT":
            s = TL.LocalMgmt(t.num(), t.numdash(), at=start)
        elif tag == "ASSIGN":
            loc = t.num(); e = self.expr()
            s = TL.Assign(loc, e)
        elif tag == "ASSIGNDUP":
            k = t.num(); loc = t.num(); e = self.expr()
            self.dupreg[k] = e
            s = TL.AssignDup(loc, e)
        elif tag in ("PUTFIELD", "PUTFIELDDUP"):
            k = t.num() if tag == "PUTFIELDDUP" else None
            op = t.num(); ref = t.num()
            tok = t.peek()
            obj = None if tok == "-" and (t.next() or True) else self.expr()
            v = self.expr()
            if tag == "PUTFIELD":
                s = TL.PutField(op, ref, obj, v, at=start)
            else:
                self.dupreg[k] = v
                s = TL.PutFieldDup(op, ref, obj, v)
        elif tag == "ARRAYSTORE":
            a = self.expr(); i2 = self.expr(); v = self.expr()
            s = TL.ArrayStore(a, i2, v)
        elif tag == "DISCARD":
            s = TL.Discard(self.expr())
        elif tag == "DELETE":
            s = TL.Delete(self.expr())
        elif tag == "EXPRSTMT":
            s = TL.ExprStmt(self.expr())
        elif tag == "RETURN":
            tok = t.peek()
            e = None if tok == "-" and (t.next() or True) else self.expr()
            s = TL.Return(e, at=start)
        elif tag == "GOTO":
            op = t.num(); tidx = t.num()
            s = TL.Goto(op, tidx, at=start)
        elif tag == "IFJUMP":
            op = t.num(); tidx = t.num(); cond = self.expr()
            s = TL.IfJump(op, cond, tidx)
        elif tag == "SWITCH":
            ncase = t.num(); val = self.expr()
            t.expect("{")
            cases = []
            for _ in range(ncase):
                t.expect("CASE")
                k = self.expr()
                cases.append((k, t.num()))
            t.expect("}")
            s = TL.SwitchStmt(val, cases)
        elif tag == "IFTHEN":
            op = t.num(); cond = self.expr()
            ctr[0] += 1                      # the cond-jump instruction
            then_i0 = ctr[0]
            thenb = self.block()
            s = TL.IfThen(op, cond, thenb, then_i0, ctr[0])
        elif tag == "IFELSE":
            op = t.num(); cond = self.expr()
            ctr[0] += 1                      # cond-jump -> else
            then_i0 = ctr[0]
            thenb = self.block()
            t.expect("ELSE")
            else_i0 = ctr[0] + 1             # skip then's trailing goto
            ctr[0] = else_i0
            elseb = self.block()
            s = TL.IfElse(op, cond, thenb, then_i0, elseb, else_i0, ctr[0])
        elif tag == "WHILE":
            op = t.num(); header = t.num(); cond = self.expr()
            ctr[0] += 1                      # exit jump
            body_i0 = ctr[0]
            body = self.block()
            exit_i = ctr[0] + 1              # skip trailing goto header
            s = TL.While(op, cond, body, body_i0, header, exit_i)
        elif tag == "DOWHILE":
            # layout: [body][cond][jmp header]; text = DOWHILE op hdr { body } WHILE cond
            op = t.num(); header = t.num()
            body = self.block()
            t.expect("WHILE")
            cond = self.expr()
            s = TL.DoWhile(op, body, start, cond, header)
        else:
            raise SrcError(f"bad stmt tag {tag!r}")
        ctr[0] = start + len(s.emit())
        return s

    def block(self):
        self.tk.expect("{")
        out = []
        while self.tk.peek() != "}":
            out.append(self.stmt())
        self.tk.expect("}")
        return out

    def run(self):
        stmts = []
        while self.tk.i < len(self.tk.t):
            stmts.append(self.stmt())
        return stmts


def _blk_len(stmts):
    return sum(len(s.emit()) for s in stmts)


def _fix_tail_indices(node, sc_start, lhs_len, abs_base):
    """Inside an SC's R, nodes belonging to the TAIL (R-offset >= lhs_len) sit
    2 instructions later in the real layout (SC-op + JMP between lhs and tail).
    Shift the absolute jump indices of nested Ternary/SC nodes accordingly."""
    st = getattr(node, "start", None)
    if st is not None and st - sc_start >= lhs_len:
        if isinstance(node, TL.Ternary):
            node.else_idx += 2
            node.end_idx += 2
        elif isinstance(node, TL.ShortCircuit):
            node.end_idx += 2
    for attr in ("x", "a", "b", "cond", "fall", "jump", "lhs", "R", "obj",
                 "recv", "len", "arr", "idx", "e", "src"):
        ch = getattr(node, attr, None)
        if isinstance(ch, TL.Node):
            _fix_tail_indices(ch, sc_start, lhs_len, abs_base)
    for attr in ("args", "elems", "dims"):
        for ch in getattr(node, attr, None) or []:
            if isinstance(ch, TL.Node):
                _fix_tail_indices(ch, sc_start, lhs_len, abs_base)


def compile_src(text, where="<src>"):
    """.lasr.src text -> .class bytes (via tasm -> tufa_ir)."""
    lines = text.splitlines()
    out = []
    i = 0
    n_src = n_raw = 0
    while i < len(lines):
        ln = lines[i]
        head = ln.split()
        if head and head[0] == "SRCREC":
            ri = int(head[1])
            toks = []
            i += 1
            while lines[i].strip() != "ENDREC":
                toks += lines[i].split()
                i += 1
            i += 1
            stmts = RecParser(toks, f"{where}:rec{ri}").run()
            instrs = TL.emit_stmts(stmts)
            out.append(f"REC {ri} {len(instrs)}")
            for op, pay in instrs:
                out.append(f"  {TI.OPS[op][0]}" + ("" if pay is None else f" {pay}"))
            out.append("END")
            n_src += 1
        elif head and head[0] == "RAWREC":
            ri = int(head[1])
            instrs = []
            i += 1
            while lines[i].strip() != "ENDREC":
                p = lines[i].split()
                if p:
                    op, w = TI.BY_NAME[p[0]]
                    pay = int(p[1]) if w == 5 else None
                    instrs.append((op, pay))
                i += 1
            i += 1
            out.append(f"REC {ri} {len(instrs)}")
            for op, pay in instrs:
                out.append(f"  {TI.OPS[op][0]}" + ("" if pay is None else f" {pay}"))
            out.append("END")
            n_raw += 1
        else:
            out.append(ln)
            i += 1
    ir = TI.from_text("\n".join(out) + "\n", where)
    return TI.serialize(ir), n_src, n_raw


# ================================================================= driver
def all_classes():
    return sorted(EX.rglob("*.class"))


def cmd_roundtrip():
    ok = bad = 0
    fails = []
    tot_src = tot_raw = 0
    for f in all_classes():
        data = f.read_bytes()
        rel = str(f.relative_to(EX))
        try:
            txt, st = dump_src(data, rel)
            back, ns, nr = compile_src(txt, rel)
            tot_src += ns
            tot_raw += nr
            if back == data:
                ok += 1
            else:
                bad += 1
                fails.append((rel, f"len {len(back)} vs {len(data)}"))
        except Exception as e:
            bad += 1
            fails.append((rel, f"{type(e).__name__}: {e}"))
    n = ok + bad
    print(f"SOURCE roundtrip (.class -> .lasr.src -> .class): {ok}/{n} byte-identical, {bad} failed")
    print(f"records compiled from structured source: {tot_src}, from RAWREC: {tot_raw}")
    for rel, why in fails[:20]:
        print("  FAIL", rel, "|", why)
    return 0 if bad == 0 else 1


def cmd_emit_all(outdir):
    out = Path(outdir)
    n = n_full = 0
    for f in all_classes():
        rel = f.relative_to(EX)
        dst = out / (str(rel) + ".lasr.src")
        dst.parent.mkdir(parents=True, exist_ok=True)
        txt, st = dump_src(f.read_bytes(), str(rel))
        dst.write_text(txt, encoding="utf-8")
        n += 1
        n_full += (st["raw"] == 0)
    print(f"wrote {n} .lasr.src files to {out} ({n_full} fully structured)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roundtrip", action="store_true")
    ap.add_argument("--emit-all", metavar="OUTDIR")
    ap.add_argument("--dump")
    ap.add_argument("--compile")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    if a.roundtrip:
        sys.exit(cmd_roundtrip())
    if a.emit_all:
        cmd_emit_all(a.emit_all)
    elif a.dump:
        txt, _ = dump_src(Path(a.dump).read_bytes(), a.dump)
        print(txt)
    elif a.compile:
        data, ns, nr = compile_src(Path(a.compile).read_text(encoding="utf-8"), a.compile)
        if not a.out:
            ap.error("--compile needs -o")
        Path(a.out).write_bytes(data)
        print(f"wrote {a.out} ({len(data):,} bytes; {ns} structured + {nr} raw records)")
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
