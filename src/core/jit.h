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
// against a copy of their opcodes each time they start, so code that is
// changed (patched, loaded over) is translated again.
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

    // From the run loop, before an instruction, when nothing unusual is
    // pending: runs a block at the PC if there is one (returns the number
    // of instructions it ran, at least 1), else 0 (the interpreter runs
    // the instruction; then call recorded()).
    int exec(M68K &c, int remaining, u64 end, u32 &total);
    void recorded(M68K &c, u16 op, bool generic, bool busError);

    // Statistics
    u32 blocks = 0, flushes = 0, invalidated = 0;
    u64 blockRuns = 0, jitInstructions = 0, interpreted = 0;
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
    void flushAll();
    // Host pages holding translated code (open addressing; 0 = empty).
    enum { kPages = 8192 };
    u64 *pages;
    u32 pageCount = 0;
    void addCodePage(M68K &c, const u8 *host);
};

} // namespace macemu

#endif
