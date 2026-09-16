"""C++ emitter: one host function per guest function, explicit portable numeric helpers."""
from gekko import LOAD_OPS, STORE_OPS


def R(n):
    return "c.r[%d]" % n


def F0(n):
    return "c.f[%d].ps0" % n


def F1(n):
    return "c.f[%d].ps1" % n


def U0(n):
    return "c.f[%d].u0" % n


def U1(n):
    return "c.f[%d].u1" % n


def hexs(v):
    v &= 0xFFFFFFFF
    return "0x%Xu" % v


def imm(v):
    """Signed immediate as a C++ literal added to uint32 arithmetic."""
    return ("+ %du" % v) if v >= 0 else ("- %du" % (-v))


def ea_d(f):
    """Effective address for D-form: (ra|0) + simm."""
    if f["ra"] == 0:
        return hexs(f["simm"])
    return "(%s %s)" % (R(f["ra"]), imm(f["simm"]))


def ea_x(f):
    if f["ra"] == 0:
        return R(f["rb"])
    return "(%s + %s)" % (R(f["ra"]), R(f["rb"]))


def ea_psq(f):
    if f["ra"] == 0:
        return hexs(f["simm12"])
    return "(%s %s)" % (R(f["ra"]), imm(f["simm12"]))


def cond_expr(bo, bi):
    parts = []
    if not (bo & 4):
        parts.append("--c.ctr %s 0" % ("==" if bo & 2 else "!="))
    if not (bo & 16):
        parts.append("%s(c.cr[%d] & %d)" % ("" if bo & 8 else "!", bi >> 2, 8 >> (bi & 3)))
    return " && ".join(parts) if parts else None


class Emitter:
    def __init__(self, dol, symbols, infos, hle_names, func_names):
        self.dol = dol
        self.symbols = symbols
        self.infos = infos
        self.hle = hle_names
        self.func_names = func_names  # addr -> C identifier
        self.referenced = set()

    def fname(self, addr):
        self.referenced.add(addr)
        return self.func_names[addr]

    # ------------------------------------------------------------------
    def emit_function(self, info):
        self.cur_info = info
        func = info.func
        out = []
        name = self.func_names[func.addr]
        out.append("// %s @ %08x size %x" % (func.name, func.addr, func.size))
        if func.name in self.hle:
            out.append("void %s(ppc::Context& c, uint8_t* m) { c.entry = 0; hle::%s(c, m); }" % (name, func.name))
            return "\n".join(out) + "\n"
        out.append("void %s(ppc::Context& __restrict c, uint8_t* __restrict m) {" % name)
        out.append("  ppc::enter(c, %s);" % hexs(func.addr))
        observer = {"HSD_JObjAlloc": "AllocateJoint", "JObjRelease": "ReleaseJoint",
                    "HSD_JObjDisp": "DisplayJoint", "SetupRigidModelMtx": "RigidMatrix",
                    "SetupSharedVtxModelMtx": "OtherMatrix", "SetupEnvelopeModelMtx": "EnvelopeMatrix"}.get(func.name)
        if observer:
            out.append("  gx::RenderObserver render_observer(c, gx::Observe::%s, m);" % observer)
        if info.has_blrl:
            # blrl jumps to LR and re-links: when LR is still this invocation's return address the
            # instruction is a return that leaves a new LR behind (Slippi's helper-table trick).
            out.append("  const uint32_t entry_lr = c.lr;")
        if info.setjmp_returns:
            # __longjmp throws; this function called __setjmp, so catch, restore and re-enter at
            # the saved return address through the entry dispatch (a goto cannot enter a try block).
            out.append("  for (;;) { try {")
        if info.entries:
            # Dispatch thunks set c.entry before calling; a plain if-chain (a switch with gotos
            # trips the MSVC backend).
            out.append("  if (c.entry) { const uint32_t e = c.entry; c.entry = 0;")
            for e in sorted(info.entries):
                out.append("    if (e == %s) goto L_%08X;" % (hexs(e), e))
            out.append("    ppc::fatal(c, \"bad function entry\", e); }")
        for idx, ins in enumerate(info.insns):
            addr = info.addrs[idx]
            if addr in info.labels:
                out.append("L_%08X:" % addr)
            alias = info.aliases.get(addr)
            if alias is not None and alias in info.labels:
                out.append("L_%08X:" % alias)
            if alias is not None and alias in info.optional_hooks:
                # Run-time optional cave: with the option off the hooked instruction runs as
                # shipped and execution resumes after it, skipping the cave entirely.
                orig, flag = info.optional_hooks[alias]
                out.append("  if (!gecko::option_%s) { %s goto L_%08X; }  // optional hook %08x" % (
                    flag, self.emit_insn(info, idx, orig) if orig is not None else "", alias + 4, alias))
            if ins is None:
                out.append("  ppc::fatal(c, \"undecodable instruction\", %s);" % hexs(addr))
                continue
            try:
                line = self.emit_insn(info, idx, ins)
                if addr in info.optional_text:
                    patched, flag = info.optional_text[addr]
                    line = "if (gecko::option_%s) { %s } else { %s }" % (flag, self.emit_insn(info, idx, patched) if patched is not None else "", line)
            except KeyError as e:
                raise RuntimeError("emit failed at %08x %s: %s" % (addr, ins.op, e))
            out.append("  " + line + "  // %08x" % addr if line else "  // %08x %s" % (addr, ins.op))
        if info.setjmp_returns:
            out.append("  } catch (ppc::GuestLongJmp& j) {")
            out.append("    const uint32_t ret = ppc::ld32(c, m, j.buf);")
            out.append("    if (!(%s)) throw;" % " || ".join("ret == %s" % hexs(r) for r in sorted(info.setjmp_returns)))
            out.append("    ppc::longjmp_restore(c, m, j.buf, j.val); c.entry = ret;")
            out.append("  } }")
        out.append("}")
        return "\n".join(out) + "\n"

    # ------------------------------------------------------------------
    def rc(self, ins, dest):
        return " ppc::cr0(c, %s);" % dest if ins.rc else ""

    def emit_insn(self, info, idx, ins):
        op, f = ins.op, ins.f
        func = info.func
        rd, ra, rb, rs = R(f["rd"]), R(f["ra"]), R(f["rb"]), R(f["rs"])
        A = "(%s)" % ("0u" if f["ra"] == 0 else ra)
        # ---------------- integer immediates ----------------
        if op == "addi":
            return "%s = %s %s;" % (rd, "0u" if f["ra"] == 0 else ra, imm(f["simm"]))
        if op == "addis":
            return "%s = %s + %s;" % (rd, "0u" if f["ra"] == 0 else ra, hexs(f["simm"] << 16))
        if op == "addic":
            return "{ uint32_t a = %s; %s = a + %s; c.ca = ppc::carry(a, %s); }" % (ra, rd, hexs(f["simm"]), hexs(f["simm"]))
        if op == "addic_rc":
            return "{ uint32_t a = %s; %s = a + %s; c.ca = ppc::carry(a, %s); ppc::cr0(c, %s); }" % (ra, rd, hexs(f["simm"]), hexs(f["simm"]), rd)
        if op == "subfic":
            return "{ uint32_t a = %s; %s = %s - a; c.ca = (a == 0) || ppc::carry(0u - a, %s); }" % (ra, rd, hexs(f["simm"]), hexs(f["simm"]))
        if op == "mulli":
            return "%s = %s * %s;" % (rd, ra, hexs(f["simm"]))
        if op == "cmpi":
            return "ppc::cr_set_s(c, %d, (int32_t)%s, %d);" % (f["crfd"], ra, f["simm"])
        if op == "cmpli":
            return "ppc::cr_set_u(c, %d, %s, %s);" % (f["crfd"], ra, hexs(f["uimm"]))
        if op == "cmp":
            return "ppc::cr_set_s(c, %d, (int32_t)%s, (int32_t)%s);" % (f["crfd"], ra, rb)
        if op == "cmpl":
            return "ppc::cr_set_u(c, %d, %s, %s);" % (f["crfd"], ra, rb)
        if op == "ori":
            return "%s = %s | %s;" % (ra, rs, hexs(f["uimm"]))
        if op == "oris":
            return "%s = %s | %s;" % (ra, rs, hexs(f["uimm"] << 16))
        if op == "xori":
            return "%s = %s ^ %s;" % (ra, rs, hexs(f["uimm"]))
        if op == "xoris":
            return "%s = %s ^ %s;" % (ra, rs, hexs(f["uimm"] << 16))
        if op == "andi_rc":
            return "%s = %s & %s; ppc::cr0(c, %s);" % (ra, rs, hexs(f["uimm"]), ra)
        if op == "andis_rc":
            return "%s = %s & %s; ppc::cr0(c, %s);" % (ra, rs, hexs(f["uimm"] << 16), ra)
        if op in ("twi", "tw"):
            return None
        # ---------------- integer register ops ----------------
        simple = {"add": "%s + %s", "subf": "%s - %s", "mullw": "%s * %s",
                  "and": "%s & %s", "or": "%s | %s", "xor": "%s ^ %s", "nand": "~(%s & %s)",
                  "nor": "~(%s | %s)", "eqv": "~(%s ^ %s)", "andc": "%s & ~%s", "orc": "%s | ~%s"}
        if op in ("add", "subf", "mullw"):
            expr = simple[op] % ((rb, ra) if op == "subf" else (ra, rb))
            return "%s = %s;%s" % (rd, expr, self.rc(ins, rd))
        if op in ("and", "or", "xor", "nand", "nor", "eqv", "andc", "orc"):
            return "%s = %s;%s" % (ra, simple[op] % (rs, rb), self.rc(ins, ra))
        if op == "neg":
            return "%s = 0u - %s;%s" % (rd, ra, self.rc(ins, rd))
        if op == "mulhw":
            return "%s = ppc::mulhw(%s, %s);%s" % (rd, ra, rb, self.rc(ins, rd))
        if op == "mulhwu":
            return "%s = (uint32_t)(((uint64_t)%s * (uint64_t)%s) >> 32);%s" % (rd, ra, rb, self.rc(ins, rd))
        if op == "divw":
            return "%s = ppc::divw((int32_t)%s, (int32_t)%s);%s" % (rd, ra, rb, self.rc(ins, rd))
        if op == "divwu":
            return "%s = ppc::divwu(%s, %s);%s" % (rd, ra, rb, self.rc(ins, rd))
        if op == "addc":
            return "{ uint32_t a = %s, b = %s; %s = a + b; c.ca = ppc::carry(a, b); }%s" % (ra, rb, rd, self.rc(ins, rd))
        if op == "adde":
            return ("{ uint32_t a = %s, b = %s, k = c.ca; %s = a + b + k; "
                    "c.ca = ppc::carry(a, b) || (k && ppc::carry(a + b, k)); }%s") % (ra, rb, rd, self.rc(ins, rd))
        if op == "addze":
            return "{ uint32_t a = %s, k = c.ca; %s = a + k; c.ca = ppc::carry(a, k); }%s" % (ra, rd, self.rc(ins, rd))
        if op == "addme":
            return "{ uint32_t a = %s, k = c.ca; %s = a + k - 1u; c.ca = ppc::carry(a, k - 1u); }%s" % (ra, rd, self.rc(ins, rd))
        if op == "subfc":
            return "{ uint32_t a = %s, b = %s; %s = b - a; c.ca = (a == 0) || ppc::carry(b, 0u - a); }%s" % (ra, rb, rd, self.rc(ins, rd))
        if op == "subfe":
            return ("{ uint32_t a = ~%s, b = %s, k = c.ca; %s = a + b + k; "
                    "c.ca = ppc::carry(a, b) || ppc::carry(a + b, k); }%s") % (ra, rb, rd, self.rc(ins, rd))
        if op == "subfze":
            return "{ uint32_t a = ~%s, k = c.ca; %s = a + k; c.ca = ppc::carry(a, k); }%s" % (ra, rd, self.rc(ins, rd))
        if op == "subfme":
            return "{ uint32_t a = ~%s, k = c.ca; %s = a + k - 1u; c.ca = ppc::carry(a, k - 1u); }%s" % (ra, rd, self.rc(ins, rd))
        if op == "extsb":
            return "%s = (uint32_t)(int32_t)(int8_t)%s;%s" % (ra, rs, self.rc(ins, ra))
        if op == "extsh":
            return "%s = (uint32_t)(int32_t)(int16_t)%s;%s" % (ra, rs, self.rc(ins, ra))
        if op == "cntlzw":
            return "%s = ppc::cntlzw(%s);%s" % (ra, rs, self.rc(ins, ra))
        if op == "slw":
            return "%s = (%s & 0x20) ? 0u : (%s << (%s & 31));%s" % (ra, rb, rs, rb, self.rc(ins, ra))
        if op == "srw":
            return "%s = (%s & 0x20) ? 0u : (%s >> (%s & 31));%s" % (ra, rb, rs, rb, self.rc(ins, ra))
        if op == "sraw":
            return "%s = ppc::sraw(c, %s, %s);%s" % (ra, rs, rb, self.rc(ins, ra))
        if op == "srawi":
            return "%s = ppc::srawi(c, %s, %d);%s" % (ra, rs, f["sh"], self.rc(ins, ra))
        if op == "rlwinm":
            m = self._mask(f["mb"], f["me"])
            return "%s = ppc::rotl32(%s, %d) & %s;%s" % (ra, rs, f["sh"], hexs(m), self.rc(ins, ra))
        if op == "rlwnm":
            m = self._mask(f["mb"], f["me"])
            return "%s = ppc::rotl32(%s, %s & 31) & %s;%s" % (ra, rs, rb, hexs(m), self.rc(ins, ra))
        if op == "rlwimi":
            m = self._mask(f["mb"], f["me"])
            return "%s = (%s & %s) | (ppc::rotl32(%s, %d) & %s);%s" % (ra, ra, hexs(~m), rs, f["sh"], hexs(m), self.rc(ins, ra))
        # ---------------- loads/stores ----------------
        load_kind = "a" if op.startswith("lha") else op[1:2]
        if op in ("lwz", "lbz", "lhz", "lha"):
            return "%s = %s;" % (rd, self._load(load_kind, ea_d(f)))
        if op in ("lwzu", "lbzu", "lhzu", "lhau"):
            return "{ uint32_t ea = %s %s; %s = %s; %s = ea; }" % (ra, imm(f["simm"]), rd, self._load(load_kind, "ea"), ra)
        if op in ("lwzx", "lbzx", "lhzx", "lhax"):
            return "%s = %s;" % (rd, self._load(load_kind, ea_x(f)))
        if op in ("lwzux", "lbzux", "lhzux", "lhaux"):
            return "{ uint32_t ea = %s + %s; %s = %s; %s = ea; }" % (ra, rb, rd, self._load(load_kind, "ea"), ra)
        if op in ("stw", "stb", "sth"):
            return "%s;" % self._store(op[2], ea_d(f), rs)
        if op in ("stwu", "stbu", "sthu"):
            return "{ uint32_t ea = %s %s; %s; %s = ea; }" % (ra, imm(f["simm"]), self._store(op[2], "ea", rs), ra)
        if op in ("stwx", "stbx", "sthx"):
            return "%s;" % self._store(op[2], ea_x(f), rs)
        if op in ("stwux", "stbux", "sthux"):
            return "{ uint32_t ea = %s + %s; %s; %s = ea; }" % (ra, rb, self._store(op[2], "ea", rs), ra)
        if op == "lwbrx":
            return "%s = ppc::ld32r(c, m, %s);" % (rd, ea_x(f))
        if op == "lhbrx":
            return "%s = ppc::ld16r(c, m, %s);" % (rd, ea_x(f))
        if op == "stwbrx":
            return "ppc::st32r(c, m, %s, %s);" % (ea_x(f), rs)
        if op == "sthbrx":
            return "ppc::st16r(c, m, %s, %s);" % (ea_x(f), rs)
        if op == "lwarx":
            return "%s = ppc::ld32(c, m, %s);" % (rd, ea_x(f))
        if op == "stwcx":
            return "ppc::st32(c, m, %s, %s); c.cr[0] = (uint8_t)(2 | c.so);" % (ea_x(f), rs)
        if op == "lmw":
            return "{ uint32_t ea = %s; for (int i = %d; i < 32; ++i, ea += 4) c.r[i] = ppc::ld32(c, m, ea); }" % (ea_d(f), f["rd"])
        if op == "stmw":
            return "{ uint32_t ea = %s; for (int i = %d; i < 32; ++i, ea += 4) ppc::st32(c, m, ea, c.r[i]); }" % (ea_d(f), f["rs"])
        if op == "lswi":
            return "ppc::lswi(c, m, %s, %d, %d);" % (A, f["rd"], f["nb"] or 32)
        if op == "stswi":
            return "ppc::stswi(c, m, %s, %d, %d);" % (A, f["rs"], f["nb"] or 32)
        if op in ("lswx", "stswx"):
            return "ppc::fatal(c, \"%s unsupported\", %s);" % (op, hexs(ins.addr))
        # float loads/stores
        if op == "lfs":
            return "%s = %s = ppc::float_bits_to_double(ppc::ld32(c, m, %s));" % (F0(f["fd"]), F1(f["fd"]), ea_d(f))
        if op == "lfsu":
            return "{ uint32_t ea = %s %s; %s = %s = ppc::float_bits_to_double(ppc::ld32(c, m, ea)); %s = ea; }" % (ra, imm(f["simm"]), F0(f["fd"]), F1(f["fd"]), ra)
        if op == "lfsx":
            return "%s = %s = ppc::float_bits_to_double(ppc::ld32(c, m, %s));" % (F0(f["fd"]), F1(f["fd"]), ea_x(f))
        if op == "lfsux":
            return "{ uint32_t ea = %s + %s; %s = %s = ppc::float_bits_to_double(ppc::ld32(c, m, ea)); %s = ea; }" % (ra, rb, F0(f["fd"]), F1(f["fd"]), ra)
        if op == "lfd":
            return "%s = ppc::ld64(c, m, %s);" % (U0(f["fd"]), ea_d(f))
        if op == "lfdu":
            return "{ uint32_t ea = %s %s; %s = ppc::ld64(c, m, ea); %s = ea; }" % (ra, imm(f["simm"]), U0(f["fd"]), ra)
        if op == "lfdx":
            return "%s = ppc::ld64(c, m, %s);" % (U0(f["fd"]), ea_x(f))
        if op == "lfdux":
            return "{ uint32_t ea = %s + %s; %s = ppc::ld64(c, m, ea); %s = ea; }" % (ra, rb, U0(f["fd"]), ra)
        if op == "stfs":
            return "ppc::st32(c, m, %s, ppc::double_to_float_bits(%s));" % (ea_d(f), F0(f["fs"]))
        if op == "stfsu":
            return "{ uint32_t ea = %s %s; ppc::st32(c, m, ea, ppc::double_to_float_bits(%s)); %s = ea; }" % (ra, imm(f["simm"]), F0(f["fs"]), ra)
        if op == "stfsx":
            return "ppc::st32(c, m, %s, ppc::double_to_float_bits(%s));" % (ea_x(f), F0(f["fs"]))
        if op == "stfsux":
            return "{ uint32_t ea = %s + %s; ppc::st32(c, m, ea, ppc::double_to_float_bits(%s)); %s = ea; }" % (ra, rb, F0(f["fs"]), ra)
        if op == "stfd":
            return "ppc::st64(c, m, %s, %s);" % (ea_d(f), U0(f["fs"]))
        if op == "stfdu":
            return "{ uint32_t ea = %s %s; ppc::st64(c, m, ea, %s); %s = ea; }" % (ra, imm(f["simm"]), U0(f["fs"]), ra)
        if op == "stfdx":
            return "ppc::st64(c, m, %s, %s);" % (ea_x(f), U0(f["fs"]))
        if op == "stfdux":
            return "{ uint32_t ea = %s + %s; ppc::st64(c, m, ea, %s); %s = ea; }" % (ra, rb, U0(f["fs"]), ra)
        if op == "stfiwx":
            return "ppc::st32(c, m, %s, (uint32_t)%s);" % (ea_x(f), U0(f["fs"]))
        # paired-single quantized
        if op == "psq_l":
            return "ppc::psq_load(c, m, %s, %d, %d, %d);" % (ea_psq(f), f["fd"], f["w"], f["i"])
        if op == "psq_lu":
            return "{ uint32_t ea = %s %s; ppc::psq_load(c, m, ea, %d, %d, %d); %s = ea; }" % (ra, imm(f["simm12"]), f["fd"], f["w"], f["i"], ra)
        if op == "psq_lx":
            return "ppc::psq_load(c, m, %s, %d, %d, %d);" % (ea_x(f), f["fd"], f["w"], f["i"])
        if op == "psq_lux":
            return "{ uint32_t ea = %s + %s; ppc::psq_load(c, m, ea, %d, %d, %d); %s = ea; }" % (ra, rb, f["fd"], f["w"], f["i"], ra)
        if op == "psq_st":
            return "ppc::psq_store(c, m, %s, %d, %d, %d);" % (ea_psq(f), f["fs"], f["w"], f["i"])
        if op == "psq_stu":
            return "{ uint32_t ea = %s %s; ppc::psq_store(c, m, ea, %d, %d, %d); %s = ea; }" % (ra, imm(f["simm12"]), f["fs"], f["w"], f["i"], ra)
        if op == "psq_stx":
            return "ppc::psq_store(c, m, %s, %d, %d, %d);" % (ea_x(f), f["fs"], f["w"], f["i"])
        if op == "psq_stux":
            return "{ uint32_t ea = %s + %s; ppc::psq_store(c, m, ea, %d, %d, %d); %s = ea; }" % (ra, rb, f["fs"], f["w"], f["i"], ra)
        # ---------------- branches ----------------
        # Loop back-edges poll for pending host events every N iterations (ppc::backedge), so a
        # guest that spins on a memory flag set by an interrupt callback (AI DMA, ARQ, alarms)
        # still receives it, like a real CPU taking the interrupt mid-loop.
        if op == "b":
            t = ins.branch_target
            if ins.lk and (ins.addr + 4) in info.local_returns:
                return "c.lr = %s; goto L_%08X;" % (hexs(ins.addr + 4), t)
            if ins.lk:
                return self._call(t, ins.addr + 4)
            if t in info.addr_set:
                poll = "ppc::backedge(c); " if t <= ins.addr else ""
                return "%sgoto L_%08X;" % (poll, t)
            return self._tail(t)
        if op == "bc":
            t = ins.branch_target
            cond = cond_expr(f["bo"], f["bi"])
            if ins.lk and (ins.addr + 4) in info.local_returns:
                body = "c.lr = %s; goto L_%08X;" % (hexs(ins.addr + 4), t)
            elif ins.lk:
                body = self._call(t, ins.addr + 4)
            elif t in info.addr_set:
                poll = "ppc::backedge(c); " if t <= ins.addr else ""
                body = "%sgoto L_%08X;" % (poll, t)
            else:
                body = self._tail(t)
            return body if cond is None else "if (%s) { %s }" % (cond, body)
        if op == "bclr":
            cond = cond_expr(f["bo"], f["bi"])
            # Local subroutine returns: dispatch on LR with a plain if-chain (a switch here trips
            # the MSVC backend). Only functions containing cave-local calls have any.
            local = " ".join("if (t == %s) goto L_%08X;" % (hexs(r), r) for r in sorted(info.local_returns))
            if ins.lk:
                body = "{ uint32_t t = c.lr; c.lr = %s; %s if (t == entry_lr) return; ppc::call(c, m, t); }" % (hexs(ins.addr + 4), local)
            else:
                body = "{ uint32_t t = c.lr; %s return; }" % local if local else "return;"
            return body if cond is None else "if (%s) { %s }" % (cond, body)
        if op == "bcctr":
            cond = cond_expr(f["bo"], f["bi"])
            if ins.lk and ins.addr in info.ctr_calls:
                t = info.ctr_calls[ins.addr]
                if (ins.addr + 4) in info.local_returns:
                    body = "c.lr = %s; goto L_%08X;" % (hexs(ins.addr + 4), t)
                else:
                    body = self._call(t, ins.addr + 4)
            elif ins.lk:
                body = "{ uint32_t t = c.ctr; c.lr = %s; ppc::call(c, m, t); }" % hexs(ins.addr + 4)
            elif ins.addr in info.ctr_targets:
                t = info.ctr_targets[ins.addr]
                if t in info.addr_set:
                    poll = "ppc::backedge(c); " if t <= ins.addr else ""
                    body = "%sgoto L_%08X;" % (poll, t)
                else:
                    body = self._tail(t)
            else:
                jt = info.jumptables.get(ins.addr)
                if jt:
                    cases = " ".join("case %s: goto L_%08X;" % (hexs(t), t) for t in sorted(set(jt.targets)))
                    body = "switch (c.ctr) { %s default: ppc::call(c, m, c.ctr); return; }" % cases
                else:
                    body = "ppc::call(c, m, c.ctr); return;"
            return body if cond is None else "if (%s) { %s }" % (cond, body)
        if op == "sc":
            return "ppc::syscall(c, m);"
        if op == "rfi":
            return "return;  // rfi"
        # ---------------- condition register ----------------
        if op == "mcrf":
            return "c.cr[%d] = c.cr[%d];" % (f["crfd"], f["crfs"])
        crops = {"crand": "a & b", "cror": "a | b", "crxor": "a ^ b", "crnand": "!(a & b)", "crnor": "!(a | b)",
                 "creqv": "!(a ^ b)", "crandc": "a & !b", "crorc": "a | !b"}
        if op in crops:
            return "{ uint32_t a = ppc::crbit(c, %d), b = ppc::crbit(c, %d); ppc::crbit_set(c, %d, %s); }" % (
                f["crba"], f["crbb"], f["crbd"], crops[op])
        if op == "mfcr":
            return "%s = ppc::mfcr(c);" % rd
        if op == "mtcrf":
            return "ppc::mtcrf(c, %s, %s);" % (hexs(f["crm"]), rs)
        if op == "mcrxr":
            return "c.cr[%d] = (uint8_t)((c.so << 3) | (c.ov << 2) | (c.ca << 1)); c.so = c.ov = c.ca = 0;" % f["crfd"]
        # ---------------- special registers ----------------
        if op == "mfspr":
            return "%s = %s;" % (rd, self._spr_read(f["spr"]))
        if op == "mtspr":
            return self._spr_write(f["spr"], rs)
        if op == "mftb":
            return "%s = %s;" % (rd, "(uint32_t)ppc::read_tb(c)" if f["spr"] == 268 else "(uint32_t)(ppc::read_tb(c) >> 32)")
        if op == "mfmsr":
            return "%s = c.msr;" % rd
        if op == "mtmsr":
            return "ppc::mtmsr(c, %s);" % rs
        if op in ("mfsr", "mfsrin"):
            return "%s = 0u;" % rd
        if op in ("mtsr", "mtsrin", "tlbie", "tlbsync", "sync", "isync", "eieio", "dcbst", "dcbf", "dcbt",
                  "dcbtst", "dcbi", "icbi"):
            return None
        if op in ("dcbz", "dcbz_l"):
            return "ppc::dcbz(c, m, %s);" % ea_x(f)
        # ---------------- floating point ----------------
        return self._emit_float(ins)

    def _mask(self, mb, me):
        begin = 0xFFFFFFFF >> mb
        end = 0x7FFFFFFF >> me
        m = begin ^ end
        return (~m) & 0xFFFFFFFF if me < mb else m

    def _load(self, kind, ea):
        if kind == "a":
            return "(uint32_t)(int32_t)(int16_t)ppc::ld16(c, m, %s)" % ea
        if kind == "w":
            return "ppc::ld32(c, m, %s)" % ea
        if kind == "b":
            return "ppc::ld8(c, m, %s)" % ea
        return "ppc::ld16(c, m, %s)" % ea if kind == "h" else None

    def _store(self, kind, ea, val):
        return {"w": "ppc::st32", "b": "ppc::st8", "h": "ppc::st16"}[kind] + "(c, m, %s, %s)" % (ea, val)

    def _call(self, target, ret):
        if target in self.func_names and target in self.infos:
            call = "c.lr = %s; %s(c, m);" % (hexs(ret), self.fname(target))
        else:
            call = "c.lr = %s; ppc::call(c, m, %s);" % (hexs(ret), hexs(target))
        # The callee may return past the call site (see analyze._return_adjust): resume there.
        for resume in self.cur_info.adjusted_returns.get(ret, ()):
            if resume in self.cur_info.addr_set:
                call += " if (c.lr == %s) goto L_%08X;" % (hexs(resume), resume)
            else:
                call += " if (c.lr == %s) ppc::fatal(c, \"adjusted return outside the caller\", c.lr);" % hexs(resume)
        return call

    def _tail(self, target):
        if target in self.func_names and target in self.infos:
            return "%s(c, m); return;" % self.fname(target)
        return "ppc::call(c, m, %s); return;" % hexs(target)

    def _spr_read(self, n):
        if n == 1:
            return "((c.so << 31) | (c.ov << 30) | (c.ca << 29))"
        if n == 8:
            return "c.lr"
        if n == 9:
            return "c.ctr"
        if 912 <= n <= 919:
            return "c.gqr[%d]" % (n - 912)
        if n == 1008:
            return "c.hid0"
        if n == 920:
            return "c.hid2"
        if n == 22:
            return "c.dec"
        if n in (268, 284):
            return "(uint32_t)ppc::read_tb(c)"
        if n in (269, 285):
            return "(uint32_t)(ppc::read_tb(c) >> 32)"
        return "ppc::spr_read(c, %d)" % n

    def _spr_write(self, n, v):
        if n == 1:
            return "c.so = (%s >> 31) & 1; c.ov = (%s >> 30) & 1; c.ca = (%s >> 29) & 1;" % (v, v, v)
        if n == 8:
            return "c.lr = %s;" % v
        if n == 9:
            return "c.ctr = %s;" % v
        if 912 <= n <= 919:
            return "c.gqr[%d] = %s;" % (n - 912, v)
        if n == 1008:
            return "c.hid0 = %s;" % v
        if n == 920:
            return "c.hid2 = %s;" % v
        if n == 22:
            return "c.dec = %s;" % v
        if n == 284:
            return "c.tb = (c.tb & 0xFFFFFFFF00000000ull) | %s;" % v
        if n == 285:
            return "c.tb = (c.tb & 0xFFFFFFFFull) | ((uint64_t)%s << 32);" % v
        return "ppc::spr_write(c, %d, %s);" % (n, v)

    # ------------------------------------------------------------------
    def _emit_float(self, ins):
        op, f = ins.op, ins.f
        d, a, b, cc = f["fd"], f["fa"], f["fb"], f["fc"]
        D0, D1 = F0(d), F1(d)
        A0, A1, B0, B1, C0, C1 = F0(a), F1(a), F0(b), F1(b), F0(cc), F1(cc)
        # double precision scalar (ps0 only)
        if op == "fadd":
            return "%s = ppc::fadd(%s, %s);" % (D0, A0, B0)
        if op == "fsub":
            return "%s = ppc::fsub(%s, %s);" % (D0, A0, B0)
        if op == "fmul":
            return "%s = ppc::fmul(%s, %s);" % (D0, A0, C0)
        if op == "fdiv":
            return "%s = ppc::fdiv(%s, %s);" % (D0, A0, B0)
        if op == "fmadd":
            return "%s = ppc::fmadd(%s, %s, %s);" % (D0, A0, C0, B0)
        if op == "fmsub":
            return "%s = ppc::fmsub(%s, %s, %s);" % (D0, A0, C0, B0)
        if op == "fnmadd":
            return "%s = ppc::fnmadd(%s, %s, %s);" % (D0, A0, C0, B0)
        if op == "fnmsub":
            return "%s = ppc::fnmsub(%s, %s, %s);" % (D0, A0, C0, B0)
        # single precision scalar (result duplicated to ps1)
        if op == "fadds":
            return "%s = %s = ppc::fs(ppc::fadd(%s, %s));" % (D0, D1, A0, B0)
        if op == "fsubs":
            return "%s = %s = ppc::fs(ppc::fsub(%s, %s));" % (D0, D1, A0, B0)
        if op == "fmuls":
            return "%s = %s = ppc::fs(ppc::fmul(%s, ppc::f25(%s)));" % (D0, D1, A0, C0)
        if op == "fdivs":
            return "%s = %s = ppc::fs(ppc::fdiv(%s, %s));" % (D0, D1, A0, B0)
        if op == "fmadds":
            return "%s = %s = ppc::fmadds(%s, ppc::f25(%s), %s);" % (D0, D1, A0, C0, B0)
        if op == "fmsubs":
            return "%s = %s = ppc::fmsubs(%s, ppc::f25(%s), %s);" % (D0, D1, A0, C0, B0)
        if op == "fnmadds":
            return "%s = %s = ppc::fnmadds(%s, ppc::f25(%s), %s);" % (D0, D1, A0, C0, B0)
        if op == "fnmsubs":
            return "%s = %s = ppc::fnmsubs(%s, ppc::f25(%s), %s);" % (D0, D1, A0, C0, B0)
        if op == "fres":
            return "%s = %s = ppc::fres(%s);" % (D0, D1, B0)
        if op == "frsqrte":
            return "%s = ppc::frsqrte(%s);" % (D0, B0)
        if op == "frsp":
            return "%s = %s = ppc::fs(%s);" % (D0, D1, B0)
        if op == "fmr":
            return "%s = %s;" % (U0(d), U0(b))
        if op == "fneg":
            return "%s = %s ^ 0x8000000000000000ull;" % (U0(d), U0(b))
        if op == "fabs":
            return "%s = %s & 0x7FFFFFFFFFFFFFFFull;" % (U0(d), U0(b))
        if op == "fnabs":
            return "%s = %s | 0x8000000000000000ull;" % (U0(d), U0(b))
        if op == "fsel":
            return "%s = (%s >= -0.0) ? %s : %s;" % (D0, A0, C0, B0)
        if op in ("fcmpu", "fcmpo"):
            return "ppc::fcmp(c, %d, %s, %s);" % (f["crfd"], A0, B0)
        if op == "fctiw":
            return "%s = ppc::fctiw(%s, false);" % (U0(d), B0)
        if op == "fctiwz":
            return "%s = ppc::fctiw(%s, true);" % (U0(d), B0)
        if op == "mffs":
            return "%s = 0xFFF8000000000000ull | c.fpscr;" % U0(d)
        if op == "mtfsf":
            m = 0
            for i in range(8):
                if f["fm"] & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            return "c.fpscr = (c.fpscr & %s) | ((uint32_t)%s & %s); ppc::update_fp_environment(c);" % (hexs(~m), U0(b), hexs(m))
        if op == "mtfsb0":
            return "c.fpscr &= ~%s; ppc::update_fp_environment(c);" % hexs(0x80000000 >> f["crbd"])
        if op == "mtfsb1":
            return "c.fpscr |= %s; ppc::update_fp_environment(c);" % hexs(0x80000000 >> f["crbd"])
        if op == "mtfsfi":
            sh = 28 - 4 * f["crfd"]
            return "c.fpscr = (c.fpscr & ~%s) | (%su << %d); ppc::update_fp_environment(c);" % (hexs(0xF << sh), f["imm"], sh)
        if op == "mcrfs":
            return "c.cr[%d] = (uint8_t)((c.fpscr >> %d) & 15);" % (f["crfd"], 28 - 4 * f["crfs"])
        # ---------------- paired single ----------------
        if op == "ps_add":
            return "{ double x = ppc::fadd(%s, %s), y = ppc::fadd(%s, %s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (A0, B0, A1, B1, D0, D1)
        if op == "ps_sub":
            return "{ double x = ppc::fsub(%s, %s), y = ppc::fsub(%s, %s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (A0, B0, A1, B1, D0, D1)
        if op == "ps_mul":
            return "{ double x = ppc::fmul(%s, ppc::f25(%s)), y = ppc::fmul(%s, ppc::f25(%s)); %s = ppc::fs(x); %s = ppc::fs(y); }" % (A0, C0, A1, C1, D0, D1)
        if op == "ps_div":
            return "{ double x = ppc::fdiv(%s, %s), y = ppc::fdiv(%s, %s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (A0, B0, A1, B1, D0, D1)
        if op == "ps_muls0":
            return "{ double k = ppc::f25(%s); double x = ppc::fmul(%s, k), y = ppc::fmul(%s, k); %s = ppc::fs(x); %s = ppc::fs(y); }" % (C0, A0, A1, D0, D1)
        if op == "ps_muls1":
            return "{ double k = ppc::f25(%s); double x = ppc::fmul(%s, k), y = ppc::fmul(%s, k); %s = ppc::fs(x); %s = ppc::fs(y); }" % (C1, A0, A1, D0, D1)
        if op in ("ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub"):
            fn = "ppc::f" + op[3:] + "s"
            return "{ double x = %s(%s, ppc::f25(%s), %s), y = %s(%s, ppc::f25(%s), %s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (
                fn, A0, C0, B0, fn, A1, C1, B1, D0, D1)
        if op == "ps_madds0":
            return "{ double k = ppc::f25(%s); double x = ppc::fmadds(%s, k, %s), y = ppc::fmadds(%s, k, %s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (
                C0, A0, B0, A1, B1, D0, D1)
        if op == "ps_madds1":
            return "{ double k = ppc::f25(%s); double x = ppc::fmadds(%s, k, %s), y = ppc::fmadds(%s, k, %s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (
                C1, A0, B0, A1, B1, D0, D1)
        if op == "ps_sum0":
            return "{ double x = ppc::fadd(%s, %s), y = %s; %s = ppc::fs(x); %s = ppc::fs(y); }" % (A0, B1, C1, D0, D1)
        if op == "ps_sum1":
            return "{ double x = %s, y = ppc::fadd(%s, %s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (C0, A0, B1, D0, D1)
        if op == "ps_res":
            return "{ double x = ppc::fres(%s), y = ppc::fres(%s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (B0, B1, D0, D1)
        if op == "ps_rsqrte":
            return "{ double x = ppc::frsqrte(%s), y = ppc::frsqrte(%s); %s = ppc::fs(x); %s = ppc::fs(y); }" % (B0, B1, D0, D1)
        if op == "ps_sel":
            return "{ double x = (%s >= -0.0) ? %s : %s, y = (%s >= -0.0) ? %s : %s; %s = x; %s = y; }" % (A0, C0, B0, A1, C1, B1, D0, D1)
        if op == "ps_mr":
            return "{ uint64_t x = %s, y = %s; %s = x; %s = y; }" % (U0(b), U1(b), U0(d), U1(d))
        if op == "ps_neg":
            return "{ uint64_t x = %s ^ 0x8000000000000000ull, y = %s ^ 0x8000000000000000ull; %s = x; %s = y; }" % (U0(b), U1(b), U0(d), U1(d))
        if op == "ps_abs":
            return "{ uint64_t x = %s & 0x7FFFFFFFFFFFFFFFull, y = %s & 0x7FFFFFFFFFFFFFFFull; %s = x; %s = y; }" % (U0(b), U1(b), U0(d), U1(d))
        if op == "ps_nabs":
            return "{ uint64_t x = %s | 0x8000000000000000ull, y = %s | 0x8000000000000000ull; %s = x; %s = y; }" % (U0(b), U1(b), U0(d), U1(d))
        if op == "ps_merge00":
            return "{ uint64_t x = %s, y = %s; %s = x; %s = y; }" % (U0(a), U0(b), U0(d), U1(d))
        if op == "ps_merge01":
            return "{ uint64_t x = %s, y = %s; %s = x; %s = y; }" % (U0(a), U1(b), U0(d), U1(d))
        if op == "ps_merge10":
            return "{ uint64_t x = %s, y = %s; %s = x; %s = y; }" % (U1(a), U0(b), U0(d), U1(d))
        if op == "ps_merge11":
            return "{ uint64_t x = %s, y = %s; %s = x; %s = y; }" % (U1(a), U1(b), U0(d), U1(d))
        if op in ("ps_cmpu0", "ps_cmpo0"):
            return "ppc::fcmp(c, %d, %s, %s);" % (f["crfd"], A0, B0)
        if op in ("ps_cmpu1", "ps_cmpo1"):
            return "ppc::fcmp(c, %d, %s, %s);" % (f["crfd"], A1, B1)
        raise KeyError(op)
