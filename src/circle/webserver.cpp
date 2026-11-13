// Web file manager for the SD card.

#include "webserver.h"
#include <circle/net/in.h>
#include <circle/sched/scheduler.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include "../frontend/util.h"
#include "../frontend/hfs.h"

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
"#drop,#upd,#tdrop{border:2px dashed #999;border-radius:6px;padding:18px;text-align:center;color:#555}"
"#drop.on,#upd.on,#tdrop.on{border-color:#06c;background:#eef5ff}progress{width:100%}.n{color:#666;font-size:13px}"
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
"<section><h2>Send files to the Mac</h2>"
"<div id=tdrop>Drop files here, or <input type=file id=tpick multiple></div>"
"<p class=n>Any size. Small files go on a new floppy in floppy drive 2; bigger ones on a new CD-ROM disk (\"Transfer 1\", ...) in the CD-ROM drive. "
"Either appears on the Mac's desktop: drag the files to your hard disk. MacBinary files (.bin) keep their icons and resource forks. "
"To get files out of a disk, click [open] next to it.</p>"
"<div id=tst></div></section>"
"<section><h2>Mac</h2><button onclick=restart()>Restart the Mac</button>"
" <span class=n>Saves the disks and starts the Mac again with the disks chosen in the menu.</span></section>"
"<section><h2>Update the emulator</h2>"
"<div id=upd>Drop the new <b>MacEmulator-SD.zip</b> here, or <input type=file id=updpick accept='.zip'></div>"
"<div id=ust></div><progress id=upr value=0 max=1 hidden></progress>"
"<p class=n>Installs the new emulator and Pi start-up files, then restarts the Pi. "
"Your settings, ROMs and disks are kept.</p></section>"
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
// The update: unzip in the browser, send each file, then reboot the Pi.
"var keep=/^(macemu\\.cfg|macemu\\.pram|wpa_supplicant\\.conf|macemu-wifi\\.conf|disks\\/.*|screenshots\\/.*|.*\\.rom)$/i;"
"function u16(d,o){return d.getUint16(o,true)}function u32(d,o){return d.getUint32(o,true)}"
"async function unzip(buf){var d=new DataView(buf),e=buf.byteLength-22;"
"while(e>=0&&u32(d,e)!=0x06054b50)e--;if(e<0)throw 'This is not a zip file.';"
"var n=u16(d,e+10),p=u32(d,e+16),out=[];"
"for(var i=0;i<n;i++){if(u32(d,p)!=0x02014b50)throw 'The zip file is damaged.';"
"var m=u16(d,p+10),cs=u32(d,p+20),nl=u16(d,p+28),xl=u16(d,p+30),cl=u16(d,p+32),lo=u32(d,p+42);"
"var name=new TextDecoder().decode(new Uint8Array(buf,p+46,nl));p+=46+nl+xl+cl;"
"if(name.endsWith('/'))continue;"
"var ds=lo+30+u16(d,lo+26)+u16(d,lo+28),raw=new Uint8Array(buf,ds,cs);"
"out.push({name:name,method:m,raw:raw})}"
"var pre=out.length&&out[0].name.indexOf('/')>0?out[0].name.split('/')[0]+'/':'';"
"if(pre&&out.every(function(f){return f.name.startsWith(pre)}))"
"out.forEach(function(f){f.name=f.name.slice(pre.length)});"
"return out}"
"async function inflate(f){if(f.method==0)return f.raw;"
"if(f.method!=8)throw f.name+': unsupported compression';"
"var s=new Blob([f.raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'));"
"return new Uint8Array(await new Response(s).arrayBuffer())}"
"async function update(file){var st=$('ust'),p=$('upr');"
"if(typeof DecompressionStream=='undefined'){alert('Please use a newer web browser for updates.');return}"
"try{st.textContent='Reading '+file.name+' ...';var fl=await unzip(await file.arrayBuffer());"
"fl=fl.filter(function(f){return !keep.test(f.name)});"
"if(!fl.some(function(f){return /^kernel.*\\.img$/.test(f.name)}))throw 'There is no emulator in this zip file.';"
"p.hidden=false;p.max=fl.length;"
"for(var i=0;i<fl.length;i++){var f=fl[i];p.value=i;st.textContent='Installing '+f.name+' ('+(i+1)+' of '+fl.length+')';"
"var r=await fetch('/update/'+f.name.split('/').map(encodeURIComponent).join('/'),{method:'PUT',body:await inflate(f)});"
"if(!r.ok)throw f.name+': '+await r.text()}"
"p.value=fl.length;st.textContent='Installed. The Pi is restarting; this page will come back in a minute.';"
"await fetch('/reboot',{method:'POST'});setTimeout(function(){location.reload()},30000)}"
"catch(x){st.textContent='Update stopped: '+x}}"
"async function transfer(fl){var st=$('tst');try{"
"if(fl.length>64)throw 'Send at most 64 files at a time.';"
"await fetch('/makefloppy?clear=1',{method:'POST'});"
"for(var i=0;i<fl.length;i++){st.textContent='Sending '+fl[i].name+' ...';"
"var r=await fetch('/files/transfer/'+encodeURIComponent(fl[i].name),{method:'PUT',body:fl[i]});"
"if(!r.ok)throw fl[i].name+': '+await r.text()}"
"st.textContent='Making the disk for the Mac ...';var r=await fetch('/makefloppy?insert=2',{method:'POST'});"
"st.textContent=await r.text();if(r.ok)setTimeout(function(){location.reload()},2500)}"
"catch(x){st.textContent='Stopped: '+(x instanceof TypeError?'the connection to the Pi was lost. Check the Wi-Fi and try again.':x)}}"
"var td=$('tdrop');$('tpick').onchange=function(){transfer(this.files)};"
"td.ondragover=function(e){e.preventDefault();td.className='on'};"
"td.ondragleave=function(){td.className=''};"
"td.ondrop=function(e){e.preventDefault();td.className='';transfer(e.dataTransfer.files)};"
"var ud=$('upd');$('updpick').onchange=function(){if(this.files[0])update(this.files[0])};"
"ud.ondragover=function(e){e.preventDefault();ud.className='on'};"
"ud.ondragleave=function(){ud.className=''};"
"ud.ondrop=function(e){e.preventDefault();ud.className='';if(e.dataTransfer.files[0])update(e.dataTransfer.files[0])};"
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
	m_bRebootRequest (FALSE),
	m_bInsertOK (FALSE),
	m_pBuffer (new u8[BufferSize]),
	m_pPage (new char[PageSize]),
	m_nPageLength (0),
	m_nHeaderLength (0),
	m_nBodyStart (0),
	m_nBodyUnread (0)
{
	m_Activity[0] = '\0';
	m_InsertName[0] = '\0';
	SetName ("web");
}

CWebServer::~CWebServer (void)
{
	delete [] m_pBuffer;
	delete [] m_pPage;
}

void CWebServer::Run (void)
{
	// Listen only once the Pi has its address: Circle's listening socket
	// keeps the address it had when it was made for its checksums, so one
	// made before DHCP finished (0.0.0.0) silently drops every connection.
	while (!m_pNet->IsRunning ())
	{
		CScheduler::Get ()->MsSleep (100);
	}

	CSocket Listener (m_pNet, IPPROTO_TCP);
	if (Listener.Bind (80) < 0 || Listener.Listen (4) < 0)
	{
		CLogger::Get ()->Write (FromWeb, LogError, "Cannot listen on port 80");
		return;
	}
	CLogger::Get ()->Write (FromWeb, LogNotice, "Listening on port 80");

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

		CString From;
		ForeignIP.Format (&From);
		CLogger::Get ()->Write (FromWeb, LogNotice, "Connection from %s", (const char *) From);

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

	if (strEq (Request.Method, "POST") && strEq (pPath, "reboot"))
	{
		Reboot (pSocket);
		return;
	}

	// Inside Mac disk images.
	if (bGet && strncmp (pPath, "hfs/", 4) == 0 && SafePath (pPath + 4) && !Private (pPath + 4))
	{
		SendHfsFolder (pSocket, pPath + 4, Request.Query);
		return;
	}
	if (bGet && strncmp (pPath, "hfsget/", 7) == 0 && SafePath (pPath + 7) && !Private (pPath + 7))
	{
		SendHfsFile (pSocket, pPath + 7, Request.Query);
		return;
	}
	if (strEq (Request.Method, "POST") && strEq (pPath, "makefloppy"))
	{
		MakeFloppy (pSocket, Request.Query);
		return;
	}

	// Software update: the files of a new SD card zip, one by one.
	if (strEq (Request.Method, "PUT") && strncmp (pPath, "update/", 7) == 0)
	{
		pPath += 7;
		if (!SafePath (pPath) || !Updatable (pPath))
		{
			SendStatus (pSocket, 403, "The update may not replace that file.");
			return;
		}
		ReceiveFile (pSocket, &Request, pPath, TRUE);
		return;
	}

	// Files for the Mac can have any name: make one the card can store
	// (only plain characters; FAT cannot take some, and the card's names
	// are not Unicode).  MacBinary files carry their real name inside.
	if (strncmp (pPath, "files/transfer/", 15) == 0)
	{
		for (char *p = Request.Path + 15; *p; p++)
		{
			unsigned char c = (unsigned char) *p;
			if (c < ' ' || c >= 0x7F || strchr ("\\/:*?\"<>|", c) != 0)
			{
				*p = '_';
			}
		}
		if (Request.Path[15] == '.')
		{
			Request.Path[15] = '_';
		}
	}

	if (strncmp (pPath, "files/", 6) != 0)
	{
		SendStatus (pSocket, 404, "Not found");
		return;
	}
	pPath += 6;

	if (!SafePath (pPath) || Private (pPath))
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
	m_nBodyUnread = 0;
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

	unsigned nWithHeader = m_nHeaderLength - m_nBodyStart;
	m_nBodyUnread = pRequest->nContentLength > nWithHeader ? pRequest->nContentLength - nWithHeader : 0;

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

	Out ("<p class=n>Emulator version: ");
	OutHtml (m_pHost->Version ());
	Out ("</p>");
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
		    && !(!bDiskDir && (Protected (Info.fname) || Private (Info.fname))))
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
			if (LooksLikeDisk (Info.fname, Info.fsize))
			{
				Out (" <a class=n href='/hfs/");
				OutUrl (Name);
				Out ("'>[open]</a>");
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

void CWebServer::ReceiveFile (CSocket *pSocket, TRequest *pRequest, const char *pPath, boolean bUpdate)
{
	if (!pRequest->bHaveLength || pRequest->nContentLength > MaxFileSize)
	{
		SendStatus (pSocket, 411, "The file is too large for the card (4 GB at most).");
		return;
	}

	if (!bUpdate && Protected (pPath))
	{
		SendStatus (pSocket, 403, "That file belongs to the Pi's start-up and cannot be replaced here.");
		return;
	}

	if (m_pHost->FileInUse (pPath))
	{
		SendStatus (pSocket, 409, "The Mac is using that file: eject it in the menu first.");
		return;
	}

	MakeFolders (pPath);

	// Write to a temporary name, then rename, so that a broken transfer
	// never leaves half a disk behind.
	char Path[192], TempPath[200];
	FullPath (Path, sizeof Path, pPath);
	strCopy (TempPath, Path, sizeof TempPath);
	strAppend (TempPath, ".part", sizeof TempPath);

	FIL File;
	if (f_open (&File, TempPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		SendStatus (pSocket, 500, "Cannot create the file.");
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
	m_nBodyUnread = 0;				// read below
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

void CWebServer::Reboot (CSocket *pSocket)
{
	SendStatus (pSocket, 200, "The Pi is restarting.");
	CScheduler::Get ()->MsSleep (300);	// let the answer go out
	m_bRebootRequest = TRUE;
}

// Create the folders on the way to a file (firmware/, overlays/ ...).
void CWebServer::MakeFolders (const char *pPath)
{
	char Path[192];
	FullPath (Path, sizeof Path, pPath);
	unsigned nSkip = strLen (m_pDrive) + 1;
	for (char *p = Path + nSkip; *p; p++)
	{
		if (*p == '/')
		{
			*p = '\0';
			f_mkdir (Path);			// fails harmlessly if it exists
			*p = '/';
		}
	}
}

// Files with the Wi-Fi password, and the settings that hold it: never
// listed or sent, and never replaced from the web.
boolean CWebServer::Private (const char *pPath)
{
	return strcasecmp (pPath, "macemu.cfg") == 0 || strcasecmp (pPath, "macemu-wifi.conf") == 0
	       || strcasecmp (pPath, "wpa_supplicant.conf") == 0 || strcasecmp (pPath, "macemu.pram") == 0;
}

// An update may replace the emulator and the Pi's start-up files, but not
// your settings, ROMs or disks.
boolean CWebServer::Updatable (const char *pPath) const
{
	if (Private (pPath) || endsWithNoCase (pPath, ".rom"))
	{
		return FALSE;
	}

	const char *pDiskDir = m_pHost->DiskDir ();
	unsigned n = strLen (pDiskDir);
	if (n > 0 && strncasecmp (pPath, pDiskDir, n) == 0 && pPath[n] == '/')
	{
		return FALSE;
	}

	return strncasecmp (pPath, "screenshots/", 12) != 0;
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

// ----------------------------------------------------------------- inside disk images

namespace {

// A file on the card, read through FatFs, as the HFS code's Storage.
class CFatStorage : public macemu::Storage
{
public:
	boolean Open (const char *pPath)
	{
		m_bOpen = f_open (&m_File, pPath, FA_READ) == FR_OK;
		return m_bOpen;
	}
	~CFatStorage (void)		{ if (m_bOpen) f_close (&m_File); }
	macemu::u32 size (void) override	{ return (macemu::u32) f_size (&m_File); }
	bool read (macemu::u32 nOffset, void *pBuffer, macemu::u32 nLength) override
	{
		UINT nRead = 0;
		return f_lseek (&m_File, nOffset) == FR_OK && f_read (&m_File, pBuffer, nLength, &nRead) == FR_OK
		       && nRead == nLength;
	}
	bool write (macemu::u32, const void *, macemu::u32) override	{ return false; }
	bool readOnly (void) override		{ return true; }

private:
	FIL m_File;
	boolean m_bOpen = FALSE;
};

void TypeString (macemu::u32 nType, char *pOut)
{
	for (int i = 0; i < 4; i++)
	{
		char c = (char) (nType >> (24 - 8 * i));
		pOut[i] = c >= ' ' && c <= '~' ? c : '?';
	}
	pOut[4] = '\0';
}

}

boolean CWebServer::QueryValue (const char *pQuery, const char *pKey, char *pOut, unsigned nMax)
{
	unsigned nKey = strLen (pKey);
	for (const char *p = pQuery; *p; )
	{
		if (strncmp (p, pKey, nKey) == 0 && p[nKey] == '=')
		{
			p += nKey + 1;
			unsigned n = 0;
			while (*p && *p != '&' && n + 1 < nMax)
			{
				char c = *p++;
				if (c == '+') c = ' ';
				else if (c == '%' && HexDigit (p[0]) >= 0 && HexDigit (p[1]) >= 0)
				{
					c = (char) (HexDigit (p[0]) * 16 + HexDigit (p[1]));
					p += 2;
				}
				pOut[n++] = c;
			}
			pOut[n] = '\0';
			return TRUE;
		}
		while (*p && *p != '&') p++;
		if (*p) p++;
	}
	return FALSE;
}

boolean CWebServer::LooksLikeDisk (const char *pName, u64 nSize)
{
	if (nSize < 400 * 1024)
	{
		return FALSE;
	}
	static const char *Ext[] = {".dsk", ".img", ".hda", ".hfv", ".hfs", ".iso", ".toast", ".cdr", ".image", ".dc42", ".vhd", 0};
	for (unsigned i = 0; Ext[i]; i++)
	{
		if (endsWithNoCase (pName, Ext[i]))
		{
			return TRUE;
		}
	}
	return FALSE;
}

static const u16 MacRoman[128] = {
	0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
	0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
	0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
	0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
	0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
	0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
	0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
	0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

// A Mac name (Mac Roman) as HTML (UTF-8).
void CWebServer::OutMacName (const char *pText)
{
	for (; *pText; pText++)
	{
		unsigned c = (unsigned char) *pText;
		if (c < 0x80)
		{
			char Ch[2] = {(char) c, '\0'};
			OutHtml (Ch);
			continue;
		}
		unsigned u = MacRoman[c - 0x80];
		char Utf[4] = {0};
		if (u < 0x800) { Utf[0] = (char) (0xC0 | (u >> 6)); Utf[1] = (char) (0x80 | (u & 0x3F)); }
		else { Utf[0] = (char) (0xE0 | (u >> 12)); Utf[1] = (char) (0x80 | ((u >> 6) & 0x3F)); Utf[2] = (char) (0x80 | (u & 0x3F)); }
		Out (Utf);
	}
}

void CWebServer::SendHfsFolder (CSocket *pSocket, const char *pImage, const char *pQuery)
{
	if (m_pHost->FileInUse (pImage))
	{
		FlushEmulator ();
	}

	char Path[192];
	FullPath (Path, sizeof Path, pImage);
	CFatStorage Disk;
	macemu::HfsVolume Volume;
	if (!Disk.Open (Path) || !Volume.open (&Disk))
	{
		SendStatus (pSocket, 415, "That is not an HFS (Mac OS Standard) disk image. "
					  "HFS+ (Mac OS Extended) is not supported.");
		return;
	}

	char Value[16];
	u32 nFolder = QueryValue (pQuery, "dir", Value, sizeof Value) ? (u32) atoi (Value) : 2;
	static macemu::HfsEntry Entries[400];
	int n = Volume.list (nFolder, Entries, 400);

	m_nPageLength = 0;
	Out (PageHead);
	Out ("<section><h2>");
	OutHtml (pImage);
	Out (": ");
	Out ("<a href='/hfs/");
	OutUrl (pImage);
	Out ("'>");
	OutMacName (Volume.name ());
	Out ("</a>");
	if (nFolder != 2)
	{
		macemu::HfsEntry Folder;
		if (Volume.find (nFolder, Folder))
		{
			Out (" / ");
			OutMacName (Folder.name);
			Out (" &nbsp;<a class=n href='/hfs/");
			OutUrl (pImage);
			Out ("?dir=");
			OutNumber (Folder.parent);
			Out ("'>[up]</a>");
		}
	}
	Out ("</h2><table>");
	for (int i = 0; i < n; i++)
	{
		const macemu::HfsEntry &E = Entries[i];
		Out ("<tr><td>");
		if (E.folder)
		{
			Out ("&#128193; <a href='/hfs/");
			OutUrl (pImage);
			Out ("?dir=");
			OutNumber (E.id);
			Out ("'>");
			OutMacName (E.name);
			Out ("</a></td><td class=s>");
			OutNumber (E.items);
			Out (" items</td><td></td></tr>");
			continue;
		}
		char Type[5], Creator[5];
		TypeString (E.type, Type);
		TypeString (E.creator, Creator);
		OutMacName (E.name);
		Out (" <span class=n>");
		OutHtml (Type);
		Out ("/");
		OutHtml (Creator);
		Out ("</span></td><td class=s>");
		OutSize ((u64) E.dataLen + E.rsrcLen);
		Out ("</td><td class=a><a href='/hfsget/");
		OutUrl (pImage);
		Out ("?id=");
		OutNumber (E.id);
		Out ("&f=bin'>MacBinary</a>");
		if (E.dataLen)
		{
			Out (" &middot; <a href='/hfsget/");
			OutUrl (pImage);
			Out ("?id=");
			OutNumber (E.id);
			Out ("&f=data'>data fork</a>");
		}
		Out ("</td></tr>");
	}
	if (n == 0)
	{
		Out ("<tr><td class=n>Empty.</td></tr>");
	}
	Out ("</table><p class=n>Free: ");
	OutSize (Volume.freeBytes ());
	Out (". MacBinary (.bin) keeps the resource fork, type and creator; StuffIt Expander and most "
	     "emulators read it.</p></section><p><a href='/'>&larr; Back</a></p></main></body></html>");

	char Head[160] = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ";
	appendUint (Head, m_nPageLength, sizeof Head);
	strAppend (Head, "\r\n\r\n", sizeof Head);
	if (SendText (pSocket, Head))
	{
		SendAll (pSocket, m_pPage, m_nPageLength);
	}
}

void CWebServer::SendHfsFile (CSocket *pSocket, const char *pImage, const char *pQuery)
{
	char Path[192];
	FullPath (Path, sizeof Path, pImage);
	CFatStorage Disk;
	macemu::HfsVolume Volume;
	char Value[16], Format[8] = "bin";
	macemu::HfsEntry E;
	if (   !Disk.Open (Path) || !Volume.open (&Disk)
	    || !QueryValue (pQuery, "id", Value, sizeof Value) || !Volume.find ((u32) atoi (Value), E) || E.folder)
	{
		SendStatus (pSocket, 404, "No such file in that disk.");
		return;
	}
	QueryValue (pQuery, "f", Format, sizeof Format);
	boolean bBin = !strEq (Format, "data");

	u64 nTotal = bBin ? 128 + macemu::macBinaryPad (E.dataLen) + macemu::macBinaryPad (E.rsrcLen) : E.dataLen;
	char Name[48];
	strCopy (Name, E.name, sizeof Name);
	for (char *p = Name; *p; p++)
	{
		if (*p == '"' || *p == '/' || *p == '\\' || (unsigned char) *p < ' ' || (unsigned char) *p > '~') *p = '_';
	}
	if (bBin) strAppend (Name, ".bin", sizeof Name);

	char Head[384] = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
			 "Connection: close\r\nContent-Disposition: attachment; filename=\"";
	strAppend (Head, Name, sizeof Head);
	strAppend (Head, "\"\r\nContent-Length: ", sizeof Head);
	appendUint (Head, (u32) nTotal, sizeof Head);
	strAppend (Head, "\r\n\r\n", sizeof Head);
	if (!SendText (pSocket, Head))
	{
		return;
	}

	if (bBin)
	{
		u8 Header[128];
		macemu::macBinaryHeader (E, Header);
		if (!SendAll (pSocket, Header, 128)) return;
	}
	for (int nFork = 0; nFork < (bBin ? 2 : 1); nFork++)
	{
		u32 nLen = nFork ? E.rsrcLen : E.dataLen;
		u32 nPadded = bBin ? macemu::macBinaryPad (nLen) : nLen;
		for (u32 nPos = 0; nPos < nPadded; )
		{
			u32 nChunk = nPadded - nPos < BufferSize ? nPadded - nPos : BufferSize;
			u32 nReal = nPos < nLen ? (nLen - nPos < nChunk ? nLen - nPos : nChunk) : 0;
			memset (m_pBuffer, 0, nChunk);
			if (nReal && !Volume.readFork (E, nFork != 0, nPos, m_pBuffer, nReal)) return;
			if (!SendAll (pSocket, m_pBuffer, nChunk)) return;
			nPos += nChunk;
		}
	}
}

// The files uploaded to SD:/transfer/ become an 800K floppy in the disk
// folder ("clear=1" empties the folder first; "insert=2" puts the floppy in
// floppy drive 2).
void CWebServer::MakeFloppy (CSocket *pSocket, const char *pQuery)
{
	char Value[16];
	char Folder[160];
	FullPath (Folder, sizeof Folder, "transfer");
	DIR Directory;
	FILINFO Info;

	if (QueryValue (pQuery, "clear", Value, sizeof Value))
	{
		ClearTransfer ();
		SendStatus (pSocket, 200, "Ready.");
		return;
	}

	// Too much for a floppy (or too many files): a CD of any size instead.
	{
		u64 nTotal = 0;
		unsigned nFiles = 0;
		if (f_findfirst (&Directory, &Info, Folder, "*") == FR_OK)
		{
			while (Info.fname[0] != '\0')
			{
				if (!(Info.fattrib & AM_DIR))
				{
					nTotal += Info.fsize;
					nFiles++;
				}
				if (f_findnext (&Directory, &Info) != FR_OK) break;
			}
			f_closedir (&Directory);
		}
		if (nFiles > macemu::kHfsFloppyMaxFiles)
		{
			SendStatus (pSocket, 413, "Send at most 64 files at a time.");
			return;
		}
		if (nTotal > m_pHost->MaxFloppyBytes () - 40 * 1024)
		{
			MakeCd (pSocket, pQuery, Folder);
			return;
		}
	}

	// Read the files (they must fit on 800K together).
	enum { MaxBytes = 1600 * 1024 };
	static u8 *pFiles = 0, *pImage = 0;
	if (pFiles == 0) pFiles = new u8[MaxBytes];
	if (pImage == 0) pImage = new u8[1474560];
	static macemu::HfsNewFile List[macemu::kHfsFloppyMaxFiles];
	int nCount = 0;
	u32 nUsed = 0;
	if (f_findfirst (&Directory, &Info, Folder, "*") == FR_OK)
	{
		while (Info.fname[0] != '\0' && nCount < macemu::kHfsFloppyMaxFiles)
		{
			if (!(Info.fattrib & AM_DIR) && Info.fsize + nUsed <= MaxBytes)
			{
				char Path[200];
				strCopy (Path, Folder, sizeof Path);
				strAppend (Path, "/", sizeof Path);
				strAppend (Path, Info.fname, sizeof Path);
				FIL File;
				UINT nRead = 0;
				if (f_open (&File, Path, FA_READ) == FR_OK)
				{
					f_read (&File, pFiles + nUsed, (UINT) Info.fsize, &nRead);
					f_close (&File);
				}
				u8 *p = pFiles + nUsed;
				nUsed += nRead;
				macemu::HfsNewFile &F = List[nCount];
				if (!macemu::macBinaryParse (p, nRead, F))
				{
					memset (&F, 0, sizeof F);
					strCopy (F.name, Info.fname, sizeof F.name);
					F.data = p;
					F.dataLen = nRead;
					F.type = 0x3F3F3F3F;	// ????
					F.creator = 0x3F3F3F3F;
					static const struct { const char *pExt; u32 nType, nCreator; } Types[] = {
						{".txt", 0x54455854, 0x74747874}, {".text", 0x54455854, 0x74747874},	// TEXT ttxt
						{".hqx", 0x54455854, 0x53495478}, {".sit", 0x53495444, 0x53495421},	// SITD SIT!
						{".pict", 0x50494354, 0x74747874}, {".gif", 0x47494666, 0x3F3F3F3F},
						{".jpg", 0x4A504547, 0x3F3F3F3F}, {".jpeg", 0x4A504547, 0x3F3F3F3F},
					};
					for (unsigned t = 0; t < sizeof Types / sizeof Types[0]; t++)
					{
						if (endsWithNoCase (Info.fname, Types[t].pExt))
						{
							F.type = Types[t].nType;
							F.creator = Types[t].nCreator;
						}
					}
					if (F.type == 0x54455854 && !endsWithNoCase (Info.fname, ".hqx"))
					{
						for (u32 i = 0; i < nRead; i++) if (p[i] == '\n') p[i] = '\r';   // Mac line ends
					}
				}
				nCount++;
			}
			if (f_findnext (&Directory, &Info) != FR_OK) break;
		}
		f_closedir (&Directory);
	}
	if (nCount == 0)
	{
		SendStatus (pSocket, 400, "No files were sent.");
		return;
	}

	// A new name in the disk folder: Transfer 1.dsk, Transfer 2.dsk, ...
	char Name[64], Path[200];
	for (unsigned i = 1; ; i++)
	{
		strCopy (Name, "Transfer ", sizeof Name);
		appendUint (Name, i, sizeof Name);
		strAppend (Name, ".dsk", sizeof Name);
		strCopy (Path, m_pHost->DiskDir (), sizeof Path);
		strAppend (Path, "/", sizeof Path);
		strAppend (Path, Name, sizeof Path);
		char Full[220];
		FullPath (Full, sizeof Full, Path);
		if (f_stat (Full, &Info) != FR_OK)
		{
			strCopy (Path, Full, sizeof Path);
			break;
		}
	}
	char Volume[28];
	strCopy (Volume, Name, sizeof Volume);
	Volume[strLen (Volume) - 4] = '\0';			// "Transfer 1", without ".dsk"

	// 800K if the files fit, else 1.44 MB where the Mac has a SuperDrive.
	u32 nBytes = macemu::kHfsFloppyBytes;
	if (!macemu::hfsMakeVolume (pImage, nBytes, Volume, List, nCount, m_pHost->MacTime ()))
	{
		nBytes = 1474560;
		if (   m_pHost->MaxFloppyBytes () < nBytes
		    || !macemu::hfsMakeVolume (pImage, nBytes, Volume, List, nCount, m_pHost->MacTime ()))
		{
			MakeCd (pSocket, pQuery, Folder);
			return;
		}
	}
	FIL File;
	UINT nWritten = 0;
	char Dir[160];
	FullPath (Dir, sizeof Dir, m_pHost->DiskDir ());
	f_mkdir (Dir);
	if (   f_open (&File, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK
	    || f_write (&File, pImage, nBytes, &nWritten) != FR_OK
	    || f_close (&File) != FR_OK || nWritten != nBytes)
	{
		SendStatus (pSocket, 507, "Cannot write the floppy (is the card full?).");
		return;
	}
	ClearTransfer ();

	char Reply[160] = "Made ";
	strAppend (Reply, Name, sizeof Reply);
	strAppend (Reply, " in the ", sizeof Reply);
	strAppend (Reply, m_pHost->DiskDir (), sizeof Reply);
	strAppend (Reply, " folder", sizeof Reply);
	if (QueryValue (pQuery, "insert", Value, sizeof Value))
	{
		// The main loop puts it in the drive; wait a moment for it.
		m_bInsertOK = FALSE;
		strCopy (m_InsertName, Name, sizeof m_InsertName);
		unsigned nStart = CTimer::Get ()->GetTicks ();
		while (m_InsertName[0] && CTimer::Get ()->GetTicks () - nStart < 3 * HZ)
		{
			CScheduler::Get ()->MsSleep (20);
		}
		strAppend (Reply, m_InsertName[0] ? " (the Mac is busy: insert it from the F12 menu)."
				  : m_bInsertOK ? " and put it in floppy drive 2." : " (could not insert it: see the F12 menu).",
			   sizeof Reply);
		m_InsertName[0] = '\0';
	}
	else
	{
		strAppend (Reply, ".", sizeof Reply);
	}
	SendStatus (pSocket, 200, Reply);
}

// The files in SD:/transfer/ as a CD-ROM image of any size ("Transfer
// 1.toast", ... in the disk folder), put in the Mac's CD-ROM drive.  The
// files are too big to hold in memory: the volume's metadata is built
// first, then each fork copied from its file on the card.
void CWebServer::MakeCd (CSocket *pSocket, const char *pQuery, const char *pFolder)
{
	struct TSource { char Path[200]; u32 nData, nRsrc; };	// where each fork starts in its file
	static TSource Source[macemu::kHfsFloppyMaxFiles];
	static macemu::HfsNewFile List[macemu::kHfsFloppyMaxFiles];
	static u32 ForkPos[2 * macemu::kHfsFloppyMaxFiles];
	int nCount = 0;

	DIR Directory;
	FILINFO Info;
	if (f_findfirst (&Directory, &Info, pFolder, "*") == FR_OK)
	{
		while (Info.fname[0] != '\0' && nCount < macemu::kHfsFloppyMaxFiles)
		{
			if (!(Info.fattrib & AM_DIR) && !endsWithNoCase (Info.fname, ".part"))
			{
				if (Info.fsize > 0x7FFFF000u)
				{
					f_closedir (&Directory);
					SendStatus (pSocket, 413, "A file for the Mac can be 2 GB at most.");
					return;
				}
				TSource &S = Source[nCount];
				macemu::HfsNewFile &F = List[nCount];
				strCopy (S.Path, pFolder, sizeof S.Path);
				strAppend (S.Path, "/", sizeof S.Path);
				strAppend (S.Path, Info.fname, sizeof S.Path);
				u8 Header[128];
				UINT nRead = 0;
				FIL File;
				if (f_open (&File, S.Path, FA_READ) == FR_OK)
				{
					f_read (&File, Header, sizeof Header, &nRead);
					f_close (&File);
				}
				if (nRead == sizeof Header && macemu::macBinaryHeaderParse (Header, (u32) Info.fsize, F))
				{
					S.nData = 128;
					S.nRsrc = 128 + macemu::macBinaryPad (F.dataLen);
				}
				else
				{
					memset (&F, 0, sizeof F);
					strCopy (F.name, Info.fname, sizeof F.name);
					F.dataLen = (u32) Info.fsize;
					F.type = 0x3F3F3F3F;		// ????
					F.creator = 0x3F3F3F3F;
					static const struct { const char *pExt; u32 nType, nCreator; } Types[] = {
						{".txt", 0x54455854, 0x74747874}, {".text", 0x54455854, 0x74747874},	// TEXT ttxt
						{".hqx", 0x54455854, 0x53495478}, {".sit", 0x53495444, 0x53495421},	// SITD SIT!
						{".pict", 0x50494354, 0x74747874}, {".gif", 0x47494666, 0x3F3F3F3F},
						{".jpg", 0x4A504547, 0x3F3F3F3F}, {".jpeg", 0x4A504547, 0x3F3F3F3F},
					};
					for (unsigned t = 0; t < sizeof Types / sizeof Types[0]; t++)
					{
						if (endsWithNoCase (Info.fname, Types[t].pExt))
						{
							F.type = Types[t].nType;
							F.creator = Types[t].nCreator;
						}
					}
					S.nData = 0;
					S.nRsrc = 0;
				}
				nCount++;
			}
			if (f_findnext (&Directory, &Info) != FR_OK) break;
		}
		f_closedir (&Directory);
	}
	if (nCount == 0)
	{
		SendStatus (pSocket, 400, "No files were sent.");
		return;
	}

	// A new name in the disk folder: Transfer 1.toast, ...
	char Name[64], Path[200];
	for (unsigned i = 1; ; i++)
	{
		strCopy (Name, "Transfer ", sizeof Name);
		appendUint (Name, i, sizeof Name);
		strCopy (Path, m_pHost->DiskDir (), sizeof Path);
		strAppend (Path, "/", sizeof Path);
		strAppend (Path, Name, sizeof Path);
		char Full[220];
		FullPath (Full, sizeof Full, Path);
		strAppend (Full, ".dsk", sizeof Full);
		boolean bTaken = f_stat (Full, &Info) == FR_OK;
		Full[strLen (Full) - 4] = '\0';
		strAppend (Full, ".toast", sizeof Full);
		if (!bTaken && f_stat (Full, &Info) != FR_OK)
		{
			strCopy (Path, Full, sizeof Path);
			break;
		}
	}
	char Volume[28];
	strCopy (Volume, Name, sizeof Volume);
	strAppend (Name, ".toast", sizeof Name);

	// The metadata: the first part of the volume.
	enum { MetaMax = 256 * 1024 };
	static u8 *pMeta = 0;
	if (pMeta == 0) pMeta = new u8[MetaMax];
	u32 nMetaBytes = 0, nVolumeBytes = 0;
	if (!macemu::hfsPlanVolume (pMeta, MetaMax, Volume, List, nCount, m_pHost->MacTime (),
				    ForkPos, nMetaBytes, nVolumeBytes))
	{
		SendStatus (pSocket, 413, "Those files are too big for one disk (2 GB at most). Send fewer at a time.");
		return;
	}

	char Dir[160];
	FullPath (Dir, sizeof Dir, m_pHost->DiskDir ());
	f_mkdir (Dir);
	FIL Out;
	if (f_open (&Out, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		SendStatus (pSocket, 507, "Cannot make the disk (is the card full?).");
		return;
	}
	CLogger::Get ()->Write (FromWeb, LogNotice, "Making %s (%u files, %u KB)", Name, nCount, nVolumeBytes / 1024);

	// Write the volume front to back: metadata, the forks in order of
	// their place (zeros between), zeros, the spare copy of the volume
	// header, a last empty sector.
	boolean bOK = TRUE;
	u32 nAt = 0;
	UINT nWritten;
	bOK = f_write (&Out, pMeta, nMetaBytes, &nWritten) == FR_OK && nWritten == nMetaBytes;
	nAt = nMetaBytes;
	u8 *pBuf = m_pBuffer;
	for (int nDone = 0; bOK && nDone < 2 * nCount; nDone++)
	{
		int nNext = -1;					// the lowest fork still to write
		for (int i = 0; i < 2 * nCount; i++)
		{
			u32 nLen = i & 1 ? List[i / 2].rsrcLen : List[i / 2].dataLen;
			if (nLen != 0 && ForkPos[i] >= nAt && (nNext < 0 || ForkPos[i] < ForkPos[nNext]))
			{
				nNext = i;
			}
		}
		if (nNext < 0)
		{
			break;
		}
		memset (pBuf, 0, BufferSize);
		while (bOK && nAt < ForkPos[nNext])
		{
			u32 n = ForkPos[nNext] - nAt > BufferSize ? BufferSize : ForkPos[nNext] - nAt;
			bOK = f_write (&Out, pBuf, n, &nWritten) == FR_OK && nWritten == n;
			nAt += n;
		}
		const TSource &S = Source[nNext / 2];
		u32 nLen = nNext & 1 ? List[nNext / 2].rsrcLen : List[nNext / 2].dataLen;
		FIL In;
		if (!bOK || f_open (&In, S.Path, FA_READ) != FR_OK)
		{
			bOK = FALSE;
			break;
		}
		bOK = f_lseek (&In, nNext & 1 ? S.nRsrc : S.nData) == FR_OK;
		for (u32 nLeft = nLen; bOK && nLeft > 0; )
		{
			UINT nRead = 0;
			u32 n = nLeft > BufferSize ? BufferSize : nLeft;
			bOK = f_read (&In, pBuf, n, &nRead) == FR_OK && nRead == n
			      && f_write (&Out, pBuf, n, &nWritten) == FR_OK && nWritten == n;
			nLeft -= n;
			nAt += n;
			CScheduler::Get ()->Yield ();		// the screen, keyboard and network carry on
			strCopy (m_Activity, "Making ", sizeof m_Activity);
			strAppend (m_Activity, Name, sizeof m_Activity);
			strAppend (m_Activity, " ", sizeof m_Activity);
			appendUint (m_Activity, (unsigned) ((u64) nAt * 100 / nVolumeBytes), sizeof m_Activity);
			strAppend (m_Activity, "%", sizeof m_Activity);
		}
		f_close (&In);
	}
	memset (pBuf, 0, BufferSize);
	while (bOK && nAt < nVolumeBytes - 1024)
	{
		u32 n = nVolumeBytes - 1024 - nAt > BufferSize ? BufferSize : nVolumeBytes - 1024 - nAt;
		bOK = f_write (&Out, pBuf, n, &nWritten) == FR_OK && nWritten == n;
		nAt += n;
	}
	if (bOK)
	{
		memcpy (pBuf, pMeta + 1024, 512);
		memset (pBuf + 512, 0, 512);
		bOK = f_write (&Out, pBuf, 1024, &nWritten) == FR_OK && nWritten == 1024;
	}
	bOK = f_close (&Out) == FR_OK && bOK;
	m_Activity[0] = '\0';
	if (!bOK)
	{
		f_unlink (Path);
		SendStatus (pSocket, 507, "Cannot write the disk (is the card full?).");
		return;
	}
	ClearTransfer ();

	char Reply[200] = "Made ";
	strAppend (Reply, Name, sizeof Reply);
	strAppend (Reply, " (", sizeof Reply);
	appendUint (Reply, (nVolumeBytes + 1023) / 1024 / 1024, sizeof Reply);
	strAppend (Reply, " MB) in the ", sizeof Reply);
	strAppend (Reply, m_pHost->DiskDir (), sizeof Reply);
	strAppend (Reply, " folder", sizeof Reply);
	char Value[16];
	if (QueryValue (pQuery, "insert", Value, sizeof Value))
	{
		m_bInsertOK = FALSE;
		strCopy (m_InsertName, Name, sizeof m_InsertName);
		unsigned nStart = CTimer::Get ()->GetTicks ();
		while (m_InsertName[0] && CTimer::Get ()->GetTicks () - nStart < 3 * HZ)
		{
			CScheduler::Get ()->MsSleep (20);
		}
		strAppend (Reply, m_InsertName[0] ? " (the Mac is busy: insert it from the F12 menu, Hard disks page)."
				  : m_bInsertOK ? " and put it in the CD-ROM drive: it appears on the Mac's desktop."
				  : " but could not put it in the CD-ROM drive: eject the CD on the Mac (drag it to the Trash), "
				    "then pick it in the F12 menu.",
			   sizeof Reply);
		m_InsertName[0] = '\0';
	}
	else
	{
		strAppend (Reply, ".", sizeof Reply);
	}
	SendStatus (pSocket, 200, Reply);
}

void CWebServer::ClearTransfer (void)
{
	char Folder[160];
	FullPath (Folder, sizeof Folder, "transfer");
	f_mkdir (Folder);
	DIR Directory;
	FILINFO Info;
	if (f_findfirst (&Directory, &Info, Folder, "*") == FR_OK)
	{
		while (Info.fname[0] != '\0')
		{
			char Path[200];
			strCopy (Path, Folder, sizeof Path);
			strAppend (Path, "/", sizeof Path);
			strAppend (Path, Info.fname, sizeof Path);
			f_unlink (Path);
			if (f_findnext (&Directory, &Info) != FR_OK) break;
		}
		f_closedir (&Directory);
	}
}

// ----------------------------------------------------------------- helpers

void CWebServer::SendStatus (CSocket *pSocket, unsigned nCode, const char *pMessage)
{
	// A browser that is still sending the body when the connection closes
	// sees only "connection lost", never the reason: read the rest of the
	// body first (up to 64 MB; more than that is not worth the wait).
	if (m_nBodyUnread > 0 && m_nBodyUnread <= 64 * 1024 * 1024)
	{
		while (m_nBodyUnread > 0)
		{
			unsigned nWant = m_nBodyUnread > FRAME_BUFFER_SIZE ? FRAME_BUFFER_SIZE : (unsigned) m_nBodyUnread;
			int nResult = pSocket->Receive (m_pBuffer, nWant, 0);
			if (nResult <= 0)
			{
				break;
			}
			m_nBodyUnread -= (unsigned) nResult;
		}
	}
	m_nBodyUnread = 0;
	if (nCode >= 400)
	{
		CLogger::Get ()->Write (FromWeb, LogWarning, "%u %s", nCode, pMessage);
	}

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
