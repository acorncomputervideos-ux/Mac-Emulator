// The JIT: runs 68k code as blocks of AArch64 code.
//
// A block is a run of instructions the interpreter has just executed one
// after another (recorded as it went), translated so that each instruction
// is called without fetching, decoding or dispatching it.  The blocks do
// exactly what the interpreter's run loop does, step for step and cycle
// for cycle: any change of course (a branch taken differently, an
// exception, an interrupt becoming due, the address map changing) leaves
// the block, and the interpreter carries on.  So a Mac runs identically
// with and without it; only faster.
//
// Blocks are found by the PC and the host address of the code, and checked
// against a copy of their opcodes, so code that is changed (patched,
// loaded over) is translated again.  The check is made once per value of
// the CPU's codeEpoch (bumped by anything that could change code or what
// an address means); until it changes, a block that ends goes straight on
// to the next one if that one was checked at this epoch ("chaining"),
// testing on the way what the run loop would have tested.
//
// Only built where the host is AArch64 (the Raspberry Pi kernels, and Linux
// on ARM for tests); elsewhere jitAvailable() is false.

#ifndef MACEMU_JIT_H
#define MACEMU_JIT_H

#include "types.h"

namespace macemu {

class M68K;

// The host provides memory that may hold code (and makes new code visible
// to the instruction cache).
void *jitCodeMemory(u32 bytes);
void jitFlushCode(void *start, u32 bytes);

bool jitAvailable();

class Jit {
public:
    Jit();
    ~Jit();
    bool init(u32 codeBytes = 32 * 1024 * 1024);
    static bool hasOwnCode(u16 op);     // translated to ARM code of its own (tests)
    bool codePage(const u8 *hostPage) const;            // holds translated code
    // A write at offset off of a host page: 0 not a page with translated
    // code, 1 one but not near any (within its 64-byte lines), 2 near some.
    int codeWrite(const u8 *hostPage, u32 off) const;

    // From the run loop, before an instruction, when nothing unusual is
    // pending: runs a block at the PC if there is one (returns the number
    // of instructions it ran, at least 1), else 0 (the interpreter runs
    // the instruction; then call recorded()).
    int exec(M68K &c, int remaining, u64 end, u32 &total);
    void recorded(M68K &c, u16 op, bool generic, bool busError);

    // Translating on another core (the Pi's core 3): the CPU's core records
    // blocks and queues them; workerLoop(), on the other core, translates
    // them; exec() puts the results in place (after checking the code has
    // not changed meanwhile).  Without it, blocks are translated at once.
    bool startBackground(M68K &c);      // call before blocks are made
    void workerLoop();                  // the other core: never returns
    bool workOnce();                    // one job, if any (tests: a thread of their own)
    void drain(M68K &c);                // tests: wait for the jobs, put them in place

    // Statistics
    u32 blocks = 0, flushes = 0, invalidated = 0;
    u32 queued = 0, dropped = 0, stale = 0;     // background: jobs, full queue, code changed meanwhile
    u64 blockRuns = 0, jitInstructions = 0, interpreted = 0;
    u64 checks = 0;                     // blocks' opcodes compared with their copies
    u32 nativeInstructions = 0;         // translated to ARM code of their own
    bool native = true;                 // false: blocks call every handler (stage 1)

private:
    typedef int (*BlockFn)(M68K *c, int remaining, u64 end, u32 *total);
    struct Block {
        u32 pc;
        const u8 *host;                 // the code: host address of pc
        u32 len;                        // bytes of opcodes to check
        const u8 *copy;                 // ... and what they were
        BlockFn fn;
    };
    enum { kTableSize = 131072, kMaxInsns = 48 };
    Block *table;
    // Per table slot: a block checked at an epoch, for chaining (the blocks'
    // code reads it: keep the layout).  key = epoch * 2 + fS; pc odd = none.
    struct Chain { u32 pc, key; u64 entry; };
    Chain *chains;
    u32 entryWords = 0;                 // where a chained block starts, in words
    u8 *code;
    u32 codeSize, codeUsed;
    u8 *copies;                         // opcode copies
    u32 copiesSize, copiesUsed;

    // Recording: what the interpreter ran since the block start.
    struct Rec { u32 pc; u16 op; };
    bool recording = false;
    u32 recStart = 0, recNext = 0;
    const u8 *recHost = 0;
    int recCount = 0;
    Rec recs[kMaxInsns];

    void startRecording(u32 pc, const u8 *host);
    void finish(M68K &c);
    void translate(M68K &c);
    // The code for a block, into buf (any address: it runs where it is copied).
    enum { kMaxWords = 128 + kMaxInsns * 260 };
    u32 emitBlock(M68K &c, const Rec *r, int count, u32 start, const u8 *src, u32 *buf, u32 &end, u32 &nNative) const;
    void install(M68K &c, u32 pc, const u8 *host, u32 len, const u8 *bytes, const u32 *words, u32 count);

    // Background jobs: free -> queued (CPU core) -> working -> done (worker) -> free (CPU core).
    enum { kSlots = 8, kSnapMax = kMaxInsns * 24 + 32 };
    struct Job {
        volatile int state;
        u32 pc; const u8 *host; int count;
        Rec recs[kMaxInsns];
        u8 snap[kSnapMax]; u32 snapLen;        // the code as recorded
        u32 words, end, nNative;
        u32 buf[kMaxWords];
    };
    Job *jobs = 0;
    M68K *worker = 0;                   // set: background on
    volatile u32 doneCount = 0;
    u32 seenDone = 0;
    void queue(M68K &c);
    void installDone(M68K &c);
    void flushAll();
    // Host pages holding translated code (open addressing; 0 = empty).
    enum { kPages = 8192 };
    u64 *pages;
    u64 *lines;                         // per page: its 64-byte lines with translated code
    u32 pageCount = 0;
    int findPage(const u8 *host) const;
    void addCode(M68K &c, const u8 *host, u32 len);
};

} // namespace macemu

#endif
