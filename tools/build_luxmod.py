#!/usr/bin/env python3
"""Compile src/luxmod.c and graft it into Urban Terror 4.3.4's qagame.qvm.

luxmod by LuxXx - https://github.com/LuxXx - https://x.com/luxdav

  build_luxmod.py --urt /path/to/UrbanTerror43/q3ut4 [--install]
  build_luxmod.py --qvm qagame.qvm            (an already extracted stock qagame)

--urt may also come from the LUXMOD_URT environment variable. The stock
qagame.qvm is read from zUrT43_qvm.pk3 there; --install copies the result
(build/zzz_luxmod.pk3) into that folder, plus config/luxmod.cfg if the
folder has no luxmod.cfg yet.

Layout of the grafted VM:
  code:  original code | luxmod code | patched function copies | lux_blob_init
  data:  original DATA + LIT unchanged (so no existing address moves)
  bss:   original BSS | luxmod DATA+LIT+BSS at GRAFT_BASE

The engine only loads DATA+LIT from the file, so luxmod's initialized
data lives in BSS and lux_blob_init (called first by hook_init) writes it.
vmMain's calls to G_InitGame and ConsoleCommand are redirected to
hook_init / hook_console.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qvmdis import OPS, QVM  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLCHAIN = os.path.join(ROOT, "toolchain/build/code")
TOOLS = os.path.join(TOOLCHAIN, "tools")
Q3LCC = os.path.join(TOOLS, "lcc/build-linux-x86_64/q3lcc")
Q3ASM = os.path.join(TOOLS, "asm/q3asm")
SYSCALLS = os.path.join(TOOLCHAIN, "game/g_syscalls.asm")
SOURCE = os.path.join(ROOT, "src/luxmod.c")
CONFIG = os.path.join(ROOT, "config/luxmod.cfg")

# 4.3.4 qagame.qvm facts (find them with tools/qvmdis.py / tools/qvmdecomp.py)
EXPECTED_SIZE = 983300
G_INITGAME = 3877
CONSOLECOMMAND = 262012
HOST_FUNCS = {           # existing qagame functions luxmod calls
    "UT_ClientFromString": 253561,
    "UT_GiveWeapon": 31449,
    "UT_FindWeaponSlot": 31691,
    "UT_GiveItem": 31729,
}
HOST_FUNCS.update({
    "G_Damage": 211239,
    "G_Knockback": 213009,
    "UT_FireHK69": 285971,
    "UT_FireGrenade": 286473,
    "UT_FireSmoke": 287330,
    "UT_FireWeapon": 289235,
    "UT_ClientSpawn": 175123,
    "UT_BulletHit": 292545,
    "UT_GiveGear": 169705,
    "G_TempEntity": 277695,
    "DirToByte": 60317,
    "G_RadiusDamage": 214860,
})
# every "CONST func; CALL" in the stock code is redirected to the hook
REDIRECTS = {
    211239: "hook_damage",
    285971: "hook_fire_hk69",
    286473: "hook_fire_grenade",
    287330: "hook_fire_smoke",
    175123: "hook_spawn",
    292545: "hook_bullet_hit",
    169705: "hook_gear",
}
# Stock functions copied into the graft with hard-coded constants replaced by
# reads of luxmod globals (CONST v -> CONST &global; LOAD4), so lux_reload can
# change them. Callers of the original are redirected to the copy.
# {func: {constant value (int bits): (global symbol, occurrences)}}
F06 = 1058642330  # 0.6f
F03 = 1050253722  # 0.3f
CLONES = {
    148149: {90: ("lux_heal_limit_medkit", 1), 50: ("lux_heal_limit", 1),
             15: ("lux_heal_step", 2)},                      # bandage a teammate
    151254: {F06: ("lux_fall_injury_far", 1), F03: ("lux_fall_injury_medium", 1)},
    49918: {750: ("lux_bandage_time_medkit", 1), 1500: ("lux_bandage_time", 1)},
}
HOOK_SITES = {           # vmMain instruction whose CONST operand is the call target
    30: ("G_InitGame", G_INITGAME, "hook_init"),
    126: ("ConsoleCommand", CONSOLECOMMAND, "hook_console"),
}

OP_ENTER, OP_LEAVE, OP_PUSH, OP_CONST, OP_STORE4 = 3, 4, 6, 8, 32


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or "error" in (r.stdout + r.stderr).lower().replace("0 total errors", ""):
        sys.exit("command failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))


def assemble(asm, out_dir, code_base, data_base, blob_addr):
    host = os.path.join(out_dir, "host.asm")
    with open(host, "w") as f:
        f.write("code\n")
        f.write("equ G_InitGame %d\n" % G_INITGAME)
        f.write("equ ConsoleCommand %d\n" % CONSOLECOMMAND)
        f.write("equ lux_blob_init %d\n" % blob_addr)
        for name, addr in HOST_FUNCS.items():
            f.write("equ %s %d\n" % (name, addr))
    out = os.path.join(out_dir, "graft")
    run([Q3ASM, "-vq3", "-m", "-cb", str(code_base), "-db", str(data_base),
         "-o", out, SYSCALLS, host, asm])
    raw = open(out + ".qvm", "rb").read()
    magic, ninstr, code_off, code_len, data_off, data_len, lit_len, bss_len = \
        struct.unpack_from("<8I", raw)
    syms = {}
    for line in open(out + ".map"):
        seg, val, name = line.split()
        syms[name] = int(val, 16)
    return {
        "ninstr": ninstr - code_base,
        "code": raw[code_off:code_off + code_len],
        "image": raw[data_off:data_off + data_len + lit_len],
        "mem_len": data_len + lit_len + bss_len,
        "syms": syms,
    }


def code_bytes(code, ninstr):
    """Byte length of the first ninstr instructions."""
    pc = 0
    for _ in range(ninstr):
        pc += 1 + OPS[code[pc]][1]
    return pc


BRANCH_OPS = {"EQ", "NE", "LTI", "LEI", "GTI", "GEI", "LTU", "LEU", "GTU", "GEU",
              "EQF", "NEF", "LTF", "LEF", "GTF", "GEF"}
OPNUM = {name: i for i, (name, _) in enumerate(OPS)}


def func_range(vm, start):
    end = start + 1
    while end < len(vm.instrs) and vm.instrs[end][2] != "ENTER":
        end += 1
    return start, end


def clone_size(vm, start, patches):
    s, e = func_range(vm, start)
    n = sum(1 for _, _, op, arg in vm.instrs[s:e] if op == "CONST" and arg in patches)
    return (e - s) + n


def clone_function(vm, start, patches, new_base, syms):
    """Copy a function to new_base, turning patched CONSTs into global loads."""
    s, e = func_range(vm, start)
    ins = vm.instrs[s:e]
    counts = {}
    newidx, n = {}, new_base
    for idx, _, op, arg in ins:
        newidx[idx] = n
        n += 2 if op == "CONST" and arg in patches else 1
    out = bytearray()
    for k, (idx, _, op, arg) in enumerate(ins):
        nxt = ins[k + 1][2] if k + 1 < len(ins) else None
        if op == "JUMP" and ins[k - 1][2] != "CONST":
            sys.exit("func_%d has a computed jump, can't clone" % start)
        if op == "CONST" and arg in patches:
            sym, _ = patches[arg]
            counts[arg] = counts.get(arg, 0) + 1
            out += struct.pack("<BiB", OPNUM["CONST"], syms[sym], OPNUM["LOAD4"])
            continue
        if op in BRANCH_OPS:
            arg = newidx[arg]
        elif op == "CONST" and nxt == "JUMP":
            arg = newidx[arg]
        elif op == "CONST" and nxt == "CALL" and arg == start:
            arg = new_base  # recursion
        size = OPS[OPNUM[op]][1]
        out.append(OPNUM[op])
        if size == 4:
            out += struct.pack("<i", arg)
        elif size == 1:
            out.append(arg)
    for value, (sym, want) in patches.items():
        if counts.get(value, 0) != want:
            sys.exit("func_%d: constant %d found %d times, expected %d"
                     % (start, value, counts.get(value, 0), want))
    return bytes(out), n - new_base


def blob_function(image, base):
    """Bytecode for: void lux_blob_init(void) { *(int*)(base+i) = word; ... }"""
    code = bytearray(struct.pack("<Bi", OP_ENTER, 8))
    n = 1
    for i in range(0, len(image), 4):
        word = struct.unpack_from("<i", image.ljust(i + 4, b"\0"), i)[0]
        if word:
            code += struct.pack("<BiBiB", OP_CONST, base + i, OP_CONST, word, OP_STORE4)
            n += 3
    # void functions still return a value: the caller POPs it
    code += struct.pack("<BBi", OP_PUSH, OP_LEAVE, 8)
    return bytes(code), n + 2


def main():
    ap = argparse.ArgumentParser(description="build zzz_luxmod.pk3")
    ap.add_argument("--urt", default=os.environ.get("LUXMOD_URT"),
                    help="Urban Terror 4.3.4 q3ut4 folder (or set LUXMOD_URT)")
    ap.add_argument("--qvm", help="stock qagame.qvm instead of --urt")
    ap.add_argument("--out", default=os.path.join(ROOT, "build"))
    ap.add_argument("--pk3", default="zzz_luxmod.pk3")
    ap.add_argument("--install", action="store_true", help="copy the pk3 into --urt")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if not os.path.exists(Q3LCC) or not os.path.exists(Q3ASM):
        sys.exit("toolchain missing: run scripts/setup_toolchain.sh first")
    if not a.qvm:
        if not a.urt:
            sys.exit("need --urt /path/to/q3ut4 (or LUXMOD_URT) or --qvm")
        src = os.path.join(a.urt, "zUrT43_qvm.pk3")
        if not os.path.exists(src):
            sys.exit("%s not found - is --urt the q3ut4 folder of UrT 4.3.4?" % src)
        a.qvm = os.path.join(a.out, "stock_qagame.qvm")
        with zipfile.ZipFile(src) as z, open(a.qvm, "wb") as f:
            f.write(z.read("vm/qagame.qvm"))
    if a.install and not a.urt:
        sys.exit("--install needs --urt")

    vm = QVM(a.qvm)
    if len(vm.raw) != EXPECTED_SIZE or vm.magic != 0x12721444:
        sys.exit("expected the stock 4.3.4 qagame.qvm")
    for idx, (name, target, _) in HOOK_SITES.items():
        i, _, op, arg = vm.instrs[idx]
        if op != "CONST" or arg != target or vm.instrs[idx + 1][2] != "CALL":
            sys.exit("hook site %d does not call %s" % (idx, name))

    asm = os.path.join(a.out, "luxmod.asm")
    run([Q3LCC, "-DQ3_VM", "-S", "-Wf-target=bytecode", "-Wf-g", "-o", asm, SOURCE])

    orig_mem = vm.data_len + vm.lit_len + vm.bss_len
    graft_base = (orig_mem + 0xfff) & ~0xfff
    code_base = vm.ninstr

    # pass 1 sizes the graft, pass 2 knows where lux_blob_init lands
    # layout: stock | graft | clones | blob
    g = assemble(asm, a.out, code_base, graft_base, 0)
    clone_at, at = {}, code_base + g["ninstr"]
    for func, patches in CLONES.items():
        clone_at[func] = at
        at += clone_size(vm, func, patches)
    blob_at = at
    g = assemble(asm, a.out, code_base, graft_base, blob_at)
    assert clone_at and min(clone_at.values()) == code_base + g["ninstr"]
    clones = b""
    for func, patches in CLONES.items():
        cb, cn = clone_function(vm, func, patches, clone_at[func], g["syms"])
        clones += cb
        print("cloned func_%d to %d (%d instrs)" % (func, clone_at[func], cn))
    blob, blob_n = blob_function(g["image"], graft_base)

    # q3asm pads code segments to 4 bytes; concatenate exact instruction bytes
    code = bytearray(vm.raw[vm.code_off:vm.code_off + code_bytes(vm.raw[vm.code_off:], vm.ninstr)])
    for idx, (_, _, hook) in HOOK_SITES.items():
        foff = vm.instrs[idx][1] - vm.code_off
        struct.pack_into("<i", code, foff + 1, g["syms"][hook])
    targets = {f: g["syms"][h] for f, h in REDIRECTS.items()}
    targets.update(clone_at)
    redirected = {}
    for k in range(len(vm.instrs) - 1):
        _, foff, op, arg = vm.instrs[k]
        if op == "CONST" and arg in targets and vm.instrs[k + 1][2] == "CALL":
            struct.pack_into("<i", code, foff - vm.code_off + 1, targets[arg])
            redirected[arg] = redirected.get(arg, 0) + 1
    for target in targets:
        if not redirected.get(target):
            sys.exit("no call sites found for func_%d" % target)
        print("redirected %d call(s) of func_%d to %d" % (redirected[target], target, targets[target]))
    code += g["code"][:code_bytes(g["code"], g["ninstr"])]
    code += clones
    code += blob
    code += b"\0" * (-len(code) % 4)

    mem_end = graft_base + g["mem_len"]
    mem_size = 1 << (mem_end - 1).bit_length()
    if mem_size != 1 << (orig_mem - 1).bit_length():
        print("note: VM memory grows to %d MB" % (mem_size >> 20))

    header = struct.pack("<8I", 0x12721444, blob_at + blob_n,
                         32, len(code), 32 + len(code), vm.data_len, vm.lit_len,
                         mem_end - vm.data_len - vm.lit_len)
    out_qvm = os.path.join(a.out, "qagame.qvm")
    with open(out_qvm, "wb") as f:
        f.write(header + code + vm.mem)

    # sanity: the result must decode cleanly and hooks must land on ENTERs
    check = QVM(out_qvm)
    for name in ["hook_init", "hook_console"] + list(REDIRECTS.values()):
        assert check.instrs[g["syms"][name]][2] == "ENTER", name
    assert check.instrs[blob_at][2] == "ENTER"
    for func, at in clone_at.items():
        assert check.instrs[at][2] == "ENTER", func
    pk3 = os.path.join(a.out, a.pk3)
    with zipfile.ZipFile(pk3, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(out_qvm, "vm/qagame.qvm")
    print("graft: %d instrs at %d, %d bytes data at 0x%x, blob %d instrs"
          % (g["ninstr"], code_base, len(g["image"]), graft_base, blob_n))
    print("hook_init=%d hook_console=%d" % (g["syms"]["hook_init"], g["syms"]["hook_console"]))
    print("wrote %s and %s" % (out_qvm, pk3))
    if a.install:
        shutil.copy(pk3, os.path.join(a.urt, a.pk3))
        print("installed %s" % os.path.join(a.urt, a.pk3))
        cfg = os.path.join(a.urt, "luxmod.cfg")
        if not os.path.exists(cfg):
            shutil.copy(CONFIG, cfg)
            print("installed %s" % cfg)
        else:
            print("kept existing %s" % cfg)


if __name__ == "__main__":
    main()
