#include "storage.h"
#include <circle/util.h>

CFileStorage::CFileStorage (void)
:	m_bOpen (FALSE),
	m_bReadOnly (FALSE),
	m_bDirty (FALSE),
	m_nSize (0)
{
}

CFileStorage::~CFileStorage (void)
{
	if (m_bOpen)
	{
		f_close (&m_File);
	}
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

	return m_nSize >= 512;
}

bool CFileStorage::read (macemu::u32 nOffset, void *pBuffer, macemu::u32 nLength)
{
	UINT nRead = 0;
	if (   !m_bOpen
	    || nOffset + nLength > m_nSize
	    || f_lseek (&m_File, nOffset) != FR_OK
	    || f_read (&m_File, pBuffer, nLength, &nRead) != FR_OK)
	{
		return false;
	}

	return nRead == nLength;
}

bool CFileStorage::write (macemu::u32 nOffset, const void *pBuffer, macemu::u32 nLength)
{
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

	return nWritten == nLength;
}

void CFileStorage::flush (void)
{
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
