#!/usr/bin/env python3
"""Turn one QVM function back into C-like statements (expression recovery only).

  qvmdecomp.py qagame.qvm FUNC_INDEX [--syscalls g_syscalls.asm]

Locals print as l<offset>, arguments as a0, a1, ...; weapon table accesses
print as WT[...] with the field offset. No control-flow structuring: branches
stay as gotos.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qvmdis import QVM, annotate_const, load_syscalls  # noqa: E402

BIN = {"ADD": "+", "SUB": "-", "MULI": "*", "MULU": "*", "DIVI": "/", "DIVU": "/",
       "MODI": "%", "MODU": "%", "BAND": "&", "BOR": "|", "BXOR": "^", "LSH": "<<",
       "RSHI": ">>", "RSHU": ">>", "ADDF": "+f", "SUBF": "-f", "MULF": "*f", "DIVF": "/f"}
CMP = {"EQ": "==", "NE": "!=", "LTI": "<", "LEI": "<=", "GTI": ">", "GEI": ">=",
       "LTU": "<u", "LEU": "<=u", "GTU": ">u", "GEU": ">=u", "EQF": "==f",
       "NEF": "!=f", "LTF": "<f", "LEF": "<=f", "GTF": ">f", "GEF": ">=f"}
UN = {"NEGI": "-", "NEGF": "-f", "BCOM": "~", "CVIF": "(float)", "CVFI": "(int)",
      "SEX8": "(char)", "SEX16": "(short)"}


class E:
    """Expression node: text plus the constant value when it is one."""
    def __init__(self, text, const=None):
        self.text, self.const = text, const

    def __str__(self):
        return self.text


def const_text(vm, v, floaty):
    if floaty and abs(v) > 0xffff:
        f = struct.unpack("<f", struct.pack("<i", v))[0]
        return "%gf" % f
    note = annotate_const(vm, v)
    if note and note.startswith('"'):
        return note
    return str(v)


def decompile(vm, start, syscalls):
    ins = vm.instrs
    frame = ins[start][3]
    end = start + 1
    while end < len(ins) and ins[end][2] != "ENTER":
        end += 1
    targets = {i[3] for i in ins[start:end] if i[2] in CMP}
    for k in range(start, end - 1):
        if ins[k][2] == "CONST" and ins[k + 1][2] == "JUMP":
            targets.add(ins[k][3])
    floatops = set(BIN[k] for k in BIN if k.endswith("F"))

    out, stack, args = [], [], []

    def local(off):
        if off >= frame + 8:
            return "a%d" % ((off - frame - 8) // 4)
        return "l%d" % off

    for k in range(start, end):
        idx, _, op, arg = ins[k]
        if idx in targets and idx != start:
            out.append("L%d:" % idx)
        nxt = ins[k + 1][2] if k + 1 < end else None
        if op == "ENTER":
            continue
        if op == "CONST":
            floaty = nxt in ("MULF", "ADDF", "SUBF", "DIVF") or (nxt in CMP and CMP[nxt].endswith("f"))
            if nxt == "CALL":
                stack.append(E(syscalls.get(arg, "sys%d" % arg) if arg < 0 else "func_%d" % arg, arg))
            else:
                stack.append(E(const_text(vm, arg, floaty), arg))
        elif op == "LOCAL":
            stack.append(E("&" + local(arg), None))
        elif op.startswith("LOAD"):
            a = stack.pop()
            stack.append(E(a.text[1:] if a.text.startswith("&") else "*(%s)" % a))
        elif op.startswith("STORE"):
            v, a = stack.pop(), stack.pop()
            lhs = a.text[1:] if a.text.startswith("&") else "*(%s)" % a
            out.append("  %s = %s;" % (lhs, v))
        elif op == "ARG":
            args.append(str(stack.pop()))
        elif op == "CALL":
            t = stack.pop()
            stack.append(E("%s(%s)" % (t, ", ".join(args))))
            args = []
        elif op == "POP":
            if stack:
                e = stack.pop()
                if "(" in e.text:
                    out.append("  %s;" % e)
        elif op == "PUSH":
            stack.append(E("0", 0))
        elif op in BIN:
            b, a = stack.pop(), stack.pop()
            if BIN[op] == "+" and a.const is not None and b.const is not None:
                stack.append(E(str(a.const + b.const), a.const + b.const))
            else:
                stack.append(E("(%s %s %s)" % (a, BIN[op].rstrip("f") if BIN[op] not in floatops else BIN[op][:-1], b)))
        elif op in UN:
            stack.append(E("%s%s" % (UN[op], stack.pop())))
        elif op in CMP:
            b, a = stack.pop(), stack.pop()
            out.append("  if (%s %s %s) goto L%d;" % (a, CMP[op].rstrip("fu"), b, arg))
        elif op == "JUMP":
            t = stack.pop()
            out.append("  goto %s;" % ("L%d" % t.const if t.const is not None else "*" + str(t)))
        elif op == "LEAVE":
            out.append("  return%s;" % (" " + str(stack.pop()) if stack else ""))
        elif op == "BLOCK_COPY":
            s, d = stack.pop(), stack.pop()
            out.append("  memcpy(%s, %s, %d);" % (d, s, arg))
    return out


def prettify(lines):
    """Name a few known structures to make output readable."""
    import re
    rep = [
        (r"\(\((\w+) \* 432\) \+ 4244\)", r"WT[\1].mode"),
        (r"\(\((\w+) \* 52\) \+ WT\[(\w+)\]\.mode\)", r"WT[\2].mode[\1]"),
        (r"\(\((\w+) \* 432\) \+ (\d+)\)", lambda m: "WT[%s]+%d" % (m.group(1), int(m.group(2)) - 0xfb4)
            if 0xfb4 <= int(m.group(2)) < 0xfb4 + 432 else m.group(0)),
    ]
    res = []
    for l in lines:
        prev = None
        while prev != l:
            prev = l
            for pat, r in rep:
                l = re.sub(pat, r, l)
        res.append(l)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("qvm")
    ap.add_argument("func", type=int)
    ap.add_argument("--syscalls")
    a = ap.parse_args()
    vm = QVM(a.qvm)
    for l in prettify(decompile(vm, a.func, load_syscalls(a.syscalls))):
        print(l)


if __name__ == "__main__":
    main()
