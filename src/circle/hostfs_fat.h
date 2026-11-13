// The shared folder's files on the SD card (FatFs).  Every call is carried
// out on core 0, where the card is served, even when the Mac (on core 1)
// makes it.
#ifndef _hostfs_fat_h
#define _hostfs_fat_h

#include <fatfs/ff.h>
#include "../core/hostfs.h"

class CFatHostFS : public macemu::HostFS
{
public:
	explicit CFatHostFS (const char *pRoot);		// e.g. "SD:/shared"

	bool stat (const char *pPath, macemu::HostStat &rStat) override;
	int openDir (const char *pPath) override;
	bool readDir (int nDir, char *pName, macemu::u32 nMax, macemu::u32 *pTime) override;
	void closeDir (int nDir) override;
	int open (const char *pPath, int nMode) override;
	macemu::s32 read (int nFile, macemu::u32 nOffset, void *pBuffer, macemu::u32 nLength) override;
	macemu::s32 write (int nFile, macemu::u32 nOffset, const void *pBuffer, macemu::u32 nLength) override;
	bool truncate (int nFile, macemu::u32 nSize) override;
	macemu::u32 size (int nFile) override;
	void close (int nFile) override;
	bool makeDir (const char *pPath) override;
	bool remove (const char *pPath) override;
	bool rename (const char *pFrom, const char *pTo) override;
	macemu::u64 freeBytes (void) override;
	macemu::u32 milliseconds (void) override;
	bool namesUtf8 (void) const override		{ return false; }

	enum { MaxFiles = 48, MaxDirs = 8 };

	struct TRequest;
private:
	static void Execute (void *pRequest);
	void Do (TRequest &rReq);
	void Full (char *pOut, const char *pPath) const;

	char m_Root[64];
	FIL m_File[MaxFiles];
	bool m_bFileUsed[MaxFiles];
	DIR m_Dir[MaxDirs];
	bool m_bDirUsed[MaxDirs];
};

#endif
