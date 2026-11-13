#include "hostfs_fat.h"
#include "storage.h"
#include <circle/timer.h>
#include <circle/util.h>

using macemu::HostStat; using macemu::HostFS;
typedef long long hs64;

enum TOp { OpStat, OpOpenDir, OpReadDir, OpCloseDir, OpOpen, OpRead, OpWrite, OpTruncate, OpSize,
	   OpClose, OpMakeDir, OpRemove, OpRename, OpFree };

struct CFatHostFS::TRequest
{
	CFatHostFS *pThis;
	int nOp;
	const char *pPath, *pPath2;
	int nHandle;
	u32 nOffset;
	void *pBuffer;
	u32 nLength;
	HostStat *pStat;
	hs64 nResult;
};

CFatHostFS::CFatHostFS (const char *pRoot)
{
	strncpy (m_Root, pRoot, sizeof m_Root - 1);
	m_Root[sizeof m_Root - 1] = '\0';
	for (unsigned i = 0; i < MaxFiles; i++) m_bFileUsed[i] = false;
	for (unsigned i = 0; i < MaxDirs; i++) m_bDirUsed[i] = false;
}

void CFatHostFS::Full (char *pOut, const char *pPath) const
{
	strcpy (pOut, m_Root);
	if (*pPath)
	{
		strcat (pOut, "/");
		strncat (pOut, pPath, 1023 - strlen (pOut));
	}
}

// FAT times (local) as Unix times.
static u32 UnixTime (WORD nDate, WORD nTime)
{
	static const u16 Days[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	unsigned nYear = 1980 + (nDate >> 9), nMonth = (nDate >> 5) & 15, nDay = nDate & 31;
	if (nMonth < 1 || nMonth > 12) nMonth = 1;
	if (nDay < 1) nDay = 1;
	u32 nDays = (nYear - 1970) * 365 + (nYear - 1969) / 4 + Days[nMonth - 1] + nDay - 1;
	if (nMonth > 2 && nYear % 4 == 0) nDays++;
	return nDays * 86400 + (nTime >> 11) * 3600 + ((nTime >> 5) & 63) * 60 + (nTime & 31) * 2;
}

void CFatHostFS::Execute (void *pParam)
{
	TRequest &R = *(TRequest *) pParam;
	R.pThis->Do (R);
}

void CFatHostFS::Do (TRequest &R)
{
	static char Path[1100], Path2[1100];
	R.nResult = -1;
	switch (R.nOp)
	{
	case OpStat: {
		Full (Path, R.pPath);
		FILINFO Info;
		if (*R.pPath == '\0')
		{
			// The folder itself (f_stat cannot look at a volume's root).
			DIR Dir;
			if (f_opendir (&Dir, Path) != FR_OK) break;
			f_closedir (&Dir);
			R.pStat->dir = true; R.pStat->size = 0; R.pStat->mtime = 0; R.pStat->readOnly = false;
			R.nResult = 0;
			break;
		}
		if (f_stat (Path, &Info) != FR_OK) break;
		R.pStat->dir = (Info.fattrib & AM_DIR) != 0;
		R.pStat->size = (u32) Info.fsize;
		R.pStat->mtime = UnixTime (Info.fdate, Info.ftime);
		R.pStat->readOnly = (Info.fattrib & AM_RDO) != 0;
		R.nResult = 0;
		break;
	}
	case OpOpenDir:
		Full (Path, R.pPath);
		for (int i = 0; i < MaxDirs; i++)
		{
			if (!m_bDirUsed[i])
			{
				if (f_opendir (&m_Dir[i], Path) == FR_OK) { m_bDirUsed[i] = true; R.nResult = i; }
				break;
			}
		}
		break;
	case OpReadDir: {
		FILINFO Info;
		if (f_readdir (&m_Dir[R.nHandle], &Info) != FR_OK || Info.fname[0] == '\0') break;
		strncpy ((char *) R.pBuffer, Info.fname, R.nLength - 1);
		((char *) R.pBuffer)[R.nLength - 1] = '\0';
		if (R.pStat) R.pStat->mtime = UnixTime (Info.fdate, Info.ftime);
		R.nResult = 0;
		break;
	}
	case OpCloseDir:
		f_closedir (&m_Dir[R.nHandle]);
		m_bDirUsed[R.nHandle] = false;
		R.nResult = 0;
		break;
	case OpOpen: {
		Full (Path, R.pPath);
		BYTE nMode = R.nOffset == ReadOnly ? FA_READ : FA_READ | FA_WRITE;
		if (R.nOffset == Create) nMode |= FA_OPEN_ALWAYS;
		for (int i = 0; i < MaxFiles; i++)
		{
			if (!m_bFileUsed[i])
			{
				if (f_open (&m_File[i], Path, nMode) == FR_OK) { m_bFileUsed[i] = true; R.nResult = i; }
				break;
			}
		}
		break;
	}
	case OpRead: case OpWrite: {
		FIL &F = m_File[R.nHandle];
		UINT nDone = 0;
		if (R.nOp == OpWrite && R.nOffset > f_size (&F))
		{
			// Writing past the end: zeros up to there first.
			static const u8 Zero[512] = {0};
			if (f_lseek (&F, f_size (&F)) != FR_OK) break;
			while (f_size (&F) < R.nOffset)
			{
				u32 n = R.nOffset - (u32) f_size (&F);
				if (f_write (&F, Zero, n > sizeof Zero ? sizeof Zero : n, &nDone) != FR_OK || !nDone) break;
			}
		}
		if (f_lseek (&F, R.nOffset) != FR_OK) break;
		FRESULT Res = R.nOp == OpRead ? f_read (&F, R.pBuffer, R.nLength, &nDone)
					      : f_write (&F, R.pBuffer, R.nLength, &nDone);
		if (Res == FR_OK) R.nResult = nDone;
		break;
	}
	case OpTruncate: {
		FIL &F = m_File[R.nHandle];
		if (R.nOffset > f_size (&F))
		{
			static const u8 Zero[512] = {0};
			UINT nDone;
			if (f_lseek (&F, f_size (&F)) != FR_OK) break;
			while (f_size (&F) < R.nOffset)
			{
				u32 n = R.nOffset - (u32) f_size (&F);
				if (f_write (&F, Zero, n > sizeof Zero ? sizeof Zero : n, &nDone) != FR_OK || !nDone) break;
			}
			R.nResult = f_size (&F) == R.nOffset ? 0 : -1;
			break;
		}
		if (f_lseek (&F, R.nOffset) == FR_OK && f_truncate (&F) == FR_OK) R.nResult = 0;
		break;
	}
	case OpSize:
		R.nResult = (hs64) f_size (&m_File[R.nHandle]);
		break;
	case OpClose:
		f_close (&m_File[R.nHandle]);
		m_bFileUsed[R.nHandle] = false;
		R.nResult = 0;
		break;
	case OpMakeDir:
		Full (Path, R.pPath);
		if (f_mkdir (Path) == FR_OK) R.nResult = 0;
		break;
	case OpRemove:
		Full (Path, R.pPath);
		if (f_unlink (Path) == FR_OK) R.nResult = 0;
		break;
	case OpRename:
		Full (Path, R.pPath);
		Full (Path2, R.pPath2);
		if (f_rename (Path, Path2) == FR_OK) R.nResult = 0;
		break;
	case OpFree: {
		DWORD nClusters;
		FATFS *pFS;
		if (f_getfree ("SD:", &nClusters, &pFS) == FR_OK) R.nResult = (hs64) nClusters * pFS->csize * 512;
		break;
	}
	}
}

#define REQUEST(op) TRequest R; memset (&R, 0, sizeof R); R.pThis = this; R.nOp = op;
#define RUN CDiskProxy::Run (Execute, &R);

bool CFatHostFS::stat (const char *pPath, HostStat &rStat)
{
	REQUEST (OpStat) R.pPath = pPath; R.pStat = &rStat; RUN
	return R.nResult == 0;
}

int CFatHostFS::openDir (const char *pPath)
{
	REQUEST (OpOpenDir) R.pPath = pPath; RUN
	return (int) R.nResult;
}

bool CFatHostFS::readDir (int nDir, char *pName, u32 nMax, u32 *pTime)
{
	HostStat St;
	REQUEST (OpReadDir) R.nHandle = nDir; R.pBuffer = pName; R.nLength = nMax; R.pStat = pTime ? &St : 0; RUN
	if (pTime) *pTime = St.mtime;
	return R.nResult == 0;
}

void CFatHostFS::closeDir (int nDir)
{
	REQUEST (OpCloseDir) R.nHandle = nDir; RUN
}

int CFatHostFS::open (const char *pPath, int nMode)
{
	REQUEST (OpOpen) R.pPath = pPath; R.nOffset = (u32) nMode; RUN
	return (int) R.nResult;
}

macemu::s32 CFatHostFS::read (int nFile, u32 nOffset, void *pBuffer, u32 nLength)
{
	REQUEST (OpRead) R.nHandle = nFile; R.nOffset = nOffset; R.pBuffer = pBuffer; R.nLength = nLength; RUN
	return (macemu::s32) R.nResult;
}

macemu::s32 CFatHostFS::write (int nFile, u32 nOffset, const void *pBuffer, u32 nLength)
{
	REQUEST (OpWrite) R.nHandle = nFile; R.nOffset = nOffset; R.pBuffer = (void *) pBuffer; R.nLength = nLength; RUN
	return (macemu::s32) R.nResult;
}

bool CFatHostFS::truncate (int nFile, u32 nSize)
{
	REQUEST (OpTruncate) R.nHandle = nFile; R.nOffset = nSize; RUN
	return R.nResult == 0;
}

u32 CFatHostFS::size (int nFile)
{
	REQUEST (OpSize) R.nHandle = nFile; RUN
	return R.nResult < 0 ? 0 : (u32) R.nResult;
}

void CFatHostFS::close (int nFile)
{
	REQUEST (OpClose) R.nHandle = nFile; RUN
}

bool CFatHostFS::makeDir (const char *pPath)
{
	REQUEST (OpMakeDir) R.pPath = pPath; RUN
	return R.nResult == 0;
}

bool CFatHostFS::remove (const char *pPath)
{
	REQUEST (OpRemove) R.pPath = pPath; RUN
	return R.nResult == 0;
}

bool CFatHostFS::rename (const char *pFrom, const char *pTo)
{
	REQUEST (OpRename) R.pPath = pFrom; R.pPath2 = pTo; RUN
	return R.nResult == 0;
}

macemu::u64 CFatHostFS::freeBytes (void)
{
	REQUEST (OpFree) RUN
	return R.nResult < 0 ? 0 : (macemu::u64) R.nResult;
}

u32 CFatHostFS::milliseconds (void)
{
	return (u32) (CTimer::GetClockTicks64 () / 1000);
}
