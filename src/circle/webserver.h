// A small web file manager for the SD card: list, upload, download and
// delete disk images and ROMs from any browser on the local network.
//
// It runs as a Circle task.  Uploads and downloads are streamed in chunks,
// so files of any size (up to the FAT32 limit of 4 GB) work.  Files the
// emulator is using cannot be replaced or deleted, and Pi boot files are
// protected.

#ifndef _macemu_webserver_h
#define _macemu_webserver_h

#include <circle/sched/task.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/types.h>

class CWebServerHost
{
public:
	virtual ~CWebServerHost (void) {}
	// Called on the web server's task: must not touch the emulator directly.
	virtual boolean FileInUse (const char *pPath) = 0;	// relative to the card's root
	virtual const char *DiskDir (void) = 0;
	virtual const char *Version (void) = 0;			// shown on the page
};

class CWebServer : public CTask
{
public:
	CWebServer (CNetSubSystem *pNet, const char *pDrive, CWebServerHost *pHost);
	~CWebServer (void);

	void Run (void) override;

	// Requests for the emulator's main loop, which owns the Mac.  It calls
	// Done*() when it has carried them out.
	boolean FlushRequested (void) const	{ return m_bFlushRequest; }
	void FlushDone (void)			{ m_bFlushRequest = FALSE; }
	boolean RestartRequested (void) const	{ return m_bRestartRequest; }
	void RestartDone (void)			{ m_bRestartRequest = FALSE; }
	boolean RebootRequested (void) const	{ return m_bRebootRequest; }

	// What is happening, for the menu: "" when idle.
	const char *Activity (void) const	{ return m_Activity; }

private:
	struct TRequest
	{
		char Method[8];
		char Path[192];		// decoded, without the leading '/'
		char Query[64];
		u64 nContentLength;
		boolean bHaveLength;
	};

	void Serve (CSocket *pSocket);
	boolean ReadRequest (CSocket *pSocket, TRequest *pRequest);
	void SendIndex (CSocket *pSocket);
	void ListFolder (const char *pFolder, boolean bDiskDir);
	void SendFile (CSocket *pSocket, const char *pPath);
	void ReceiveFile (CSocket *pSocket, TRequest *pRequest, const char *pPath, boolean bUpdate = FALSE);
	void Reboot (CSocket *pSocket);
	void MakeFolders (const char *pPath);
	static boolean Private (const char *pPath);
	boolean Updatable (const char *pPath) const;
	void DeleteFile (CSocket *pSocket, const char *pPath);
	void Restart (CSocket *pSocket);

	void SendStatus (CSocket *pSocket, unsigned nCode, const char *pMessage);
	boolean SendAll (CSocket *pSocket, const void *pData, unsigned nLength);
	boolean SendText (CSocket *pSocket, const char *pText);

	void FlushEmulator (void);
	boolean SafePath (const char *pPath) const;
	static boolean Protected (const char *pPath);
	void FullPath (char *pOut, unsigned nMax, const char *pPath) const;

	// Page builder
	void Out (const char *pText);
	void OutHtml (const char *pText);		// escaped
	void OutUrl (const char *pText);		// percent-encoded
	void OutNumber (u64 nValue);
	void OutSize (u64 nBytes);

private:
	CNetSubSystem *m_pNet;
	const char *m_pDrive;
	CWebServerHost *m_pHost;
	volatile boolean m_bFlushRequest;
	volatile boolean m_bRestartRequest;
	volatile boolean m_bRebootRequest;
	char m_Activity[64];

	u8 *m_pBuffer;				// transfer buffer
	char *m_pPage;				// HTML page being built
	unsigned m_nPageLength;
	char m_Header[2048];
	unsigned m_nHeaderLength;		// bytes in m_Header
	unsigned m_nBodyStart;			// body bytes already read with the header
};

#endif
