#!/usr/bin/env python3
"""Quake 3 QVM disassembler with string/syscall annotation.

Usage: qvmdis.py qagame.qvm [-o out.asm] [--syscalls g_syscalls.asm]

Memory layout (per q3vm spec): DATA at address 0, LIT right after it,
BSS after LIT. Code addresses are instruction indices.
"""
import argparse
import re
import struct
import sys

OPS = [
    ("UNDEF", 0), ("IGNORE", 0), ("BREAK", 0), ("ENTER", 4), ("LEAVE", 4),
    ("CALL", 0), ("PUSH", 0), ("POP", 0), ("CONST", 4), ("LOCAL", 4),
    ("JUMP", 0), ("EQ", 4), ("NE", 4), ("LTI", 4), ("LEI", 4), ("GTI", 4),
    ("GEI", 4), ("LTU", 4), ("LEU", 4), ("GTU", 4), ("GEU", 4), ("EQF", 4),
    ("NEF", 4), ("LTF", 4), ("LEF", 4), ("GTF", 4), ("GEF", 4), ("LOAD1", 0),
    ("LOAD2", 0), ("LOAD4", 0), ("STORE1", 0), ("STORE2", 0), ("STORE4", 0),
    ("ARG", 1), ("BLOCK_COPY", 4), ("SEX8", 0), ("SEX16", 0), ("NEGI", 0),
    ("ADD", 0), ("SUB", 0), ("DIVI", 0), ("DIVU", 0), ("MODI", 0),
    ("MODU", 0), ("MULI", 0), ("MULU", 0), ("BAND", 0), ("BOR", 0),
    ("BXOR", 0), ("BCOM", 0), ("LSH", 0), ("RSHI", 0), ("RSHU", 0),
    ("NEGF", 0), ("ADDF", 0), ("SUBF", 0), ("DIVF", 0), ("MULF", 0),
    ("CVIF", 0), ("CVFI", 0),
]
BRANCHES = {"EQ", "NE", "LTI", "LEI", "GTI", "GEI", "LTU", "LEU", "GTU",
            "GEU", "EQF", "NEF", "LTF", "LEF", "GTF", "GEF"}


class QVM:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        (self.magic, self.ninstr, self.code_off, self.code_len, self.data_off,
         self.data_len, self.lit_len, self.bss_len) = struct.unpack_from("<8I", self.raw)
        if self.magic not in (0x12721444, 0x12721445):
            raise ValueError("bad magic %08x" % self.magic)
        # Initialized memory image: DATA then LIT
        self.mem = self.raw[self.data_off:self.data_off + self.data_len + self.lit_len]
        self.instrs = self._decode()

    def _decode(self):
        """List of (index, file_offset, opname, operand)."""
        out = []
        pc = self.code_off
        end = self.code_off + self.code_len
        for i in range(self.ninstr):
            op = self.raw[pc]
            name, size = OPS[op]
            if size == 4:
                arg = struct.unpack_from("<i", self.raw, pc + 1)[0]
            elif size == 1:
                arg = self.raw[pc + 1]
            else:
                arg = None
            out.append((i, pc, name, arg))
            pc += 1 + size
        assert pc <= end, "decoded past code segment"
        return out

    def string_at(self, addr):
        """Return the C string at a LIT address, or None."""
        lo = self.data_len
        hi = self.data_len + self.lit_len
        if not (lo <= addr < hi):
            return None
        if addr > lo and self.mem[addr - 1] != 0:
            return None  # points into the middle of a string
        e = self.mem.find(b"\0", addr)
        s = self.mem[addr:e]
        try:
            return s.decode("latin-1")
        except Exception:
            return None

    def word(self, addr):
        return struct.unpack_from("<i", self.mem, addr)[0]

    def functions(self):
        """Yield (start_index, end_index_exclusive) per ENTER."""
        starts = [i for i, _, n, _ in self.instrs if n == "ENTER"]
        for a, b in zip(starts, starts[1:] + [len(self.instrs)]):
            yield a, b


def load_syscalls(path):
    names = {}
    if not path:
        return names
    for line in open(path):
        m = re.match(r"\s*equ\s+(\w+)\s+(-?\d+)", line)
        if m:
            names[int(m.group(2))] = m.group(1)
    return names


def annotate_const(vm, val):
    s = vm.string_at(val)
    if s is not None:
        return '"%s"' % s.replace("\n", "\\n")[:80]
    if 0 <= val < vm.data_len:
        return "data[0x%x]" % val
    if vm.data_len + vm.lit_len <= val < vm.data_len + vm.lit_len + vm.bss_len:
        return "bss[0x%x]" % val
    return None


def disassemble(vm, syscalls, out):
    fstart = {a for a, _ in vm.functions()}
    targets = set()
    for i, (idx, _, name, arg) in enumerate(vm.instrs):
        if name in BRANCHES:
            targets.add(arg)
    w = out.write
    w("; magic %08x  instrs %d  data %d  lit %d  bss %d\n" %
      (vm.magic, vm.ninstr, vm.data_len, vm.lit_len, vm.bss_len))
    ins = vm.instrs
    for k, (idx, foff, name, arg) in enumerate(ins):
        if idx in fstart:
            w("\nfunc_%d:\n" % idx)
        elif idx in targets:
            w("L%d:\n" % idx)
        txt = name if arg is None else "%s %d" % (name, arg)
        note = ""
        if name == "CONST":
            nxt = ins[k + 1][2] if k + 1 < len(ins) else None
            if nxt == "CALL":
                if arg < 0:
                    note = "syscall %s" % syscalls.get(arg, arg)
                else:
                    note = "-> func_%d" % arg
            elif nxt == "JUMP":
                note = "-> L%d" % arg
            else:
                note = annotate_const(vm, arg) or ""
                if not note and -1e6 < arg < 1e6:
                    pass
                elif not note:
                    f = struct.unpack("<f", struct.pack("<i", arg))[0]
                    if 1e-4 < abs(f) < 1e7:
                        note = "float %g" % f
        w("  %06d @%06x  %-20s%s\n" % (idx, foff, txt, ("; " + note) if note else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("qvm")
    ap.add_argument("-o", "--out")
    ap.add_argument("--syscalls")
    a = ap.parse_args()
    vm = QVM(a.qvm)
    out = open(a.out, "w") if a.out else sys.stdout
    disassemble(vm, load_syscalls(a.syscalls), out)


if __name__ == "__main__":
    main()
