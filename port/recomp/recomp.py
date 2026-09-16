"""Static recompiler driver: DOL + symbols -> C++ translation units.

Usage: python recomp.py [--out DIR] [--dol PATH] [--hle hle_list.txt] [--tu-insns N]
Generated code is not committed (see .gitignore); rerun after changing the emitter.
"""
import argparse
from dataclasses import dataclass
import hashlib
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from dol import Dol
from symbols import SymbolMap
from analyze import analyze_all
from emit import Emitter
import gecko

ROOT = Path(__file__).resolve().parents[2]
SLIPPI_SYS = ROOT / "port/slippi_sys"   # Slippi's Sys files (GPL-2.0, from the Slippi Ishiiruka repo), vendored so a clone builds


@dataclass(frozen=True)
class AbsorbedHook:
    hle_name: str
    payload_sha256: str


# HLE implementations may absorb a Gecko hook only when the canonical payload
# is exactly the audited implementation. Any source drift fails generation.
ABSORBED_HOOKS = {
    0x8034DED8: AbsorbedHook(
        "PADControlMotor",
        "1e20989f0e6841111dad179f7086e15365907dc8ce8a96b19d231007bae7a5e0",
    ),
}


def validate_absorbed_hooks(hooks, symbols, hle_funcs,
                            registrations=ABSORBED_HOOKS, require_all=True):
    validated = set()
    for hook in hooks:
        owner = symbols.containing(hook.hook)
        if owner is None or owner.name not in hle_funcs:
            continue
        registration = registrations.get(hook.hook)
        if registration is None:
            raise ValueError(
                "Gecko hook at %08X lands in HLE'd %s but is not registered "
                "as absorbed" % (hook.hook, owner.name)
            )
        if registration.hle_name != owner.name:
            raise ValueError(
                "absorbed Gecko hook at %08X is registered for %s, not %s" %
                (hook.hook, registration.hle_name, owner.name)
            )
        fingerprint = gecko.hook_fingerprint(hook)
        if fingerprint != registration.payload_sha256:
            raise ValueError(
                "absorbed Gecko hook at %08X fingerprint drift: %s != %s" %
                (hook.hook, fingerprint, registration.payload_sha256)
            )
        hook.absorbed_by = owner.name
        validated.add(hook.hook)
    missing = sorted(set(registrations) - validated)
    # The registrations describe the online code set; other sets (playback) need not carry them.
    if missing and require_all:
        raise ValueError(
            "registered absorbed Gecko hook(s) absent or not HLE-owned: %s" %
            ", ".join("%08X" % address for address in missing)
        )


class GeckoSet:
    """Everything the recompiler and runtime need for the Slippi code tables."""
    def __init__(self, gct_base, sys_dir=None, extra_gct=None, extra_base=0):
        sys_dir = Path(sys_dir) if sys_dir else SLIPPI_SYS
        self.hooks, self.caves = [], []
        self.codehandler = (sys_dir / "codehandler.bin").read_bytes()
        self.bootloader = (sys_dir / "bootloader.gct").read_bytes()
        assert len(self.codehandler) == 4288, "unexpected codehandler.bin"
        self.codes = gecko.load_ini(sys_dir / "GameSettings/GALE01r2.ini")
        self.gct, self.optional_offset = gecko.generate_gct(self.codes)
        self.boot = gecko.parse_gct(self.bootloader, gecko.BOOTLOADER_BASE)
        self.gct_base = gct_base
        self.main = gecko.parse_gct(self.gct, gct_base if gct_base else 0x81900000)
        # Playback: the list a replay carries (served over EXI, installed by the Playback code at
        # `extra_base`) is translated too, so its caves run as compiled code.
        self.extra = gecko.parse_gct(Path(extra_gct).read_bytes(), extra_base) if extra_gct else None
        # Run-time optional codes: everything the full table adds over the base table.
        base_gct, _ = gecko.generate_gct(self.codes, include_optional=False)
        base = gecko.parse_gct(base_gct, gct_base if gct_base else 0x81900000)
        base_writes = set(base.writes)
        base_hooks = {h.hook for h in base.hooks}
        flags = [c.optional for c in self.codes if c.enabled and c.optional]
        self.optional_flag = flags[0] if flags else None
        self.optional_flags = sorted(set(flags))
        self.optional_write_list = [(a, b) for (a, b) in self.main.writes if (a, b) not in base_writes]
        for h in self.main.hooks:
            if h.hook not in base_hooks:
                h.optional = self.optional_flag
        self.optional_text = {}    # addr -> (patched word, flag): translated as both variants
        self.optional_data = []    # (addr, patched bytes, original bytes): applied/restored at run time

    def apply(self, dol):
        """Applies the memory writes to the image and collects hooks/caves for translation."""
        text_writes = data_writes = 0
        optional = set(self.optional_write_list)
        for p in [self.boot, self.main] + ([self.extra] if self.extra else []):
            for addr, blob in p.writes:
                if not dol.in_ram(addr) or not dol.in_ram(addr + len(blob) - 1):
                    continue
                if (addr, blob) in optional:
                    # Left out of the image: text becomes a two-way instruction, data is written at run time.
                    if dol.in_text(addr):
                        assert len(blob) == 4, "optional text patch must be one instruction"
                        self.optional_text[addr] = (int.from_bytes(blob, "big"), self.optional_flag)
                    else:
                        original = bytes(dol.u32(addr + k).to_bytes(4, "big")[0] for k in range(len(blob)))
                        self.optional_data.append((addr, blob, original))
                    continue
                dol.write_bytes(addr, blob)
                if dol.in_text(addr):
                    text_writes += 1
                else:
                    data_writes += 1
        all_hooks = []
        for p in [self.boot, self.main] + ([self.extra] if self.extra else []):
            all_hooks.extend(p.hooks)
        self.hooks, duplicates = gecko.canonicalize_hooks(all_hooks)
        for address, identical in duplicates:
            if identical:
                print("info: identical Gecko hooks at %08X; canonical later table wins" % address)
            else:
                print("warning: different Gecko hooks at %08X; the later table wins" % address)
        if self.gct_base:
            self.caves = list(self.main.c0) + list(self.boot.c0) + (list(self.extra.c0) if self.extra else [])
        return text_writes, data_writes

    def enabled_names(self):
        return [c.name for c in self.codes if c.enabled]


def write_gecko_data(out, gs):
    def cbytes(name, blob):
        rows = []
        for i in range(0, len(blob), 24):
            rows.append("  " + ", ".join("0x%02X" % b for b in blob[i:i + 24]) + ",")
        return "const uint8_t %s[%d] = {\n%s\n};\nconst size_t %s_size = %d;\n" % (name, len(blob), "\n".join(rows), name, len(blob))
    text = ["// Generated by port/recomp/recomp.py from slippi/Data/Sys. Do not edit.\n#include \"gecko_data.h\"\nnamespace gecko {\n"]
    text.append(cbytes("codehandler_bin", gs.codehandler))
    text.append(cbytes("bootloader_gct", gs.bootloader))
    text.append(cbytes("slippi_gct", gs.gct))
    blobs = []
    for i, (addr, blob) in enumerate(gs.boot.writes):
        blobs.append("static const uint8_t boot_write_%d[] = {%s};\n" % (i, ", ".join("0x%02X" % b for b in blob)))
    text.extend(blobs)
    text.append("const Write boot_writes[] = {\n%s\n};\nconst size_t boot_writes_count = %d;\n" % (
        "\n".join("  {0x%08Xu, %du, boot_write_%d}," % (addr, len(blob), i) for i, (addr, blob) in enumerate(gs.boot.writes)) or "  {0, 0, nullptr},",
        len(gs.boot.writes)))
    text.append("const HookInstall boot_hooks[] = {\n%s\n};\nconst size_t boot_hooks_count = %d;\n" % (
        "\n".join("  {0x%08Xu, 0x%08Xu, %du}," % (h.hook, h.cave_addr, len(h.words)) for h in gs.boot.hooks) or "  {0, 0, 0},",
        len(gs.boot.hooks)))
    text.append("const uint32_t gct_base_used = 0x%08Xu;\n" % (gs.gct_base or 0))
    text.append("const uint32_t optional_gct_offset = %du;\n" % gs.optional_offset)
    for flag in gs.optional_flags:
        text.append("bool option_%s = false;\n" % flag)
    for i, (addr, patched, original) in enumerate(gs.optional_data):
        text.append("static const uint8_t optional_patched_%d[] = {%s};\nstatic const uint8_t optional_original_%d[] = {%s};\n" % (
            i, ", ".join("0x%02X" % b for b in patched), i, ", ".join("0x%02X" % b for b in original)))
    text.append("const OptionalWrite optional_writes[] = {\n%s\n};\nconst size_t optional_writes_count = %d;\n" % (
        "\n".join("  {0x%08Xu, %du, optional_patched_%d, optional_original_%d}," % (addr, len(patched), i, i) for i, (addr, patched, original) in enumerate(gs.optional_data)) or "  {0, 0, nullptr, nullptr},",
        len(gs.optional_data)))
    text.append("}\n")
    return write_if_changed(out / "gecko_data.cpp", "".join(text))


MACRO_LIKE = {"errno", "stdin", "stdout", "stderr", "NULL", "EOF", "min", "max", "TRUE", "FALSE", "BOOL",
              "assert", "offsetof", "alloca", "environ", "abs", "unix", "linux", "far", "near", "pascal",
              "interface", "small", "hyper"}


def c_ident(name):
    s = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not s or s[0].isdigit():
        s = "_" + s
    # CRT/Windows macros: _errno, errno, stdin... Prefix so the header never expands a macro.
    if s in MACRO_LIKE or re.match(r"^_[a-z]", s):
        s = "s" + s if s.startswith("_") else "s_" + s
    return s


def write_if_changed(path, text):
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.write_text(text, encoding="utf-8")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "port/generated"))
    ap.add_argument("--dol", default=str(ROOT / "melee/orig/GALE01/sys/main.dol"))
    ap.add_argument("--symbols", default=str(ROOT / "melee/config/GALE01/symbols.txt"))
    ap.add_argument("--hle", default=str(ROOT / "port/recomp/hle_list.txt"))
    ap.add_argument("--tu-insns", type=int, default=7000)
    ap.add_argument("--no-slippi", action="store_true", help="translate the vanilla game without the Slippi code tables")
    ap.add_argument("--extra-gct", help="playback: the replay code list (gecko_list.bin the port wrote) to translate as well")
    ap.add_argument("--extra-gct-base", default="0", help="guest address where the Playback code installs that list (from the port log)")
    ap.add_argument("--sys-dir", default=str(SLIPPI_SYS), help="Slippi Sys folder with the code list to bake (port/slippi_sys, or port/slippi_sys_playback for the playback build)")
    ap.add_argument("--gct-base", default="0", help="guest address where the game loads the main GCT (from a previous run's log); "
                                                     "enables translation of C0 caves at their real addresses")
    args = ap.parse_args()

    t0 = time.time()
    if hashlib.sha1(Path(args.dol).read_bytes()).hexdigest() != "08e0bf20134dfcb260699671004527b2d6bb1a45":
        ap.error("this port requires the verified vanilla Melee NTSC 1.02 DOL")
    dol = Dol(args.dol)
    symbols = SymbolMap(args.symbols)
    gs = None
    if not args.no_slippi:
        gs = GeckoSet(int(args.gct_base, 0), args.sys_dir, args.extra_gct, int(args.extra_gct_base, 0))
        text_writes, data_writes = gs.apply(dol)
        print("slippi: %d codes enabled (%s); GCT %d bytes; %d hooks, %d C0 caves%s; %d text + %d data writes; %d unsupported lines" % (
            len(gs.enabled_names()), ", ".join(gs.enabled_names()), len(gs.gct), len(gs.hooks), len(gs.main.c0) + len(gs.boot.c0),
            "" if gs.gct_base else " (skipped: pass --gct-base)", text_writes, data_writes,
            len(gs.boot.unsupported) + len(gs.main.unsupported)))
        print("slippi: run-time optional codes: %s; %d two-way instructions, %d optional hooks, %d run-time data writes, table offset %d" % (
            ", ".join(gs.optional_flags) or "none", len(gs.optional_text), sum(1 for h in gs.hooks if h.optional), len(gs.optional_data), gs.optional_offset))
        for idx, a, b in (gs.boot.unsupported + gs.main.unsupported)[:10]:
            print("  unsupported Gecko line %d: %08X %08X" % (idx, a, b))
    hle = set()
    with open(args.hle, encoding="utf-8") as hle_source:
        for line in hle_source:
            line = line.split("#", 1)[0].strip()
            if line:
                hle.add(line)
    missing = sorted(n for n in hle if n not in symbols.by_name)
    if missing:
        print("warning: HLE names not in symbol map (they will be recompiled instead of hooked):", ", ".join(missing))
    hle_funcs = {n for n in hle if n in symbols.by_name}
    if gs is not None:
        try:
            validate_absorbed_hooks(gs.hooks, symbols, hle_funcs,
                                    require_all=Path(args.sys_dir).resolve() == Path(SLIPPI_SYS).resolve())
        except ValueError as error:
            ap.error(str(error))
    infos, extra, thunks = analyze_all(dol, symbols, gs)
    if gs is not None:
        for h in gs.hooks:
            if h.absorbed_by is not None:
                print("slippi: Gecko hook at %08X absorbed by HLE'd %s (%s)" %
                      (h.hook, h.absorbed_by, gecko.hook_fingerprint(h)))
                continue
            owner = symbols.containing(h.hook)
            if owner is not None and owner.name in hle_funcs:
                raise AssertionError("unvalidated HLE/Gecko overlap")
        Path(args.out).mkdir(parents=True, exist_ok=True)
        write_gecko_data(Path(args.out), gs)

    for t, owner in list(thunks.items()):
        if symbols.by_addr[owner].name in hle_funcs:
            print("warning: entry %08X lands in HLE'd %s; dropped" % (t, symbols.by_addr[owner].name))
            del thunks[t]
    func_names = {addr: "f_%08X" % addr for addr in infos}
    emitter = Emitter(dol, symbols, infos, hle_funcs, func_names)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    changed = 0
    if gs is None:
        changed += write_if_changed(out / "gecko_data.cpp", "// Generated: no Slippi code tables (translated with --no-slippi).\n#include \"gecko_data.h\"\nnamespace gecko {\n"
                                    "const uint8_t codehandler_bin[1] = {0}; const size_t codehandler_bin_size = 0;\nconst uint8_t bootloader_gct[1] = {0}; const size_t bootloader_gct_size = 0;\n"
                                    "const uint8_t slippi_gct[1] = {0}; const size_t slippi_gct_size = 0;\n"
                                    "const Write boot_writes[1] = {{0, 0, nullptr}}; const size_t boot_writes_count = 0;\n"
                                    "const HookInstall boot_hooks[1] = {{0, 0, 0}}; const size_t boot_hooks_count = 0;\nconst uint32_t gct_base_used = 0;\n"
                                    "const uint32_t optional_gct_offset = 0; bool option_widescreen = false;\n"
                                    "const OptionalWrite optional_writes[1] = {{0, 0, nullptr, nullptr}}; const size_t optional_writes_count = 0;\n}\n")

    # Prototypes for every function.
    text = ["// Generated by port/recomp/recomp.py. Do not edit.\n#pragma once\n#include \"ppc.h\"\n",
            "#include \"hle_decls.h\"\nnamespace guest {\n"]
    for addr in sorted(infos):
        text.append("void %s(ppc::Context&, uint8_t*);\n" % func_names[addr])
    text.append("struct FnEntry { uint32_t addr; ppc::Fn fn; };\n")
    text.append("extern const FnEntry fn_table[];\nextern const size_t fn_table_count;\n}\n")
    changed += write_if_changed(out / "functions.h", "".join(text))

    # HLE declarations.
    text = ["// Generated. HLE overrides referenced by generated code.\n#pragma once\n#include \"ppc.h\"\nnamespace hle {\n"]
    for name in sorted(hle_funcs):
        text.append("void %s(ppc::Context& c, uint8_t* m);\n" % name)
    text.append("}\n")
    changed += write_if_changed(out / "hle_decls.h", "".join(text))

    # Guest symbol addresses for host code (functions and objects).
    text = ["// Generated guest symbol addresses.\n#pragma once\n#include <cstdint>\nnamespace gs {\n"]
    used = set()
    for addr, name in sorted(symbols.names.items()):
        ident = c_ident(name)
        if ident in used:
            ident = "%s_%08X" % (ident, addr)
        used.add(ident)
        text.append("constexpr uint32_t %s = 0x%08Xu;\n" % (ident, addr))
    text.append("}\n")
    changed += write_if_changed(out / "guest_symbols.h", "".join(text))

    # Translation units.
    ordered = sorted(infos)
    tu_index = 0
    count = 0
    chunk = []
    written = []

    def flush():
        nonlocal tu_index, chunk, count, changed
        if not chunk:
            return
        path = out / ("guest_%03d.cpp" % tu_index)
        body = ("// Generated by port/recomp/recomp.py from main.dol. Do not edit.\n"
                "#include \"functions.h\"\n#include \"gecko_data.h\"\nnamespace guest {\n" + "".join(chunk) + "}\n")
        if "gx::RenderObserver" in body:
            body = '#include "render_observer.h"\n' + body
        changed += write_if_changed(path, body)
        written.append(path)
        tu_index += 1
        chunk = []
        count = 0

    for addr in ordered:
        info = infos[addr]
        chunk.append(emitter.emit_function(info))
        chunk.append("\n")
        count += len(info.insns)
        if count >= args.tu_insns:
            flush()
    flush()
    for old in out.glob("guest_*.cpp"):
        if old not in written and old.name != "guest_table.cpp":
            old.unlink()

    text = ["#include \"functions.h\"\nnamespace guest {\n"]
    for addr in sorted(thunks):
        text.append("static void t_%08X(ppc::Context& c, uint8_t* m) { c.entry = 0x%08Xu; %s(c, m); }\n" % (addr, addr, func_names[thunks[addr]]))
    text.append("const FnEntry fn_table[] = {\n")
    for addr in sorted(set(ordered) | set(thunks)):
        text.append("  {0x%08Xu, &%s},\n" % (addr, func_names[addr] if addr in infos else "t_%08X" % addr))
    text.append("};\nconst size_t fn_table_count = sizeof(fn_table) / sizeof(fn_table[0]);\n}\n")
    changed += write_if_changed(out / "guest_table.cpp", "".join(text))

    text = ["set(GUEST_SOURCES\n"]
    for p in written:
        text.append("  \"${CMAKE_CURRENT_LIST_DIR}/%s\"\n" % p.name)
    text.append("  \"${CMAKE_CURRENT_LIST_DIR}/guest_table.cpp\"\n)\n")
    changed += write_if_changed(out / "guest_sources.cmake", "".join(text))

    # Names for diagnostics.
    text = ["#include <cstdint>\n#include <cstddef>\nnamespace guest {\nstruct NameEntry { uint32_t addr; const char* name; };\n",
            "extern const NameEntry name_table[];\nextern const size_t name_table_count;\n",
            "const NameEntry name_table[] = {\n"]
    for func in symbols.functions:
        text.append("  {0x%08Xu, \"%s\"},\n" % (func.addr, func.name.replace("\\", "\\\\").replace('"', '\\"')))
    text.append("};\nconst size_t name_table_count = sizeof(name_table) / sizeof(name_table[0]);\n}\n")
    changed += write_if_changed(out / "function_names.cpp", "".join(text))

    digest = hashlib.sha1(dol.ram).hexdigest()[:12]
    print("generated %d TUs, %d functions, %d HLE overrides, %d files changed, image %s, %.1fs" % (
        len(written), len(infos), len(hle_funcs), changed, digest, time.time() - t0))


if __name__ == "__main__":
    main()
