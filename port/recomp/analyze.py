"""Per-function control-flow analysis: labels, calls, jump tables, extra entry points."""
SETJMP_ADDR = 0x803227CC   # MSL __setjmp: callers get a longjmp catch wrapper (see emit.py)
from gekko import decode
from dol import RAM_BASE


class JumpTable:
    __slots__ = ("bctr_addr", "table_addr", "count", "targets")

    def __init__(self, bctr_addr, table_addr, count, targets):
        self.bctr_addr, self.table_addr, self.count, self.targets = bctr_addr, table_addr, count, targets


class FuncInfo:
    def __init__(self, func):
        self.func = func
        self.insns = []           # decoded instructions (None for undecodable words)
        self.addrs = []           # address of each instruction (Gecko caves are spliced in, so not contiguous)
        self.addr_set = set()
        self.aliases = {}         # cave first address -> hooked address it replaced (branches to the hook enter the cave)
        self.local_returns = set()  # return addresses of `bl`s whose target is inside this function (local subroutines)
        self.ctr_targets = {}     # bctr addr -> statically known CTR value (lis/ori/mtctr sequences, Gecko caves)
        self.ctr_calls = {}       # bctrl addr -> statically known CTR value
        self.entries = set()      # addresses (besides func.addr) the dispatch table may enter this function at
        self.labels = set()       # addresses inside this function that are branch targets
        self.optional_hooks = {}  # hook addr -> (original Insn, flag): cave runs only while the flag is set
        self.setjmp_returns = set()  # return addresses of `bl __setjmp`: longjmp re-enters the function here
        self.optional_text = {}   # addr -> (patched Insn, flag): patched instruction while the flag is set
        self.calls = set()        # direct call targets (bl)
        self.tail_targets = set() # `b` targets outside the function
        self.jumptables = {}      # bctr addr -> JumpTable
        self.unresolved_bctr = [] # bctr addresses with no table
        self.bad = []             # undecodable words
        self.has_bctrl = False
        self.has_blrl = False
        self.return_adjusts = set()  # N where this function can return to its caller's return address + N
        self.adjusted_returns = {}   # call-site return addr -> addresses a callee may return to instead


_WRITERS = ("addi", "addis", "or", "lwz", "lwzx", "lbz", "lhz", "add", "subf", "rlwinm", "mulli",
            "lha", "lfs", "mr", "ori", "oris", "xor", "and", "neg", "extsb", "extsh", "srawi", "slw",
            "srw", "lwzu", "lbzx", "lhzx", "mfspr", "subfic", "mullw", "divw", "divwu", "andi_rc",
            "cntlzw", "lwarx", "nor", "andc", "rlwimi", "lmw")


def _const_value(insns, reg, pos, depth=0):
    """Linear backward scan for the constant value of `reg` before instruction `pos`.
    Follows lis/addi/addis/mr chains (CodeWarrior hoists table bases this way)."""
    if depth > 6 or reg == 0 and depth > 0:
        return None
    m = pos - 1
    while m >= 0:
        ins = insns[m]
        if ins is None:
            m -= 1
            continue
        f = ins.f
        if ins.op == "addis" and f["rd"] == reg:
            if f["ra"] == 0:
                return (f["simm"] << 16) & 0xFFFFFFFF
            v = _const_value(insns, f["ra"], m, depth + 1)
            return None if v is None else (v + (f["simm"] << 16)) & 0xFFFFFFFF
        if ins.op == "addi" and f["rd"] == reg:
            if f["ra"] == 0:
                return f["simm"] & 0xFFFFFFFF
            v = _const_value(insns, f["ra"], m, depth + 1)
            return None if v is None else (v + f["simm"]) & 0xFFFFFFFF
        if ins.op == "or" and f["ra"] == reg and f["rs"] == f["rb"]:
            return _const_value(insns, f["rs"], m, depth + 1)
        if ins.op == "ori" and f["ra"] == reg:
            v = _const_value(insns, f["rs"], m, depth + 1)
            return None if v is None else v | f["uimm"]
        if ins.op in _WRITERS and (f.get("rd") == reg if ins.op not in ("or", "ori", "oris", "xor", "and",
                                   "andc", "nor", "rlwinm", "rlwimi", "extsb", "extsh", "srawi", "slw",
                                   "srw", "cntlzw", "andi_rc") else f.get("ra") == reg):
            return None
        if ins.op == "lmw" and f["rd"] <= reg:
            return None
        m -= 1
    return None


def _ctr_constant(insns, idx):
    """Value of CTR at instruction `idx` when it was loaded from a constant (mtctr rX after lis/ori)."""
    j = idx - 1
    while j >= max(0, idx - 24):
        ins = insns[j]
        if ins is not None and ins.op == "mtspr" and ins.f["spr"] == 9:
            return _const_value(insns, ins.f["rs"], j)
        j -= 1
    return None


def _match_jumptable(dol, symbols, info, idx):
    """CodeWarrior switch: cmplwi rX, N ; bgt default ; [lis/addi hoisted anywhere above]
    rlwinm rI, rX, 2, 0, 29 ; lwzx rT2, rT, rI ; mtctr rT2 ; bctr."""
    insns = info.insns
    bctr = insns[idx]
    lo = max(0, idx - 24)
    j = idx - 1
    ctr_src = None
    while j >= lo:
        ins = insns[j]
        if ins is not None and ins.op == "mtspr" and ins.f["spr"] == 9:
            ctr_src = ins.f["rs"]
            break
        j -= 1
    if ctr_src is None:
        return None
    k = j - 1
    table_regs = None
    table_off = 0
    while k >= lo:
        ins = insns[k]
        if ins is None:
            k -= 1
            continue
        if ins.op == "lwzx" and ins.f["rd"] == ctr_src:
            table_regs = (ins.f["ra"], ins.f["rb"])
            break
        if ins.op == "lwz" and ins.f["rd"] == ctr_src:
            table_regs = (ins.f["ra"],)
            table_off = ins.f["simm"]
            break
        k -= 1
    if table_regs is None:
        return None
    base = None
    for reg in table_regs:
        v = _const_value(insns, reg, k)
        if v is not None and dol.in_ram((v + table_off) & 0xFFFFFFFF) and not dol.in_text(v):
            base = (v + table_off) & 0xFFFFFFFF
            break
    if base is None:
        return None
    count = None
    m = idx - 1
    while m >= max(0, idx - 40):
        ins = insns[m]
        if ins is not None and ins.op in ("cmpli", "cmpi"):
            count = ins.f["uimm"] + 1 if ins.op == "cmpli" else ins.f["simm"] + 1
            break
        m -= 1
    func = info.func
    targets = []
    limit = count if count and count > 0 else 4096
    for n in range(limit):
        a = base + n * 4
        if not dol.in_ram(a):
            break
        t = dol.u32(a)
        if not (func.addr <= t < func.end) or (t & 3):
            break
        targets.append(t)
    if not targets:
        return None
    return JumpTable(bctr.addr, base, len(targets), targets)


def _data_scan_tables(dol, func, claimed):
    """Fallback: find runs of >=2 consecutive .data words that point into `func`.
    Because the emitter switches on the actual CTR value, over-approximating the
    target set only adds labels; it never changes behaviour."""
    targets = set()
    for s in dol.sections:
        if s.kind != "data":
            continue
        run = []
        for a in range(s.addr, s.end - 3, 4):
            t = dol.u32(a)
            if func.addr <= t < func.end and not (t & 3):
                run.append((a, t))
            else:
                if len(run) >= 2 and run[0][0] not in claimed:
                    targets.update(t for _, t in run)
                run = []
        if len(run) >= 2 and run[0][0] not in claimed:
            targets.update(t for _, t in run)
    return sorted(targets)


def _return_adjust(insns, idx):
    """For `mtlr rS` at idx followed by blr: N if rS is the saved return address plus N.

    Gecko codes (UCF's shield drop, for one) return to the caller's return address + 8 to skip
    the caller's next instructions: `lwz rS, d(r1); ...; addi rS, rS, 8; mtlr rS; blr`, or the
    same from `mflr rS`. The translated caller must then resume at that address, not after the call.
    """
    ins = insns[idx]
    reg = ins.f["rs"]
    if not any(j < len(insns) and insns[j] is not None and insns[j].op == "bclr" and not insns[j].lk
               and insns[j].f["bo"] == 20 for j in range(idx + 1, idx + 4)):
        return None
    total = 0
    for j in range(idx - 1, max(idx - 9, -1), -1):
        prev = insns[j]
        if prev is None:
            return None
        f = prev.f
        if prev.op == "addi" and f["rd"] == reg and f["ra"] == reg:
            total += f["simm"]
        elif (prev.op == "lwz" and f["rd"] == reg and f["ra"] == 1) or (prev.op == "mfspr" and f["rd"] == reg and f["spr"] == 8):
            return total if total and total % 4 == 0 and 0 < total <= 64 else None
        elif prev.op in _WRITERS and f.get("rd") == reg or prev.op in ("b", "bc", "bclr", "bcctr"):
            return None
    return None


def analyze_function(dol, symbols, func, hooks=None, body=None, optional_text=None):
    """`hooks`: {hook addr: Hook} (Gecko C2 caves spliced in place of the hooked instruction).
    `body`: optional explicit (addr, word) sequence for synthetic functions (caves).
    `optional_text`: {addr: (patched word, flag)} instructions translated both ways."""
    info = FuncInfo(func)
    seq = []
    if body is not None:
        seq = list(body)
    else:
        for a in range(func.addr, func.end, 4):
            hook = hooks.get(a) if hooks else None
            if hook is not None:
                info.aliases[hook.cave_addr] = a
                if hook.optional:
                    info.optional_hooks[a] = (decode(a, dol.u32(a)), hook.optional)
                for k, w in enumerate(hook.words):
                    seq.append((hook.cave_addr + k * 4, w))
            else:
                seq.append((a, dol.u32(a)))
                if optional_text and a in optional_text:
                    word, flag = optional_text[a]
                    info.optional_text[a] = (decode(a, word), flag)
    for a, w in seq:
        ins = decode(a, w)
        if ins is None:
            info.bad.append((a, w))
        info.insns.append(ins)
        info.addrs.append(a)
    info.addr_set = set(info.addrs) | set(info.aliases.values())
    # Two-way instructions: both variants must find their branch targets as labels.
    for a, (ins, flag) in list(info.optional_text.items()) + list(info.optional_hooks.items()):
        if ins is not None and ins.op in ("b", "bc") and not ins.lk and ins.branch_target in info.addr_set:
            info.labels.add(ins.branch_target)
        if a in info.optional_hooks and (a + 4) in info.addr_set:
            info.labels.add(a + 4)
    for idx, ins in enumerate(info.insns):
        if ins is None:
            continue
        if ins.op in ("b", "bc"):
            t = ins.branch_target
            in_body = lambda a: func.addr <= a < func.end
            if ins.lk and t in info.addr_set and not (in_body(t) and in_body(ins.addr)):
                # Local subroutine inside Gecko cave code (or the `bl` to a `blrl` self-address
                # trick): jump with LR set; the matching blr/blrl dispatches back on LR. Ordinary
                # recursion within the function body keeps the real call path.
                info.labels.add(t)
                info.labels.add(ins.addr + 4)
                info.local_returns.add(ins.addr + 4)
            elif ins.lk:
                info.calls.add(t)
                if t == SETJMP_ADDR:
                    info.setjmp_returns.add(ins.addr + 4)
                    info.labels.add(ins.addr + 4)
            elif t in info.addr_set:
                info.labels.add(t)
            else:
                info.tail_targets.add(t)
        elif ins.op == "bcctr":
            const = _ctr_constant(info.insns, idx)
            if const is not None and const != 0 and (const & 3) == 0 and (const in info.addr_set or symbols.containing(const) is not None):
                # Absolute jump/call through CTR with a known constant (Gecko caves do this to
                # re-enter the hooked function or call helpers): resolve statically.
                if ins.lk:
                    info.ctr_calls[ins.addr] = const
                    if const in info.addr_set and not (func.addr <= const < func.end and func.addr <= ins.addr < func.end):
                        info.labels.add(const); info.labels.add(ins.addr + 4); info.local_returns.add(ins.addr + 4)
                    else:
                        info.calls.add(const)
                else:
                    info.ctr_targets[ins.addr] = const
                    if const in info.addr_set:
                        info.labels.add(const)
                    else:
                        info.tail_targets.add(const)
                continue
            if ins.lk:
                info.has_bctrl = True
            else:
                jt = _match_jumptable(dol, symbols, info, idx)
                if jt is None:
                    claimed = {t.table_addr for t in info.jumptables.values()}
                    targets = _data_scan_tables(dol, func, claimed)
                    if targets:
                        jt = JumpTable(ins.addr, 0, len(targets), targets)
                if jt:
                    info.jumptables[ins.addr] = jt
                    info.labels.update(jt.targets)
                else:
                    info.unresolved_bctr.append(ins.addr)
        elif ins.op == "bclr" and ins.lk:
            info.has_blrl = True
        elif ins.op == "mtspr" and ins.f["spr"] == 8:
            adjust = _return_adjust(info.insns, idx)
            if adjust:
                info.return_adjusts.add(adjust)
    return info


def analyze_all(dol, symbols, gecko=None):
    """`gecko`: optional object with `.hooks` (list of Hook) and `.caves` (list of Cave)."""
    from symbols import Function
    hooks = {}
    synthetic = []   # (Function, body sequence)
    if gecko is not None:
        for h in gecko.hooks:
            if getattr(h, "absorbed_by", None) is not None:
                continue
            owner = symbols.containing(h.hook)
            if owner is not None and dol.in_text(owner.addr):
                hooks[h.hook] = h
            else:
                # Hook outside any known function: the cave is the function (Slippi plants EXI
                # helpers this way in unused .init space). It falls through to hook+4 only if the
                # cave never returns, which these do not.
                f = Function("gecko_hook_%08X" % h.hook, h.hook, 4, ".text", "global")
                body = [(h.cave_addr + k * 4, w) for k, w in enumerate(h.words)]
                synthetic.append((f, body))
        for cv in gecko.caves:
            f = Function("gecko_cave_%08X" % cv.cave_addr, cv.cave_addr, len(cv.words) * 4, ".text", "global")
            synthetic.append((f, [(cv.cave_addr + k * 4, w) for k, w in enumerate(cv.words)]))
        # Slippi's code table is one assembled blob: caves call helper routines that live inside
        # other caves (relative `bl`, `b` between caves, lis/ori/mtctr pointers) and take their own
        # address with `bl x; x: blrl` to hand callbacks around. Every such in-cave target is an
        # entry reached through the dispatch table, so the cave tail from that address becomes a
        # function of its own (it returns with blr long before the cave's branch-back).
        cave_list = [(h.cave_addr, h.words) for h in gecko.hooks
                     if getattr(h, "absorbed_by", None) is None]
        cave_list += [(c.cave_addr, c.words) for c in gecko.caves]
        cave_list.sort()
        starts = [s for s, _ in cave_list]
        import bisect

        def find_cave(a):
            i = bisect.bisect_right(starts, a) - 1
            if i >= 0:
                s, w = cave_list[i]
                if s <= a < s + len(w) * 4:
                    return s, w
            return None

        entries = set()
        for start, words in cave_list:
            insns = [decode(start + k * 4, w) for k, w in enumerate(words)]
            for i, ins in enumerate(insns):
                if ins is None:
                    continue
                if ins.op == "bclr" and ins.lk and i + 1 < len(words):
                    entries.add(start + (i + 1) * 4)
                    continue
                if ins.op in ("b", "bc"):
                    t = ins.branch_target
                elif ins.op == "bcctr":
                    t = _ctr_constant(insns, i)
                else:
                    continue
                if t is None:
                    continue
                c = find_cave(t)
                if c is None:
                    continue
                if ins.lk or c[0] != start:
                    entries.add(t)
        existing = {f.addr for f, _ in synthetic}
        for entry in sorted(entries):
            if entry in existing or entry in symbols.by_addr:
                continue
            start, words = find_cave(entry)
            tail = words[(entry - start) // 4:]
            f = Function("gecko_fn_%08X" % entry, entry, len(tail) * 4, ".text", "global")
            synthetic.append((f, [(entry + i * 4, ww) for i, ww in enumerate(tail)]))
    infos = {}
    for func in symbols.functions:
        if not dol.in_text(func.addr):
            continue
        infos[func.addr] = analyze_function(dol, symbols, func, hooks, None, getattr(gecko, "optional_text", None))
    for f, body in synthetic:
        symbols.add_function(f)
        infos[f.addr] = analyze_function(dol, symbols, f, None, body)
    # Gecko cave code is entered at arbitrary addresses at run time: Slippi's helper-table trick
    # (`bl x; x: blrl`, then `mflr; addi; mtctr; bctrl` into a table of branches), function
    # pointers handed to the game, and computed resumes at hook+4. Every cave instruction, every
    # hook address and hook+4 therefore gets a dispatch-table thunk that enters the owning
    # function at that label (see Emitter: the function starts with an entry if-chain).
    thunks = {}   # entry addr -> owning function addr (only addresses that are not functions themselves)
    for addr in sorted(infos, key=lambda a: (infos[a].func.name.startswith("gecko_fn_"), a)):
        info = infos[addr]
        f = info.func
        synthetic = f.name.startswith("gecko_")
        cand = set()
        for a in info.addrs:
            if a != f.addr and (synthetic or not (f.addr <= a < f.end)):
                cand.add(a)
        for cave, hook in info.aliases.items():
            if hook != f.addr:
                cand.add(hook)
            if f.addr < hook + 4 < f.end and (hook + 4) in info.addr_set:
                cand.add(hook + 4)
        cand |= info.setjmp_returns
        info.entries = cand
        info.labels.update(cand)
        for a in cand:
            if a not in infos and a not in thunks:
                thunks[a] = f.addr
    # Extra entry points: targets of calls/tail branches that land mid-function.
    extra_entries = {}  # containing function addr -> set(entry addrs)
    for info in infos.values():
        for t in list(info.calls) + list(info.tail_targets):
            if t in infos:
                continue
            owner = symbols.containing(t)
            if owner is None:
                extra_entries.setdefault(None, set()).add(t)
            else:
                extra_entries.setdefault(owner.addr, set()).add(t)
    # Mid-function targets of calls/tail branches (Gecko caves jump into a hooked function's
    # epilogue with lis/ori/mtctr/bctr) enter the owner through a thunk as well.
    for owner, targets in extra_entries.items():
        if owner is None or owner not in infos:
            continue
        info = infos[owner]
        for t in targets:
            if t in info.addr_set and t not in infos and t not in thunks:
                info.entries.add(t)
                info.labels.add(t)
                thunks[t] = owner
    # Callers of functions that can return past their call site resume at that address.
    for info in infos.values():
        for idx, ins in enumerate(info.insns):
            if ins is None or not ins.lk:
                continue
            if ins.op in ("b", "bc"):
                target = ins.branch_target
            elif ins.op == "bcctr":
                target = info.ctr_calls.get(ins.addr)
            else:
                continue
            callee = infos.get(target)
            if callee is None or not callee.return_adjusts or (ins.addr + 4) in info.local_returns:
                continue
            ret = ins.addr + 4
            info.adjusted_returns[ret] = sorted(ret + n for n in callee.return_adjusts)
            info.labels.update(a for a in info.adjusted_returns[ret] if a in info.addr_set)
    return infos, extra_entries, thunks
