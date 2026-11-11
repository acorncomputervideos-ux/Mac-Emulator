// Web file manager for the SD card.

#include "webserver.h"
#include <circle/net/in.h>
#include <circle/sched/scheduler.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include "../frontend/util.h"

using macemu::strLen; using macemu::strCopy; using macemu::strAppend; using macemu::strEq;
using macemu::endsWithNoCase; using macemu::appendUint; using macemu::lower;

static const char FromWeb[] = "web";

static const unsigned BufferSize = 64 * 1024;
static const unsigned PageSize = 160 * 1024;
static const u64 MaxFileSize = 0xFFFFFFFFull;		// FAT32

static const char PageHead[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Mac Emulator</title><style>"
"body{font:15px -apple-system,Helvetica,Arial,sans-serif;margin:0;background:#eee;color:#222}"
"header{background:#333;color:#fff;padding:12px 18px;font-size:20px}"
"main{max-width:900px;margin:0 auto;padding:12px}"
"section{background:#fff;border:1px solid #ccc;border-radius:6px;margin:12px 0;padding:10px 14px}"
"h2{font-size:16px;margin:4px 0 8px}table{border-collapse:collapse;width:100%}"
"td{padding:5px 6px;border-top:1px solid #eee}td.s{text-align:right;white-space:nowrap;color:#555}"
"td.a{text-align:right;white-space:nowrap}a{color:#0645ad;text-decoration:none}"
"button{font-size:14px;padding:4px 10px;margin:2px}.u{color:#a60;font-size:13px}"
"#drop{border:2px dashed #999;border-radius:6px;padding:18px;text-align:center;color:#555}"
"#drop.on{border-color:#06c;background:#eef5ff}progress{width:100%}.n{color:#666;font-size:13px}"
"</style></head><body><header>Mac Emulator &mdash; SD card</header><main>";

static const char PageUpload[] =
"<section><h2>Copy files to the card</h2>"
"<div id=drop>Drop disk images or ROMs here, or <input type=file id=pick multiple></div>"
"<p>Put them in <select id=dir>"
"<option value='@DISKDIR@/'>the @DISKDIR@ folder (floppies and hard disks)</option>"
"<option value=''>the top of the card (ROMs, hd0.img ...)</option></select></p>"
"<div id=st></div><progress id=pr value=0 max=1 hidden></progress>"
"<p class=n>Open the menu (F12) on the Mac and pick the new disk under Floppy or Hard disks. "
"Transfers are fastest while the menu is open, because the Mac is paused.</p></section>";

static const char PageTail[] =
"<section><h2>Mac</h2><button onclick=restart()>Restart the Mac</button>"
" <span class=n>Saves the disks and starts the Mac again with the disks chosen in the menu.</span></section>"
"</main><script>"
"var q=[],busy=0;"
"function $(i){return document.getElementById(i)}"
"function add(fl){for(var i=0;i<fl.length;i++)q.push(fl[i]);next()}"
"function next(){if(busy)return;var f=q.shift();if(!f){return}"
"busy=1;var x=new XMLHttpRequest(),p=$('pr');p.hidden=false;p.value=0;"
"$('st').textContent='Sending '+f.name+' ...';"
"x.upload.onprogress=function(e){if(e.lengthComputable){p.max=e.total;p.value=e.loaded;"
"$('st').textContent='Sending '+f.name+': '+Math.floor(e.loaded*100/e.total)+'%'}};"
"x.onloadend=function(){busy=0;p.hidden=true;"
"if(x.status==201||x.status==200){$('st').textContent=f.name+' copied.';if(!q.length)location.reload()}"
"else{$('st').textContent=f.name+': '+(x.responseText||'failed')}next()};"
"x.open('PUT','/files/'+$('dir').value+encodeURIComponent(f.name));x.send(f)}"
"function del(n){if(!confirm('Delete '+n+'?'))return;"
"fetch('/files/'+n,{method:'DELETE'}).then(function(r){return r.text().then(function(t){"
"if(r.ok)location.reload();else alert(t)})})}"
"function restart(){fetch('/restart',{method:'POST'}).then(function(r){return r.text()}).then(alert)}"
"var d=$('drop');$('pick').onchange=function(){add(this.files)};"
"d.ondragover=function(e){e.preventDefault();d.className='on'};"
"d.ondragleave=function(){d.className=''};"
"d.ondrop=function(e){e.preventDefault();d.className='';add(e.dataTransfer.files)};"
"</script></body></html>";

CWebServer::CWebServer (CNetSubSystem *pNet, const char *pDrive, CWebServerHost *pHost)
:	CTask (0x10000),
	m_pNet (pNet),
	m_pDrive (pDrive),
	m_pHost (pHost),
	m_bFlushRequest (FALSE),
	m_bRestartRequest (FALSE),
	m_pBuffer (new u8[BufferSize]),
	m_pPage (new char[PageSize]),
	m_nPageLength (0),
	m_nHeaderLength (0),
	m_nBodyStart (0)
{
	m_Activity[0] = '\0';
	SetName ("web");
}

CWebServer::~CWebServer (void)
{
	delete [] m_pBuffer;
	delete [] m_pPage;
}

void CWebServer::Run (void)
{
	CSocket Listener (m_pNet, IPPROTO_TCP);
	if (Listener.Bind (80) < 0 || Listener.Listen (4) < 0)
	{
		CLogger::Get ()->Write (FromWeb, LogError, "Cannot listen on port 80");
		return;
	}

	while (1)
	{
		CIPAddress ForeignIP;
		u16 nForeignPort;
		CSocket *pConnection = Listener.Accept (&ForeignIP, &nForeignPort);
		if (pConnection == 0)
		{
			CScheduler::Get ()->MsSleep (100);
			continue;
		}

		pConnection->SetOptionReceiveTimeout (20 * 1000000);
		pConnection->SetOptionSendTimeout (20 * 1000000);
		Serve (pConnection);
		delete pConnection;		// closes the connection
		m_Activity[0] = '\0';
	}
}

// ----------------------------------------------------------------- requests

void CWebServer::Serve (CSocket *pSocket)
{
	TRequest Request;
	if (!ReadRequest (pSocket, &Request))
	{
		SendStatus (pSocket, 400, "Bad request");
		return;
	}

	const char *pPath = Request.Path;
	boolean bGet = strEq (Request.Method, "GET") || strEq (Request.Method, "HEAD");
	if (bGet && pPath[0] == '\0')
	{
		SendIndex (pSocket);
		return;
	}

	if (strEq (Request.Method, "POST") && strEq (pPath, "restart"))
	{
		Restart (pSocket);
		return;
	}

	if (strncmp (pPath, "files/", 6) != 0)
	{
		SendStatus (pSocket, 404, "Not found");
		return;
	}
	pPath += 6;

	if (!SafePath (pPath))
	{
		SendStatus (pSocket, 403, "That name is not allowed.");
		return;
	}

	if (bGet)
	{
		SendFile (pSocket, pPath);
	}
	else if (strEq (Request.Method, "PUT"))
	{
		ReceiveFile (pSocket, &Request, pPath);
	}
	else if (strEq (Request.Method, "DELETE"))
	{
		DeleteFile (pSocket, pPath);
	}
	else
	{
		SendStatus (pSocket, 405, "Method not allowed");
	}
}

static int HexDigit (char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

boolean CWebServer::ReadRequest (CSocket *pSocket, TRequest *pRequest)
{
	// Read until the blank line that ends the header.
	m_nHeaderLength = 0;
	unsigned nEnd = 0;
	while (nEnd == 0)
	{
		if (m_nHeaderLength >= sizeof m_Header - 1)
		{
			return FALSE;
		}

		int nResult = pSocket->Receive (m_Header + m_nHeaderLength,
						sizeof m_Header - 1 - m_nHeaderLength, 0);
		if (nResult <= 0)
		{
			return FALSE;
		}
		m_nHeaderLength += (unsigned) nResult;

		for (unsigned i = 3; i < m_nHeaderLength; i++)
		{
			if (m_Header[i - 3] == '\r' && m_Header[i - 2] == '\n'
			    && m_Header[i - 1] == '\r' && m_Header[i] == '\n')
			{
				nEnd = i + 1;
				break;
			}
		}
	}
	m_nBodyStart = nEnd;

	// Request line: METHOD /path?query HTTP/1.1
	const char *p = m_Header;
	unsigned n = 0;
	while (*p != ' ' && p < m_Header + nEnd && n < sizeof pRequest->Method - 1)
	{
		pRequest->Method[n++] = *p++;
	}
	pRequest->Method[n] = '\0';
	if (*p++ != ' ' || *p++ != '/')
	{
		return FALSE;
	}

	n = 0;
	while (*p != ' ' && *p != '?' && *p != '\r' && n < sizeof pRequest->Path - 1)
	{
		char c = *p++;
		if (c == '%' && HexDigit (p[0]) >= 0 && HexDigit (p[1]) >= 0)
		{
			c = (char) (HexDigit (p[0]) * 16 + HexDigit (p[1]));
			p += 2;
		}
		else if (c == '+')
		{
			c = ' ';
		}
		if (c == '\0')
		{
			return FALSE;
		}
		pRequest->Path[n++] = c;
	}
	pRequest->Path[n] = '\0';
	if (*p != ' ' && *p != '?')
	{
		return FALSE;			// path too long
	}

	n = 0;
	if (*p == '?')
	{
		p++;
		while (*p != ' ' && *p != '\r' && n < sizeof pRequest->Query - 1)
		{
			pRequest->Query[n++] = *p++;
		}
	}
	pRequest->Query[n] = '\0';

	// Headers: only Content-Length matters.
	pRequest->nContentLength = 0;
	pRequest->bHaveLength = FALSE;
	char chSaved = m_Header[nEnd];		// the first body byte, maybe
	m_Header[nEnd] = '\0';
	for (const char *pLine = m_Header; pLine < m_Header + nEnd; )
	{
		const char *pNext = pLine;
		while (pNext < m_Header + nEnd && *pNext != '\n') pNext++;
		pNext++;

		static const char Key[] = "content-length:";
		unsigned k = 0;
		while (Key[k] && lower (pLine[k]) == Key[k]) k++;
		if (Key[k] == '\0')
		{
			const char *v = pLine + k;
			while (*v == ' ') v++;
			u64 nValue = 0;
			while (*v >= '0' && *v <= '9' && nValue <= MaxFileSize)
			{
				nValue = nValue * 10 + (u64) (*v++ - '0');
			}
			pRequest->nContentLength = nValue;
			pRequest->bHaveLength = TRUE;
		}

		pLine = pNext;
	}
	m_Header[nEnd] = chSaved;

	return TRUE;
}

// ----------------------------------------------------------------- the page

void CWebServer::SendIndex (CSocket *pSocket)
{
	m_nPageLength = 0;
	Out (PageHead);

	// The upload box, with the disk folder's name filled in.
	const char *pDiskDir = m_pHost->DiskDir ();
	for (const char *p = PageUpload; *p; )
	{
		if (strncmp (p, "@DISKDIR@", 9) == 0)
		{
			OutHtml (pDiskDir);
			p += 9;
		}
		else
		{
			char Ch[2] = {*p++, '\0'};
			Out (Ch);
		}
	}

	ListFolder ("", FALSE);
	if (pDiskDir[0] != '\0')
	{
		ListFolder (pDiskDir, TRUE);
	}

	// Free space.
	char Root[8];
	strCopy (Root, m_pDrive, sizeof Root);
	strAppend (Root, "/", sizeof Root);
	DWORD nFreeClusters;
	FATFS *pFS;
	if (f_getfree (Root, &nFreeClusters, &pFS) == FR_OK)
	{
		Out ("<p class=n>Free space on the card: ");
		OutSize ((u64) nFreeClusters * pFS->csize * 512);
		Out ("</p>");
	}

	Out (PageTail);

	char Head[160] = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ";
	appendUint (Head, m_nPageLength, sizeof Head);
	strAppend (Head, "\r\n\r\n", sizeof Head);
	if (SendText (pSocket, Head))
	{
		SendAll (pSocket, m_pPage, m_nPageLength);
	}
}

void CWebServer::ListFolder (const char *pFolder, boolean bDiskDir)
{
	Out ("<section><h2>");
	if (bDiskDir)
	{
		Out ("Folder ");
		OutHtml (pFolder);
	}
	else
	{
		Out ("Top of the card");
	}
	Out ("</h2><table>");

	char Path[192];
	FullPath (Path, sizeof Path, pFolder);
	DIR Directory;
	FILINFO Info;
	unsigned nCount = 0;
	FRESULT Result = f_findfirst (&Directory, &Info, Path, "*");
	while (Result == FR_OK && Info.fname[0] != '\0')
	{
		char Name[192] = "";
		if (bDiskDir)
		{
			strCopy (Name, pFolder, sizeof Name);
			strAppend (Name, "/", sizeof Name);
		}
		strAppend (Name, Info.fname, sizeof Name);

		if (!(Info.fattrib & (AM_DIR | AM_HID | AM_SYS)) && Info.fname[0] != '.'
		    && !(!bDiskDir && Protected (Info.fname)))
		{
			Out ("<tr><td><a href='/files/");
			OutUrl (Name);
			Out ("' download>");
			OutHtml (Info.fname);
			Out ("</a>");
			if (m_pHost->FileInUse (Name))
			{
				Out (" <span class=u>in use</span>");
			}
			Out ("</td><td class=s>");
			OutSize (Info.fsize);
			Out ("</td><td class=a><button onclick=\"del('");
			OutUrl (Name);
			Out ("')\">Delete</button></td></tr>");
			nCount++;
		}

		Result = f_findnext (&Directory, &Info);
	}
	f_closedir (&Directory);

	if (nCount == 0)
	{
		Out ("<tr><td class=n>No files.</td></tr>");
	}
	Out ("</table></section>");
}

// ----------------------------------------------------------------- transfers

void CWebServer::SendFile (CSocket *pSocket, const char *pPath)
{
	if (m_pHost->FileInUse (pPath))
	{
		FlushEmulator ();		// so that the copy is up to date
	}

	char Path[192];
	FullPath (Path, sizeof Path, pPath);
	FIL File;
	if (f_open (&File, Path, FA_READ) != FR_OK)
	{
		SendStatus (pSocket, 404, "No such file.");
		return;
	}

	u32 nSize = (u32) f_size (&File);
	const char *pName = pPath;
	for (const char *p = pPath; *p; p++)
	{
		if (*p == '/') pName = p + 1;
	}

	char Head[384] = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
			 "Connection: close\r\nContent-Disposition: attachment; filename=\"";
	strAppend (Head, pName, sizeof Head);
	strAppend (Head, "\"\r\nContent-Length: ", sizeof Head);
	appendUint (Head, nSize, sizeof Head);
	strAppend (Head, "\r\n\r\n", sizeof Head);

	strCopy (m_Activity, "Sending ", sizeof m_Activity);
	strAppend (m_Activity, pName, sizeof m_Activity);

	if (SendText (pSocket, Head))
	{
		u32 nLeft = nSize;
		while (nLeft > 0)
		{
			UINT nRead = 0;
			if (f_read (&File, m_pBuffer, BufferSize, &nRead) != FR_OK || nRead == 0
			    || !SendAll (pSocket, m_pBuffer, nRead))
			{
				break;
			}
			nLeft -= nRead < nLeft ? nRead : nLeft;
		}
	}

	f_close (&File);
}

void CWebServer::ReceiveFile (CSocket *pSocket, TRequest *pRequest, const char *pPath)
{
	if (!pRequest->bHaveLength || pRequest->nContentLength > MaxFileSize)
	{
		SendStatus (pSocket, 411, "The file is too large for the card (4 GB at most).");
		return;
	}

	if (Protected (pPath))
	{
		SendStatus (pSocket, 403, "That file belongs to the Pi's start-up and cannot be replaced here.");
		return;
	}

	if (m_pHost->FileInUse (pPath))
	{
		SendStatus (pSocket, 409, "The Mac is using that file: eject it in the menu first.");
		return;
	}

	// Write to a temporary name, then rename, so that a broken transfer
	// never leaves half a disk behind.
	char Path[192], TempPath[200];
	FullPath (Path, sizeof Path, pPath);
	strCopy (TempPath, Path, sizeof TempPath);
	strAppend (TempPath, ".part", sizeof TempPath);

	FIL File;
	if (f_open (&File, TempPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		SendStatus (pSocket, 500, "Cannot create the file (is the folder there?).");
		return;
	}

	const char *pName = pPath;
	for (const char *p = pPath; *p; p++)
	{
		if (*p == '/') pName = p + 1;
	}
	CLogger::Get ()->Write (FromWeb, LogNotice, "Receiving %s (%u bytes)", pPath,
				(unsigned) pRequest->nContentLength);

	u64 nLeft = pRequest->nContentLength;
	u64 nTotal = nLeft;
	boolean bOK = TRUE;

	// Body bytes that came with the header.
	unsigned nHave = m_nHeaderLength - m_nBodyStart;
	if (nHave > nLeft) nHave = (unsigned) nLeft;
	memcpy (m_pBuffer, m_Header + m_nBodyStart, nHave);
	nLeft -= nHave;

	unsigned nLastPercent = 101;
	while (bOK && (nLeft > 0 || nHave > 0))
	{
		while (nLeft > 0 && nHave < BufferSize)
		{
			unsigned nWant = BufferSize - nHave;
			if (nWant > nLeft) nWant = (unsigned) nLeft;
			if (nWant > FRAME_BUFFER_SIZE) nWant = FRAME_BUFFER_SIZE;
			int nResult = pSocket->Receive (m_pBuffer + nHave, nWant, 0);
			if (nResult <= 0)
			{
				bOK = FALSE;
				break;
			}
			nHave += (unsigned) nResult;
			nLeft -= (unsigned) nResult;
		}

		UINT nWritten = 0;
		if (bOK && nHave > 0
		    && (f_write (&File, m_pBuffer, nHave, &nWritten) != FR_OK || nWritten != nHave))
		{
			f_close (&File);
			f_unlink (TempPath);
			SendStatus (pSocket, 507, "The card is full.");
			return;
		}
		nHave = 0;

		unsigned nPercent = nTotal ? (unsigned) ((nTotal - nLeft) * 100 / nTotal) : 100;
		if (nPercent != nLastPercent)
		{
			nLastPercent = nPercent;
			strCopy (m_Activity, "Receiving ", sizeof m_Activity);
			strAppend (m_Activity, pName, sizeof m_Activity - 6);
			strAppend (m_Activity, " ", sizeof m_Activity);
			appendUint (m_Activity, nPercent, sizeof m_Activity);
			strAppend (m_Activity, "%", sizeof m_Activity);
		}
	}

	f_close (&File);
	if (!bOK)
	{
		f_unlink (TempPath);
		CLogger::Get ()->Write (FromWeb, LogWarning, "Transfer of %s broken off", pPath);
		return;				// the connection is gone: no answer
	}

	f_unlink (Path);
	if (f_rename (TempPath, Path) != FR_OK)
	{
		f_unlink (TempPath);
		SendStatus (pSocket, 500, "Cannot rename the file.");
		return;
	}

	CLogger::Get ()->Write (FromWeb, LogNotice, "Received %s", pPath);
	SendStatus (pSocket, 201, "Copied.");
}

void CWebServer::DeleteFile (CSocket *pSocket, const char *pPath)
{
	if (Protected (pPath))
	{
		SendStatus (pSocket, 403, "That file belongs to the Pi's start-up and cannot be deleted here.");
		return;
	}

	if (m_pHost->FileInUse (pPath))
	{
		SendStatus (pSocket, 409, "The Mac is using that file: eject it in the menu first.");
		return;
	}

	char Path[192];
	FullPath (Path, sizeof Path, pPath);
	if (f_unlink (Path) != FR_OK)
	{
		SendStatus (pSocket, 404, "Cannot delete that file.");
		return;
	}

	CLogger::Get ()->Write (FromWeb, LogNotice, "Deleted %s", pPath);
	SendStatus (pSocket, 200, "Deleted.");
}

void CWebServer::Restart (CSocket *pSocket)
{
	m_bRestartRequest = TRUE;
	unsigned nStart = CTimer::Get ()->GetTicks ();
	while (m_bRestartRequest && CTimer::Get ()->GetTicks () - nStart < 3 * HZ)
	{
		CScheduler::Get ()->MsSleep (20);
	}

	SendStatus (pSocket, 200, m_bRestartRequest ? "The Mac is busy; it will restart shortly."
						    : "The Mac has been restarted.");
}

// Ask the main loop to write the emulator's disk caches back, and wait.
void CWebServer::FlushEmulator (void)
{
	m_bFlushRequest = TRUE;
	unsigned nStart = CTimer::Get ()->GetTicks ();
	while (m_bFlushRequest && CTimer::Get ()->GetTicks () - nStart < 2 * HZ)
	{
		CScheduler::Get ()->MsSleep (10);
	}
}

// ----------------------------------------------------------------- helpers

void CWebServer::SendStatus (CSocket *pSocket, unsigned nCode, const char *pMessage)
{
	char Head[512] = "HTTP/1.1 ";
	appendUint (Head, nCode, sizeof Head);
	strAppend (Head, nCode < 300 ? " OK" : " Error", sizeof Head);
	strAppend (Head, "\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n"
			 "Content-Length: ", sizeof Head);
	appendUint (Head, strLen (pMessage), sizeof Head);
	strAppend (Head, "\r\n\r\n", sizeof Head);
	strAppend (Head, pMessage, sizeof Head);
	SendText (pSocket, Head);
}

boolean CWebServer::SendAll (CSocket *pSocket, const void *pData, unsigned nLength)
{
	const u8 *p = (const u8 *) pData;
	while (nLength > 0)
	{
		unsigned nChunk = nLength > FRAME_BUFFER_SIZE ? FRAME_BUFFER_SIZE : nLength;
		int nResult = pSocket->Send (p, nChunk, 0);
		if (nResult <= 0)
		{
			return FALSE;
		}
		p += nResult;
		nLength -= (unsigned) nResult;
	}

	return TRUE;
}

boolean CWebServer::SendText (CSocket *pSocket, const char *pText)
{
	return SendAll (pSocket, pText, strLen (pText));
}

// Relative paths only, no "..", no odd characters FAT cannot store.
boolean CWebServer::SafePath (const char *pPath) const
{
	if (pPath[0] == '\0' || pPath[0] == '/' || strLen (pPath) > 150)
	{
		return FALSE;
	}

	for (const char *p = pPath; *p; p++)
	{
		unsigned char c = (unsigned char) *p;
		if (c < ' ' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"'
		    || c == '<' || c == '>' || c == '|')
		{
			return FALSE;
		}
		if (c == '.' && (p == pPath || p[-1] == '/') && p[1] == '.')
		{
			return FALSE;
		}
	}

	return !endsWithNoCase (pPath, "/") && !endsWithNoCase (pPath, ".part");
}

// The Pi's own start-up files at the top of the card.
boolean CWebServer::Protected (const char *pPath)
{
	for (const char *p = pPath; *p; p++)
	{
		if (*p == '/')
		{
			return strncasecmp (pPath, "overlays/", 9) == 0;
		}
	}

	const char *pPrefixes[] = {"kernel", "start", "fixup", "bootcode", "armstub", "config.txt",
				   "cmdline.txt", "LICENCE", 0};
	for (unsigned i = 0; pPrefixes[i] != 0; i++)
	{
		if (strncasecmp (pPath, pPrefixes[i], strlen (pPrefixes[i])) == 0)
		{
			return TRUE;
		}
	}

	return endsWithNoCase (pPath, ".dtb") || endsWithNoCase (pPath, ".elf");
}

void CWebServer::FullPath (char *pOut, unsigned nMax, const char *pPath) const
{
	strCopy (pOut, m_pDrive, nMax);
	strAppend (pOut, "/", nMax);
	strAppend (pOut, pPath, nMax);
}

void CWebServer::Out (const char *pText)
{
	while (*pText && m_nPageLength < PageSize - 64)
	{
		m_pPage[m_nPageLength++] = *pText++;
	}
}

void CWebServer::OutHtml (const char *pText)
{
	for (; *pText; pText++)
	{
		switch (*pText)
		{
		case '<':  Out ("&lt;"); break;
		case '>':  Out ("&gt;"); break;
		case '&':  Out ("&amp;"); break;
		case '\'': Out ("&#39;"); break;
		case '"':  Out ("&quot;"); break;
		default:   { char Ch[2] = {*pText, '\0'}; Out (Ch); } break;
		}
	}
}

void CWebServer::OutUrl (const char *pText)
{
	for (; *pText; pText++)
	{
		unsigned char c = (unsigned char) *pText;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
		    || c == '-' || c == '_' || c == '.' || c == '/' || c == '~')
		{
			char Ch[2] = {(char) c, '\0'};
			Out (Ch);
		}
		else
		{
			char Hex[4] = {'%', "0123456789ABCDEF"[c >> 4], "0123456789ABCDEF"[c & 15], '\0'};
			Out (Hex);
		}
	}
}

void CWebServer::OutNumber (u64 nValue)
{
	char Text[24] = "";
	char Digits[24];
	unsigned n = 0;
	do { Digits[n++] = (char) ('0' + nValue % 10); nValue /= 10; } while (nValue);
	for (unsigned i = 0; i < n; i++) Text[i] = Digits[n - 1 - i];
	Text[n] = '\0';
	Out (Text);
}

void CWebServer::OutSize (u64 nBytes)
{
	if (nBytes >= 10ull * 1024 * 1024 * 1024)
	{
		OutNumber (nBytes >> 30);
		Out (" GB");
	}
	else if (nBytes >= 1024 * 1024 * 10)
	{
		OutNumber (nBytes >> 20);
		Out (" MB");
	}
	else if (nBytes >= 1024 * 10)
	{
		OutNumber (nBytes >> 10);
		Out (" KB");
	}
	else
	{
		OutNumber (nBytes);
		Out (" bytes");
	}
}
