// Disk image storage on the SD card (FatFs).

#ifndef _macemu_storage_h
#define _macemu_storage_h

#include <circle/types.h>
#include <fatfs/ff.h>
#include "../core/types.h"

// The SD card (FatFs) may only be used from core 0.  When the Mac runs on
// another core, its disk accesses are handed to core 0 through this single
// request slot: the Mac's core waits while core 0 calls Service().
class CDiskProxy
{
public:
	enum TOp { OpRead, OpWrite, OpFlush, OpCall };

	// TRUE when called on a core other than 0.
	static boolean Remote (void);
	static bool Call (macemu::Storage *pStorage, TOp Op, u32 nOffset, void *pBuffer, u32 nLength);
	static void Service (void);			// core 0: carry out a waiting request
	// Any work for core 0 (the SD card is only touched from there).
	static void Run (void (*pFunc) (void *pParam), void *pParam);

private:
	static macemu::Storage * volatile s_pStorage;
	static volatile int s_nOp;
	static volatile u32 s_nOffset;
	static void * volatile s_pBuffer;
	static volatile u32 s_nLength;
	static volatile bool s_bResult;
	static volatile int s_nState;			// 0 idle, 1 requested, 2 done
};

// A hard disk or CD image: read and written in place, block by block.
//
// The image is also kept in memory, as far as memory allows (a 500 MB disk
// fits on a Pi with 2 GB or more): reads found there need no help from
// core 0, so the Mac never stops for them.  Core 0 fills the copy in its
// idle time (Prefetch); a read that misses fetches a whole 64 KB piece.
// Writes go to the card at once and to the copy.
class CFileStorage : public macemu::Storage
{
public:
	// Core 0, when idle: read one more piece of an open image into memory.
	// FALSE when there is nothing left to read.
	static boolean Prefetch (void);

	CFileStorage (void);
	~CFileStorage (void);

	boolean Open (const char *pPath);

	macemu::u32 size (void) override		{ return m_nSize; }
	bool read (macemu::u32 nOffset, void *pBuffer, macemu::u32 nLength) override;
	bool write (macemu::u32 nOffset, const void *pBuffer, macemu::u32 nLength) override;
	bool readOnly (void) override			{ return m_bReadOnly; }
	void flush (void) override;

	unsigned CachedMB (void) const;			// how much is in memory now

private:
	FIL m_File;
	boolean m_bOpen;
	boolean m_bReadOnly;
	boolean m_bDirty;
	u32 m_nSize;

	// The copy in memory: pieces of 64 KB, piece n in slot n % m_nSlots.
	enum { PieceShift = 16, PieceSize = 1 << PieceShift, NoPiece = 0xFFFFFFFF };
	u8 *m_pCache;
	size_t m_nCacheBytes;
	u32 m_nSlots;
	volatile u32 *m_pTag;				// the piece in each slot, or NoPiece
	u32 m_nPieces;
	u32 m_nPrefetchNext;				// the next piece Prefetch() looks at
	void SetUpCache (void);
	void FreeCache (void);
	bool ReadPiece (u32 nPiece);			// core 0: into its slot

	enum { MaxOpen = 12 };
	static CFileStorage *s_pOpen[MaxOpen];
};

// A floppy image: small enough to live in memory.  Changes are written back
// to the file by flush().
class CMemoryStorage : public macemu::Storage
{
public:
	CMemoryStorage (void);
	~CMemoryStorage (void);

	boolean Load (const char *pPath, u32 nMaxSize);

	macemu::u32 size (void) override		{ return m_nSize; }
	bool read (macemu::u32 nOffset, void *pBuffer, macemu::u32 nLength) override;
	bool write (macemu::u32 nOffset, const void *pBuffer, macemu::u32 nLength) override;
	bool readOnly (void) override			{ return m_bReadOnly; }
	void flush (void) override;

	boolean IsDirty (void) const			{ return m_bDirty; }

private:
	char m_Path[160];
	u8 *m_pData;
	u32 m_nSize;
	boolean m_bReadOnly;
	boolean m_bDirty;
};

#endif
