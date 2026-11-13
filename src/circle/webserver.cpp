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
#include "../core/macnames.h"

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
":root{--bg:#eceef1;--card:#fff;--line:#d9dde3;--text:#1d2229;--dim:#69717c;--accent:#2a5db0;--drop:#eef4ff}"
"@media(prefers-color-scheme:dark){:root{--bg:#16181c;--card:#22252b;--line:#363b44;--text:#e4e7eb;--dim:#9aa3ad;--accent:#7aa7f0;--drop:#1d2a3d}}"
"body{font:15px -apple-system,Helvetica,Arial,sans-serif;margin:0;background:var(--bg);color:var(--text)}"
"header{background:#2b2f36;color:#fff;padding:12px 18px;display:flex;flex-wrap:wrap;align-items:baseline;gap:4px 16px}"
"header b{font-size:20px;font-weight:600}header span{color:#b8c0ca;font-size:13px}"
"main{max-width:900px;margin:0 auto;padding:8px 12px}"
"section{background:var(--card);border:1px solid var(--line);border-radius:8px;margin:12px 0;padding:10px 14px}"
"h2{font-size:16px;margin:4px 0 8px}table{border-collapse:collapse;width:100%}"
"td{padding:5px 6px;border-top:1px solid var(--line)}td.s{text-align:right;white-space:nowrap;color:var(--dim)}"
"td.a{text-align:right;white-space:nowrap}td.k{color:var(--dim);white-space:nowrap;width:1%;padding-right:14px}"
"#stat td{border:0;padding:3px 6px}a{color:var(--accent);text-decoration:none}"
"button{font-size:14px;padding:4px 10px;margin:2px}.u{color:#c27a00;font-size:13px}"
".drop{border:2px dashed var(--line);border-radius:8px;padding:18px;text-align:center;color:var(--dim)}"
".drop.on{border-color:var(--accent);background:var(--drop)}progress{width:100%}.n{color:var(--dim);font-size:13px}"
"label{font-size:14px}.ok{color:#2a8a3e}"
"</style></head><body>";

static const char PageUpload[] =
"<section><h2>Disk images and ROMs</h2>"
"<div id=drop class=drop>Drop disk images or ROMs here, or <input type=file id=pick multiple></div>"
"<p>Put them in <select id=dir>"
"<option value='@DISKDIR@/'>the @DISKDIR@ folder (floppies and hard disks)</option>"
"<option value=''>the top of the card (ROMs, hd0.img ...)</option></select></p>"
"<div id=st></div><progress id=pr value=0 max=1 hidden></progress>"
"<p class=n>Then pick the new disk in the Mac's menu (F12: Floppy drive or Hard disks). "
"Copying is fastest while the menu is open, because the Mac is paused.</p></section>";

static const char PageSend[] =
"<section><h2>Send files to the Mac</h2>"
"<div id=tdrop class=drop>Drop files here, or <input type=file id=tpick multiple></div>"
"<div id=tst></div><progress id=tpr value=0 max=1 hidden></progress>"
"@SENDTEXT@</section>";

static const char SendShared[] =
"<p class=n>They go on the Mac's <b>Shared</b> disk, any size, and are there within seconds. "
"MacBinary files (.bin) are unpacked: they keep their icons and resource forks.</p>"
"<p><label><input type=checkbox id=tfl> Put them on a transfer floppy or CD instead (for Mac OS before 7.5)</label></p>";

static const char SendDisk[] =
"<p class=n>Any size. Small files go on a new floppy in floppy drive 2; bigger ones on a new CD-ROM disk "
"(\"Transfer 1\", ...) in the CD-ROM drive. Either appears on the Mac's desktop: drag the files to your hard disk. "
"MacBinary files (.bin) keep their icons and resource forks. To get files out of a disk, click [open] next to it.</p>";

static const char PageTail[] =
"<section><h2>Update the emulator</h2>"
"<div id=upd class=drop>Drop the new <b>MacEmulator-SD.zip</b> here, or <input type=file id=updpick accept='.zip'></div>"
"<div id=ust></div><progress id=upr value=0 max=1 hidden></progress>"
"<p class=n>Installs the new emulator and Pi start-up files, then restarts the Pi. "
"Your settings, ROMs, disks and shared files are kept.</p></section>"
"</main><script>"
"var q=[],busy=0,xfer=0;"
"function $(i){return document.getElementById(i)}"
"function esc(t){return t.replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c]})}"
"function add(fl,d,s,p){for(var i=0;i<fl.length;i++)q.push({f:fl[i],d:d,s:s,p:p});next()}"
"function sleep(ms){return new Promise(function(z){setTimeout(z,ms)})}"
// Files go in pieces of 256 KB, each its own short request with a few tries:
// one long transfer can stall on the Pi's network.
"async function put(u,b,pr){var P=262144,at=0,n=b.size,t='';"
"do{var e=Math.min(at+P,n),k=0;"
"for(;;){var c=new AbortController(),tm=setTimeout(function(){c.abort()},10000);"
"try{var r=await fetch(u+'?at='+at+'&total='+n,{method:'PUT',body:b.slice(at,e),signal:c.signal});"
"t=await r.text();clearTimeout(tm);"
"if(r.status==409&&t.slice(0,5)=='have '){at=+t.slice(5);e=Math.min(at+P,n);continue}"
"if(!r.ok)throw t||('error '+r.status);break}"
"catch(x){clearTimeout(tm);if(typeof x=='string')throw x;"
"if(++k>5)throw 'the connection to the Pi keeps failing. Check the Wi-Fi and try again.';await sleep(1000)}}"
"at=e;if(pr)pr(at,n)}while(at<n);return t}"
"function next(){if(busy)return;var j=q.shift();if(!j)return;"
"busy=1;var f=j.f,p=$(j.p),st=$(j.s);p.hidden=false;p.value=0;p.max=1;"
"st.textContent='Sending '+f.name+' ...';"
"put('/files/'+j.d+encodeURIComponent(f.name),f,function(a,n){p.max=n||1;p.value=a;"
"st.textContent='Sending '+f.name+': '+(n?Math.floor(a*100/n):100)+'%'})"
".then(function(t){st.innerHTML='<span class=ok>&#10003;</span> '+esc(f.name)+': '+esc(t);"
"if(!q.length)setTimeout(function(){location.reload()},1500)})"
".catch(function(x){st.textContent=f.name+': '+x}).finally(function(){busy=0;p.hidden=true;next()})}"
"function send(fl){if(SHARE&&!$('tfl').checked)add(fl,SHARE+'/','tst','tpr');else transfer(fl)}"
"function stat(){if(busy||xfer||q.length)return;fetch('/status').then(function(r){return r.text()}).then(function(t){var h='';"
"t.split('\\n').forEach(function(l){var c=l.split('\\t');if(c.length>1)h+='<tr><td class=k>'+esc(c[0])+'</td><td>'+esc(c[1])"
"+(c[2]?' <button onclick=\\\"drv(\\''+c[2]+'\\',\\'\\')\\\">Eject</button>':'')+'</td></tr>'});"
"$('stat').innerHTML=h}).catch(function(){})}setInterval(stat,5000);"
"function del(n){if(!confirm('Delete '+n+'?'))return;"
"fetch('/files/'+n,{method:'DELETE'}).then(function(r){return r.text().then(function(t){"
"if(r.ok)location.reload();else alert(t)})})}"
"function drv(w,n){$('dst').textContent='...';fetch('/drive?d='+w+'&name='+n,{method:'POST'})"
".then(function(r){return r.text()}).then(function(t){$('dst').textContent=t;stat()})}"
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
"if(typeof DecompressionStream=='undefined'){alert('Please use a newer web browser for updates.');return}xfer++;"
"try{st.textContent='Reading '+file.name+' ...';var fl=await unzip(await file.arrayBuffer());"
"fl=fl.filter(function(f){return !keep.test(f.name)});"
"if(!fl.some(function(f){return /^kernel.*\\.img$/.test(f.name)}))throw 'There is no emulator in this zip file.';"
"p.hidden=false;p.max=fl.length;"
"for(var i=0;i<fl.length;i++){var f=fl[i];p.value=i;st.textContent='Installing '+f.name+' ('+(i+1)+' of '+fl.length+')';"
"try{await put('/update/'+f.name.split('/').map(encodeURIComponent).join('/'),new Blob([await inflate(f)]))}"
"catch(x){throw f.name+': '+x}}"
"p.value=fl.length;st.textContent='Installed. The Pi is restarting; this page will come back in a minute.';"
"await fetch('/reboot',{method:'POST'});setTimeout(function(){location.reload()},30000)}"
"catch(x){st.textContent='Update stopped: '+x}xfer--}"
"async function transfer(fl){var st=$('tst');xfer++;try{"
"if(fl.length>64)throw 'Send at most 64 files at a time.';"
"await fetch('/makefloppy?clear=1',{method:'POST'});"
"for(var i=0;i<fl.length;i++){st.textContent='Sending '+fl[i].name+' ...';"
"try{await put('/files/transfer/'+encodeURIComponent(fl[i].name),fl[i])}catch(x){throw fl[i].name+': '+x}}"
"st.textContent='Making the disk for the Mac ...';var r=await fetch('/makefloppy?insert=2',{method:'POST'});"
"st.textContent=await r.text();if(r.ok)setTimeout(function(){location.reload()},2500)}"
"catch(x){st.textContent='Stopped: '+(x instanceof TypeError?'the connection to the Pi was lost. Check the Wi-Fi and try again.':x)}xfer--}"
"var td=$('tdrop');$('tpick').onchange=function(){send(this.files)};"
"td.ondragover=function(e){e.preventDefault();td.className='drop on'};"
"td.ondragleave=function(){td.className='drop'};"
"td.ondrop=function(e){e.preventDefault();td.className='drop';send(e.dataTransfer.files)};"
"var ud=$('upd');$('updpick').onchange=function(){if(this.files[0])update(this.files[0])};"
"ud.ondragover=function(e){e.preventDefault();ud.className='drop on'};"
"ud.ondragleave=function(){ud.className='drop'};"
"ud.ondrop=function(e){e.preventDefault();ud.className='drop';if(e.dataTransfer.files[0])update(e.dataTransfer.files[0])};"
"var d=$('drop');$('pick').onchange=function(){add(this.files,$('dir').value,'st','pr')};"
"d.ondragover=function(e){e.preventDefault();d.className='drop on'};"
"d.ondragleave=function(){d.className='drop'};"
"d.ondrop=function(e){e.preventDefault();d.className='drop';add(e.dataTransfer.files,$('dir').value,'st','pr')};"
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
	m_bDriveRequest = FALSE;
	m_nDriveWhich = 0;
	m_DriveName[0] = m_DriveReply[0] = '\0';
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

		pConnection->SetOptionReceiveTimeout (10 * 1000000);	// (the page tries a piece again after 10 s)
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

	if (bGet && strEq (pPath, "status"))
	{
		static char Text[1024];
		m_pHost->StatusText (Text, sizeof Text);
		char Head[160] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
				 "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ";
		appendUint (Head, strLen (Text), sizeof Head);
		strAppend (Head, "\r\n\r\n", sizeof Head);
		if (SendText (pSocket, Head))
		{
			SendAll (pSocket, Text, strLen (Text));
		}
		return;
	}

	if (strEq (Request.Method, "POST") && strEq (pPath, "drive"))
	{
		DriveRequest (pSocket, Request.Query);
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

	// The header: the version and the card's free space.
	Out ("<header><b>Mac Emulator</b><span>");
	OutHtml (m_pHost->Version ());
	char Root[8];
	strCopy (Root, m_pDrive, sizeof Root);
	strAppend (Root, "/", sizeof Root);
	DWORD nFreeClusters;
	FATFS *pFS;
	if (f_getfree (Root, &nFreeClusters, &pFS) == FR_OK)
	{
		Out (" &middot; ");
		OutSize ((u64) nFreeClusters * pFS->csize * 512);
		Out (" free on the card");
	}
	Out ("</span></header><main>");

	// What the Mac is doing (refreshed by the page every few seconds).
	Out ("<section><h2>This Mac</h2><table id=stat>");
	OutStatusRows ();
	Out ("</table><p id=dst class=n></p><p><button onclick=restart()>Restart the Mac</button> "
	     "<span class=n>Saves the disks and starts the Mac again with the disks chosen in the menu.</span></p></section>");

	// Sending files: to the shared folder when there is one.
	const char *pShared = m_pHost->SharedDir ();
	for (const char *p = PageSend; *p; )
	{
		if (strncmp (p, "@SENDTEXT@", 10) == 0)
		{
			Out (pShared[0] != '\0' ? SendShared : SendDisk);
			p += 10;
		}
		else
		{
			char Ch[2] = {*p++, '\0'};
			Out (Ch);
		}
	}
	if (pShared[0] != '\0')
	{
		ListFolder (pShared, TRUE, "Shared folder (the Mac's Shared disk)");
	}

	// Disk images and ROMs.
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
	if (pDiskDir[0] != '\0')
	{
		ListFolder (pDiskDir, TRUE, 0);
	}
	ListFolder ("", FALSE, "Top of the card");

	Out ("<script>var SHARE='");
	OutHtml (pShared);
	Out ("';</script>");
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

void CWebServer::ListFolder (const char *pFolder, boolean bDiskDir, const char *pTitle)
{
	Out ("<section><h2>");
	if (pTitle != 0)
	{
		Out (pTitle);
	}
	else
	{
		Out ("The ");
		OutHtml (pFolder);
		Out (" folder (floppies, hard disks, CDs)");
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

		boolean bFinderFile = strEq (pFolder, m_pHost->SharedDir ()) && strEq (Info.fname, "Desktop");
		if (!(Info.fattrib & (AM_DIR | AM_HID | AM_SYS)) && Info.fname[0] != '.' && !bFinderFile
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
			Out ("</td><td class=a>");
			if (bDiskDir && strEq (pFolder, m_pHost->DiskDir ()) && !m_pHost->FileInUse (Name))
			{
				// Floppies (up to 1.44 MB, DiskCopy header included) and CDs go in a drive from here.
				boolean bCd = endsWithNoCase (Info.fname, ".iso") || endsWithNoCase (Info.fname, ".toast")
					      || endsWithNoCase (Info.fname, ".cdr");
				boolean bFloppy = !bCd && Info.fsize >= 400 * 1024 && Info.fsize <= m_pHost->MaxFloppyBytes () + 84
						  && LooksLikeDisk (Info.fname, Info.fsize);
				if (bCd || bFloppy)
				{
					Out ("<button onclick=\"drv('");
					Out (bCd ? "cd" : "any");
					Out ("','");
					OutUrl (Info.fname);
					Out ("')\">Insert</button> ");
				}
			}
			Out ("<button onclick=\"del('");
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

// ----------------------------------------------------------------- drives

// POST /drive?d=any|0|1|cd&name=NAME (in the disk folder; empty: eject).
void CWebServer::DriveRequest (CSocket *pSocket, const char *pQuery)
{
	char Which[8], Name[128];
	if (!QueryValue (pQuery, "d", Which, sizeof Which) || !QueryValue (pQuery, "name", Name, sizeof Name)
	    || strchr (Name, '/') != 0 || strchr (Name, '\\') != 0 || (Name[0] == '.' && Name[1] == '.'))
	{
		SendStatus (pSocket, 400, "Bad request");
		return;
	}
	int nWhich = strEq (Which, "cd") ? 2 : strEq (Which, "0") ? 0 : strEq (Which, "1") ? 1 : -1;

	// The main loop does it; wait a moment for it.
	strCopy (m_DriveName, Name, sizeof m_DriveName);
	m_nDriveWhich = nWhich;
	m_DriveReply[0] = '\0';
	m_bDriveRequest = TRUE;
	unsigned nStart = CTimer::Get ()->GetTicks ();
	while (m_bDriveRequest && CTimer::Get ()->GetTicks () - nStart < 3 * HZ)
	{
		CScheduler::Get ()->MsSleep (20);
	}
	if (m_bDriveRequest)
	{
		m_bDriveRequest = FALSE;
		SendStatus (pSocket, 503, "The Mac is busy: try again, or use the F12 menu.");
		return;
	}
	SendStatus (pSocket, 200, m_DriveReply);
}

void CWebServer::DriveDone (const char *pReply)
{
	strCopy (m_DriveReply, pReply, sizeof m_DriveReply);
	m_bDriveRequest = FALSE;
}

// ----------------------------------------------------------------- transfers

// A MacBinary file (pPath, on the card) as the Mac file it holds, beside
// it: the data fork, and an AppleDouble "._name" with the Finder info and
// the resource fork (as the shared folder keeps them).  The .bin goes.
boolean CWebServer::UnpackMacBinary (const char *pPath, char *pReply, unsigned nMax)
{
	FIL In;
	if (f_open (&In, pPath, FA_READ) != FR_OK)
	{
		return FALSE;
	}
	u8 Header[128];
	UINT nRead = 0;
	macemu::HfsNewFile F;
	if (   f_read (&In, Header, sizeof Header, &nRead) != FR_OK || nRead != sizeof Header
	    || !macemu::macBinaryHeaderParse (Header, (u32) f_size (&In), F))
	{
		f_close (&In);
		return FALSE;
	}

	// The Mac name, as the shared folder names files on the card.
	char Name[256], Dir[200], Out[460], Side[470];
	macemu::macToHostName ((const u8 *) F.name, (int) strLen (F.name), Name, sizeof Name, false);
	strCopy (Dir, pPath, sizeof Dir);
	for (int i = (int) strLen (Dir) - 1; i >= 0 && Dir[i] != '/'; i--) Dir[i] = '\0';
	strCopy (Out, Dir, sizeof Out);
	strAppend (Out, Name, sizeof Out);
	strCopy (Side, Dir, sizeof Side);
	strAppend (Side, "._", sizeof Side);
	strAppend (Side, Name, sizeof Side);
	FILINFO Info;
	if (f_stat (Out, &Info) == FR_OK)			// already there: keep the .bin as it is
	{
		f_close (&In);
		return FALSE;
	}

	boolean bOK = TRUE;
	FIL Data, Double;
	bOK = f_open (&Data, Out, FA_WRITE | FA_CREATE_NEW) == FR_OK;
	if (bOK && f_open (&Double, Side, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		f_close (&Data);
		f_unlink (Out);
		bOK = FALSE;
	}
	if (!bOK)
	{
		f_close (&In);
		return FALSE;
	}

	// AppleDouble: header, two entries (Finder info at 50, the resource
	// fork from 82), as src/core/sharedfs.cpp writes it.
	u8 AD[82];
	memset (AD, 0, sizeof AD);
	auto Put32 = [] (u8 *p, u32 v) { p[0] = (u8) (v >> 24); p[1] = (u8) (v >> 16); p[2] = (u8) (v >> 8); p[3] = (u8) v; };
	Put32 (AD, 0x00051607); Put32 (AD + 4, 0x00020000); AD[25] = 2;
	Put32 (AD + 26, 9); Put32 (AD + 30, 50); Put32 (AD + 34, 32);
	Put32 (AD + 38, 2); Put32 (AD + 42, 82); Put32 (AD + 46, F.rsrcLen);
	Put32 (AD + 50, F.type); Put32 (AD + 54, F.creator);
	AD[58] = (u8) (F.flags >> 8); AD[59] = (u8) F.flags;
	UINT nWritten;
	bOK = f_write (&Double, AD, sizeof AD, &nWritten) == FR_OK && nWritten == sizeof AD;

	// The forks.
	struct { FIL *pFile; u32 nFrom, nLength; } Fork[2] =
		{{&Data, 128, F.dataLen}, {&Double, 128 + macemu::macBinaryPad (F.dataLen), F.rsrcLen}};
	for (unsigned k = 0; k < 2 && bOK; k++)
	{
		bOK = f_lseek (&In, Fork[k].nFrom) == FR_OK;
		for (u32 nLeft = Fork[k].nLength; bOK && nLeft > 0; )
		{
			u32 n = nLeft > BufferSize ? BufferSize : nLeft;
			bOK = f_read (&In, m_pBuffer, n, &nRead) == FR_OK && nRead == n
			      && f_write (Fork[k].pFile, m_pBuffer, n, &nWritten) == FR_OK && nWritten == n;
			nLeft -= n;
		}
	}
	f_close (&In);
	bOK = f_close (&Data) == FR_OK && bOK;
	bOK = f_close (&Double) == FR_OK && bOK;
	if (!bOK)
	{
		f_unlink (Out);
		f_unlink (Side);
		return FALSE;
	}
	f_unlink (pPath);

	strCopy (pReply, "unpacked as \"", nMax);
	strAppend (pReply, F.name, nMax);
	strAppend (pReply, "\" on the Mac's Shared disk (with its resource fork).", nMax);
	return TRUE;
}

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

static u64 ParseNumber (const char *p)
{
	u64 n = 0;
	while (*p >= '0' && *p <= '9' && n <= MaxFileSize)
	{
		n = n * 10 + (u64) (*p++ - '0');
	}
	return n;
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

	// In pieces (?at=OFFSET&total=SIZE): the page sends big files as many
	// short requests, retrying any that fail, as one long transfer can stall
	// on the Pi's network.  The first piece starts the temporary file, each
	// goes where it says (one sent twice is harmless), the last finishes it.
	char Value[24];
	boolean bPieces = QueryValue (pRequest->Query, "at", Value, sizeof Value);
	u64 nAt = bPieces ? ParseNumber (Value) : 0;
	u64 nWhole = pRequest->nContentLength;
	if (bPieces)
	{
		nWhole = QueryValue (pRequest->Query, "total", Value, sizeof Value) ? ParseNumber (Value) : 0;
		if (nWhole > MaxFileSize || nAt + pRequest->nContentLength > nWhole)
		{
			SendStatus (pSocket, 400, "Bad request");
			return;
		}
	}

	FIL File;
	if (nAt == 0)
	{
		if (f_open (&File, TempPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		{
			SendStatus (pSocket, 500, "Cannot create the file.");
			return;
		}
	}
	else
	{
		if (f_open (&File, TempPath, FA_WRITE) != FR_OK)
		{
			SendStatus (pSocket, 409, "have 0");		// start again
			return;
		}
		u64 nHave = f_size (&File);
		if (nHave < nAt)
		{
			f_close (&File);
			char Have[32] = "have ";
			appendUint (Have, (u32) nHave, sizeof Have);
			SendStatus (pSocket, 409, Have);		// go on from there
			return;
		}
		if (f_lseek (&File, nAt) != FR_OK || f_truncate (&File) != FR_OK)
		{
			f_close (&File);
			SendStatus (pSocket, 500, "Cannot write the file.");
			return;
		}
	}

	const char *pName = pPath;
	for (const char *p = pPath; *p; p++)
	{
		if (*p == '/') pName = p + 1;
	}
	if (nAt == 0)
	{
		CLogger::Get ()->Write (FromWeb, LogNotice, "Receiving %s (%u bytes)", pPath, (unsigned) nWhole);
	}

	u64 nLeft = pRequest->nContentLength;
	u64 nPiece = nLeft;
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

		u64 nDone = nAt + nPiece - nLeft;
		unsigned nPercent = nWhole ? (unsigned) (nDone * 100 / nWhole) : 100;
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
		if (!bPieces)
		{
			f_unlink (TempPath);
		}
		CLogger::Get ()->Write (FromWeb, LogWarning, "Transfer of %s broken off at %u of %u bytes%s", pPath,
					(unsigned) (nAt + nPiece - nLeft), (unsigned) nWhole, bPieces ? " (the page tries again)" : "");
		return;				// the connection is gone: no answer
	}
	if (bPieces && nAt + nPiece < nWhole)
	{
		SendStatus (pSocket, 200, "piece");
		return;
	}

	f_unlink (Path);
	if (f_rename (TempPath, Path) != FR_OK)
	{
		f_unlink (TempPath);
		SendStatus (pSocket, 500, "Cannot rename the file.");
		return;
	}

	CLogger::Get ()->Write (FromWeb, LogNotice, "Received %s", pPath);

	// MacBinary into the shared folder: the Mac file it holds, with its
	// resource fork and Finder info.
	const char *pShared = m_pHost->SharedDir ();
	u32 nShared = strLen (pShared);
	if (   !bUpdate && nShared && strncmp (pPath, pShared, nShared) == 0 && pPath[nShared] == '/'
	    && endsWithNoCase (pPath, ".bin"))
	{
		char Reply[160];
		if (UnpackMacBinary (Path, Reply, sizeof Reply))
		{
			SendStatus (pSocket, 201, Reply);
			return;
		}
	}
	SendStatus (pSocket, 201, nShared && strncmp (pPath, pShared, nShared) == 0 ? "on the Mac's Shared disk." : "copied.");
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
	static macemu::HfsEntry *Entries = 0;			// (on the heap: the kernel image is limited to 2 MB)
	if (Entries == 0) Entries = new macemu::HfsEntry[400];
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
	static TSource *Source = 0;				// (on the heap: the kernel image is limited)
	if (Source == 0) Source = new TSource[macemu::kHfsFloppyMaxFiles];
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
	// 900 bytes, 4.5 KB, 800 KB, 1.4 MB, 500 MB, 3.2 GB
	static const char *Units[] = {" KB", " MB", " GB"};
	if (nBytes < 1000)
	{
		OutNumber (nBytes);
		Out (" bytes");
		return;
	}
	u64 nScaled = nBytes * 10 / 1024;			// tenths of a KB
	int nUnit = 0;
	while (nScaled >= 10240 && nUnit < 2)
	{
		nScaled /= 1024;
		nUnit++;
	}
	OutNumber (nScaled >= 100 ? (nScaled + 5) / 10 : nScaled / 10);
	if (nScaled < 100)
	{
		char Tenth[3] = {'.', (char) ('0' + nScaled % 10), '\0'};
		Out (Tenth);
	}
	Out (Units[nUnit]);
}

// The status lines ("name<TAB>value" each) as table rows.
void CWebServer::OutStatusRows (void)
{
	static char Text[1024];
	m_pHost->StatusText (Text, sizeof Text);
	for (char *pLine = Text; *pLine; )
	{
		char *pEnd = pLine;
		while (*pEnd && *pEnd != '\n') pEnd++;
		char Save = *pEnd;
		*pEnd = '\0';
		char *pTab = pLine;
		while (*pTab && *pTab != '\t') pTab++;
		if (*pTab)
		{
			*pTab = '\0';
			char *pAct = pTab + 1;
			while (*pAct && *pAct != '\t') pAct++;
			char cAct = *pAct;
			*pAct = '\0';
			Out ("<tr><td class=k>");
			OutHtml (pLine);
			Out ("</td><td>");
			OutHtml (pTab + 1);
			if (cAct)
			{
				Out (" <button onclick=\"drv('");
				OutHtml (pAct + 1);
				Out ("','')\">Eject</button>");
			}
			Out ("</td></tr>");
			*pAct = cAct;
			*pTab = '\t';
		}
		*pEnd = Save;
		pLine = *pEnd ? pEnd + 1 : pEnd;
	}
}
