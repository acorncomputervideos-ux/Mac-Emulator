#include "storage.h"
#include <circle/multicore.h>
#include <circle/synchronize.h>
#include <circle/util.h>
#include <circle/memory.h>
#include <circle/logger.h>

macemu::Storage * volatile CDiskProxy::s_pStorage = 0;
volatile int CDiskProxy::s_nOp = 0;
volatile u32 CDiskProxy::s_nOffset = 0;
void * volatile CDiskProxy::s_pBuffer = 0;
volatile u32 CDiskProxy::s_nLength = 0;
volatile bool CDiskProxy::s_bResult = false;
volatile int CDiskProxy::s_nState = 0;

boolean CDiskProxy::Remote (void)
{
#ifdef ARM_ALLOW_MULTI_CORE
	return CMultiCoreSupport::ThisCore () != 0;
#else
	return FALSE;
#endif
}

bool CDiskProxy::Call (macemu::Storage *pStorage, TOp Op, u32 nOffset, void *pBuffer, u32 nLength)
{
	s_pStorage = pStorage;
	s_nOp = Op;
	s_nOffset = nOffset;
	s_pBuffer = pBuffer;
	s_nLength = nLength;
	DataMemBarrier ();
	s_nState = 1;
	while (s_nState != 2)
	{
		// core 0 is on it
	}
	DataMemBarrier ();
	bool bResult = s_bResult;
	s_nState = 0;
	DataMemBarrier ();
	return bResult;
}

void CDiskProxy::Run (void (*pFunc) (void *pParam), void *pParam)
{
	if (!Remote ())
	{
		pFunc (pParam);
		return;
	}
	Call ((macemu::Storage *) (void *) pFunc, OpCall, 0, pParam, 0);
}

void CDiskProxy::Service (void)
{
	if (s_nState != 1)
	{
		return;
	}
	DataMemBarrier ();
	macemu::Storage *pStorage = s_pStorage;
	bool bResult = true;
	switch (s_nOp)
	{
	case OpRead:	bResult = pStorage->read (s_nOffset, s_pBuffer, s_nLength);	break;
	case OpWrite:	bResult = pStorage->write (s_nOffset, s_pBuffer, s_nLength);	break;
	case OpCall:	((void (*) (void *)) (void *) pStorage) (s_pBuffer);		break;
	default:	pStorage->flush ();						break;
	}
	s_bResult = bResult;
	DataMemBarrier ();
	s_nState = 2;
}

CFileStorage *CFileStorage::s_pOpen[MaxOpen];

// Cache buffers are kept for the next image once allocated: Circle does not
// take back blocks this big, and the disks are opened again at each restart.
struct TSpareCache { u8 *pBuffer; size_t nBytes; };
static TSpareCache s_Spare[8];

static u8 *TakeCache (size_t nWant, size_t *pGot)
{
	// A spare that holds it all, the smallest such.
	int nBest = -1;
	for (int i = 0; i < 8; i++)
	{
		if (   s_Spare[i].pBuffer != 0 && s_Spare[i].nBytes >= nWant
		    && (nBest < 0 || s_Spare[i].nBytes < s_Spare[nBest].nBytes))
		{
			nBest = i;
		}
	}

	// Otherwise new memory, leaving plenty for everything else.
	if (nBest < 0)
	{
		CMemorySystem *pMemory = CMemorySystem::Get ();
		size_t nHigh = pMemory->GetHeapFreeSpace (HEAP_HIGH);
		size_t nLow = pMemory->GetHeapFreeSpace (HEAP_LOW);
		const size_t nKeepHigh = 32u << 20, nKeepLow = 160u << 20;
		size_t nRoom = nHigh > nKeepHigh + (64u << 20) ? nHigh - nKeepHigh : 0;
		int nHeap = HEAP_HIGH;
		if (nRoom == 0)
		{
			nRoom = nLow > nKeepLow ? nLow - nKeepLow : 0;
			nHeap = HEAP_LOW;
		}
		size_t nBytes = nWant < nRoom ? nWant : nRoom;
		nBytes &= ~(size_t) 0xFFFF;
		if (nBytes >= (4u << 20))
		{
			u8 *p = (u8 *) CMemorySystem::HeapAllocate (nBytes, nHeap);
			if (p != 0)
			{
				*pGot = nBytes;
				return p;
			}
		}

		// The largest spare, then: part of the image.
		for (int i = 0; i < 8; i++)
		{
			if (s_Spare[i].pBuffer != 0 && (nBest < 0 || s_Spare[i].nBytes > s_Spare[nBest].nBytes))
			{
				nBest = i;
			}
		}
		if (nBest < 0)
		{
			*pGot = 0;
			return 0;
		}
	}

	u8 *p = s_Spare[nBest].pBuffer;
	*pGot = s_Spare[nBest].nBytes;
	s_Spare[nBest].pBuffer = 0;
	return p;
}

static void GiveBackCache (u8 *pBuffer, size_t nBytes)
{
	for (int i = 0; i < 8; i++)
	{
		if (s_Spare[i].pBuffer == 0)
		{
			s_Spare[i].pBuffer = pBuffer;
			s_Spare[i].nBytes = nBytes;
			return;
		}
	}
	// (more than eight: lost, as Circle would lose it anyway)
}

CFileStorage::CFileStorage (void)
:	m_bOpen (FALSE),
	m_bReadOnly (FALSE),
	m_bDirty (FALSE),
	m_nSize (0),
	m_pCache (0),
	m_nCacheBytes (0),
	m_nSlots (0),
	m_pTag (0),
	m_nPieces (0),
	m_nPrefetchNext (0),
	m_nPrefetchPiece (NoPiece),
	m_nPrefetchDone (0)
{
}

CFileStorage::~CFileStorage (void)
{
	FreeCache ();
	if (m_bOpen)
	{
		f_close (&m_File);
	}
}

void CFileStorage::SetUpCache (void)
{
	m_nPieces = (m_nSize + PieceSize - 1) >> PieceShift;
	m_pCache = TakeCache ((size_t) m_nPieces << PieceShift, &m_nCacheBytes);
	if (m_pCache == 0)
	{
		return;
	}
	m_nSlots = (u32) (m_nCacheBytes >> PieceShift);
	if (m_nSlots > m_nPieces)
	{
		m_nSlots = m_nPieces;
	}
	m_pTag = new u32[m_nSlots];
	for (u32 i = 0; i < m_nSlots; i++)
	{
		m_pTag[i] = NoPiece;
	}
	for (unsigned i = 0; i < MaxOpen; i++)
	{
		if (s_pOpen[i] == 0)
		{
			s_pOpen[i] = this;
			break;
		}
	}
	CLogger::Get ()->Write ("disk", LogNotice, "%u of %u MB kept in memory", m_nSlots >> 4, m_nPieces >> 4);
}

void CFileStorage::FreeCache (void)
{
	for (unsigned i = 0; i < MaxOpen; i++)
	{
		if (s_pOpen[i] == this)
		{
			s_pOpen[i] = 0;
		}
	}
	if (m_pCache != 0)
	{
		GiveBackCache (m_pCache, m_nCacheBytes);
		m_pCache = 0;
	}
	delete [] (u32 *) m_pTag;
	m_pTag = 0;
	m_nSlots = 0;
}

unsigned CFileStorage::CachedMB (void) const
{
	u32 n = 0;
	for (u32 i = 0; i < m_nSlots; i++)
	{
		n += m_pTag[i] != NoPiece;
	}
	return n >> 4;
}

bool CFileStorage::ReadPiece (u32 nPiece)
{
	u32 nSlot = nPiece % m_nSlots;
	m_pTag[nSlot] = NoPiece;
	DataMemBarrier ();
	u32 nOffset = nPiece << PieceShift;
	u32 nLength = m_nSize - nOffset < PieceSize ? m_nSize - nOffset : PieceSize;
	UINT nRead = 0;
	if (   f_lseek (&m_File, nOffset) != FR_OK
	    || f_read (&m_File, m_pCache + ((size_t) nSlot << PieceShift), nLength, &nRead) != FR_OK
	    || nRead != nLength)
	{
		return false;
	}
	DataMemBarrier ();				// the data, then the tag (read in that order elsewhere)
	m_pTag[nSlot] = nPiece;
	return true;
}

// Core 0, when it has a moment: a little more of a disk into memory, 8 KB
// at a time (well under a millisecond, so the screen and the Mac's own disk
// requests never wait long for it).  A piece's tag is set once all of it
// is there; until then the Mac reads that piece the usual way.
boolean CFileStorage::Prefetch (void)
{
	enum { Step = 8192 };
	for (unsigned i = 0; i < MaxOpen; i++)
	{
		CFileStorage *p = s_pOpen[i];
		if (p == 0 || p->m_nSlots == 0)
		{
			continue;
		}
		if (p->m_nPrefetchPiece == NoPiece)
		{
			// Only into empty slots: a slot the Mac may be reading is never changed here.
			while (p->m_nPrefetchNext < p->m_nPieces)
			{
				u32 nPiece = p->m_nPrefetchNext++;
				if (p->m_pTag[nPiece % p->m_nSlots] == NoPiece)
				{
					p->m_nPrefetchPiece = nPiece;
					p->m_nPrefetchDone = 0;
					break;
				}
			}
			if (p->m_nPrefetchPiece == NoPiece)
			{
				continue;			// all of this one done
			}
		}

		u32 nPiece = p->m_nPrefetchPiece;
		u32 nSlot = nPiece % p->m_nSlots;
		if (p->m_pTag[nSlot] != NoPiece)
		{
			p->m_nPrefetchPiece = NoPiece;		// the Mac's reads brought it (or another) meanwhile
			return TRUE;
		}
		u32 nOffset = (nPiece << PieceShift) + p->m_nPrefetchDone;
		u32 nLength = p->m_nSize - (nPiece << PieceShift) < PieceSize ? p->m_nSize - (nPiece << PieceShift) : PieceSize;
		u32 n = nLength - p->m_nPrefetchDone < Step ? nLength - p->m_nPrefetchDone : Step;
		UINT nRead = 0;
		if (   f_lseek (&p->m_File, nOffset) != FR_OK
		    || f_read (&p->m_File, p->m_pCache + ((size_t) nSlot << PieceShift) + p->m_nPrefetchDone, n, &nRead) != FR_OK
		    || nRead != n)
		{
			p->m_nPrefetchPiece = NoPiece;
			p->m_nPrefetchNext = p->m_nPieces;	// a bad card: leave it
			return TRUE;
		}
		p->m_nPrefetchDone += n;
		if (p->m_nPrefetchDone >= nLength)
		{
			DataMemBarrier ();			// the data, then the tag (read in that order elsewhere)
			p->m_pTag[nSlot] = nPiece;
			p->m_nPrefetchPiece = NoPiece;
			if (p->m_nPrefetchNext == p->m_nPieces && p->m_nSlots == p->m_nPieces)
			{
				CLogger::Get ()->Write ("disk", LogNotice, "All %u MB now in memory", p->m_nPieces >> 4);
			}
		}
		return TRUE;
	}
	return FALSE;
}

boolean CFileStorage::Open (const char *pPath)
{
	m_bReadOnly = FALSE;
	if (f_open (&m_File, pPath, FA_READ | FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
	{
		if (f_open (&m_File, pPath, FA_READ | FA_OPEN_EXISTING) != FR_OK)
		{
			return FALSE;
		}

		m_bReadOnly = TRUE;
	}

	m_bOpen = TRUE;
	m_nSize = (u32) f_size (&m_File);
	if (m_nSize < 512)
	{
		return FALSE;
	}
	SetUpCache ();
	return TRUE;
}

bool CFileStorage::read (macemu::u32 nOffset, void *pBuffer, macemu::u32 nLength)
{
	if (!m_bOpen || nOffset + nLength > m_nSize || nOffset + nLength < nOffset)
	{
		return false;
	}

	// From memory, as far as it is there (any core).
	u8 *pOut = (u8 *) pBuffer;
	while (m_nSlots != 0 && nLength > 0)
	{
		u32 nPiece = nOffset >> PieceShift;
		u32 nSlot = nPiece % m_nSlots;
		if (m_pTag[nSlot] != nPiece)
		{
			break;
		}
		DataMemBarrier ();
		u32 nIn = nOffset & (PieceSize - 1);
		u32 n = PieceSize - nIn < nLength ? PieceSize - nIn : nLength;
		memcpy (pOut, m_pCache + ((size_t) nSlot << PieceShift) + nIn, n);
		pOut += n;
		nOffset += n;
		nLength -= n;
	}
	if (nLength == 0)
	{
		return true;
	}

	if (CDiskProxy::Remote ())
	{
		return CDiskProxy::Call (this, CDiskProxy::OpRead, nOffset, pOut, nLength);
	}

	// Core 0: the missing pieces into memory, then from there.
	if (m_nSlots != 0)
	{
		while (nLength > 0)
		{
			u32 nPiece = nOffset >> PieceShift;
			u32 nSlot = nPiece % m_nSlots;
			if (m_pTag[nSlot] != nPiece && !ReadPiece (nPiece))
			{
				return false;
			}
			u32 nIn = nOffset & (PieceSize - 1);
			u32 n = PieceSize - nIn < nLength ? PieceSize - nIn : nLength;
			memcpy (pOut, m_pCache + ((size_t) nSlot << PieceShift) + nIn, n);
			pOut += n;
			nOffset += n;
			nLength -= n;
		}
		return true;
	}

	UINT nRead = 0;
	if (   f_lseek (&m_File, nOffset) != FR_OK
	    || f_read (&m_File, pOut, nLength, &nRead) != FR_OK)
	{
		return false;
	}

	return nRead == nLength;
}

bool CFileStorage::write (macemu::u32 nOffset, const void *pBuffer, macemu::u32 nLength)
{
	if (CDiskProxy::Remote ())
	{
		return CDiskProxy::Call (this, CDiskProxy::OpWrite, nOffset, (void *) pBuffer, nLength);
	}

	UINT nWritten = 0;
	if (   !m_bOpen
	    || m_bReadOnly
	    || nOffset + nLength > m_nSize
	    || f_lseek (&m_File, nOffset) != FR_OK
	    || f_write (&m_File, pBuffer, nLength, &nWritten) != FR_OK)
	{
		return false;
	}

	m_bDirty = TRUE;

	// A piece half copied into memory by Prefetch() would keep the old
	// bytes of its first part: it starts again.
	if (   m_nPrefetchPiece != NoPiece
	    && nOffset < ((m_nPrefetchPiece + 1) << PieceShift)
	    && nOffset + nWritten > (m_nPrefetchPiece << PieceShift))
	{
		m_nPrefetchNext = m_nPrefetchPiece;
		m_nPrefetchPiece = NoPiece;
	}

	// The copy in memory too (the Mac is waiting for this: it reads none of it now).
	const u8 *pIn = (const u8 *) pBuffer;
	for (u32 nDone = 0; m_nSlots != 0 && nDone < nWritten; )
	{
		u32 nAt = nOffset + nDone;
		u32 nPiece = nAt >> PieceShift;
		u32 nSlot = nPiece % m_nSlots;
		u32 nIn = nAt & (PieceSize - 1);
		u32 n = PieceSize - nIn < nWritten - nDone ? PieceSize - nIn : nWritten - nDone;
		if (m_pTag[nSlot] == nPiece)
		{
			memcpy (m_pCache + ((size_t) nSlot << PieceShift) + nIn, pIn + nDone, n);
		}
		nDone += n;
	}

	return nWritten == nLength;
}

void CFileStorage::flush (void)
{
	if (CDiskProxy::Remote ())
	{
		CDiskProxy::Call (this, CDiskProxy::OpFlush, 0, 0, 0);
		return;
	}

	if (m_bOpen && m_bDirty)
	{
		f_sync (&m_File);
		m_bDirty = FALSE;
	}
}

CMemoryStorage::CMemoryStorage (void)
:	m_pData (0),
	m_nSize (0),
	m_bReadOnly (FALSE),
	m_bDirty (FALSE)
{
	m_Path[0] = '\0';
}

CMemoryStorage::~CMemoryStorage (void)
{
	delete [] m_pData;
}

boolean CMemoryStorage::Load (const char *pPath, u32 nMaxSize)
{
	strncpy (m_Path, pPath, sizeof m_Path - 1);
	m_Path[sizeof m_Path - 1] = '\0';

	FIL File;
	if (f_open (&File, pPath, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return FALSE;
	}

	m_nSize = (u32) f_size (&File);
	if (m_nSize == 0 || m_nSize > nMaxSize)
	{
		f_close (&File);
		return FALSE;
	}

	m_pData = new u8[m_nSize];
	UINT nRead = 0;
	boolean bOK =    m_pData != 0
		      && f_read (&File, m_pData, m_nSize, &nRead) == FR_OK
		      && nRead == m_nSize;
	f_close (&File);

	// Probe whether the image can be written back later.
	FILINFO Info;
	m_bReadOnly = f_stat (pPath, &Info) != FR_OK || (Info.fattrib & AM_RDO) != 0;

	return bOK;
}

bool CMemoryStorage::read (macemu::u32 nOffset, void *pBuffer, macemu::u32 nLength)
{
	if (m_pData == 0 || nOffset + nLength > m_nSize)
	{
		return false;
	}

	memcpy (pBuffer, m_pData + nOffset, nLength);

	return true;
}

bool CMemoryStorage::write (macemu::u32 nOffset, const void *pBuffer, macemu::u32 nLength)
{
	if (m_pData == 0 || m_bReadOnly || nOffset + nLength > m_nSize)
	{
		return false;
	}

	memcpy (m_pData + nOffset, pBuffer, nLength);
	m_bDirty = TRUE;

	return true;
}

void CMemoryStorage::flush (void)
{
	if (CDiskProxy::Remote ())
	{
		CDiskProxy::Call (this, CDiskProxy::OpFlush, 0, 0, 0);
		return;
	}

	if (!m_bDirty || m_pData == 0)
	{
		return;
	}

	FIL File;
	if (f_open (&File, m_Path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
	{
		return;				// stays dirty, tried again later
	}

	UINT nWritten = 0;
	if (   f_write (&File, m_pData, m_nSize, &nWritten) == FR_OK
	    && nWritten == m_nSize)
	{
		m_bDirty = FALSE;
	}

	f_close (&File);
}
