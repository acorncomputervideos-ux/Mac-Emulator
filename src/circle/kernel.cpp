// Mac Emulator for Raspberry Pi (Circle bare-metal kernel).

#include "kernel.h"
#include <circle/util.h>
#include <circle/usb/usbhid.h>
#include <circle/synchronize.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/bcmpropertytags.h>
#include <circle/startup.h>
#include <circle/net/mdnsdaemon.h>

#include "../core/keys.h"
#include "../frontend/keymap.h"
#include "../frontend/util.h"

using macemu::Machine; using macemu::Osd; using macemu::Config; using macemu::Scsi;
using namespace macemu;
typedef ::u64 hostu64;

#define DRIVE		"SD:"
#define CONFIG_FILE	DRIVE "/macemu.cfg"
#define PRAM_FILE	DRIVE "/macemu.pram"
#define SHOT_DIR	DRIVE "/screenshots"

static const char FromKernel[] = "macemu";

static const unsigned FrameMicros = 16626;	// 370 lines x 352 cycles at 7.8336 MHz
static const unsigned MaxFloppyBytes = 819200 + 84 + 4096;

CKernel *CKernel::s_pThis = 0;

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_pFrameBuffer (0),
	m_pSound (0),
	m_pKeyboard (0),
	m_pMouse (0),
	m_pMachine (0),
	m_pPlus (0),
	m_pMacII (0),
	m_pQuadra (0),
	m_pOsd (0),
	m_pROM (0),
	m_nROMSize (0),
	m_pRAM (0),
	m_pPrevScreen (0),
	m_bForceRedraw (TRUE),
	m_bHaveVideo (FALSE),
	m_nVideoWidth (0),
	m_nVideoHeight (0),
	m_nClutVersion (0),
	m_nKeyIn (0),
	m_nKeyOut (0),
	m_LastModifiers (0),
	m_nMouseX (0),
	m_nMouseY (0),
	m_nMouseButtons (0),
	m_nMouseRemX (0),
	m_nMouseRemY (0),
	m_bCapsLock (FALSE),
	m_bTurbo (FALSE),
	m_nSampleRate (48000),
	m_nSoundFraction (0),
	m_nFrames (0),
	m_nLastActivity (0),
	m_nIdleFrames (0),
	m_nScreenshot (0),
	m_nEmulatedFrames (0),
	m_nStatsTime (0),
	m_nSpeedPercent (100),
	m_pNet (0),
	m_pWebServer (0),
	m_bNetRunning (FALSE),
	m_nNetTicks (0)
{
	s_pThis = this;

	m_pFrameBufferPixels = 0;
	m_nFrameBufferPitch = 0;
	m_nDisplayWidth = m_nDisplayHeight = 0;
	m_Surface.pixels = 0;
	m_Surface.width = m_Surface.height = m_Surface.pitch = 0;
	m_Surface.dirtyTop = m_Surface.dirtyBottom = 0;
	m_Surface.swapRB = false;


	memset (m_LastKeys, 0, sizeof m_LastKeys);
	memset (m_bMacKeyDown, 0, sizeof m_bMacKeyDown);
	m_pFloppy[0] = m_pFloppy[1] = 0;
	m_FloppyName[0][0] = m_FloppyName[1][0] = '\0';
	for (unsigned i = 0; i < Scsi::kTargets; i++)
	{
		m_pHardDisk[i] = 0;
		m_HardDiskPath[i][0] = '\0';
	}
	m_InfoLine[0][0] = m_InfoLine[1][0] = '\0';
	strCopy (m_NetLine[0], "Network: off (network=0 in macemu.cfg)", sizeof m_NetLine[0]);
	m_NetLine[1][0] = m_NetLine[2][0] = '\0';
	m_ROMName[0] = '\0';
	m_Config.setDefaults ();
}

CKernel::~CKernel (void)
{
	s_pThis = 0;
}

boolean CKernel::Initialize (void)
{
	boolean bOK = m_Serial.Initialize (115200);

	if (bOK)
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice (m_Options.GetLogDevice (), FALSE);
		bOK = m_Logger.Initialize (pTarget != 0 ? pTarget : &m_Serial);
	}

	if (bOK)
	{
		bOK = m_Interrupt.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Timer.Initialize ();
	}

	if (bOK)
	{
		bOK = m_EMMC.Initialize ();
	}

	if (bOK)
	{
		bOK = m_USBHCI.Initialize (FALSE);	// devices are found later, while the Mac starts up
	}

	return bOK;
}

// ----------------------------------------------------------------- start-up

boolean CKernel::LoadConfig (void)
{
	FIL File;
	if (f_open (&File, CONFIG_FILE, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		m_Logger.Write (FromKernel, LogNotice, "No macemu.cfg, using defaults");
		return FALSE;
	}

	static char Buffer[8192];
	UINT nRead = 0;
	f_read (&File, Buffer, sizeof Buffer - 1, &nRead);
	f_close (&File);

	m_Config.parse (Buffer, nRead);

	return TRUE;
}

boolean CKernel::LoadROM (void)
{
	static const char *Candidates[] =
	{
		"", "mac.rom", "MacPlus.rom", "Plus.rom", "vMac.ROM", "mac512ke.rom", "Mac128K.rom", 0
	};

	for (unsigned i = 0; Candidates[i] != 0; i++)
	{
		const char *pName = i == 0 ? m_Config.rom : Candidates[i];
		if (*pName == '\0')
		{
			continue;
		}

		char Path[160] = DRIVE "/";
		strAppend (Path, pName, sizeof Path);

		FIL File;
		if (f_open (&File, Path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
		{
			continue;
		}

		u32 nSize = (u32) f_size (&File);
		if (nSize != 0x10000 && nSize != 0x20000 && nSize != 0x40000 && nSize != 0x80000 && nSize != 0x100000)
		{
			m_Logger.Write (FromKernel, LogWarning, "%s: unsupported size %u", pName, nSize);
			f_close (&File);
			continue;
		}

		m_pROM = new u8[nSize];
		UINT nRead = 0;
		FRESULT Result = f_read (&File, m_pROM, nSize, &nRead);
		f_close (&File);
		if (Result != FR_OK || nRead != nSize)
		{
			delete [] m_pROM;
			m_pROM = 0;
			continue;
		}

		m_nROMSize = nSize;
		strCopy (m_ROMName, pName, sizeof m_ROMName);
		m_Logger.Write (FromKernel, LogNotice, "ROM %s (%uK, checksum %08X)",
				pName, nSize / 1024, be32 (m_pROM));

		return TRUE;
	}

	return FALSE;
}

// The Pi's frame buffer lives in uncached device memory, so writing it is
// slow.  Make it only a little larger than the Mac screen, with the
// monitor's aspect ratio, and let the GPU scale it to the display for free.
// (width= and height= in macemu.cfg override this.)
boolean CKernel::SetupDisplay (int nMacWidth, int nMacHeight)
{
	if (m_nDisplayWidth == 0)
	{
		CBcmPropertyTags Tags;
		TPropertyTagDisplayDimensions Dim;
		if (   Tags.GetTag (PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim)
		    && Dim.nWidth >= 320 && Dim.nHeight >= 200)
		{
			m_nDisplayWidth = Dim.nWidth;
			m_nDisplayHeight = Dim.nHeight;
		}
		else
		{
			m_nDisplayWidth = 1920;
			m_nDisplayHeight = 1080;
		}
	}

	unsigned nWidth = m_Config.width, nHeight = m_Config.height;
	if (nWidth == 0 || nHeight == 0)
	{
		nHeight = nMacHeight < 384 ? 384 : nMacHeight;	// room for the menu
		nWidth = (nHeight * m_nDisplayWidth / m_nDisplayHeight + 15) & ~15u;
		if (nWidth < (unsigned) nMacWidth)
		{
			nWidth = (nMacWidth + 15) & ~15u;
			nHeight = nWidth * m_nDisplayHeight / m_nDisplayWidth;
			if (nHeight < (unsigned) nMacHeight)
			{
				nHeight = nMacHeight;
			}
		}
	}

	if (m_pFrameBuffer != 0)
	{
		delete m_pFrameBuffer;
		m_pFrameBuffer = 0;
	}
	delete [] m_Surface.pixels;
	m_Surface.pixels = 0;
	m_pFrameBufferPixels = 0;

	m_pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, 32);
	if (!m_pFrameBuffer->Initialize ())
	{
		return FALSE;
	}
	m_Logger.Write (FromKernel, LogNotice, "Frame buffer %ux%u for a %dx%d Mac screen on a %ux%u display",
			nWidth, nHeight, nMacWidth, nMacHeight, m_nDisplayWidth, m_nDisplayHeight);

	// Device memory faults on the wide and unaligned stores of optimised
	// drawing code: draw into a back buffer and copy changed rows out word
	// by word (Present).
	m_pFrameBufferPixels = (volatile u32 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	m_nFrameBufferPitch = m_pFrameBuffer->GetPitch () / 4;
	m_Surface.width = (int) m_pFrameBuffer->GetWidth ();
	m_Surface.height = (int) m_pFrameBuffer->GetHeight ();
	m_Surface.pitch = m_Surface.width;
	m_Surface.pixels = new macemu::u32[m_Surface.width * m_Surface.height];
	m_Surface.dirtyTop = 0;
	m_Surface.dirtyBottom = m_Surface.height;

	// Pixel order: 1 = RGB in memory (0xBBGGRR as a word), 0 = BGR.
	CBcmPropertyTags Tags;
	TPropertyTagSimple Order;
	m_Surface.swapRB = Tags.GetTag (0x00040006, &Order, sizeof Order) && Order.nValue == 1;
	ApplyPalette ();

	fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
	m_Layout = layoutFor (m_Surface, nMacWidth, nMacHeight, m_Config.scale);
	m_bForceRedraw = TRUE;

	return TRUE;
}

void CKernel::ApplyPalette (void)
{
	setPalette (m_Surface, m_Config.palette);
	m_bForceRedraw = TRUE;
}

void CKernel::SetupSound (void)
{
	if (strEqNoCase (m_Config.soundDevice, "pwm"))
	{
		m_nSampleRate = 44100;
		m_pSound = new CPWMSoundBaseDevice (&m_Interrupt, m_nSampleRate, 2048);
	}
	else
	{
#if RASPPI <= 4
		m_nSampleRate = 48000;
		m_pSound = new CHDMISoundBaseDevice (&m_Interrupt, m_nSampleRate, 384 * 10);
#else
		m_nSampleRate = 44100;
		m_pSound = new CPWMSoundBaseDevice (&m_Interrupt, m_nSampleRate, 2048);
#endif
	}

	if (   !m_pSound->AllocateQueue (100)
	    || (m_pSound->SetWriteFormat (SoundFormatSigned16, 2), !m_pSound->Start ()))
	{
		m_Logger.Write (FromKernel, LogWarning, "Sound device not available");
		delete m_pSound;
		m_pSound = 0;
	}
}

void CKernel::AttachDisks (void)
{
	for (unsigned d = 0; d < 2; d++)
	{
		if (m_Config.floppy[d][0] != '\0' && !insertDisk ((int) d, m_Config.floppy[d]))
		{
			m_Logger.Write (FromKernel, LogWarning, "Cannot insert %s", m_Config.floppy[d]);
		}
	}

	AttachHardDisks ();
}

// (Re)attach the SCSI disks named in the settings.  "none" means no disk;
// an empty setting means hdN.img if it exists.
void CKernel::AttachHardDisks (void)
{
	m_pMachine->flushDisks ();
	for (unsigned id = 0; id < Scsi::kTargets; id++)
	{
		if (m_pHardDisk[id] != 0)
		{
			m_pMachine->attachDisk ((int) id, 0);
			delete m_pHardDisk[id];
			m_pHardDisk[id] = 0;
			m_HardDiskPath[id][0] = '\0';
		}

		if (strEqNoCase (m_Config.hd[id], "none"))
		{
			continue;
		}

		char Path[160] = DRIVE "/";
		if (m_Config.hd[id][0] != '\0')
		{
			strAppend (Path, m_Config.hd[id], sizeof Path);
		}
		else
		{
			strAppend (Path, "hd", sizeof Path);
			appendUint (Path, id, sizeof Path);
			strAppend (Path, ".img", sizeof Path);
		}

		CFileStorage *pDisk = new CFileStorage;
		if (pDisk->Open (Path))
		{
			m_pHardDisk[id] = pDisk;
			strCopy (m_HardDiskPath[id], Path + sizeof DRIVE, sizeof m_HardDiskPath[id]);
			m_pMachine->attachDisk ((int) id, pDisk);
			m_Logger.Write (FromKernel, LogNotice, "SCSI ID %u: %s (%u MB%s)", id, Path,
					pDisk->size () >> 20, pDisk->readOnly () ? ", read-only" : "");
		}
		else
		{
			delete pDisk;
			if (m_Config.hd[id][0] != '\0')
			{
				m_Logger.Write (FromKernel, LogWarning, "Cannot open %s", Path);
			}
		}
	}
}

void CKernel::LoadPRAM (void)
{
	FIL File;
	if (f_open (&File, PRAM_FILE, FA_READ | FA_OPEN_EXISTING) == FR_OK)
	{
		UINT nRead;
		f_read (&File, m_pMachine->clockChip ().pram, sizeof m_pMachine->clockChip ().pram, &nRead);
		f_close (&File);
	}

	m_pMachine->clockChip ().pramDirty = false;
}

void CKernel::SavePRAM (void)
{
	if (!m_pMachine->clockChip ().pramDirty)
	{
		return;
	}

	FIL File;
	if (f_open (&File, PRAM_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
	{
		UINT nWritten;
		f_write (&File, m_pMachine->clockChip ().pram, sizeof m_pMachine->clockChip ().pram, &nWritten);
		f_close (&File);
		m_pMachine->clockChip ().pramDirty = false;
	}
}

void CKernel::Fatal (const char *pLine1, const char *pLine2, const char *pLine3)
{
	m_Logger.Write (FromKernel, LogError, "%s %s %s", pLine1, pLine2, pLine3);

	if (m_Surface.pixels != 0)
	{
		const char *Lines[] = {"Mac Emulator cannot start", "", pLine1, pLine2, pLine3};
		int fh = m_Font.height ();
		int y = m_Surface.height / 2 - 3 * fh;
		fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
		for (unsigned i = 0; i < sizeof Lines / sizeof Lines[0]; i++, y += fh + fh / 2)
		{
			int x = (m_Surface.width - (int) strlen (Lines[i]) * m_Font.width ()) / 2;
			drawText (m_Surface, m_Font, x, y, Lines[i], ColSelFg);
		}
	}

	Present ();
	while (1)
	{
		m_ActLED.Blink (2, 100, 100);
		m_Timer.MsDelay (1500);
	}
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "Mac Emulator, built " __DATE__ " " __TIME__);
	m_CPUThrottle.SetSpeed (CPUSpeedMaximum);	// the firmware starts the ARM at its lowest clock

	boolean bMounted = f_mount (&m_FileSystem, DRIVE, 1) == FR_OK;
	if (bMounted)
	{
		LoadConfig ();
	}

	if (!SetupDisplay (640, 480))
	{
		m_Logger.Write (FromKernel, LogPanic, "Cannot initialize the framebuffer");
	}

	if (!bMounted)
	{
		Fatal ("Cannot mount the SD card (FAT32 expected).");
	}

	if (!LoadROM ())
	{
		Fatal ("No Macintosh ROM found on the SD card.",
		       "Copy a 128K Mac Plus ROM (or 64K 128K/512K ROM) there",
		       "and name it mac.rom, or set rom= in macemu.cfg.");
	}

	// Pick the model: 64K/128K ROMs are the compact Macs, 256K is the
	// Mac II, 1 MB the Quadra 610/650/800 family.
	boolean bQuadra = strEqNoCase (m_Config.machine, "quadra")
			  || (strEqNoCase (m_Config.machine, "auto") && m_nROMSize == 0x100000);
	boolean bMacII = !bQuadra
			 && (strEqNoCase (m_Config.machine, "ii")
			     || (!strEqNoCase (m_Config.machine, "plus") && m_nROMSize >= 0x40000));
	if (bQuadra)
	{
		if (m_nROMSize != 0x100000 || be32 (m_pROM) != 0xF1ACAD13)
		{
			Fatal ("The Quadra emulation needs the 1 MB ROM of the",
			       "Quadra 610/650/800 or Centris 610/650",
			       "(checksum F1ACAD13).");
		}
		if (m_Config.ramKB < 8192)
		{
			m_Config.ramKB = 8192;
		}
		m_pQuadra = new Quadra;
	}
	else if (bMacII)
	{
		if (m_nROMSize != 0x40000)
		{
			Fatal ("This ROM belongs to a Macintosh model that is",
			       "not emulated yet (IIx, IIci, IIfx, Quadra 700 ...).",
			       "Use a Quadra 610/650/800, Mac II or Mac Plus ROM.");
		}
		if (m_Config.ramKB < 1024)
		{
			m_Config.ramKB = 1024;
		}
		m_pMacII = new MacII (m_Config.videoWidth, m_Config.videoHeight);
	}
	else
	{
		if (m_Config.ramKB > 4096)
		{
			m_Config.ramKB = 4096;		// the Plus addresses at most 4 MB
		}
		if (m_nROMSize == 0x10000 && m_Config.ramKB > 512)
		{
			m_Config.ramKB = 512;		// the 64K ROMs cannot use more
		}
	}

	m_pRAM = new u8[m_Config.ramKB * 1024];
	m_pPrevScreen = new u8[0x100000];
	boolean bOK = FALSE;
	if (m_pRAM != 0 && m_pPrevScreen != 0)
	{
		if (m_pQuadra != 0)
		{
			bOK = m_pQuadra->init (m_pROM, m_nROMSize, m_pRAM, m_Config.ramKB * 1024);
			m_pMachine = m_pQuadra;
		}
		else if (m_pMacII != 0)
		{
			bOK = m_pMacII->init (m_pROM, m_nROMSize, m_pRAM, m_Config.ramKB * 1024);
			m_pMachine = m_pMacII;
		}
		else
		{
			m_pPlus = new Machine;
			bOK = m_pPlus != 0 && m_pPlus->init (m_pROM, m_nROMSize, m_pRAM, m_Config.ramKB * 1024);
			m_pMachine = m_pPlus;
		}
	}
	if (!bOK)
	{
		Fatal ("Out of memory, or unusable ROM image.");
	}

	m_pMachine->clockChip ().seconds = m_Config.clockSeconds (__DATE__, __TIME__);
	LoadPRAM ();
	AttachDisks ();

	m_pOsd = new Osd (m_Config, *this);
	StartNetwork ();

	if (m_Config.sound)
	{
		SetupSound ();
	}

	UpdateInfo ();
	m_Logger.Write (FromKernel, LogNotice, "Running: %uK RAM, display %ux%u, zoom %d",
			m_Config.ramKB, m_Surface.width, m_Surface.height, m_Layout.scale);

	hostu64 nNextFrame = CTimer::GetClockTicks64 ();
	m_nStatsTime = nNextFrame;

	while (1)
	{
		UpdateUSB ();
		UpdateNetwork ();
		ProcessInput ();

		unsigned nSpeed = m_bTurbo ? 0 : (unsigned) m_Config.speed;
		if (m_pOsd->active ())
		{
			// The Mac is paused while the menu is open.
			if (m_pOsd->needsRedraw ())
			{
				// Pages differ in size: clear what the last one covered.
				fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
				DrawMac (TRUE);
				m_pOsd->draw (m_Surface, m_Font);
				Present ();
			}

			if (!m_pOsd->active ())
			{
				fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
				m_bForceRedraw = TRUE;
			}

			WaitUntil (CTimer::GetClockTicks64 () + 10000);
			nNextFrame = CTimer::GetClockTicks64 ();
			continue;
		}

		if (nSpeed == 0)
		{
			// Unlimited: emulate for one display frame's worth of real time.
			hostu64 nUntil = CTimer::GetClockTicks64 () + FrameMicros;
			do
			{
				m_pMachine->runFrame ();
				m_nEmulatedFrames++;
				Housekeeping ();
				m_Scheduler.Yield ();		// let the network run
			}
			while (CTimer::GetClockTicks64 () < nUntil);

			nNextFrame = CTimer::GetClockTicks64 ();
		}
		else
		{
			for (unsigned i = 0; i < nSpeed; i++)
			{
				m_pMachine->runFrame ();
				m_nEmulatedFrames++;
				Housekeeping ();
			}

			if (nSpeed == 1)
			{
				QueueSound ();
			}

			nNextFrame += FrameMicros;
			hostu64 nNow = CTimer::GetClockTicks64 ();
			if (nNow + 4 * FrameMicros < nNextFrame || nNow > nNextFrame + 4 * FrameMicros)
			{
				nNextFrame = nNow;	// fell far behind (or clock jumped): resync
			}

			WaitUntil (nNextFrame);			// the network runs meanwhile
		}

		UpdateScreen ();
		Present ();
		m_CPUThrottle.Update ();
		BootLog ();
		m_nFrames++;

		if (m_pMachine->poweredOff ())
		{
			// Shut Down: save everything and wait for a key to start again.
			m_pMachine->flushDisks ();
			SavePRAM ();
			m_ActLED.Off ();
			ShowMessage ("It is now safe to switch off your Macintosh.",
				     "Press any key to start it again.");
			Present ();
			m_nKeyOut = m_nKeyIn;
			while (m_nKeyOut == m_nKeyIn)
			{
				UpdateUSB ();
				UpdateNetwork ();
				WaitUntil (CTimer::GetClockTicks64 () + 20000);
			}
			m_nKeyOut = m_nKeyIn;
			ReleaseAllKeys ();
			resetMachine ();
			m_bHaveVideo = FALSE;
			fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
			nNextFrame = CTimer::GetClockTicks64 ();
		}

		hostu64 nNow = CTimer::GetClockTicks64 ();
		if (nNow - m_nStatsTime >= 2000000)
		{
			m_nSpeedPercent = (unsigned) ((hostu64) m_nEmulatedFrames * FrameMicros * 100
						      / (nNow - m_nStatsTime));
			m_nEmulatedFrames = 0;
			m_nStatsTime = nNow;
			UpdateInfo ();
		}
	}

	return ShutdownHalt;
}

// ----------------------------------------------------------------- per frame

void CKernel::UpdateScreen (void)
{
	DrawMac (m_bForceRedraw);
	m_bForceRedraw = FALSE;
}

void CKernel::Present (void)
{
	if (m_pFrameBufferPixels == 0 || m_Surface.pixels == 0)
	{
		return;
	}
	int nTop = m_Surface.dirtyTop, nBottom = m_Surface.dirtyBottom;
	if (nTop < 0) nTop = 0;
	if (nBottom > m_Surface.height) nBottom = m_Surface.height;
	for (int y = nTop; y < nBottom; y++)
	{
		const macemu::u32 *pSrc = m_Surface.pixels + y * m_Surface.pitch;
		volatile u32 *pDst = m_pFrameBufferPixels + y * m_nFrameBufferPitch;
		for (int x = 0; x < m_Surface.width; x++)
		{
			pDst[x] = pSrc[x];
		}
	}
	m_Surface.dirtyTop = m_Surface.height;
	m_Surface.dirtyBottom = 0;
}

// For the first three minutes, note progress in SD:/macemu.log so that a
// problem on real hardware can be diagnosed.
void CKernel::BootLog (void)
{
	static unsigned nLastSecond = 0;
	unsigned nSecond = m_Timer.GetUptime ();
	if (nSecond == nLastSecond || nSecond % 5 != 0 || nSecond > 180)
	{
		return;
	}
	nLastSecond = nSecond;

	char Line[160] = "";
	appendUint (Line, nSecond, sizeof Line);
	strAppend (Line, "s frames=", sizeof Line);
	appendUint (Line, m_nFrames, sizeof Line);
	strAppend (Line, " speed=", sizeof Line);
	appendUint (Line, m_nSpeedPercent, sizeof Line);
	strAppend (Line, "% pc=", sizeof Line);
	appendHex (Line, m_pMachine->debugPC (), 8, sizeof Line);
	strAppend (Line, " disk=", sizeof Line);
	appendUint (Line, m_pMachine->diskActivity (), sizeof Line);
	strAppend (Line, " clock=", sizeof Line);
	appendUint (Line, m_CPUThrottle.GetClockRate () / 1000000, sizeof Line);
	strAppend (Line, "MHz temp=", sizeof Line);
	appendUint (Line, m_CPUThrottle.GetTemperature (), sizeof Line);
	strAppend (Line, "C\r\n", sizeof Line);
	m_Logger.Write (FromKernel, LogNotice, "%s", Line);

	FIL File;
	BYTE Mode = nSecond == 5 ? (FA_WRITE | FA_CREATE_ALWAYS) : (FA_WRITE | FA_OPEN_APPEND);
	if (f_open (&File, DRIVE "/macemu.log", Mode) == FR_OK)
	{
		UINT nWritten;
		f_write (&File, Line, strLen (Line), &nWritten);
		f_close (&File);
	}
}

void CKernel::DrawMac (boolean bForce)
{
	VideoFrame Frame;
	if (!m_pMachine->video (Frame))
	{
		if (m_bHaveVideo)
		{
			fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
			m_bHaveVideo = FALSE;
		}
		return;
	}

	if (   !m_bHaveVideo
	    || Frame.width != m_nVideoWidth
	    || Frame.height != m_nVideoHeight)
	{
		m_bHaveVideo = TRUE;
		if (Frame.width != m_nVideoWidth || Frame.height != m_nVideoHeight)
		{
			m_nVideoWidth = Frame.width;
			m_nVideoHeight = Frame.height;
			SetupDisplay (Frame.width, Frame.height);
		}
		m_Layout = layoutFor (m_Surface, Frame.width, Frame.height, m_Config.scale);
		fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
		bForce = TRUE;
	}

	if (Frame.clutVersion != m_nClutVersion)
	{
		m_nClutVersion = Frame.clutVersion;
		bForce = TRUE;
	}

	blitScreen (m_Surface, m_Layout, Frame, m_pPrevScreen, bForce);
}

void CKernel::ShowMessage (const char *pLine1, const char *pLine2)
{
	int fh = m_Font.height ();
	fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
	int x = (m_Surface.width - (int) strlen (pLine1) * m_Font.width ()) / 2;
	drawText (m_Surface, m_Font, x, m_Surface.height / 2 - fh, pLine1, ColSelFg);
	x = (m_Surface.width - (int) strlen (pLine2) * m_Font.width ()) / 2;
	drawText (m_Surface, m_Font, x, m_Surface.height / 2 + fh, pLine2, ColDim);
}

void CKernel::QueueSound (void)
{
	if (m_pSound == 0 || !m_Config.sound)
	{
		return;
	}

	// Resample 370 samples/frame (22254.5 Hz) to the device rate.
	static s16 Buffer[2 * 1200];
	u32 nInCount, nInRate;
	const s16 *pIn = m_pMachine->sound (nInCount, nInRate);
	unsigned nOut = 0;
	int nVolume = m_Config.volume;
	while (nOut < 1200)
	{
		unsigned nIndex = m_nSoundFraction / m_nSampleRate;
		if (nIndex >= nInCount)
		{
			m_nSoundFraction -= nInCount * m_nSampleRate;
			break;
		}

		s16 nSample = (s16) (pIn[nIndex] * nVolume / 10);
		Buffer[2 * nOut] = nSample;
		Buffer[2 * nOut + 1] = nSample;
		nOut++;
		m_nSoundFraction += nInRate;
	}

	unsigned nFree = m_pSound->GetQueueFramesAvail ();
	if (nOut > nFree)
	{
		nOut = nFree;			// queue full: drop rather than block
	}

	if (nOut > 0)
	{
		m_pSound->Write (Buffer, nOut * 2 * sizeof (s16));
	}
}

void CKernel::Housekeeping (void)
{
	// Disks the Mac ejected by itself.
	for (unsigned d = 0; d < 2; d++)
	{
		if (m_pMachine->floppy ().ejectedByGuest[d])
		{
			m_pMachine->floppy ().ejectedByGuest[d] = false;
			ejectDisk ((int) d);
		}
	}

	// Activity LED, and write-back once the disks have been idle for a while.
	u32 nActivity = m_pMachine->diskActivity ();
	if (nActivity != m_nLastActivity)
	{
		m_nLastActivity = nActivity;
		m_nIdleFrames = 0;
		m_ActLED.On ();
		return;
	}

	if (++m_nIdleFrames == 6)
	{
		m_ActLED.Off ();
	}
	else if (m_nIdleFrames == 120)
	{
		for (unsigned d = 0; d < 2; d++)
		{
			if (m_pFloppy[d] != 0)
			{
				m_pFloppy[d]->flush ();
			}
		}

		m_pMachine->flushDisks ();
		SavePRAM ();
	}
	else if (m_nIdleFrames % 600 == 0)
	{
		SavePRAM ();
	}
}

void CKernel::UpdateInfo (void)
{
	char *p = m_InfoLine[0];
	p[0] = '\0';
	strAppend (p, m_pMachine->modelName (), 64);
	strAppend (p, ", ", 64);
	appendUint (p, m_Config.ramKB, 64);
	strAppend (p, "K RAM, ROM ", 64);
	strAppend (p, m_ROMName, 64);

	p = m_InfoLine[1];
	p[0] = '\0';
	strAppend (p, "Speed ", 64);
	appendUint (p, m_nSpeedPercent, 64);
	strAppend (p, "%   SCSI:", 64);
	boolean bAny = FALSE;
	for (unsigned id = 0; id < Scsi::kTargets; id++)
	{
		if (m_pHardDisk[id] != 0)
		{
			strAppend (p, " ", 64);
			appendUint (p, id, 64);
			bAny = TRUE;
		}
	}

	if (!bAny)
	{
		strAppend (p, " none", 64);
	}
}

// ----------------------------------------------------------------- input

// ----------------------------------------------------------------- network

// Wired Ethernet with DHCP, and the web file manager on port 80.  Nothing
// waits for the network: it comes up in the background while the Mac starts.
void CKernel::StartNetwork (void)
{
	if (!m_Config.network)
	{
		return;
	}

	strCopy (m_NetLine[0], "Network: waiting for a connection...", sizeof m_NetLine[0]);
	strCopy (m_NetLine[1], "Plug in an Ethernet cable (Wi-Fi is not supported).", sizeof m_NetLine[1]);

	m_pNet = new CNetSubSystem (0, 0, 0, 0, "macemu");
	if (m_pNet == 0 || !m_pNet->Initialize (FALSE))
	{
		m_Logger.Write (FromKernel, LogWarning, "No network");
		strCopy (m_NetLine[0], "Network: no Ethernet adapter found", sizeof m_NetLine[0]);
		m_NetLine[1][0] = '\0';
		return;
	}

	m_pWebServer = new CWebServer (m_pNet, DRIVE, this);
}

void CKernel::UpdateNetwork (void)
{
	if (m_pNet == 0)
	{
		return;
	}

	m_Scheduler.Yield ();

	if (m_pWebServer != 0)
	{
		if (m_pWebServer->FlushRequested ())
		{
			for (unsigned d = 0; d < 2; d++)
			{
				if (m_pFloppy[d] != 0)
				{
					m_pFloppy[d]->flush ();
				}
			}
			m_pMachine->flushDisks ();
			m_pWebServer->FlushDone ();
		}

		if (m_pWebServer->RestartRequested ())
		{
			resetMachine ();
			m_pWebServer->RestartDone ();
		}
	}

	// Status for the menu, about twice a second.
	unsigned nTicks = m_Timer.GetTicks ();
	if (nTicks - m_nNetTicks < HZ / 2)
	{
		return;
	}
	m_nNetTicks = nTicks;

	if (!m_bNetRunning && m_pNet->IsRunning ())
	{
		m_bNetRunning = TRUE;
		CmDNSDaemon::Get ();			// answers to macemu.local

		CString IPString;
		m_pNet->GetConfig ()->GetIPAddress ()->Format (&IPString);
		strCopy (m_NetLine[0], "Web file manager: http://", sizeof m_NetLine[0]);
		strAppend (m_NetLine[0], (const char *) IPString, sizeof m_NetLine[0]);
		strAppend (m_NetLine[0], "/", sizeof m_NetLine[0]);
		strCopy (m_NetLine[1], "or http://macemu.local/ - open it in a browser", sizeof m_NetLine[1]);
		m_Logger.Write (FromKernel, LogNotice, "Web file manager at http://%s/",
				(const char *) IPString);
	}

	if (m_bNetRunning && m_pWebServer != 0)
	{
		const char *pActivity = m_pWebServer->Activity ();
		strCopy (m_NetLine[2], pActivity[0] ? pActivity : "Idle.", sizeof m_NetLine[2]);
	}

	m_pOsd->refresh ();
}

// Wait for a moment in time, letting the network tasks run meanwhile.
void CKernel::WaitUntil (hostu64 nUntil)
{
	while (CTimer::GetClockTicks64 () < nUntil)
	{
		if (m_pNet != 0)
		{
			m_Scheduler.Yield ();
		}
	}
}

boolean CKernel::FileInUse (const char *pPath)
{
	for (unsigned id = 0; id < Scsi::kTargets; id++)
	{
		if (m_pHardDisk[id] != 0 && strEqNoCase (m_HardDiskPath[id], pPath))
		{
			return TRUE;
		}
	}

	for (unsigned d = 0; d < 2; d++)
	{
		if (m_pFloppy[d] == 0)
		{
			continue;
		}

		char Path[160] = "";
		boolean bHasFolder = FALSE;
		for (const char *p = m_FloppyName[d]; *p; p++)
		{
			bHasFolder |= *p == '/';
		}
		if (!bHasFolder)
		{
			strCopy (Path, m_Config.diskDir, sizeof Path);
			strAppend (Path, "/", sizeof Path);
		}
		strAppend (Path, m_FloppyName[d], sizeof Path);
		if (strEqNoCase (Path, pPath))
		{
			return TRUE;
		}
	}

	return FALSE;
}

void CKernel::UpdateUSB (void)
{
	if (!m_USBHCI.UpdatePlugAndPlay ())
	{
		return;
	}

	if (m_pKeyboard == 0)
	{
		CUSBKeyboardDevice *pKeyboard =
			(CUSBKeyboardDevice *) m_DeviceNameService.GetDevice ("ukbd1", FALSE);
		if (pKeyboard != 0)
		{
			pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler);
			pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw);
			m_pKeyboard = pKeyboard;
			m_Logger.Write (FromKernel, LogNotice, "Keyboard attached");
		}
	}

	if (m_pMouse == 0)
	{
		CMouseDevice *pMouse = (CMouseDevice *) m_DeviceNameService.GetDevice ("mouse1", FALSE);
		if (pMouse != 0)
		{
			pMouse->RegisterRemovedHandler (MouseRemovedHandler);
			pMouse->RegisterStatusHandler (MouseStatusHandler);
			m_pMouse = pMouse;
			m_Logger.Write (FromKernel, LogNotice, "Mouse attached");
		}
	}
}

// Interrupt context: turn the keyboard report into press/release events.
void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	CKernel *pThis = s_pThis;
	if (pThis == 0)
	{
		return;
	}

	unsigned nIn = pThis->m_nKeyIn;
	auto Push = [&] (u16 usEvent)
	{
		unsigned nNext = (nIn + 1) % KeyQueueSize;
		if (nNext != pThis->m_nKeyOut)
		{
			pThis->m_KeyQueue[nIn] = usEvent;
			nIn = nNext;
		}
	};

	u8 ucChanged = ucModifiers ^ pThis->m_LastModifiers;
	for (unsigned i = 0; i < 8; i++)
	{
		u8 ucBit = (u8) (1 << i);
		if (ucChanged & ucBit)
		{
			Push ((u16) (0x4000 | ((ucModifiers & ucBit) ? 0x8000 : 0) | ucBit));
		}
	}
	pThis->m_LastModifiers = ucModifiers;

	for (unsigned i = 0; i < 6; i++)			// released keys
	{
		u8 k = pThis->m_LastKeys[i];
		if (k < 4)
		{
			continue;
		}

		boolean bStill = FALSE;
		for (unsigned j = 0; j < 6; j++)
		{
			bStill |= RawKeys[j] == k;
		}

		if (!bStill)
		{
			Push (k);
		}
	}

	for (unsigned i = 0; i < 6; i++)			// pressed keys
	{
		u8 k = RawKeys[i];
		if (k < 4)					// 1..3 are rollover/error codes
		{
			continue;
		}

		boolean bWas = FALSE;
		for (unsigned j = 0; j < 6; j++)
		{
			bWas |= pThis->m_LastKeys[j] == k;
		}

		if (!bWas)
		{
			Push ((u16) (0x8000 | k));
		}
	}

	if (RawKeys[0] != 1)					// ignore phantom (rollover) reports
	{
		memcpy (pThis->m_LastKeys, RawKeys, 6);
	}

	pThis->m_nKeyIn = nIn;
}

void CKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	if (s_pThis != 0)
	{
		s_pThis->m_pKeyboard = 0;
	}
}

void CKernel::MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY,
				  int nWheelMove)
{
	if (s_pThis != 0)
	{
		s_pThis->m_nMouseX += nDisplacementX;
		s_pThis->m_nMouseY += nDisplacementY;
		s_pThis->m_nMouseButtons = nButtons;
	}
}

void CKernel::MouseRemovedHandler (CDevice *pDevice, void *pContext)
{
	if (s_pThis != 0)
	{
		s_pThis->m_pMouse = 0;
		s_pThis->m_nMouseButtons = 0;
	}
}

void CKernel::ProcessInput (void)
{
	while (m_nKeyOut != m_nKeyIn)
	{
		u16 usEvent = m_KeyQueue[m_nKeyOut];
		m_nKeyOut = (m_nKeyOut + 1) % KeyQueueSize;

		boolean bDown = (usEvent & 0x8000) != 0;
		if (usEvent & 0x4000)
		{
			HandleModifier ((u8) usEvent, bDown);
		}
		else
		{
			HandleKey ((u8) usEvent, bDown);
		}
	}

	EnterCritical ();
	int dx = m_nMouseX, dy = m_nMouseY;
	m_nMouseX = m_nMouseY = 0;
	unsigned nButtons = m_nMouseButtons;
	LeaveCritical ();

	if (m_pOsd->active ())
	{
		return;
	}

	// Mouse speed 4 is 1:1; each step is a quarter.
	int nSpeed = m_Config.mouseSpeed;
	m_nMouseRemX += dx * nSpeed;
	m_nMouseRemY += dy * nSpeed;
	int mx = m_nMouseRemX / 4, my = m_nMouseRemY / 4;
	m_nMouseRemX -= mx * 4;
	m_nMouseRemY -= my * 4;
	if (mx != 0 || my != 0)
	{
		m_pMachine->mouseMove (mx, my);
	}

	m_pMachine->mouseButton ((nButtons & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT)) != 0);
}

void CKernel::MacKey (int nKey, boolean bDown)
{
	if (nKey < 0 || nKey >= 128 || m_bMacKeyDown[nKey] == bDown)
	{
		return;
	}

	m_bMacKeyDown[nKey] = bDown;
	m_pMachine->keyEvent (nKey, bDown);
}

void CKernel::ReleaseAllKeys (void)
{
	for (int k = 0; k < 128; k++)
	{
		if (k != MK_CapsLock)
		{
			MacKey (k, FALSE);
		}
	}
}

void CKernel::HandleModifier (u8 ucBit, boolean bDown)
{
	if (m_pOsd->active ())
	{
		return;
	}

	// Both sides of a modifier map to one Mac key: only release it when
	// no other key holding it remains down.
	int nKey = usbModifierToMacKey (ucBit, m_Config.altIsOption != 0);
	if (nKey == MK_None)
	{
		return;
	}

	if (!bDown)
	{
		for (unsigned i = 0; i < 8; i++)
		{
			u8 ucOther = (u8) (1 << i);
			if (   ucOther != ucBit
			    && (m_LastModifiers & ucOther)
			    && usbModifierToMacKey (ucOther, m_Config.altIsOption != 0) == nKey)
			{
				return;
			}
		}
	}

	MacKey (nKey, bDown);
}

void CKernel::HandleKey (u8 ucUsage, boolean bDown)
{
	// Emulator hot keys.
	// F12, or Pause, Scroll Lock or the Menu key (for keyboards whose F12
	// is a media key unless Fn is held).
	if (ucUsage == HID_F12 || ucUsage == 0x48 || ucUsage == 0x47 || ucUsage == 0x65)
	{
		if (bDown)
		{
			if (m_pOsd->active ())
			{
				m_pOsd->close ();
				fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
				m_bForceRedraw = TRUE;
			}
			else
			{
				ReleaseAllKeys ();
				m_pOsd->open ();
			}
		}

		return;
	}

	if (m_pOsd->active ())
	{
		if (!bDown)
		{
			return;
		}

		switch (ucUsage)
		{
		case HID_UP:		m_pOsd->key (Osd::NavUp);	break;
		case HID_DOWN:		m_pOsd->key (Osd::NavDown);	break;
		case HID_LEFT:		m_pOsd->key (Osd::NavLeft);	break;
		case HID_RIGHT:		m_pOsd->key (Osd::NavRight);	break;
		case HID_PAGEUP:	m_pOsd->key (Osd::NavPageUp);	break;
		case HID_PAGEDOWN:	m_pOsd->key (Osd::NavPageDown);	break;
		case HID_ENTER:
		case HID_KP_ENTER:
		case HID_SPACE:		m_pOsd->key (Osd::NavSelect);	break;
		case HID_ESC:		m_pOsd->key (Osd::NavBack);	break;
		default:						break;
		}

		return;
	}

	switch (ucUsage)
	{
	case HID_F11:
		if (bDown)
		{
			m_bTurbo = !m_bTurbo;
		}
		return;

	case HID_F10:
		if (bDown)
		{
			screenshot ();
		}
		return;

	case HID_F9:
		if (bDown)
		{
			m_pMachine->interruptButton ();
		}
		return;

	case HID_CAPSLOCK:
		// The Mac's Caps Lock key latches mechanically.
		if (bDown)
		{
			m_bCapsLock = !m_bCapsLock;
			MacKey (MK_CapsLock, m_bCapsLock);
		}
		return;

	default:
		MacKey (usbToMacKey (ucUsage), bDown);
		return;
	}
}

// ----------------------------------------------------------------- OsdHost

// Files on the card for the menu's pickers.  Floppies come from disk_dir;
// hard disk images from the card's root and disk_dir (anything larger than a
// floppy); ROMs from the root (64 KB to 1 MB).
int CKernel::listFiles (FileKind Kind, char (*ppNames)[kNameLen], int nMax)
{
	int n = 0;
	for (int nPass = 0; nPass < 2 && n < nMax; nPass++)
	{
		boolean bDiskDir = nPass == 1;
		if (Kind == Floppies && !bDiskDir) continue;
		if (Kind == Roms && bDiskDir) continue;

		char Path[160] = DRIVE "/";
		if (bDiskDir)
		{
			strAppend (Path, m_Config.diskDir, sizeof Path);
		}

		DIR Directory;
		FILINFO Info;
		FRESULT Result = f_findfirst (&Directory, &Info, Path, "*");
		while (Result == FR_OK && Info.fname[0] != '\0' && n < nMax)
		{
			u32 nSize = (u32) Info.fsize;
			boolean bUse = !(Info.fattrib & (AM_DIR | AM_HID | AM_SYS)) && Info.fname[0] != '.';
			switch (Kind)
			{
			case Floppies:
				bUse = bUse && (nSize == 409600 || nSize == 819200
						|| (nSize > 409600 + 84 && nSize <= MaxFloppyBytes));
				break;
			case HardDisks:
				bUse = bUse && nSize > 2 * 1024 * 1024 && !endsWithNoCase (Info.fname, ".zip");
				break;
			default:
				bUse = bUse && (nSize == 0x10000 || nSize == 0x20000 || nSize == 0x40000
						|| nSize == 0x80000 || nSize == 0x100000);
				break;
			}

			char Name[kNameLen] = "";
			if (bDiskDir && Kind != Floppies)
			{
				strAppend (Name, m_Config.diskDir, sizeof Name);
				strAppend (Name, "/", sizeof Name);
			}
			strAppend (Name, Info.fname, sizeof Name);
			if (bUse && strLen (Name) < kNameLen - 1)
			{
				strCopy (ppNames[n++], Name, kNameLen);
			}

			Result = f_findnext (&Directory, &Info);
		}

		f_closedir (&Directory);
	}

	return n;
}

bool CKernel::insertDisk (int nDrive, const char *pName)
{
	if (m_pFloppy[nDrive] != 0)
	{
		ejectDisk (nDrive);
	}

	// Names from macemu.cfg may carry a folder; picker names live in disk_dir.
	char Path[160] = DRIVE "/";
	boolean bHasFolder = FALSE;
	for (const char *p = pName; *p; p++)
	{
		bHasFolder |= *p == '/';
	}

	if (!bHasFolder)
	{
		strAppend (Path, m_Config.diskDir, sizeof Path);
		strAppend (Path, "/", sizeof Path);
	}
	strAppend (Path, pName, sizeof Path);

	CMemoryStorage *pDisk = new CMemoryStorage;
	if (   !pDisk->Load (Path, MaxFloppyBytes)
	    || !m_pMachine->floppy ().insert (nDrive, pDisk, m_Config.floppyReadOnly != 0))
	{
		delete pDisk;
		return false;
	}

	m_pFloppy[nDrive] = pDisk;
	strCopy (m_FloppyName[nDrive], pName, kNameLen);
	strCopy (m_Config.floppy[nDrive], pName, Config::kPath);
	m_Logger.Write (FromKernel, LogNotice, "Floppy %d: %s", nDrive + 1, Path);

	return true;
}

void CKernel::ejectDisk (int nDrive)
{
	m_pMachine->floppy ().eject (nDrive);		// flushes the image
	delete m_pFloppy[nDrive];
	m_pFloppy[nDrive] = 0;
	m_FloppyName[nDrive][0] = '\0';
	m_Config.floppy[nDrive][0] = '\0';
}

void CKernel::resetMachine (void)
{
	m_pMachine->flushDisks ();
	SavePRAM ();
	ReleaseAllKeys ();
	AttachHardDisks ();				// the menu may have changed them
	m_pMachine->reset ();
	m_bForceRedraw = TRUE;
}

// Model, ROM, memory and video changes need a fresh start of the whole
// emulator: save everything and reboot the Pi (it is quick).
void CKernel::saveAndReboot (void)
{
	saveSettings ();
	m_pMachine->flushDisks ();
	SavePRAM ();
	ShowMessage ("Restarting with the new settings...", "");
	Present ();
	m_Timer.MsDelay (200);
	reboot ();
}

void CKernel::interruptButton (void)
{
	m_pMachine->interruptButton ();
}

void CKernel::settingsChanged (void)
{
	ApplyPalette ();
	m_Layout = layoutFor (m_Surface, m_nVideoWidth ? m_nVideoWidth : 512, m_nVideoHeight ? m_nVideoHeight : 342, m_Config.scale);
	fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
	m_bForceRedraw = TRUE;

	if (m_Config.sound && m_pSound == 0)
	{
		SetupSound ();
	}
}

bool CKernel::saveSettings (void)
{
	static char Buffer[4096];
	u32 nLength = m_Config.serialize (Buffer, sizeof Buffer);

	FIL File;
	if (f_open (&File, CONFIG_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return false;
	}

	UINT nWritten = 0;
	boolean bOK = f_write (&File, Buffer, nLength, &nWritten) == FR_OK && nWritten == nLength;

	return f_close (&File) == FR_OK && bOK;
}

bool CKernel::screenshot (void)
{
	VideoFrame Frame;
	if (!m_pMachine->video (Frame))
	{
		return false;
	}
	static u8 *pBitmap = 0;
	if (pBitmap == 0)
	{
		pBitmap = new u8[54 + 1024 * 3 * 768];		// largest screen
	}
	u32 nBmpSize = bmpSize (Frame);
	makeBmp (Frame, m_Surface.palette, pBitmap);

	f_mkdir (SHOT_DIR);

	// Find the next free name.
	for (unsigned nTries = 0; nTries < 1000; nTries++)
	{
		char Path[64] = SHOT_DIR "/mac";
		appendUint (Path, m_nScreenshot++ % 1000, sizeof Path, 3);
		strAppend (Path, ".bmp", sizeof Path);

		FIL File;
		if (f_open (&File, Path, FA_WRITE | FA_CREATE_NEW) != FR_OK)
		{
			continue;
		}

		UINT nWritten = 0;
		boolean bOK = f_write (&File, pBitmap, nBmpSize, &nWritten) == FR_OK && nWritten == nBmpSize;
		f_close (&File);
		m_Logger.Write (FromKernel, LogNotice, "Screenshot %s", Path);

		return bOK;
	}

	return false;
}
