// The JIT (see jit.h).
#include "jit.h"
#include "m68k.h"
#include "arm64emit.h"
#if defined(JIT_DUMP)
#include <cstdio>
#endif

#pragma GCC diagnostic ignored "-Winvalid-offsetof"

namespace macemu {

#if defined(__aarch64__)
bool jitAvailable() { return true; }
#else
bool jitAvailable() { return false; }
#endif

#if defined(__aarch64__) && defined(__linux__) && !defined(__circle__)
}   // namespace macemu
#include <sys/mman.h>
namespace macemu {
void *jitCodeMemory(u32 bytes)
{
    void *p = mmap(0, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
void jitFlushCode(void *start, u32 bytes) { __builtin___clear_cache((char *)start, (char *)start + bytes); }
#elif !defined(__aarch64__)
void *jitCodeMemory(u32) { return 0; }
void jitFlushCode(void *, u32) {}
#endif
// (Bare-metal AArch64: the Circle kernel provides both.)

// The interpreter's pieces the blocks call, and the CPU's fields they use.
struct JitAccess {
    static void busError(M68K *c) { c->busErrorException(); }
    static u32 rdSlow(M68K *c, u32 addr, int sz) { return c->rdSlow(addr, sz); }
    static void dropTlb(M68K &c) { c.faultSeen(); }
    static void wrSlow(M68K *c, u32 addr, int sz, u32 v) { c->wrSlow(addr, sz, v); }
    typedef M68K::Handler Handler;
    static Handler handler(M68K &c, u16 op) { return c.table[op]; }
    static Handler generic() { return M68K::s_generic; }
    static const u8 *codeHost(M68K &c, u32 pc)
    {
        if ((pc & 1) || (pc & 0xFFF) == 0xFFE) return 0;
        const M68K::TlbEntry &e = c.codeTlb[(pc >> 12) & (M68K::kCodeTlbSize - 1)];
        return e.tag == c.tlbTag(pc) ? e.ptr + (pc & 0xFFF) : 0;
    }
    static int model(M68K &c) { return c.model; }
    enum : u32 {
        oPc = __builtin_offsetof(M68K, pc), oInstPC = __builtin_offsetof(M68K, instPC), oCyc = __builtin_offsetof(M68K, cyc),
        oTotal = __builtin_offsetof(M68K, totalCycles), oStopped = __builtin_offsetof(M68K, stopped),
        oNmiSeen = __builtin_offsetof(M68K, nmiSeen), oTraceNext = __builtin_offsetof(M68K, traceNext),
        oOpcode = __builtin_offsetof(M68K, opcode), oBus = __builtin_offsetof(M68K, bus), oIrq = __builtin_offsetof(M68K, irq),
        oImask = __builtin_offsetof(M68K, imask), oFT = __builtin_offsetof(M68K, fT), oTlbGen = __builtin_offsetof(M68K, tlbGeneration),
        oSlow = __builtin_offsetof(M68K, slowAccesses),
        oD = __builtin_offsetof(M68K, d), oA = __builtin_offsetof(M68K, a),
        oFC = __builtin_offsetof(M68K, fC), oFV = __builtin_offsetof(M68K, fV), oFZ = __builtin_offsetof(M68K, fZ),
        oFN = __builtin_offsetof(M68K, fN), oFX = __builtin_offsetof(M68K, fX), oFS = __builtin_offsetof(M68K, fS),
        oTlb = __builtin_offsetof(M68K, tlb),
        oTlbTag = __builtin_offsetof(M68K::TlbEntry, tag), oTlbPtr = __builtin_offsetof(M68K::TlbEntry, ptr),
        oTlbWritable = __builtin_offsetof(M68K::TlbEntry, writable),
        oBusError = __builtin_offsetof(Bus, busError),
    };
};

Jit::Jit() : table(0), code(0), codeSize(0), codeUsed(0), copies(0), copiesSize(0), copiesUsed(0), pages(0) {}

Jit::~Jit()
{
    delete[] jobs;
    delete[] table;
    delete[] copies;
    delete[] pages;
}

static inline u32 pageHash(u64 page) { return (u32)(page ^ (page >> 13)) & 8191u; }   // kPages - 1

bool Jit::codePage(const u8 *host) const
{
    u64 page = (u64)(unsigned long)host >> 12;
    if (!pages || !pageCount) return false;
    for (u32 i = pageHash(page); ; i = (i + 1) & (kPages - 1)) {
        if (pages[i] == page) return true;
        if (!pages[i]) return false;
    }
}

void Jit::addCodePage(M68K &c, const u8 *host)
{
    u64 page = (u64)(unsigned long)host >> 12;
    if (codePage(host)) return;
    if (pageCount >= kPages / 2) return;               // full: flushAll empties it
    u32 i = pageHash(page);
    while (pages[i]) i = (i + 1) & (kPages - 1);
    pages[i] = page;
    pageCount++;
    JitAccess::dropTlb(c);                              // writable entries for it must go
}

bool Jit::init(u32 codeBytes)
{
    if (!jitAvailable()) return false;
    code = (u8 *)jitCodeMemory(codeBytes);
    if (!code) return false;
    codeSize = codeBytes;
    table = new Block[kTableSize];
    copiesSize = 4 * 1024 * 1024;
    copies = new u8[copiesSize];
    pages = new u64[kPages];
    flushAll();
    flushes = 0;
    return true;
}

void Jit::flushAll()
{
    for (u32 i = 0; i < kTableSize; i++) table[i].fn = 0;
    codeUsed = 0;
    copiesUsed = 0;
    for (u32 i = 0; i < kPages; i++) pages[i] = 0;
    pageCount = 0;
    recording = false;
    flushes++;
}

static inline u32 slot(u32 pc) { return (pc >> 1) & 131071; }

static inline bool sameBytes(const u8 *a, const u8 *b, u32 n)
{
    for (u32 i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

int Jit::exec(M68K &c, int remaining, u64 end, u32 &total)
{
    if (worker && __atomic_load_n(&doneCount, __ATOMIC_ACQUIRE) != seenDone) installDone(c);
    u32 pc = c.pc;
    const u8 *host = JitAccess::codeHost(c, pc);
    if (!host) {
        if (recording) finish(c);
        return 0;
    }
    Block &b = table[slot(pc)];
    bool found = b.fn && b.pc == pc && b.host == host;
    if (found && !sameBytes(host, b.copy, b.len)) {     // the code changed
        b.fn = 0;
        found = false;
        invalidated++;
    }
    if (recording) {
        if (pc != recNext || found) finish(c);          // left the recorded path, or met a block
        else return 0;                                  // keep recording
        found = found && b.fn && b.pc == pc;            // (translating may have flushed it)
    }
    if (!found) {
        startRecording(pc, host);
        return 0;
    }
    blockRuns++;
    int n = b.fn(&c, remaining, end, &total);
    jitInstructions += (u32)n;
    return n;
}

void Jit::startRecording(u32 pc, const u8 *host)
{
    recording = true;
    recStart = pc;
    recHost = host;
    recCount = 0;
    recNext = pc;
}

void Jit::recorded(M68K &c, u16 op, bool generic, bool busError)
{
    interpreted++;
    if (!recording) return;
    u32 at = c.instPC;
    if (at != recNext) { finish(c); return; }
    recs[recCount].pc = at;
    recs[recCount].op = op;
    recCount++;
    recNext = c.pc;
    // A block ends after an instruction that went elsewhere, may change
    // the CPU's mode or the address map (exec()'s instructions), faulted,
    // or at the end of the page.
    bool onward = !generic && !busError && c.pc > at && c.pc - at <= 22
                  && (c.pc >> 12) == (recStart >> 12) && (c.pc & 0xFFF) < 0xFFE;
    if (!onward || recCount == kMaxInsns) finish(c);
}

void Jit::finish(M68K &c)
{
    recording = false;
    // The code may have changed since it was recorded (loaded over, or the
    // recording ran on after a TLB miss): then the recording is no use.
    bool same = recCount > 0;
    for (int i = 0; same && i < recCount; i++) {
        const u8 *p = recHost + (recs[i].pc - recStart);
        same = (u16)((p[0] << 8) | p[1]) == recs[i].op;
    }
    if (same) {
        if (worker) queue(c);
        else translate(c);
    }
    recCount = 0;
}

#include "jitnative.inc"

bool Jit::hasOwnCode(u16 op) { return nativeSupported(op); }

// Instructions that can send the PC somewhere other than the next
// instruction (the fast handlers of the others cannot, short of a bus
// error, which is a slow access).
static bool mayBranch(u16 op)
{
    return (op >> 12) == 6 || (op & 0xF0F8) == 0x50C8 || op == 0x4E75 || (op & 0xFF80) == 0x4E80;
}

// The code for a block:
//
//   x19 the CPU, w20 instructions allowed, x21 the cycle limit, x22 &total,
//   w23 instructions run, x25 totalCycles, w26 cycles to add to *total,
//   w27 the CPU's slow-access count at the start.
//
// Each instruction does what M68K::run's loop does for it: set PC past
// the opcode and 4 cycles for its fetch, call its handler, scale and add
// the cycles.  What else the loop checks can only change on a slow access
// (a device, a TLB miss: interrupts, the address map, a bus error) or in
// the instructions exec() decodes, which end blocks: after a slow access
// the block takes any bus error and stops.  It also stops where the loop
// would (instruction count, cycle limit) and where a branch went elsewhere
// than last time.
void Jit::translate(M68K &c)
{
    u32 end = 0, nNative = 0;
    static u32 buf[kMaxWords];                          // (the CPU's core only)
    u32 words = emitBlock(c, recs, recCount, recStart, recHost, buf, end, nNative);
    nativeInstructions += nNative;
    install(c, recStart, recHost, end - recStart, recHost, buf, words);
}

// Puts a block's code in place (the CPU's core).
void Jit::install(M68K &c, u32 pc, const u8 *host, u32 len, const u8 *bytes, const u32 *words, u32 count)
{
    if (codeUsed + count * 4 + 64 > codeSize || copiesUsed + 4096 > copiesSize || pageCount >= kPages / 2) flushAll();
    addCodePage(c, (const u8 *)((unsigned long)host & ~4095ul));
    u32 *at = (u32 *)(code + codeUsed);
    for (u32 i = 0; i < count; i++) at[i] = words[i];
    jitFlushCode(at, count * 4);
    codeUsed += (count * 4 + 63) & ~63u;

    Block &b = table[slot(pc)];
    b.pc = pc;
    b.host = host;
    b.len = len;
    u8 *copy = copies + copiesUsed;
    for (u32 k = 0; k < len; k++) copy[k] = bytes[k];
    copiesUsed += (len + 7) & ~7u;
    b.copy = copy;
    b.fn = (BlockFn)(void *)at;
    blocks++;
}

// The code for a block: what translate() always did.  Reads only the CPU's
// fixed tables (its handlers, its model) and the code at src, so another
// core may run it.  Returns its length in words; end is where the code it
// depends on ends.
u32 Jit::emitBlock(M68K &c, const Rec *recs, int recCount, u32 recStart, const u8 *recHost, u32 *buf,
                   u32 &endOut, u32 &nNative) const
{
    using namespace a64;
    typedef JitAccess J;
    const u32 maxWords = 128 + (u32)recCount * 260;
    nNative = 0;
    Emitter e(buf, maxWords);
    const int shift = J::model(c) >= M68K::MC68040 ? 2 : 1;

    e.stpPre(29, 30, SP, -128);                        // 96..127: kept across slow accesses
    e.stp(19, 20, SP, 16);
    e.stp(21, 22, SP, 32);
    e.stp(23, 24, SP, 48);
    e.stp(25, 26, SP, 64);
    e.stp(27, 28, SP, 80);
    e.movrx(19, 0);
    e.movrw(20, 1);
    e.movrx(21, 2);
    e.movrx(22, 3);
    e.movw(23, 0);
    e.ldrx(25, 19, J::oTotal);
    e.movw(26, 0);
    e.ldrw(27, 19, J::oSlow);
    e.strb(ZR, 19, J::oNmiSeen);                       // (the loop clears these each time;
    e.strb(ZR, 19, J::oTraceNext);                     // only exceptions set them)
    e.movx(24, J::oTlb);
    e.addxLsl(24, 19, 24, 0);                           // x24: the data TLB
    e.ldrw(28, 19, J::oFS);
    e.lslwi(28, 28, 1);
    e.movw(2, 1);
    e.orrw(28, 28, 2);                                  // w28: TLB tag bits (fS is fixed in a block)

    u32 *exits[kMaxInsns * 4], *slows[kMaxInsns];
    int nExits = 0, nSlows = 0;
    u32 end = recs[recCount - 1].pc + 2;                // the code the block depends on
    for (int i = 0; i < recCount; i++) {
        const Rec &r = recs[i];
        J::Handler h = J::handler(c, r.op);
        e.movw(0, r.pc);
        e.strw(0, 19, J::oInstPC);
        e.movw(1, r.op);
        e.strh(1, 19, J::oOpcode);
        bool own = native && h != J::generic() && nativeSupported(r.op);
        if (own) {
            u32 *mark = e.here();
            NativeGen g(e, recHost + (r.pc - recStart), r.pc);
            e.movw(9, 0);
            u32 len = 2;
            g.translate(r.op, len);
            if ((r.pc & 0xFFF) + len > 0x1000) {        // runs into the next page: the handler's
                e.rewind(mark);
                own = false;
            } else {
                e.addwi(9, 9, 4 + g.extra);
                e.strw(9, 19, J::oCyc);
                e.movw(0, r.pc + len);
                e.strw(0, 19, J::oPc);
                if (r.pc + len > end) end = r.pc + len;
                nNative++;
            }
        }
        if (!own) {
            e.movw(0, r.pc + 2);
            e.strw(0, 19, J::oPc);
            e.movw(0, 4);
            e.strw(0, 19, J::oCyc);
            e.movrx(0, 19);
            e.movx(16, (u64)(unsigned long)h);
            e.blr(16);
        }
        e.ldrw(1, 19, J::oSlow);
        e.cmpw(1, 27);
        slows[nSlows++] = e.bcond(NE);
        e.ldrw(0, 19, J::oCyc);
        e.addwi(0, 0, (1u << shift) - 1);
        e.asrwi(0, 0, (u32)shift);
        e.strw(0, 19, J::oCyc);
        e.addxUxtw(25, 25, 0);
        e.strx(25, 19, J::oTotal);
        e.addw(26, 26, 0);
        e.addwi(23, 23, 1);
        if (i == recCount - 1) break;
        if (mayBranch(r.op)) {
            e.ldrw(0, 19, J::oPc);
            e.movw(2, recs[i + 1].pc);
            e.cmpw(0, 2);
            exits[nExits++] = e.bcond(NE);
        }
        e.cmpw(23, 20);
        exits[nExits++] = e.bcond(GE);
        e.cmpx(25, 21);
        exits[nExits++] = e.bcond(HS);
    }
    u32 *toExit = e.b();

    // After a slow access: a bus error's exception, the cycles, and stop.
    for (int k = 0; k < nSlows; k++) Emitter::patch(slows[k], e.here());
    e.ldrx(0, 19, J::oBus);
    e.ldrb(1, 0, J::oBusError);
    u32 *noFault = e.cbz(1);
    e.movrx(0, 19);
    e.movx(16, (u64)(unsigned long)&J::busError);
    e.blr(16);
    Emitter::patch(noFault, e.here());
    e.ldrw(0, 19, J::oCyc);
    e.addwi(0, 0, (1u << shift) - 1);
    e.asrwi(0, 0, (u32)shift);
    e.strw(0, 19, J::oCyc);
    e.addxUxtw(25, 25, 0);
    e.strx(25, 19, J::oTotal);
    e.addw(26, 26, 0);
    e.addwi(23, 23, 1);

    Emitter::patch(toExit, e.here());
    for (int k = 0; k < nExits; k++) Emitter::patch(exits[k], e.here());
    e.ldrw(0, 22, 0);
    e.addw(0, 0, 26);
    e.strw(0, 22, 0);
    e.movrw(0, 23);
    e.ldp(27, 28, SP, 80);
    e.ldp(25, 26, SP, 64);
    e.ldp(23, 24, SP, 48);
    e.ldp(21, 22, SP, 32);
    e.ldp(19, 20, SP, 16);
    e.ldpPost(29, 30, SP, 128);
    e.ret();

#if defined(JIT_DUMP)                                   // each block's code to jit-PC-ADDRESS.bin
    { char fn[64]; snprintf(fn, sizeof fn, "jit-%08X-%p.bin", recStart, (void *)buf); if (FILE *f = fopen(fn, "wb")) { fwrite(buf, 4, e.size(), f); fclose(f); } }
#endif
    endOut = end;
    return e.size();
}

// ---------------------------------------------------------------- background

bool Jit::startBackground(M68K &c)
{
    if (!code) return false;
    if (!jobs) {
        jobs = new Job[kSlots];
        for (u32 i = 0; i < kSlots; i++) jobs[i].state = 0;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    worker = &c;
    return true;
}

static inline void wakeWorker()
{
#if defined(__aarch64__)
    asm volatile("dsb ish\n sev" ::: "memory");
#endif
}

// The CPU's core: the recording as a job (a copy of the code with it, as
// the worker must not read the Mac's memory while it changes).
void Jit::queue(M68K &c)
{
    (void)c;
    for (u32 i = 0; i < kSlots; i++)
        if (__atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE) != 0 && jobs[i].pc == recStart && jobs[i].host == recHost)
            return;                                     // already on its way
    for (u32 i = 0; i < kSlots; i++) {
        Job &j = jobs[i];
        if (__atomic_load_n(&j.state, __ATOMIC_ACQUIRE) != 0) continue;
        j.pc = recStart;
        j.host = recHost;
        j.count = recCount;
        for (int k = 0; k < recCount; k++) j.recs[k] = recs[k];
        u32 want = recs[recCount - 1].pc - recStart + 24, room = 4096 - (recStart & 0xFFF);
        j.snapLen = want < room ? want : room;
        if (j.snapLen > kSnapMax) j.snapLen = kSnapMax;
        for (u32 k = 0; k < j.snapLen; k++) j.snap[k] = recHost[k];
        __atomic_store_n(&j.state, 1, __ATOMIC_RELEASE);
        queued++;
        wakeWorker();
        return;
    }
    dropped++;                                          // all busy: it will be recorded again
}

bool Jit::workOnce()
{
    M68K *c = worker;
    if (!c || !jobs) return false;
    for (u32 i = 0; i < kSlots; i++) {
        Job &j = jobs[i];
        int queuedState = 1;
        if (!__atomic_compare_exchange_n(&j.state, &queuedState, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        j.words = emitBlock(*c, j.recs, j.count, j.pc, j.snap, j.buf, j.end, j.nNative);
        __atomic_store_n(&j.state, 3, __ATOMIC_RELEASE);
        __atomic_add_fetch(&doneCount, 1, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

void Jit::workerLoop()
{
    for (;;) {
        if (!workOnce()) {
#if defined(__aarch64__)
            asm volatile("wfe");                        // until the CPU's core queues a job (sev)
#endif
        }
    }
}

// The CPU's core: finished jobs into place, if their code is still as recorded.
void Jit::installDone(M68K &c)
{
    seenDone = __atomic_load_n(&doneCount, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < kSlots; i++) {
        Job &j = jobs[i];
        if (__atomic_load_n(&j.state, __ATOMIC_ACQUIRE) != 3) continue;
        u32 len = j.end - j.pc;
        bool same = len <= j.snapLen && sameBytes(j.host, j.snap, len);
        if (same) {
            nativeInstructions += j.nNative;
            install(c, j.pc, j.host, len, j.snap, j.buf, j.words);
        } else {
            stale++;
        }
        __atomic_store_n(&j.state, 0, __ATOMIC_RELEASE);
    }
}

void Jit::drain(M68K &c)
{
    if (!worker) return;
    for (;;) {
        bool busy = false;
        for (u32 i = 0; i < kSlots; i++) {
            int st = __atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE);
            if (st == 1 || st == 2) busy = true;
        }
        if (!busy) break;
    }
    installDone(c);
}

} // namespace macemu
