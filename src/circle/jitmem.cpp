// Memory for the JIT's code on the Pi.
//
// Circle maps everything after the kernel's own code as never executable
// at EL1 (PXN).  The JIT's buffer is taken from the heap on whole 64 KB
// pages, and those pages' entries in the translation table are made
// executable.  Circle maps memory one to one (virtual = physical) with one
// level 2 table of 512 MB entries and level 3 tables of 64 KB pages.

#include <circle/types.h>
#include <circle/synchronize.h>
#include "../core/jit.h"

namespace macemu {

#if AARCH == 64

void *jitCodeMemory (u32 nBytes)
{
	const u64 Page = 0x10000;
	u8 *pBlock = new u8[nBytes + Page];
	if (pBlock == 0)
	{
		return 0;
	}
	u64 nStart = ((u64) pBlock + Page - 1) & ~(Page - 1);

	u64 nTTBR;
	asm volatile ("mrs %0, ttbr0_el1" : "=r" (nTTBR));
	u64 *pLevel2 = (u64 *) (nTTBR & 0x0000FFFFFFFF0000ULL);
	for (u64 nPage = nStart; nPage < nStart + nBytes; nPage += Page)
	{
		u64 nTable = pLevel2[nPage >> 29];
		if ((nTable & 3) != 3)
		{
			return 0;
		}
		u64 *pLevel3 = (u64 *) (nTable & 0x0000FFFFFFFF0000ULL);
		pLevel3[(nPage >> 16) & 0x1FFF] &= ~(1ULL << 53);	// PXN off
	}
	asm volatile ("dsb ishst\n"
		      "tlbi vmalle1is\n"
		      "dsb ish\n"
		      "isb" ::: "memory");

	return (void *) nStart;
}

// New code: clean it from the data cache, drop stale copies from the
// instruction cache (on this core and the others).
void jitFlushCode (void *pStart, u32 nBytes)
{
	u64 nCtr;
	asm volatile ("mrs %0, ctr_el0" : "=r" (nCtr));
	u64 nDLine = 4ULL << ((nCtr >> 16) & 15), nILine = 4ULL << (nCtr & 15);
	u64 nFrom = (u64) pStart, nTo = nFrom + nBytes;
	for (u64 a = nFrom & ~(nDLine - 1); a < nTo; a += nDLine)
	{
		asm volatile ("dc cvau, %0" :: "r" (a) : "memory");
	}
	asm volatile ("dsb ish" ::: "memory");
	for (u64 a = nFrom & ~(nILine - 1); a < nTo; a += nILine)
	{
		asm volatile ("ic ivau, %0" :: "r" (a) : "memory");
	}
	asm volatile ("dsb ish\n"
		      "isb" ::: "memory");
}

#endif	// (32-bit kernels: no JIT; src/core/jit.cpp has the stubs)

} // namespace macemu
