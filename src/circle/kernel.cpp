// Mac Emulator for Raspberry Pi (Circle bare-metal kernel).

#include "kernel.h"
#include "../core/state.h"
#include "hostfs_fat.h"
#include <circle/util.h>
#include <circle/usb/usbhid.h>
#include <circle/synchronize.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/sound/hdmisoundbasedevice.h>
#if RASPPI >= 4
#include <circle/sound/usbsoundbasedevice.h>
#endif
#include <circle/bcmpropertytags.h>
#include <circle/net/ntpdaemon.h>
#include <circle/startup.h>
#include <circle/net/mdnsdaemon.h>
#include <circle/bcmwatchdog.h>
#include <circle/machineinfo.h>
#include <circle/sched/task.h>
#include <circle/synchronize.h>

#include "../core/keys.h"
#include "../frontend/keymap.h"
#include "../frontend/util.h"
#include "../frontend/romid.h"
#include "../frontend/hfs.h"
#include "../core/macnames.h"

using macemu::Machine; using macemu::Osd; using macemu::Config; using macemu::Scsi;
using namespace macemu;
typedef ::u64 hostu64;

#define DRIVE		"SD:"
#define WLAN_FIRMWARE	DRIVE "/firmware/"
#define WPA_CONFIG	DRIVE "/macemu-wifi.conf"	// written from macemu.cfg
#define WPA_USER_CONFIG	DRIVE "/wpa_supplicant.conf"	// or one of your own
#define CONFIG_FILE	DRIVE "/macemu.cfg"
#define PRAM_FILE	DRIVE "/macemu.pram"
#define NETADDR_FILE	DRIVE "/macemu-net.dat"		// the last IP address, for the Mac's Ethernet
#define SHOT_DIR	DRIVE "/screenshots"

static const char FromKernel[] = "macemu";

static const unsigned FrameMicros = 16626;	// 370 lines x 352 cycles at 7.8336 MHz
static const unsigned FloppyFileMax = 1474560 + 84 + 4096;   // up to 1.44 MB (Quadra SuperDrive)

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
	m_pJit (0),
	m_pJitWorker (0),
	m_pShareFS (0),
	m_pShared (0),
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
	m_bUSBSound (FALSE),
	m_bUSBSoundFailed (FALSE),
	m_nSoundFraction (0),
	m_nFrames (0),
	m_nLastActivity (0),
	m_nIdleFrames (0),
	m_nScreenshot (0),
	m_nEmulatedFrames (0),
	m_nEmulatedSeen (0),
	m_pCdrom (0),
#ifdef ARM_ALLOW_MULTI_CORE
	m_pCores (0),
#endif
	m_bEmuCoreRunning (FALSE),
	m_nPauseRequests (0),
	m_nPauseSeq (0),
	m_nPauseAck (0),
	m_nEventIn (0),
	m_nEventOut (0),
	m_nSoundIn (0),
	m_nSoundOut (0),
	m_nSoundRate (22255),
	m_nStatsTime (0),
	m_nSpeedPercent (100),
	m_pNet (0),
	m_pWebServer (0),
	m_pWLAN (0),
	m_pWPASupplicant (0),
	m_bWiFi (FALSE),
	m_bWiFiPending (FALSE),
	m_nNetStartTicks (0),
	m_bPowerKey (FALSE),
	m_nMacPowerKeyTicks (0),
	m_nLoops (0),
	m_nWatchedLoops (0),
	m_nStuckSeconds (0),
	m_bEmergencyShown (FALSE),
	m_bWrongPassword (FALSE),
	m_nLogBytes (0),
	m_bNetRunning (FALSE),
	m_nNetTicks (0),
	m_pEther (0)
{
	s_pThis = this;

	m_pFrameBufferPixels = 0;
	m_nFrameBufferPitch = 0;
	m_bDoubleBuffered = FALSE;
	m_nFront = 0;
	m_nPendTop[0] = m_nPendTop[1] = 0;
	m_nPendBottom[0] = m_nPendBottom[1] = 0;
	m_nLastFlip = 0;
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
	m_SavedFloppy[0][0] = m_SavedFloppy[1][0] = m_SavedCdrom[0] = '\0';
	m_SoundOutput[0] = m_SoundName[0] = '\0';
	for (unsigned n = 0; n < SlotCount; n++) m_SlotInfo[n][0] = '\0';
	m_nMacOffTicks = 0;
	strCopy (m_InfoLine[2], "Built " __DATE__, sizeof m_InfoLine[2]);	// shows which version runs
	strCopy (m_NetLine[0], "Network: off (Connection is set to Off)", sizeof m_NetLine[0]);
	m_NetLine[1][0] = m_NetLine[2][0] = '\0';
	m_ROMName[0] = '\0';
	m_LogTail[0] = '\0';
	m_WpaLast[0] = '\0';
	CUSBPowerKeyDevice::RegisterHandler (PowerKeyHandler);
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
	// Files written whole (WriteFileSafely): finish one the power interrupted.
	static const char *Files[] = {CONFIG_FILE, PRAM_FILE, WPA_CONFIG, DRIVE "/config.txt"};
	for (const char *pFile : Files)
	{
		RecoverFile (pFile);
	}

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
		if (nSize < 0x10000 || nSize > 0x110000)
		{
			m_Logger.Write (FromKernel, LogWarning, "%s: unsupported size %u", pName, nSize);
			f_close (&File);
			continue;
		}

		m_pROM = new u8[nSize];
		UINT nRead = 0;
		FRESULT Result = f_read (&File, m_pROM, nSize, &nRead);
		f_close (&File);
		u32 nImage = Result == FR_OK && nRead == nSize ? romImageSize (m_pROM, nSize) : 0;
		if (nImage == 0)
		{
			m_Logger.Write (FromKernel, LogWarning, "%s: not a ROM image (size %u)", pName, nSize);
			delete [] m_pROM;
			m_pROM = 0;
			continue;
		}
		nSize = nImage;		// extra bytes after the ROM are ignored

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
	if (   (nWidth == 0 || nHeight == 0) && m_Config.sharp == 2
	    && m_nDisplayWidth >= 2 * (unsigned) nMacWidth && m_nDisplayHeight >= 2 * (unsigned) nMacHeight)
	{
		// Scanlines: drawn at the display's own size, two or more screen
		// lines a Mac row, the last of them dimmed.
		nWidth = m_nDisplayWidth;
		nHeight = m_nDisplayHeight;
	}
	if ((nWidth == 0 || nHeight == 0) && m_Config.sharp)
	{
		// Sharp: the display is a whole multiple of the frame buffer, and
		// the firmware scales without smoothing (scaling_kernel=8), so each
		// Mac pixel becomes an exact square of screen pixels.
		unsigned nFactor = m_nDisplayWidth / (unsigned) nMacWidth;
		unsigned nFactorY = m_nDisplayHeight / (unsigned) (nMacHeight < 384 ? 384 : nMacHeight);
		if (nFactorY < nFactor) nFactor = nFactorY;
		if (nFactor < 1) nFactor = 1;
		nWidth = m_nDisplayWidth / nFactor;
		nHeight = m_nDisplayHeight / nFactor;
	}
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

	// Two screens high, for flipping; one if the firmware cannot.
	m_bDoubleBuffered = TRUE;
	m_pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, 32, nWidth, 2 * nHeight);
	if (!m_pFrameBuffer->Initialize () || m_pFrameBuffer->GetVirtHeight () < 2 * nHeight)
	{
		delete m_pFrameBuffer;
		m_bDoubleBuffered = FALSE;
		m_pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, 32);
		if (!m_pFrameBuffer->Initialize ())
		{
			return FALSE;
		}
	}
	m_nFront = 0;
	m_Logger.Write (FromKernel, LogNotice, "Frame buffer %ux%u for a %dx%d Mac screen on a %ux%u display%s",
			nWidth, nHeight, nMacWidth, nMacHeight, m_nDisplayWidth, m_nDisplayHeight,
			m_bDoubleBuffered ? ", switched at each refresh" : " (single)");

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
	m_nPendTop[0] = m_nPendTop[1] = 0;
	m_nPendBottom[0] = m_nPendBottom[1] = m_Surface.height;
	if (m_bDoubleBuffered)
	{
		m_pFrameBuffer->SetVirtualOffset (0, 0);
	}

	// Pixel order: 1 = RGB in memory (0xBBGGRR as a word), 0 = BGR.
	CBcmPropertyTags Tags;
	TPropertyTagSimple Order;
	m_Surface.swapRB = Tags.GetTag (0x00040006, &Order, sizeof Order) && Order.nValue == 1;
	ApplyPalette ();

	fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
	m_Layout = layoutFor (m_Surface, nMacWidth, nMacHeight, m_Config.scale);
	m_Layout.scanlines = m_Config.sharp == 2;
	m_bForceRedraw = TRUE;

	return TRUE;
}

void CKernel::ApplyPalette (void)
{
	setPalette (m_Surface, m_Config.palette);
	m_bForceRedraw = TRUE;
}

// The sound output: "auto" is a USB audio device when one is plugged in
// (Pi 4, 400, 5: speakers, a headset, a sound stick), otherwise HDMI;
// "hdmi", "pwm" (the headphone jack) and "usb" choose one.
boolean CKernel::WantUSBSound (void) const
{
#if RASPPI >= 4
	return strEqNoCase (m_Config.soundDevice, "auto") || strEqNoCase (m_Config.soundDevice, "usb");
#else
	return FALSE;
#endif
}

void CKernel::SetupSound (void)
{
	StopSound ();
	strCopy (m_SoundOutput, m_Config.soundDevice, sizeof m_SoundOutput);

#if RASPPI >= 4
	if (WantUSBSound () && m_DeviceNameService.GetDevice ("uaudio1-1", FALSE) != 0)
	{
		static const unsigned Rates[] = {48000, 44100};
		for (unsigned nRate : Rates)
		{
			m_nSampleRate = nRate;
			m_pSound = new CUSBSoundBaseDevice (nRate);
			if (StartSound ())
			{
				m_bUSBSound = TRUE;
				m_bUSBSoundFailed = FALSE;
				strCopy (m_SoundName, "USB", sizeof m_SoundName);
				m_Logger.Write (FromKernel, LogNotice, "Sound: USB audio device, %u Hz", nRate);
				return;
			}
		}
		m_bUSBSoundFailed = TRUE;
		m_Logger.Write (FromKernel, LogWarning, "Sound: the USB audio device does not start");
	}
	if (strEqNoCase (m_Config.soundDevice, "usb"))
	{
		strCopy (m_SoundName, "USB (none plugged in)", sizeof m_SoundName);
		return;					// USB only: wait for one
	}
#endif

	boolean bPWM = strEqNoCase (m_Config.soundDevice, "pwm");
#if RASPPI >= 5
	bPWM = TRUE;					// (no HDMI sound driver here)
#endif
	if (bPWM)
	{
		m_nSampleRate = 44100;
		m_pSound = new CPWMSoundBaseDevice (&m_Interrupt, m_nSampleRate, 2048);
	}
	else
	{
		m_nSampleRate = 48000;
		m_pSound = new CHDMISoundBaseDevice (&m_Interrupt, m_nSampleRate, 384 * 10);
	}
	if (StartSound ())
	{
		strCopy (m_SoundName, bPWM ? "headphone jack" : "HDMI", sizeof m_SoundName);
	}
	else
	{
		strCopy (m_SoundName, "none", sizeof m_SoundName);
		m_Logger.Write (FromKernel, LogWarning, "Sound device not available");
	}
}

boolean CKernel::StartSound (void)
{
	if (   m_pSound->AllocateQueue (100)
	    && (m_pSound->SetWriteFormat (SoundFormatSigned16, 2), m_pSound->Start ()))
	{
		return TRUE;
	}
	delete m_pSound;
	m_pSound = 0;
	return FALSE;
}

void CKernel::StopSound (void)
{
	if (m_pSound == 0)
	{
		return;
	}
	m_pSound->Cancel ();
	for (unsigned i = 0; i < 50 && m_pSound->IsActive (); i++)
	{
		m_Timer.MsDelay (10);
	}
	delete m_pSound;
	m_pSound = 0;
	m_bUSBSound = FALSE;
	m_SoundName[0] = '\0';
}

void CKernel::AttachDisks (void)
{
	// As macemu.cfg has them (RememberDisks keeps it up to date).
	strCopy (m_SavedFloppy[0], m_Config.floppy[0], sizeof m_SavedFloppy[0]);
	strCopy (m_SavedFloppy[1], m_Config.floppy[1], sizeof m_SavedFloppy[1]);
	strCopy (m_SavedCdrom, m_Config.cdrom, sizeof m_SavedCdrom);

	for (unsigned d = 0; d < 2; d++)
	{
		if (m_Config.floppy[d][0] != '\0' && !insertDisk ((int) d, m_Config.floppy[d]))
		{
			m_Logger.Write (FromKernel, LogWarning, "Cannot insert %s", m_Config.floppy[d]);
		}
	}

	AttachHardDisks ();

	if (m_Config.cdromId >= 0)
	{
		if (m_pHardDisk[m_Config.cdromId] != 0)
		{
			m_Logger.Write (FromKernel, LogWarning, "SCSI ID %d: the CD-ROM drive replaces the hard disk",
					m_Config.cdromId);
		}
		if (m_Config.cdrom[0] == '\0' || !insertCd (m_Config.cdrom))
		{
			m_pMachine->insertCdrom (m_Config.cdromId, 0);	// an empty drive
		}
	}
}

// Put a CD-ROM image in the drive (null or "": eject).  Names from the menu
// are relative to the card's root.
bool CKernel::insertCd (const char *pName)
{
	if (m_Config.cdromId < 0)
	{
		return false;
	}

	PauseEmu ();
	m_pMachine->insertCdrom (m_Config.cdromId, 0);
	delete m_pCdrom;
	m_pCdrom = 0;

	boolean bOK = TRUE;
	if (pName != 0 && pName[0] != '\0')
	{
		char Path[160] = DRIVE "/";
		strAppend (Path, pName, sizeof Path);
		CFileStorage *pDisc = new CFileStorage;
		if (pDisc->Open (Path) && pDisc->size () >= 2048)
		{
			m_pCdrom = pDisc;
			if (pName != m_Config.cdrom)
			{
				strCopy (m_Config.cdrom, pName, Config::kPath);
			}
			m_pMachine->insertCdrom (m_Config.cdromId, pDisc);
			m_Logger.Write (FromKernel, LogNotice, "CD-ROM (SCSI ID %d): %s (%u MB)", m_Config.cdromId,
					Path, pDisc->size () >> 20);
		}
		else
		{
			delete pDisc;
			bOK = FALSE;
		}
	}
	if (m_pCdrom == 0)
	{
		m_Config.cdrom[0] = '\0';
	}

	ResumeEmu ();
	return bOK;
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

// The Mac's own Ethernet (the Quadra's), carried over the Pi's network.
void CKernel::StartMacEthernet (void)
{
	if ((m_Config.macEthernet == 0 && !m_Config.macInternet) || strEqNoCase (m_Config.network, "off"))
	{
		return;
	}
	m_pEther = new CEtherTunnel;
	if (m_pEther == 0)
	{
		return;
	}
	if (!m_pMachine->attachEthernet (m_pEther))
	{
		delete m_pEther;		// this model has no Ethernet
		m_pEther = 0;
		return;
	}

	FIL File;
	if (f_open (&File, NETADDR_FILE, FA_READ | FA_OPEN_EXISTING) == FR_OK)
	{
		u8 IP[4];
		UINT nRead;
		if (f_read (&File, IP, sizeof IP, &nRead) == FR_OK && nRead == sizeof IP)
		{
			m_pEther->SetLastIP (IP);
		}
		f_close (&File);
	}
}

// The root certificates the HTTPS helper trusts (Mozilla's list, from curl).
void CKernel::LoadRootCertificates (void)
{
	FIL File;
	if (f_open (&File, DRIVE "/certs/cacert.pem", FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		m_Logger.Write (FromKernel, LogWarning, "HTTPS helper: certs/cacert.pem is missing");
		return;
	}
	unsigned nSize = (unsigned) f_size (&File);
	char *pPEM = new char[nSize + 1];
	UINT nRead = 0;
	if (pPEM != 0 && f_read (&File, pPEM, nSize, &nRead) == FR_OK && nRead == nSize)
	{
		unsigned nRoots = m_pEther->LoadRoots (pPEM, nSize);
		m_Logger.Write (FromKernel, LogNotice, "HTTPS helper: %u root certificates", nRoots);
	}
	f_close (&File);
	delete [] pPEM;
}

void CKernel::SavePRAM (void)
{
	if (!m_pMachine->clockChip ().pramDirty)
	{
		return;
	}

	if (WriteFileSafely (PRAM_FILE, m_pMachine->clockChip ().pram, sizeof m_pMachine->clockChip ().pram))
	{
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

	// Pick the model from the ROM: the compact Macs (128K to Classic), the
	// Mac II, and the Quadra 610/650/800 and Centris 610/650.
	const RomInfo &Rom = romIdentify (m_pROM, m_nROMSize);
	m_Logger.Write (FromKernel, LogNotice, "ROM is a %s", Rom.name);
	boolean bAuto = strEqNoCase (m_Config.machine, "auto");
	boolean bCompact = bAuto && (Rom.family == RomCompact || Rom.family == RomSE);
	boolean bQuadra = strEqNoCase (m_Config.machine, "quadra")
			  || (bAuto && !bCompact && m_nROMSize == 0x100000);
	boolean bMacII = !bQuadra && !bCompact
			 && (strEqNoCase (m_Config.machine, "ii")
			     || (!strEqNoCase (m_Config.machine, "plus") && m_nROMSize >= 0x40000));
	if (Rom.family == RomNotYet && !strEqNoCase (m_Config.machine, "plus"))
	{
		static char Line[64];
		strCopy (Line, "This ROM is from a ", sizeof Line);
		strAppend (Line, Rom.name, sizeof Line);
		Fatal (Line, "which is not emulated yet. Use a Quadra 650/800,",
		       "Centris 650, Mac II, Classic, SE or Plus ROM.");
	}
	if (bQuadra)
	{
		if (m_nROMSize != 0x100000 || Rom.family != RomQuadra)
		{
			Fatal ("The Quadra emulation needs the 1 MB ROM of the",
			       "Quadra 610/650/800 or Centris 610/650",
			       "(checksum F1ACAD13 or F1A6F343).");
		}
		if (m_Config.ramKB < 8192)
		{
			m_Config.ramKB = 8192;
		}
		m_pQuadra = new Quadra (strEq (m_Config.monitor, "21") ? Dafb::Apple21 : Dafb::VGA);
	}
	else if (bMacII)
	{
		if (m_nROMSize != 0x40000 || Rom.family == RomSE)
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
	m_pPrevScreen = new u8[kPreviousFrameBytes];
	boolean bOK = FALSE;
	if (m_pRAM != 0 && m_pPrevScreen != 0)
	{
		if (m_pQuadra != 0)
		{
			bOK = m_pQuadra->init (m_pROM, m_nROMSize, m_pRAM, m_Config.ramKB * 1024);
			m_pMachine = m_pQuadra;
			if (bOK)
			{
				UpdateJit ();
				StartSharedFolder ();
			}
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
	ResumeHibernation ();
	RefreshSlots ();
	StartMacEthernet ();

	m_pOsd = new Osd (m_Config, *this);
	StartNetwork ();

	if (m_Config.sound)
	{
		SetupSound ();
	}

	UpdateInfo ();
	m_Logger.Write (FromKernel, LogNotice, "Running: %uK RAM, display %ux%u, zoom %d",
			m_Config.ramKB, m_Surface.width, m_Surface.height, m_Layout.scale);

	m_Logger.RegisterPanicHandler (PanicHandler);
	m_Timer.StartKernelTimer (HZ, WatchdogTimer, 0, this);

#ifdef ARM_ALLOW_MULTI_CORE
	// The Mac gets core 1 to itself; this core keeps USB, the network, the
	// screen, sound and the SD card.
	m_pCores = new CEmuCores (this);
	if (m_pCores == 0 || !m_pCores->Initialize ())
	{
		m_Logger.Write (FromKernel, LogWarning, "Cannot start the other cores: running on one");
	}
	else
	{
		hostu64 nStart = CTimer::GetClockTicks64 ();
		while (!m_bEmuCoreRunning && CTimer::GetClockTicks64 () - nStart < 1000000)
		{
		}
		m_Logger.Write (FromKernel, LogNotice, m_bEmuCoreRunning ? "The Mac runs on core 1"
								     : "Core 1 did not start: running on one");
		StartJitCore ();
	}
#endif

	hostu64 nNextFrame = CTimer::GetClockTicks64 ();
	m_nStatsTime = nNextFrame;

	boolean bMenuWasOpen = FALSE;
	while (1)
	{
		m_nLoops++;
		UpdateUSB ();
		UpdateNetwork ();
		ProcessInput ();

#if RASPPI >= 5
		if (is_power_button_pressed ())
		{
			m_bPowerKey = TRUE;
		}
#endif
		if (m_bPowerKey)
		{
			m_bPowerKey = FALSE;
			HandlePowerKey ();
		}

		// The menu was closed (Esc, F12, Resume, Restart ...): repaint the
		// whole Mac screen over it.
		if (!bMenuWasOpen && m_pOsd->active ())
		{
			PauseEmu ();			// the Mac waits while the menu is open
		}
		if (bMenuWasOpen && !m_pOsd->active ())
		{
			UpdateJit ();			// switched in the menu: from now on
			ResumeEmu ();
			fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
			m_bForceRedraw = TRUE;
			DrawMac (TRUE);
			Present ();
		}
		bMenuWasOpen = m_pOsd->active ();

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

			BootLog ();
			WaitUntil (CTimer::GetClockTicks64 () + 10000);
			nNextFrame = CTimer::GetClockTicks64 ();
			continue;
		}

		if (m_bEmuCoreRunning)
		{
			// The Mac runs on its own core: this one just draws, plays the
			// sound and serves the SD card at the display's pace.
			nNextFrame += FrameMicros;
			hostu64 nNow = CTimer::GetClockTicks64 ();
			if (nNow + 4 * FrameMicros < nNextFrame || nNow > nNextFrame + 4 * FrameMicros)
			{
				nNextFrame = nNow;
			}
			WaitUntil (nNextFrame);
		}
		else if (nSpeed == 0)
		{
			// Unlimited: emulate for one display frame's worth of real time.
			hostu64 nUntil = CTimer::GetClockTicks64 () + FrameMicros;
			do
			{
				ExecuteFrame ();
				m_Scheduler.Yield ();		// let the network run
			}
			while (CTimer::GetClockTicks64 () < nUntil);

			nNextFrame = CTimer::GetClockTicks64 ();
		}
		else
		{
			for (unsigned i = 0; i < nSpeed; i++)
			{
				ExecuteFrame ();
			}

			nNextFrame += FrameMicros;
			hostu64 nNow = CTimer::GetClockTicks64 ();
			if (nNow + 4 * FrameMicros < nNextFrame || nNow > nNextFrame + 4 * FrameMicros)
			{
				nNextFrame = nNow;	// fell far behind (or clock jumped): resync
			}

			WaitUntil (nNextFrame);			// the network runs meanwhile
		}

		// While the network is connecting, give its tasks a few ms each
		// frame: Wi-Fi's WPA handshake must answer the router in time.
		if (m_pNet != 0 && !m_bNetRunning && m_Timer.GetTicks () - m_nNetStartTicks < 300 * HZ)
		{
			WaitUntil (CTimer::GetClockTicks64 () + 3000);
		}

		Housekeeping ();
		ReportSharedFolder ();
		QueueSound ();
		UpdateScreen ();
		Present (TRUE);
		m_CPUThrottle.Update ();
		BootLog ();
		m_nFrames++;

		if (m_pMachine->poweredOff ())
		{
			// Shut Down.  The Mac tried to turn its power off; it keeps
			// running and, as a real one does when the power stays on,
			// shows "It is now safe to switch off your Macintosh." with a
			// Restart button.  Save everything now.  With a power button
			// (Pi 400, Pi 5) the Pi then turns off, like Raspberry Pi OS,
			// leaving that screen up until the power goes.
			m_pMachine->clearPowerOff ();
			if (m_nMacOffTicks == 0)
			{
				m_Logger.Write (FromKernel, LogNotice, "The Mac shut down");
				m_nMacOffTicks = m_Timer.GetTicks () | 1;
			}
			PauseEmu ();
			for (unsigned d = 0; d < 2; d++)
			{
				if (m_pFloppy[d] != 0)
				{
					m_pFloppy[d]->flush ();
				}
			}
			m_pMachine->flushDisks ();
			SavePRAM ();
			ResumeEmu ();
		}
		if (   m_nMacOffTicks != 0 && HasPowerButton () && !m_pOsd->active ()
		    && m_Timer.GetTicks () - m_nMacOffTicks > 4 * HZ)
		{
			TurnOff (FALSE);
		}

		hostu64 nNow = CTimer::GetClockTicks64 ();
		if (nNow - m_nStatsTime >= 2000000)
		{
			unsigned nFrames = m_nEmulatedFrames;
			m_nSpeedPercent = (unsigned) ((hostu64) (nFrames - m_nEmulatedSeen) * FrameMicros * 100
						      / (nNow - m_nStatsTime));
			m_nEmulatedSeen = nFrames;
			m_nStatsTime = nNow;
			UpdateInfo ();
			RememberDisks ();
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

void CKernel::Present (boolean bFrame)
{
	if (m_pFrameBufferPixels == 0 || m_Surface.pixels == 0)
	{
		return;
	}
	int nTop = m_Surface.dirtyTop, nBottom = m_Surface.dirtyBottom;
	if (nTop < 0) nTop = 0;
	if (nBottom > m_Surface.height) nBottom = m_Surface.height;
	m_Surface.dirtyTop = m_Surface.height;
	m_Surface.dirtyBottom = 0;

	if (!m_bDoubleBuffered)
	{
		CopyRows (m_pFrameBufferPixels, nTop, nBottom);
		return;
	}

	// Both halves now lack these lines; the hidden one gets them (and what
	// it lacked from last time), then is shown.
	for (unsigned h = 0; h < 2; h++)
	{
		if (nTop < nBottom)
		{
			if (m_nPendTop[h] >= m_nPendBottom[h])
			{
				m_nPendTop[h] = nTop;
				m_nPendBottom[h] = nBottom;
			}
			else
			{
				if (nTop < m_nPendTop[h]) m_nPendTop[h] = nTop;
				if (nBottom > m_nPendBottom[h]) m_nPendBottom[h] = nBottom;
			}
		}
	}
	unsigned nBack = 1 - m_nFront;
	if (m_nPendTop[nBack] >= m_nPendBottom[nBack])
	{
		return;					// nothing new: no need to switch
	}
	// The hidden half may still be on the display if it was switched away
	// from only just now (the switch happens at the display's next refresh).
	// Frames come every 16.6 ms: then a refresh has been.  Closer than half
	// a refresh, the regular frame leaves it to the next (core 0 must not
	// wait: it serves the Mac's disk and network); other screens wait.
	hostu64 nNow = CTimer::GetClockTicks64 ();
	if (nNow - m_nLastFlip < 8000)
	{
		if (bFrame)
		{
			return;				// (its lines stay pending)
		}
		m_pFrameBuffer->WaitForVerticalSync ();
	}
	CopyRows (m_pFrameBufferPixels + nBack * m_Surface.height * m_nFrameBufferPitch,
		  m_nPendTop[nBack], m_nPendBottom[nBack]);
	m_nPendTop[nBack] = m_Surface.height;
	m_nPendBottom[nBack] = 0;
	m_pFrameBuffer->SetVirtualOffset (0, nBack * (unsigned) m_Surface.height);
	m_nFront = nBack;
	m_nLastFlip = CTimer::GetClockTicks64 ();
}

void CKernel::CopyRows (volatile u32 *pBuffer, int nTop, int nBottom)
{
	for (int y = nTop; y < nBottom; y++)
	{
		const macemu::u32 *pSrc = m_Surface.pixels + y * m_Surface.pitch;
		volatile u32 *pDst = pBuffer + y * m_nFrameBufferPitch;
		for (int x = 0; x < m_Surface.width; x++)
		{
			pDst[x] = pSrc[x];
		}
	}
}

// For the first three minutes, note progress in SD:/macemu.log so that a
// problem on real hardware can be diagnosed.
void CKernel::BootLog (boolean bForce)
{
	// Everything Circle logs (USB, Wi-Fi driver, WPA, DHCP ...) goes to
	// macemu.log on the card every two seconds, plus a status line every
	// five seconds for the first three minutes (then every minute).
	static unsigned nLastSecond = 0;
	unsigned nSecond = m_Timer.GetUptime ();
	if (!bForce && (nSecond == nLastSecond || nSecond % 2 != 0))
	{
		return;
	}
	nLastSecond = nSecond;

	if (!bForce && nSecond % 5 == 0 && (nSecond <= 180 || nSecond % 60 == 0))
	{
		char Line[160] = "";
		appendUint (Line, nSecond, sizeof Line);
		strAppend (Line, "s frames=", sizeof Line);
		appendUint (Line, m_nFrames, sizeof Line);
		strAppend (Line, " speed=", sizeof Line);
		appendUint (Line, m_nSpeedPercent, sizeof Line);
		strAppend (Line, "% pc=", sizeof Line);
		appendHex (Line, m_pMachine->debugPC (), 8, sizeof Line);
		strAppend (Line, " trap=", sizeof Line);
		appendHex (Line, m_pMachine->debugTrap (), 4, sizeof Line);
		strAppend (Line, " disk=", sizeof Line);
		appendUint (Line, m_pMachine->diskActivity (), sizeof Line);
		strAppend (Line, " clock=", sizeof Line);
		appendUint (Line, m_CPUThrottle.GetClockRate () / 1000000, sizeof Line);
		strAppend (Line, "MHz temp=", sizeof Line);
		appendUint (Line, m_CPUThrottle.GetTemperature (), sizeof Line);
		strAppend (Line, "C", sizeof Line);
		if (m_pShared != 0)
		{
			strAppend (Line, m_pShared->mounted ? " share=up/" : " share=down/", sizeof Line);
			appendUint (Line, m_pShared->callsServed, sizeof Line);
		}
		if (m_pJit != 0)
		{
			// Blocks made, % of instructions run in blocks, flushes.
			macemu::u64 nAll = m_pJit->jitInstructions + m_pJit->interpreted;
			strAppend (Line, " jit=", sizeof Line);
			appendUint (Line, m_pJit->blocks, sizeof Line);
			strAppend (Line, "/", sizeof Line);
			appendUint (Line, nAll ? (unsigned) (m_pJit->jitInstructions * 100 / nAll) : 0, sizeof Line);
			strAppend (Line, "%/", sizeof Line);
			appendUint (Line, m_pJit->flushes, sizeof Line);
			if (m_pJitWorker != 0)
			{
				// Core 3: blocks queued, dropped (queue full), stale (code changed).
				strAppend (Line, " core3=", sizeof Line);
				appendUint (Line, m_pJit->queued, sizeof Line);
				strAppend (Line, "/", sizeof Line);
				appendUint (Line, m_pJit->dropped, sizeof Line);
				strAppend (Line, "/", sizeof Line);
				appendUint (Line, m_pJit->stale, sizeof Line);
			}
		}
		if (m_bWiFi)
		{
			strAppend (Line, " wifi=", sizeof Line);
			strAppend (Line, m_pWLAN == 0 ? "off" : m_pWLAN->IsLinkUp () ? "link" : "nolink", sizeof Line);
			strAppend (Line, CWPASupplicant::IsConnected () ? ",wpa" : ",nowpa", sizeof Line);
		}
		strAppend (Line, m_bNetRunning ? " ip=yes" : " ip=no", sizeof Line);
		if (m_pEther != 0 && m_pEther->Running ())
		{
			strAppend (Line, " eth=", sizeof Line);	// the Mac's frames out/in
			appendUint (Line, m_pEther->Sent (), sizeof Line);
			strAppend (Line, "/", sizeof Line);
			appendUint (Line, m_pEther->Received (), sizeof Line);
			if (m_pEther->Internet ())
			{
				strAppend (Line, " tcp=", sizeof Line);	// the Mac's internet connections
				appendUint (Line, m_pEther->Connections (), sizeof Line);
			}
			if (m_pEther->Proxy () != 0)
			{
				strAppend (Line, " https=", sizeof Line);	// pages fetched / certificates refused
				appendUint (Line, m_pEther->Proxy ()->pagesFetched, sizeof Line);
				strAppend (Line, "/", sizeof Line);
				appendUint (Line, m_pEther->Proxy ()->certificateErrors, sizeof Line);
			}
		}
		m_Logger.Write (FromKernel, LogNotice, "%s", Line);
	}

	static char *Text = 0;				// (on the heap: the kernel image is limited)
	if (Text == 0) Text = new char[LOGGER_BUFSIZE + 1];
	int nLength = m_Logger.Read (Text, LOGGER_BUFSIZE);
	if (nLength <= 0 || m_nLogBytes > 2 * 1024 * 1024)
	{
		return;
	}
	Text[nLength] = '\0';

	// The WPA supplicant says so when the password is wrong; its latest
	// message goes on the menu's Network page.
	for (const char *p = Text; *p; p++)
	{
		if (*p == 'm' && strncmp (p, "may be incorrect", 16) == 0)
		{
			m_bWrongPassword = TRUE;
		}
		if (*p == 'w' && strncmp (p, "wpa: ", 5) == 0)
		{
			const char *q = p + 5;
			unsigned n = 0;
			while (q[n] != '\0' && q[n] != '\n' && q[n] != '\r' && n + 1 < sizeof m_WpaLast)
			{
				n++;
			}
			memcpy (m_WpaLast, q, n);
			m_WpaLast[n] = '\0';
		}
	}

	unsigned nKeep = (unsigned) nLength < sizeof m_LogTail - 1 ? (unsigned) nLength : sizeof m_LogTail - 1;
	unsigned nOld = strLen (m_LogTail);
	if (nOld + nKeep >= sizeof m_LogTail)
	{
		unsigned nDrop = nOld + nKeep - (sizeof m_LogTail - 1);
		memmove (m_LogTail, m_LogTail + nDrop, nOld - nDrop + 1);
		nOld -= nDrop;
	}
	memcpy (m_LogTail + nOld, Text + nLength - nKeep, nKeep);
	m_LogTail[nOld + nKeep] = '\0';

	FIL File;
	BYTE Mode = m_nLogBytes == 0 ? (FA_WRITE | FA_CREATE_ALWAYS) : (FA_WRITE | FA_OPEN_APPEND);
	if (f_open (&File, DRIVE "/macemu.log", Mode) == FR_OK)
	{
		UINT nWritten;
		f_write (&File, Text, (UINT) nLength, &nWritten);
		f_close (&File);
		m_nLogBytes += (unsigned) nLength;
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
		m_Layout.scanlines = m_Config.sharp == 2;
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
	// Take what the Mac produced since last time (370 samples a frame at
	// 22254.5 Hz) and resample it to the device rate.
	static s16 In[4096];
	unsigned nInCount = 0;
	unsigned nOutPos = m_nSoundOut;
	DataMemBarrier ();
	while (nOutPos != m_nSoundIn && nInCount < sizeof In / sizeof In[0])
	{
		In[nInCount++] = m_SoundRing[nOutPos];
		nOutPos = (nOutPos + 1) % SoundRingSize;
	}
	DataMemBarrier ();
	m_nSoundOut = nOutPos;

	if (m_pSound == 0 || !m_Config.sound || nInCount == 0)
	{
		return;
	}

	static s16 Buffer[2 * 2400];
	unsigned nInRate = m_nSoundRate;
	unsigned nOut = 0;
	int nVolume = m_Config.volume;
	while (nOut < 2400)
	{
		unsigned nIndex = m_nSoundFraction / m_nSampleRate;
		if (nIndex >= nInCount)
		{
			m_nSoundFraction -= nInCount * m_nSampleRate;
			break;
		}

		s16 nSample = (s16) (In[nIndex] * nVolume / 10);
		Buffer[2 * nOut] = nSample;
		Buffer[2 * nOut + 1] = nSample;
		nOut++;
		m_nSoundFraction += nInRate;
	}
	if (nOut == 2400)
	{
		m_nSoundFraction = 0;
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

// One emulated frame: the input that arrived first, then the Mac, then its
// sound (only at normal speed) into the ring for core 0.
void CKernel::ExecuteFrame (void)
{
	DrainEvents ();
	m_pMachine->runFrame ();
	m_nEmulatedFrames++;

	unsigned nSpeed = m_bTurbo ? 0 : (unsigned) m_Config.speed;
	if (nSpeed == 1)
	{
		u32 nCount, nRate;
		const s16 *pIn = m_pMachine->sound (nCount, nRate);
		m_nSoundRate = nRate;
		unsigned nIn = m_nSoundIn;
		for (u32 i = 0; i < nCount; i++)
		{
			unsigned nNext = (nIn + 1) % SoundRingSize;
			if (nNext == m_nSoundOut)
			{
				break;			// core 0 is behind: drop
			}
			m_SoundRing[nIn] = pIn[i];
			nIn = nNext;
		}
		DataMemBarrier ();
		m_nSoundIn = nIn;
	}
}

void CKernel::PostEvent (TEmuEvent Type, int a, int b)
{
	unsigned nNext = (m_nEventIn + 1) % EventQueueSize;
	if (nNext == m_nEventOut)
	{
		return;				// full: drop
	}
	TEvent &E = m_Events[m_nEventIn];
	E.nType = (u8) Type;
	E.a = (s16) a;
	E.b = (s16) b;
	DataMemBarrier ();
	m_nEventIn = nNext;
}

void CKernel::DrainEvents (void)
{
	while (m_nEventOut != m_nEventIn)
	{
		DataMemBarrier ();
		const TEvent &E = m_Events[m_nEventOut];
		switch (E.nType)
		{
		case EvKey:		m_pMachine->keyEvent (E.a, E.b != 0);		break;
		case EvMouseMove:
			if (m_Config.directMouse)
			{
				m_pMachine->mouseMoveDirect (E.a, E.b);	// (the ADB mouse if Mac OS is not up)
			}
			else
			{
				m_pMachine->mouseMove (E.a, E.b);
			}
			break;
		case EvMouseButton:	m_pMachine->mouseButton (E.a != 0);		break;
		default:		m_pMachine->interruptButton ();			break;
		}
		DataMemBarrier ();
		m_nEventOut = (m_nEventOut + 1) % EventQueueSize;
	}
}

// Stop the Mac's core between frames, so that this core may change the
// machine (reset, disks, PRAM).  Nests; ResumeEmu() releases it.
void CKernel::PauseEmu (void)
{
	if (!m_bEmuCoreRunning)
	{
		return;
	}
	m_nPauseRequests++;
	unsigned nSeq = ++m_nPauseSeq;
	DataMemBarrier ();
	while (m_nPauseAck != nSeq)
	{
		CDiskProxy::Service ();		// it may be waiting for the disk
	}
	DataMemBarrier ();
}

void CKernel::ResumeEmu (void)
{
	if (!m_bEmuCoreRunning || m_nPauseRequests == 0)
	{
		return;
	}
	DataMemBarrier ();
	m_nPauseRequests--;
	DataMemBarrier ();
}

// Core 1: the Mac, and nothing else.  Paces itself to the chosen speed.
void CKernel::EmuCoreRun (void)
{
	m_bEmuCoreRunning = TRUE;
	DataMemBarrier ();
	hostu64 nNext = CTimer::GetClockTicks64 ();
	while (1)
	{
		if (m_nPauseRequests != 0)
		{
			do
			{
				m_nPauseAck = m_nPauseSeq;
				DataMemBarrier ();
			}
			while (m_nPauseRequests != 0);
			nNext = CTimer::GetClockTicks64 ();
			continue;
		}

		if (m_pMachine->poweredOff ())
		{
			nNext = CTimer::GetClockTicks64 () + 1000;
			while (CTimer::GetClockTicks64 () < nNext && m_nPauseRequests == 0)
			{
			}
			continue;
		}

		unsigned nSpeed = m_bTurbo ? 0 : (unsigned) m_Config.speed;
		if (nSpeed == 0)
		{
			ExecuteFrame ();
			nNext = CTimer::GetClockTicks64 ();
			continue;
		}

		for (unsigned i = 0; i < nSpeed && m_nPauseRequests == 0; i++)
		{
			ExecuteFrame ();
		}

		nNext += FrameMicros;
		hostu64 nNow = CTimer::GetClockTicks64 ();
		if (nNow + 4 * FrameMicros < nNext || nNow > nNext + 4 * FrameMicros)
		{
			nNext = nNow;
		}
		while (CTimer::GetClockTicks64 () < nNext && m_nPauseRequests == 0)
		{
		}
	}
}

#ifdef ARM_ALLOW_MULTI_CORE
void CEmuCores::Run (unsigned nCore)
{
	if (nCore == 1)
	{
		m_pKernel->EmuCoreRun ();
	}
	else if (nCore == 3)
	{
		m_pKernel->JitCoreRun ();
	}
	// core 2: nothing to do yet
}
#endif

void CKernel::Housekeeping (void)
{
	// Disks the Mac ejected by itself.
	for (unsigned d = 0; d < 2; d++)
	{
		if (m_pMachine->floppy ().ejectedByGuest[d])
		{
			PauseEmu ();
			m_pMachine->floppy ().ejectedByGuest[d] = false;
			m_Logger.Write (FromKernel, LogNotice, "The Mac ejected floppy %u", d + 1);
			ejectDisk ((int) d);
			ResumeEmu ();
		}
	}

	// The Mac ejected the CD.
	if (m_pCdrom != 0 && m_Config.cdromId >= 0 && m_pMachine->scsiDevice (m_Config.cdromId).ejected)
	{
		m_Logger.Write (FromKernel, LogNotice, "The Mac ejected the CD-ROM");
		insertCd (0);
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
		PauseEmu ();
		for (unsigned d = 0; d < 2; d++)
		{
			if (m_pFloppy[d] != 0)
			{
				m_pFloppy[d]->flush ();
			}
		}

		m_pMachine->flushDisks ();
		SavePRAM ();
		ResumeEmu ();
	}
	else if (m_nIdleFrames % 600 == 0)
	{
		PauseEmu ();
		SavePRAM ();
		ResumeEmu ();
	}
}

void CKernel::UpdateInfo (void)
{
	// The Mac: model, memory, speed, the JIT.
	char *p = m_InfoLine[0];
	p[0] = '\0';
	strAppend (p, m_pMachine->modelName (), 64);
	strAppend (p, ", ", 64);
	if (m_Config.ramKB >= 1024)
	{
		appendUint (p, m_Config.ramKB / 1024, 64);
		strAppend (p, " MB", 64);
	}
	else
	{
		appendUint (p, m_Config.ramKB, 64);
		strAppend (p, " KB", 64);
	}
	strAppend (p, ", speed ", 64);
	appendUint (p, m_nSpeedPercent, 64);
	strAppend (p, "%", 64);
	if (m_pQuadra != 0 && m_pQuadra->cpu.jit != 0)
	{
		strAppend (p, ", JIT", 64);
	}

	// Sharing: the shared disk and the web page.
	p = m_InfoLine[1];
	p[0] = '\0';
	if (m_pShared != 0 && m_pShared->wantVolume)
	{
		strAppend (p, m_pShared->mounted ? "Shared disk: on   " : "Shared disk: waiting   ", 64);
	}
	if (m_bNetRunning && m_pNet != 0)
	{
		CString IP;
		m_pNet->GetConfig ()->GetIPAddress ()->Format (&IP);
		strAppend (p, "Web: http://", 64);
		strAppend (p, (const char *) IP, 64);
		strAppend (p, "/", 64);
	}
	else
	{
		strAppend (p, "Network: off", 64);
	}
}

// ----------------------------------------------------------------- input

// ----------------------------------------------------------------- hibernation

// The Mac, frozen to SD:/macemu.state and carried on at the next start:
// the whole Quadra (memory, CPU, every chip) after a header saying what it
// needs to go on.  The disks must be just as they were, so the state is
// used once (deleted as it is read), only by the same build, ROM, memory
// size and hard disks, and the Pi turns off straight after saving it.

#define HIBERNATION_FILE	DRIVE "/macemu.state"

struct THibernationHeader
{
	char	Magic[16];			// "MACEMU-STATE-V02"
	u32	nFormat;			// macemu::kStateFormat
	char	Build[32];			// __DATE__ " " __TIME__ (for the log)
	u32	nROMChecksum;
	u32	nRAMKB;
	char	HardDisk[macemu::Scsi::kTargets][160];
	char	Floppy[2][64];
	char	Cd[160];
};

namespace {

// The state through FatFs, 64 KB at a time.
class CFileState : public macemu::StateIO
{
public:
	CFileState (FIL *pFile, boolean bSave, u8 *pBuffer)
	:	StateIO (bSave), m_pFile (pFile), m_pBuffer (pBuffer), m_nFill (0), m_nPos (0) {}

	void raw (void *p, u32 n) override
	{
		u8 *q = (u8 *) p;
		while (ok && n > 0)
		{
			if (saving ())
			{
				u32 nChunk = n < Size - m_nFill ? n : Size - m_nFill;
				memcpy (m_pBuffer + m_nFill, q, nChunk);
				m_nFill += nChunk;
				q += nChunk;
				n -= nChunk;
				if (m_nFill == Size)
				{
					Flush ();
				}
			}
			else
			{
				if (m_nPos == m_nFill)
				{
					UINT nRead = 0;
					if (f_read (m_pFile, m_pBuffer, Size, &nRead) != FR_OK || nRead == 0)
					{
						ok = false;
						return;
					}
					m_nFill = nRead;
					m_nPos = 0;
				}
				u32 nChunk = n < m_nFill - m_nPos ? n : m_nFill - m_nPos;
				memcpy (q, m_pBuffer + m_nPos, nChunk);
				m_nPos += nChunk;
				q += nChunk;
				n -= nChunk;
			}
		}
	}

	void Flush (void)
	{
		UINT nWritten = 0;
		if (m_nFill > 0 && (f_write (m_pFile, m_pBuffer, m_nFill, &nWritten) != FR_OK || nWritten != m_nFill))
		{
			ok = false;
		}
		m_nFill = 0;
	}

	static const u32 Size = 64 * 1024;

private:
	FIL *m_pFile;
	u8 *m_pBuffer;
	u32 m_nFill, m_nPos;
};

}

static void FillHibernationHeader (THibernationHeader *pHeader, const CKernel *, u32 nROMChecksum, u32 nRAMKB)
{
	memset (pHeader, 0, sizeof *pHeader);
	memcpy (pHeader->Magic, "MACEMU-STATE-V02", 16);
	pHeader->nFormat = macemu::kStateFormat;
	strCopy (pHeader->Build, __DATE__ " " __TIME__, sizeof pHeader->Build);
	pHeader->nROMChecksum = nROMChecksum;
	pHeader->nRAMKB = nRAMKB;
}

boolean CKernel::WriteHibernation (void)
{
	return WriteState (HIBERNATION_FILE);
}

boolean CKernel::WriteState (const char *pPath)
{
	THibernationHeader Header;
	FillHibernationHeader (&Header, this, be32 (m_pROM), m_Config.ramKB);
	for (unsigned id = 0; id < macemu::Scsi::kTargets; id++)
	{
		if (m_pHardDisk[id] != 0)
		{
			strCopy (Header.HardDisk[id], m_HardDiskPath[id], sizeof Header.HardDisk[id]);
		}
	}
	for (unsigned d = 0; d < 2; d++)
	{
		strCopy (Header.Floppy[d], m_FloppyName[d], sizeof Header.Floppy[d]);
	}
	if (m_pCdrom != 0)
	{
		strCopy (Header.Cd, m_Config.cdrom, sizeof Header.Cd);
	}

	FIL File;
	if (f_open (&File, pPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return FALSE;
	}
	u8 *pBuffer = new u8[CFileState::Size];
	CFileState State (&File, TRUE, pBuffer);
	State.raw (&Header, sizeof Header);
	m_pQuadra->state (State);
	State.Flush ();
	boolean bOK = State.ok;
	bOK = f_close (&File) == FR_OK && bOK;
	delete [] pBuffer;
	if (!bOK)
	{
		f_unlink (pPath);
	}
	return bOK;
}

// Why a saved state cannot be loaded into this Mac, or 0.
const char *CKernel::StateMismatch (const void *pHeader)
{
	const THibernationHeader &Header = *(const THibernationHeader *) pHeader;
	THibernationHeader Now;
	FillHibernationHeader (&Now, this, be32 (m_pROM), m_Config.ramKB);
	if (m_pQuadra == 0 || memcmp (Header.Magic, Now.Magic, 16) != 0)
	{
		return "not a saved Quadra";
	}
	if (Header.nFormat != Now.nFormat)
	{
		return "saved by a version of the emulator that saves differently";
	}
	if (Header.nROMChecksum != Now.nROMChecksum)
	{
		return "saved with another ROM";
	}
	if (Header.nRAMKB != Now.nRAMKB)
	{
		return "saved with another memory size (Machine page)";
	}
	return 0;
}

bool CKernel::hibernate (void)
{
	if (m_pQuadra == 0)
	{
		return false;
	}
	m_Logger.Write (FromKernel, LogNotice, "Hibernating");
	PauseEmu ();
	for (unsigned d = 0; d < 2; d++)
	{
		if (m_pFloppy[d] != 0)
		{
			m_pFloppy[d]->flush ();
		}
	}
	m_pMachine->flushDisks ();
	ShowMessage ("Saving the Mac...", "");
	Present ();
	boolean bOK = WriteHibernation ();
	ResumeEmu ();
	if (!bOK)
	{
		m_Logger.Write (FromKernel, LogWarning, "Could not save the Mac");
		return false;
	}
	powerOff ();
	return true;
}

// At start-up, after the disks are attached: carry on from a saved state.
void CKernel::ResumeHibernation (void)
{
	FIL File;
	if (f_open (&File, HIBERNATION_FILE, FA_READ) != FR_OK)
	{
		return;
	}

	THibernationHeader Header;
	UINT nRead = 0;
	const char *pWhy = 0;
	if (f_read (&File, &Header, sizeof Header, &nRead) != FR_OK || nRead != sizeof Header)
	{
		pWhy = "the file is damaged";
	}
	else
	{
		pWhy = StateMismatch (&Header);
	}
	for (unsigned id = 0; pWhy == 0 && id < macemu::Scsi::kTargets; id++)
	{
		const char *pNow = m_pHardDisk[id] != 0 ? m_HardDiskPath[id] : "";
		if (strcmp (Header.HardDisk[id], pNow) != 0)
		{
			pWhy = "the hard disks have changed";
		}
	}

	if (pWhy == 0)
	{
		// The floppies and the CD as they were, then everything else.
		for (unsigned d = 0; d < 2; d++)
		{
			if (m_pFloppy[d] != 0)
			{
				ejectDisk ((int) d);
			}
			if (Header.Floppy[d][0] != '\0')
			{
				insertDisk ((int) d, Header.Floppy[d]);
			}
		}
		if (Header.Cd[0] != '\0')
		{
			insertCd (Header.Cd);
		}

		u8 *pBuffer = new u8[CFileState::Size];
		CFileState State (&File, FALSE, pBuffer);
		m_pQuadra->state (State);
		delete [] pBuffer;
		if (!State.ok)
		{
			pWhy = "the file is damaged";
			m_pMachine->reset ();
		}
	}
	f_close (&File);
	f_unlink (HIBERNATION_FILE);			// once only: the disks change from now on

	if (pWhy != 0)
	{
		m_Logger.Write (FromKernel, LogWarning, "Not resuming the hibernated Mac: %s", pWhy);
		return;
	}
	m_Logger.Write (FromKernel, LogNotice, "Resumed the hibernated Mac");
}

// ----------------------------------------------------------------- saved Macs (slots)

// A slot is a snapshot: the Mac's state and copies of its disks, in
// saved/slotN/ on the card (mac.state, diskN.img, floppyN.dsk, info.txt).
// Restoring one puts the disks back as they were, so the Mac may be used
// in between.  mac.state is written last: without it a slot is empty.

#define SLOT_DIR	DRIVE "/saved"

static void SlotPath (char *pOut, unsigned nMax, unsigned nSlot, const char *pName)
{
	strCopy (pOut, SLOT_DIR "/slot", nMax);
	appendUint (pOut, nSlot + 1, nMax);
	strAppend (pOut, "/", nMax);
	strAppend (pOut, pName, nMax);
}

// Mac time (seconds since 1904) as "13 Nov 2026 16:20".
static void MacDateText (u32 nSeconds, char *pOut, unsigned nMax)
{
	static const char *Months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	u32 nDays = nSeconds / 86400, nSecs = nSeconds % 86400;
	// Days since 1904-01-01 to a date (Howard Hinnant's civil_from_days:
	// days from 0000-03-01; 1904-01-01 is 24107 days before 1970-01-01).
	int z = (int) nDays + 719468 - 24107;
	int era = z / 146097, doe = z - era * 146097;
	int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
	if (m <= 2) y++;
	pOut[0] = '\0';
	appendUint (pOut, (unsigned) d, nMax);
	strAppend (pOut, " ", nMax);
	strAppend (pOut, Months[m - 1], nMax);
	strAppend (pOut, " ", nMax);
	appendUint (pOut, (unsigned) y, nMax);
	strAppend (pOut, " ", nMax);
	appendUint (pOut, nSecs / 3600, nMax, 2);
	strAppend (pOut, ":", nMax);
	appendUint (pOut, nSecs / 60 % 60, nMax, 2);
}

void CKernel::RefreshSlots (void)
{
	for (unsigned n = 0; n < SlotCount; n++)
	{
		m_SlotInfo[n][0] = '\0';
		char Path[64];
		FILINFO Info;
		SlotPath (Path, sizeof Path, n, "mac.state");
		if (f_stat (Path, &Info) != FR_OK)
		{
			continue;
		}
		SlotPath (Path, sizeof Path, n, "info.txt");
		FIL File;
		UINT nRead = 0;
		if (f_open (&File, Path, FA_READ) == FR_OK)
		{
			f_read (&File, m_SlotInfo[n], sizeof m_SlotInfo[n] - 1, &nRead);
			f_close (&File);
		}
		m_SlotInfo[n][nRead] = '\0';
		if (nRead == 0)
		{
			strCopy (m_SlotInfo[n], "saved", sizeof m_SlotInfo[n]);
		}
	}
}

// Copies with progress on the screen; pFrom is a disk the Mac uses (its
// copy in memory makes this fast) or, with pFromPath, a file.
boolean CKernel::CopyOut (macemu::Storage *pFrom, const char *pFromPath, const char *pTo, const char *pTitle,
			  hostu64 &nDone, hostu64 nTotal)
{
	FIL In, Out;
	u32 nSize = 0;
	if (pFrom != 0)
	{
		nSize = pFrom->size ();
	}
	else
	{
		if (f_open (&In, pFromPath, FA_READ) != FR_OK)
		{
			return FALSE;
		}
		nSize = (u32) f_size (&In);
	}
	if (f_open (&Out, pTo, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		if (pFrom == 0) f_close (&In);
		return FALSE;
	}

	static u8 *pBuffer = 0;
	const u32 nChunk = 256 * 1024;
	if (pBuffer == 0)
	{
		pBuffer = new u8[nChunk];
	}
	boolean bOK = TRUE;
	unsigned nLastPercent = 101;
	for (u32 nAt = 0; bOK && nAt < nSize; )
	{
		u32 n = nSize - nAt < nChunk ? nSize - nAt : nChunk;
		UINT nIO = 0;
		bOK = pFrom != 0 ? pFrom->read (nAt, pBuffer, n)
				 : f_read (&In, pBuffer, n, &nIO) == FR_OK && nIO == n;
		bOK = bOK && f_write (&Out, pBuffer, n, &nIO) == FR_OK && nIO == n;
		nAt += n;
		nDone += n;
		unsigned nPercent = nTotal ? (unsigned) (nDone * 100 / nTotal) : 100;
		if (nPercent != nLastPercent)
		{
			nLastPercent = nPercent;
			char Line[40] = "";
			appendUint (Line, nPercent, sizeof Line);
			strAppend (Line, "%", sizeof Line);
			ShowMessage (pTitle, Line);
			Present ();
		}
		CDiskProxy::Service ();
	}
	if (pFrom == 0) f_close (&In);
	bOK = f_close (&Out) == FR_OK && bOK;
	if (!bOK)
	{
		f_unlink (pTo);
	}
	return bOK;
}

static hostu64 FreeBytes (void)
{
	DWORD nClusters = 0;
	FATFS *pFS = 0;
	if (f_getfree (DRIVE, &nClusters, &pFS) != FR_OK)
	{
		return 0;
	}
	return (hostu64) nClusters * pFS->csize * 512;
}

static void ClearSlot (unsigned nSlot)
{
	static const char *Names[] = {"mac.state", "mac.state.new", "info.txt", "floppy1.dsk", "floppy2.dsk",
				      "disk0.img", "disk1.img", "disk2.img", "disk3.img", "disk4.img", "disk5.img", "disk6.img", 0};
	for (unsigned i = 0; Names[i] != 0; i++)
	{
		char Path[64];
		SlotPath (Path, sizeof Path, nSlot, Names[i]);
		f_unlink (Path);
	}
}

bool CKernel::saveSlot (int nSlot)
{
	if (m_pQuadra == 0 || nSlot < 0 || nSlot >= (int) SlotCount)
	{
		return false;
	}
	m_Logger.Write (FromKernel, LogNotice, "Saving the Mac to slot %d", nSlot + 1);
	PauseEmu ();
	for (unsigned d = 0; d < 2; d++)
	{
		if (m_pFloppy[d] != 0)
		{
			m_pFloppy[d]->flush ();
		}
	}
	m_pMachine->flushDisks ();
	SavePRAM ();

	// Room for it (what is there now will go).
	hostu64 nTotal = 0;
	for (unsigned id = 0; id < macemu::Scsi::kTargets; id++)
	{
		if (m_pHardDisk[id] != 0) nTotal += m_pHardDisk[id]->size ();
	}
	for (unsigned d = 0; d < 2; d++)
	{
		if (m_pFloppy[d] != 0) nTotal += m_pFloppy[d]->size ();
	}
	ClearSlot ((unsigned) nSlot);
	RefreshSlots ();
	if (FreeBytes () < nTotal + ((hostu64) m_Config.ramKB << 10) + (16u << 20))
	{
		ResumeEmu ();
		m_Logger.Write (FromKernel, LogWarning, "Not enough room on the card for slot %d", nSlot + 1);
		return false;
	}

	char Title[48] = "Saving the Mac to slot ";
	appendUint (Title, (unsigned) nSlot + 1, sizeof Title);
	char Path[64], Final[64];
	f_mkdir (SLOT_DIR);
	SlotPath (Path, sizeof Path, (unsigned) nSlot, "");
	Path[strLen (Path) - 1] = '\0';
	f_mkdir (Path);

	boolean bOK = TRUE;
	hostu64 nDone = 0;
	for (unsigned id = 0; bOK && id < macemu::Scsi::kTargets; id++)
	{
		if (m_pHardDisk[id] != 0)
		{
			char Name[16] = "disk";
			appendUint (Name, id, sizeof Name);
			strAppend (Name, ".img", sizeof Name);
			SlotPath (Path, sizeof Path, (unsigned) nSlot, Name);
			bOK = CopyOut (m_pHardDisk[id], 0, Path, Title, nDone, nTotal);
		}
	}
	for (unsigned d = 0; bOK && d < 2; d++)
	{
		if (m_pFloppy[d] != 0)
		{
			SlotPath (Path, sizeof Path, (unsigned) nSlot, d == 0 ? "floppy1.dsk" : "floppy2.dsk");
			bOK = CopyOut (m_pFloppy[d], 0, Path, Title, nDone, nTotal);
		}
	}

	// What it is, for the menu; then the state, which makes the slot whole.
	if (bOK)
	{
		char Info[64];
		MacDateText (MacTime (), Info, sizeof Info);
		const char *pDisk = m_pHardDisk[0] != 0 ? m_HardDiskPath[0] : m_FloppyName[0];
		for (const char *q = pDisk; *q; q++) if (*q == '/') pDisk = q + 1;
		if (*pDisk)
		{
			strAppend (Info, "  ", sizeof Info);
			strAppend (Info, pDisk, sizeof Info);
		}
		SlotPath (Path, sizeof Path, (unsigned) nSlot, "info.txt");
		bOK = WriteFileSafely (Path, Info, strLen (Info));
	}
	if (bOK)
	{
		ShowMessage (Title, "the Mac's memory");
		Present ();
		SlotPath (Path, sizeof Path, (unsigned) nSlot, "mac.state.new");
		SlotPath (Final, sizeof Final, (unsigned) nSlot, "mac.state");
		bOK = WriteState (Path) && f_rename (Path, Final) == FR_OK;
	}
	if (!bOK)
	{
		ClearSlot ((unsigned) nSlot);
	}
	RefreshSlots ();
	m_bForceRedraw = TRUE;
	ResumeEmu ();
	m_Logger.Write (FromKernel, bOK ? LogNotice : LogWarning, bOK ? "Saved the Mac to slot %d" : "Could not save slot %d",
			nSlot + 1);
	return bOK;
}

bool CKernel::loadSlot (int nSlot, char *pWhy, unsigned nMax)
{
	pWhy[0] = '\0';
	char Path[64], New[200], To[200];
	SlotPath (Path, sizeof Path, (unsigned) nSlot, "mac.state");
	FIL File;
	if (nSlot < 0 || nSlot >= (int) SlotCount || f_open (&File, Path, FA_READ) != FR_OK)
	{
		strCopy (pWhy, "Nothing saved there", nMax);
		return false;
	}
	THibernationHeader Header;
	UINT nRead = 0;
	const char *pBad = f_read (&File, &Header, sizeof Header, &nRead) != FR_OK || nRead != sizeof Header
			   ? "the file is damaged" : StateMismatch (&Header);
	if (pBad != 0)
	{
		f_close (&File);
		strCopy (pWhy, "Cannot: ", nMax);
		strAppend (pWhy, pBad, nMax);
		return false;
	}

	// Room for the disks as they were, beside the ones now there.
	hostu64 nTotal = 0;
	for (unsigned id = 0; id < macemu::Scsi::kTargets; id++)
	{
		char Name[16] = "disk";
		appendUint (Name, id, sizeof Name);
		strAppend (Name, ".img", sizeof Name);
		SlotPath (Path, sizeof Path, (unsigned) nSlot, Name);
		FILINFO Info;
		if (Header.HardDisk[id][0] != '\0' && f_stat (Path, &Info) == FR_OK) nTotal += Info.fsize;
	}
	if (FreeBytes () < nTotal + (16u << 20))
	{
		f_close (&File);
		strCopy (pWhy, "Not enough room on the card to restore it", nMax);
		return false;
	}

	m_Logger.Write (FromKernel, LogNotice, "Restoring the Mac from slot %d", nSlot + 1);
	char Title[48] = "Restoring the Mac from slot ";
	appendUint (Title, (unsigned) nSlot + 1, sizeof Title);
	PauseEmu ();

	// The disks back as they were: copied beside, then in place of, the
	// ones there now (a power cut leaves those whole).
	for (unsigned id = 0; id < macemu::Scsi::kTargets; id++)
	{
		strCopy (m_Config.hd[id], "none", Config::kPath);
	}
	AttachHardDisks ();				// (none: the files are free)
	for (unsigned d = 0; d < 2; d++)
	{
		if (m_pFloppy[d] != 0)
		{
			ejectDisk ((int) d);
		}
	}
	boolean bOK = TRUE;
	hostu64 nDone = 0;
	for (unsigned id = 0; bOK && id < macemu::Scsi::kTargets; id++)
	{
		if (Header.HardDisk[id][0] == '\0')
		{
			continue;
		}
		char Name[16] = "disk";
		appendUint (Name, id, sizeof Name);
		strAppend (Name, ".img", sizeof Name);
		SlotPath (Path, sizeof Path, (unsigned) nSlot, Name);
		strCopy (To, DRIVE "/", sizeof To);
		strAppend (To, Header.HardDisk[id], sizeof To);
		strCopy (New, To, sizeof New);
		strAppend (New, ".new", sizeof New);
		bOK = CopyOut (0, Path, New, Title, nDone, nTotal);
		if (bOK)
		{
			f_unlink (To);
			bOK = f_rename (New, To) == FR_OK;
		}
		strCopy (m_Config.hd[id], Header.HardDisk[id], Config::kPath);
	}
	AttachHardDisks ();
	for (unsigned d = 0; bOK && d < 2; d++)
	{
		if (Header.Floppy[d][0] == '\0')
		{
			continue;
		}
		SlotPath (Path, sizeof Path, (unsigned) nSlot, d == 0 ? "floppy1.dsk" : "floppy2.dsk");
		strCopy (To, DRIVE "/", sizeof To);
		if (strchr (Header.Floppy[d], '/') == 0)
		{
			strAppend (To, m_Config.diskDir, sizeof To);
			strAppend (To, "/", sizeof To);
		}
		strAppend (To, Header.Floppy[d], sizeof To);
		strCopy (New, To, sizeof New);
		strAppend (New, ".new", sizeof New);
		if (CopyOut (0, Path, New, Title, nDone, nTotal))
		{
			f_unlink (To);
			f_rename (New, To);
		}
		InsertDiskPaused ((int) d, Header.Floppy[d]);
	}
	if (Header.Cd[0] != '\0')
	{
		ResumeEmu ();
		insertCd (Header.Cd);			// (pauses by itself)
		PauseEmu ();
	}

	// Then the Mac itself.
	if (bOK)
	{
		u8 *pBuffer = new u8[CFileState::Size];
		CFileState State (&File, FALSE, pBuffer);
		m_pQuadra->state (State);
		delete [] pBuffer;
		bOK = State.ok;
	}
	f_close (&File);
	if (!bOK)
	{
		m_pMachine->reset ();
		strCopy (pWhy, "Restoring failed: the Mac starts afresh", nMax);
	}
	m_nMacOffTicks = 0;
	m_bHaveVideo = FALSE;
	m_bForceRedraw = TRUE;
	fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
	ResumeEmu ();
	m_Logger.Write (FromKernel, bOK ? LogNotice : LogWarning, bOK ? "Restored slot %d" : "Could not restore slot %d",
			nSlot + 1);
	return bOK;
}

// ----------------------------------------------------------------- the web page's status

// What the Mac is doing, as "name<TAB>value" lines (the web page's table).
void CKernel::StatusText (char *pOut, unsigned nMax)
{
	pOut[0] = '\0';
	auto Line = [&] (const char *pName, const char *pValue) {
		strAppend (pOut, pName, nMax);
		strAppend (pOut, "\t", nMax);
		strAppend (pOut, pValue, nMax);
		strAppend (pOut, "\n", nMax);
	};
	char V[200];

	strCopy (V, m_pMachine != 0 ? m_pMachine->modelName () : "(none)", sizeof V);
	if (m_pOsd != 0 && m_pOsd->active ())
	{
		strAppend (V, ", paused (the menu is open)", sizeof V);
	}
	else
	{
		strAppend (V, ", running at ", sizeof V);
		appendUint (V, m_nSpeedPercent, sizeof V);
		strAppend (V, m_bTurbo || m_Config.speed == 0 ? "% (unlimited)" : "%", sizeof V);
	}
	if (m_pQuadra != 0 && m_pQuadra->cpu.jit != 0)
	{
		strAppend (V, ", JIT on", sizeof V);
	}
	Line ("Mac", V);

	for (unsigned id = 0; id < macemu::Scsi::kTargets; id++)
	{
		if (m_pHardDisk[id] == 0)
		{
			continue;
		}
		char Name[24] = "Hard disk (ID ";
		appendUint (Name, id, sizeof Name);
		strAppend (Name, ")", sizeof Name);
		const char *p = m_HardDiskPath[id];
		for (const char *q = p; *q; q++) if (*q == '/') p = q + 1;
		Line (Name, p);
	}
	// Drives: a third field names the page's Eject for them.
	for (unsigned d = 0; d < 2; d++)
	{
		const char *pName = m_FloppyName[d][0] != '\0' ? m_FloppyName[d] : "(empty)";
		strCopy (V, pName, sizeof V);
		if (m_FloppyName[d][0] != '\0')
		{
			strAppend (V, d == 0 ? "\t0" : "\t1", sizeof V);
		}
		Line (d == 0 ? "Floppy drive 1" : "Floppy drive 2", V);
	}
	if (m_Config.cdromId >= 0)
	{
		const char *p = m_pCdrom != 0 ? m_Config.cdrom : "(empty)";
		for (const char *q = p; *q; q++) if (*q == '/') p = q + 1;
		strCopy (V, p, sizeof V);
		if (m_pCdrom != 0)
		{
			strAppend (V, "\tcd", sizeof V);
		}
		Line ("CD-ROM", V);
	}
	strCopy (V, !m_Config.sound ? "off" : m_SoundName[0] ? m_SoundName : "none", sizeof V);
	Line ("Sound", V);

	if (m_pShared != 0 && m_pShared->wantVolume)
	{
		strCopy (V, m_pShared->status, sizeof V);
		strAppend (V, " (folder ", sizeof V);
		strAppend (V, m_Config.sharedFolder, sizeof V);
		strAppend (V, ")", sizeof V);
		Line ("Shared folder", V);
	}
	if (m_bNetRunning && m_pNet != 0)
	{
		CString IP;
		m_pNet->GetConfig ()->GetIPAddress ()->Format (&IP);
		strCopy (V, m_bWiFi ? "Wi-Fi, " : "Cable, ", sizeof V);
		strAppend (V, (const char *) IP, sizeof V);
		Line ("Network", V);
		strCopy (V, m_Config.macInternet ? "on" : "off", sizeof V);
		if (m_Config.macInternet)
		{
			strAppend (V, m_Config.httpsHelper ? ", with the HTTPS helper for its browsers" : ", without the HTTPS helper", sizeof V);
		}
		Line ("The Mac's internet", V);
	}
}

// ----------------------------------------------------------------- shared folder

// A folder on the card as a disk on the Mac's desktop (the Quadra, Mac OS
// 7.5 and later); it appears once Mac OS has started.
// The Mac integration (the Quadra): the shared folder as a disk, when one
// is chosen, and the clipboard shared with the web page, always.
void CKernel::StartSharedFolder (void)
{
	if (m_pQuadra == 0)
	{
		return;
	}
	boolean bFolder = m_Config.sharedFolder[0] != '\0';
	char Root[160] = DRIVE "/";
	strAppend (Root, bFolder ? m_Config.sharedFolder : "shared", sizeof Root);
	if (bFolder)
	{
		f_mkdir (Root);
	}
	m_pShareFS = new CFatHostFS (Root);
	m_pShared = new macemu::SharedFolder (m_pQuadra->cpu, *m_pShareFS);
	m_pShared->wantVolume = bFolder;
	m_pQuadra->attachSharedFolder (m_pShared);
	if (bFolder)
	{
		m_Logger.Write (FromKernel, LogNotice, "Shared folder: %s", Root);
	}
}

boolean CKernel::ClipboardText (char *pOut, unsigned nMax)
{
	if (m_pShared == 0 || !m_pShared->clipboardReady ())
	{
		return FALSE;
	}
	// A consistent copy: the Mac's core may be writing it (the count is odd then).
	static u8 Mac[macemu::SharedFolder::kClipMax];
	u32 nLength = 0;
	for (unsigned nTry = 0; nTry < 20; nTry++)
	{
		u32 nBefore = m_pShared->clipSerial;
		DataMemBarrier ();
		nLength = m_pShared->clipLength ();
		if (nLength > sizeof Mac) nLength = sizeof Mac;
		memcpy (Mac, m_pShared->clipText (), nLength);
		DataMemBarrier ();
		if (!(nBefore & 1) && m_pShared->clipSerial == nBefore)
		{
			break;
		}
		m_Timer.MsDelay (1);
	}
	macemu::macTextToUtf8 (Mac, nLength, pOut, nMax);
	return TRUE;
}

boolean CKernel::SetClipboardText (const char *pText, unsigned nLength)
{
	if (m_pShared == 0 || !m_pShared->clipboardReady ())
	{
		return FALSE;
	}
	static u8 Mac[macemu::SharedFolder::kClipMax];
	u32 n = macemu::utf8ToMacText (pText, nLength, Mac, sizeof Mac);
	m_pShared->setClipText ((const char *) Mac, n);
	return TRUE;
}

// The shared folder's state, in the log when it changes.
void CKernel::ReportSharedFolder (void)
{
	static const char *pLast = 0;
	if (m_pShared != 0 && m_pShared->status != pLast)
	{
		pLast = m_pShared->status;
		m_Logger.Write (FromKernel, LogNotice, "Shared folder: %s", pLast);
	}
}

// ----------------------------------------------------------------- JIT

// Translation on core 3, so that the Mac does not wait for it: once the
// JIT is made and the Mac runs on core 1 (whichever comes last).
void CKernel::StartJitCore (void)
{
#ifdef ARM_ALLOW_MULTI_CORE
	if (m_pJit != 0 && m_pJitWorker == 0 && m_bEmuCoreRunning && m_pJit->startBackground (m_pQuadra->cpu))
	{
		m_pJitWorker = m_pJit;
		asm volatile ("dsb ish\n\tsev" ::: "memory");
		m_Logger.Write (FromKernel, LogNotice, "The JIT translates on core 3");
	}
#endif
}

#ifdef ARM_ALLOW_MULTI_CORE
// Core 3: asleep until the JIT is made, then its translator for good.
void CKernel::JitCoreRun (void)
{
	while (m_pJitWorker == 0)
	{
		asm volatile ("wfe");
	}
	m_pJitWorker->workerLoop ();
}
#endif

// The JIT on or off as the settings say (the Quadra only; while the Mac
// is paused).  It is made the first time it is wanted.
void CKernel::UpdateJit (void)
{
	if (m_pQuadra == 0)
	{
		return;
	}
	if (!m_Config.jit)
	{
		if (m_pQuadra->cpu.jit != 0)
		{
			m_Logger.Write (FromKernel, LogNotice, "JIT off");
		}
		m_pQuadra->cpu.jit = 0;
		return;
	}
	if (m_pJit == 0)
	{
		m_pJit = new Jit;
		if (m_pJit == 0 || !m_pJit->init ())
		{
			m_Logger.Write (FromKernel, LogWarning, "No JIT on this Pi (64-bit kernels only)");
			delete m_pJit;
			m_pJit = 0;
			m_Config.jit = 0;
			return;
		}
		StartJitCore ();
	}
	if (m_pQuadra->cpu.jit == 0)
	{
		m_Logger.Write (FromKernel, LogNotice, "JIT on");
	}
	m_pQuadra->cpu.jit = m_pJit;
}

// ----------------------------------------------------------------- network

// Wired Ethernet or Wi-Fi with DHCP, and the web file manager on port 80.
// Nothing waits for the network: it comes up in the background while the
// Mac starts.  Circle drives one network device at a time, so "auto" picks
// Wi-Fi when a Wi-Fi network has been set up and the cable otherwise.
void CKernel::StartNetwork (void)
{
	if (strEqNoCase (m_Config.network, "off"))
	{
		return;
	}

	FILINFO Info;
	boolean bUserConfig = f_stat (WPA_USER_CONFIG, &Info) == FR_OK;
	m_bWiFi = strEqNoCase (m_Config.network, "wifi")
		  || (strEqNoCase (m_Config.network, "auto")
		      && (m_Config.wifiSsid[0] != '\0' || bUserConfig));

	if (!m_bWiFi)
	{
		StartEthernet ();
		return;
	}

	// Loading the Wi-Fi firmware takes a moment: do it once the Mac has
	// started, so that the Happy Mac is not held up.
	strCopy (m_NetLine[0], "Wi-Fi: starting...", sizeof m_NetLine[0]);
	m_NetLine[1][0] = '\0';
	m_bWiFiPending = TRUE;
}

// The network stack, with the address from the router (DHCP) or, with
// ip_address set, a fixed one: for a Pi cabled straight to another, say.
void CKernel::NewNetSubSystem (boolean bWiFi)
{
	static u8 IP[4], Mask[4] = {255, 255, 255, 0}, Gateway[4];
	const u8 *pIP = 0, *pMask = 0, *pGateway = 0;
	if (macemu::parseIPv4 (m_Config.ipAddress, IP))
	{
		pIP = IP;
		macemu::parseIPv4 (m_Config.ipNetmask, Mask);
		pMask = Mask;
		if (macemu::parseIPv4 (m_Config.ipGateway, Gateway))
		{
			pGateway = Gateway;
		}
		m_Logger.Write (FromKernel, LogNotice, "Fixed IP address %s", m_Config.ipAddress);
	}
	m_pNet = new CNetSubSystem (pIP, pMask, pGateway, pGateway, "macemu",
				    bWiFi ? NetDeviceTypeWLAN : NetDeviceTypeEthernet);
}

void CKernel::StartEthernet (void)
{
	strCopy (m_NetLine[0], "Ethernet: waiting for a connection...", sizeof m_NetLine[0]);
	strCopy (m_NetLine[1], "Plug in a network cable.", sizeof m_NetLine[1]);

	NewNetSubSystem (FALSE);
	if (m_pNet == 0 || !m_pNet->Initialize (FALSE))
	{
		m_Logger.Write (FromKernel, LogWarning, "No Ethernet");
		strCopy (m_NetLine[0], "Ethernet: no adapter found on this Pi", sizeof m_NetLine[0]);
		strCopy (m_NetLine[1], "Choose Wi-Fi below.", sizeof m_NetLine[1]);
		delete m_pNet;
		m_pNet = 0;
		return;
	}

	m_nNetStartTicks = m_Timer.GetTicks ();
	m_pWebServer = new CWebServer (m_pNet, DRIVE, this);
}

void CKernel::StartWiFi (void)
{
	m_bWiFiPending = FALSE;

	const char *pConfig = WPA_USER_CONFIG;
	if (m_Config.wifiSsid[0] != '\0')
	{
		if (m_Config.wifiPassword[0] != '\0' && strLen (m_Config.wifiPassword) < 8)
		{
			strCopy (m_NetLine[0], "Wi-Fi: the password is too short", sizeof m_NetLine[0]);
			strCopy (m_NetLine[1], "WPA passwords have 8 to 63 characters.", sizeof m_NetLine[1]);
			return;
		}

		pConfig = WPA_CONFIG;
		if (m_Config.wifiPassword[0] != '\0' && !WriteWiFiConfig (pConfig))
		{
			strCopy (m_NetLine[0], "Wi-Fi: cannot write " WPA_CONFIG, sizeof m_NetLine[0]);
			return;
		}
	}
	else
	{
		FILINFO Info;
		if (f_stat (WPA_USER_CONFIG, &Info) != FR_OK)
		{
			strCopy (m_NetLine[0], "Wi-Fi: no network set up yet", sizeof m_NetLine[0]);
			strCopy (m_NetLine[1], "Enter the Wi-Fi name and password below.", sizeof m_NetLine[1]);
			return;
		}
	}

	m_Logger.Write (FromKernel, LogNotice, "Starting Wi-Fi");
	m_pWLAN = new CBcm4343Device (WLAN_FIRMWARE);
	if (m_pWLAN == 0 || !m_pWLAN->Initialize ())
	{
		m_Logger.Write (FromKernel, LogWarning, "No Wi-Fi");
		strCopy (m_NetLine[0], "Wi-Fi: not available on this Pi", sizeof m_NetLine[0]);
		strCopy (m_NetLine[1], "(or the firmware folder is missing)", sizeof m_NetLine[1]);
		return;
	}

	NewNetSubSystem (TRUE);
	if (m_pNet == 0 || !m_pNet->Initialize (FALSE))
	{
		strCopy (m_NetLine[0], "Wi-Fi: cannot start the network", sizeof m_NetLine[0]);
		delete m_pNet;
		m_pNet = 0;
		return;
	}

	if (m_Config.wifiSsid[0] != '\0' && m_Config.wifiPassword[0] == '\0')
	{
		m_pWLAN->JoinOpenNet (m_Config.wifiSsid);	// no password: an open network
	}
	else
	{
		m_pWPASupplicant = new CWPASupplicant (pConfig);
		if (m_pWPASupplicant == 0 || !m_pWPASupplicant->Initialize ())
		{
			strCopy (m_NetLine[0], "Wi-Fi: cannot start WPA", sizeof m_NetLine[0]);
			return;
		}
	}

	strCopy (m_NetLine[0], "Wi-Fi: connecting to ", sizeof m_NetLine[0]);
	strAppend (m_NetLine[0], m_Config.wifiSsid[0] ? m_Config.wifiSsid : "the network", sizeof m_NetLine[0]);
	strAppend (m_NetLine[0], "...", sizeof m_NetLine[0]);
	m_NetLine[1][0] = '\0';

	m_nNetStartTicks = m_Timer.GetTicks ();
	m_pWebServer = new CWebServer (m_pNet, DRIVE, this);
}

// The WPA supplicant's settings, from the menu's Wi-Fi name and password.
// The name is written in hex so that any characters work.
boolean CKernel::WriteWiFiConfig (const char *pPath)
{
	static char Text[512];
	Text[0] = '\0';
	strAppend (Text, "# Written by the Mac Emulator from macemu.cfg\ncountry=", sizeof Text);
	strAppend (Text, m_Config.wifiCountry, sizeof Text);
	strAppend (Text, "\nnetwork={\n\tssid=", sizeof Text);
	for (const char *p = m_Config.wifiSsid; *p; p++)
	{
		appendHex (Text, (u8) *p, 2, sizeof Text);
	}
	strAppend (Text, "\n\tpsk=\"", sizeof Text);
	strAppend (Text, m_Config.wifiPassword, sizeof Text);
	strAppend (Text, "\"\n\tkey_mgmt=WPA-PSK\n\tscan_ssid=1\n}\n", sizeof Text);

	return WriteFileSafely (pPath, Text, strLen (Text));
}

void CKernel::UpdateNetwork (void)
{
	if (m_bWiFiPending && m_nFrames >= 90)
	{
		StartWiFi ();
	}

	if (m_pNet == 0)
	{
		return;
	}

	m_Scheduler.Yield ();
	if (m_pEther != 0)
	{
		m_pEther->Update ();
	}

	if (m_pWebServer != 0)
	{
		if (m_pWebServer->FlushRequested ())
		{
			PauseEmu ();
			for (unsigned d = 0; d < 2; d++)
			{
				if (m_pFloppy[d] != 0)
				{
					m_pFloppy[d]->flush ();
				}
			}
			m_pMachine->flushDisks ();
			ResumeEmu ();
			m_pWebServer->FlushDone ();
		}

		if (m_pWebServer->RestartRequested ())
		{
			resetMachine ();
			m_pWebServer->RestartDone ();
		}

		if (const char *pName = m_pWebServer->InsertRequested ())
		{
			// A transfer disk made on the web page: a floppy into drive
			// 2, a CD into the CD-ROM drive (not while the Mac is using
			// the one in it: it would lose it without warning).
			if (endsWithNoCase (pName, ".toast"))
			{
				boolean bOK = FALSE;
				if (   m_Config.cdromId >= 0
				    && !(m_pCdrom != 0 && m_pMachine->scsiDevice (m_Config.cdromId).locked))
				{
					char Path[160];
					strCopy (Path, m_Config.diskDir, sizeof Path);
					strAppend (Path, "/", sizeof Path);
					strAppend (Path, pName, sizeof Path);
					bOK = insertCd (Path);
				}
				m_pWebServer->InsertDone (bOK);
			}
			else
			{
				m_pWebServer->InsertDone (insertDisk (1, pName));
			}
		}

		int nWhich;
		const char *pDriveName;
		if (m_pWebServer->DriveRequested (&nWhich, &pDriveName))
		{
			m_pWebServer->DriveDone (WebDrive (nWhich, pDriveName));
		}

		if (m_pWebServer->RebootRequested ())
		{
			// A software update has been installed: start it.
			m_pOsd->close ();
			saveAndReboot ();
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
		strCopy (m_NetLine[1], m_bWiFi ? "(Wi-Fi) or http://macemu.local/ in a browser"
					       : "(cable) or http://macemu.local/ in a browser", sizeof m_NetLine[1]);
		m_Logger.Write (FromKernel, LogNotice, "Web file manager at http://%s/",
				(const char *) IPString);

		if (m_pEther != 0)
		{
			boolean bHttps = m_Config.macInternet && m_Config.httpsHelper;
			if (m_pEther->Start (m_pNet, (u16) m_Config.macEthernet, m_Config.macEthernetPeer,
					     m_Config.macInternet != 0, bHttps))
			{
				m_Logger.Write (FromKernel, LogNotice, "Mac Ethernet: AppleTalk on UDP port %d, internet %s",
						m_Config.macEthernet, m_Config.macInternet ? "on" : "off");
			}
			if (bHttps)
			{
				LoadRootCertificates ();
				// The HTTPS helper checks certificates' dates: the Pi has
				// no clock of its own, so it asks the internet.
				new CNTPDaemon ("pool.ntp.org", m_pNet);
			}
			// Remember the address: next time the Mac may ask for its
			// Ethernet address before the network is up.
			u8 IP[4], Old[4] = {0, 0, 0, 0};
			m_pNet->GetConfig ()->GetIPAddress ()->CopyTo (IP);
			FIL File;
			UINT nDone = 0;
			if (f_open (&File, NETADDR_FILE, FA_READ | FA_OPEN_EXISTING) == FR_OK)
			{
				f_read (&File, Old, sizeof Old, &nDone);
				f_close (&File);
			}
			if (memcmp (IP, Old, sizeof IP) != 0
			    && f_open (&File, NETADDR_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
			{
				f_write (&File, IP, sizeof IP, &nDone);
				f_close (&File);
			}
		}
	}

	if (!m_bNetRunning && m_bWiFi && m_pWLAN != 0)
	{
		unsigned nSeconds = (nTicks - m_nNetStartTicks) / HZ;
		boolean bLink = m_pWLAN->IsLinkUp ();
		boolean bWPA = m_pWPASupplicant == 0 || CWPASupplicant::IsConnected ();
		char *p = m_NetLine[0];
		if (bLink && bWPA)
		{
			strCopy (p, "Wi-Fi: connected, waiting for an address", sizeof m_NetLine[0]);
			strCopy (m_NetLine[1], "(from the router's DHCP)", sizeof m_NetLine[1]);
		}
		else
		{
			strCopy (p, "Wi-Fi: connecting to ", sizeof m_NetLine[0]);
			strAppend (p, m_Config.wifiSsid[0] ? m_Config.wifiSsid : "the network", sizeof m_NetLine[0]);
			strAppend (p, " (", sizeof m_NetLine[0]);
			appendUint (p, nSeconds, sizeof m_NetLine[0]);
			strAppend (p, "s)", sizeof m_NetLine[0]);
			if (m_bWrongPassword)
			{
				strCopy (m_NetLine[1], "The router rejects the password.", sizeof m_NetLine[1]);
			}
			else if (nSeconds > 30)
			{
				strCopy (m_NetLine[1], bLink ? "Joined, but the password exchange fails."
							     : "Network not found: check the name (case!).", sizeof m_NetLine[1]);
			}
		}
		// The WPA supplicant's latest message, whatever the state.
		strCopy (m_NetLine[2], "Wi-Fi says: ", sizeof m_NetLine[2]);
		strAppend (m_NetLine[2], m_WpaLast[0] ? m_WpaLast : "(nothing yet)", sizeof m_NetLine[2]);
	}

	if (m_bNetRunning && m_pWebServer != 0)
	{
		const char *pActivity = m_pWebServer->Activity ();
		strCopy (m_NetLine[2], pActivity[0] ? pActivity : "Idle.", sizeof m_NetLine[2]);
		if (!pActivity[0] && m_pEther != 0 && m_pEther->Running ()
		    && (m_pEther->Sent () | m_pEther->Received ()) != 0)
		{
			char *p = m_NetLine[2];
			strCopy (p, "Idle.  Mac Ethernet: ", sizeof m_NetLine[2]);
			appendUint (p, m_pEther->Sent (), sizeof m_NetLine[2]);
			strAppend (p, " out, ", sizeof m_NetLine[2]);
			appendUint (p, m_pEther->Received (), sizeof m_NetLine[2]);
			strAppend (p, " in", sizeof m_NetLine[2]);
		}
	}

	m_pOsd->refresh ();
}

// Wait for a moment in time, letting the network tasks run meanwhile.
void CKernel::WaitUntil (hostu64 nUntil)
{
	m_nLoops++;
	while (CTimer::GetClockTicks64 () < nUntil)
	{
		CDiskProxy::Service ();

		// Idle: copy a little more of the disks into memory (64 KB takes
		// a few ms), so that the Mac's reads need not wait for the card.
		if (   nUntil - CTimer::GetClockTicks64 () > 4000
		    && (m_pWebServer == 0 || m_pWebServer->Activity ()[0] == '\0')	// not during a transfer
		    && CFileStorage::Prefetch ())
		{
			CDiskProxy::Service ();
		}

		if (m_pNet != 0)
		{
			m_Scheduler.Yield ();
			if (m_pEther != 0)
			{
				m_pEther->Update ();
			}
		}
	}
}

boolean CKernel::FileInUse (const char *pPath)
{
	if (m_pCdrom != 0 && strEqNoCase (m_Config.cdrom, pPath))
	{
		return TRUE;
	}

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

// ----------------------------------------------------------------- power

void CKernel::PowerKeyHandler (void)
{
	if (s_pThis != 0)
	{
		s_pThis->m_bPowerKey = TRUE;
	}
}

// Pi 400 (Fn+F10) and Pi 5 have a power button that starts a halted Pi.
boolean CKernel::HasPowerButton (void)
{
	TMachineModel Model = CMachineInfo::Get ()->GetMachineModel ();
	return Model == MachineModel400 || Model == MachineModel5 || Model == MachineModel500;
}

// The power key: the Mac II and Quadra get their own power key, which asks
// "Shut Down?"; shutting down then turns the Pi off.  A second press within
// five seconds, a press in the menu, or one on a Mac Plus turns the Pi off
// straight away (after saving).
void CKernel::HandlePowerKey (void)
{
	m_Logger.Write (FromKernel, LogNotice, "Power key");
	unsigned nNow = m_Timer.GetTicks ();
	if (   m_pOsd->active ()
	    || m_pPlus != 0
	    || m_nMacOffTicks != 0
	    || (m_nMacPowerKeyTicks != 0 && nNow - m_nMacPowerKeyTicks < 5 * HZ))
	{
		powerOff ();
	}

	m_nMacPowerKeyTicks = nNow;
	MacKey (MK_Power, TRUE);
	MacKey (MK_Power, FALSE);
}

void CKernel::powerOff (void)
{
	TurnOff (TRUE);
}

void CKernel::TurnOff (boolean bShowMessage)
{
	m_Logger.Write (FromKernel, LogNotice, "Turning off");
	PauseEmu ();					// for good
	for (unsigned d = 0; d < 2; d++)
	{
		if (m_pFloppy[d] != 0)
		{
			m_pFloppy[d]->flush ();
		}
	}
	m_pMachine->flushDisks ();
	SavePRAM ();
	BootLog (TRUE);

	m_pOsd->close ();
	if (bShowMessage)
	{
		ShowMessage ("Turning off.", HasPowerButton () ? "Press the power key to start again."
							       : "You can unplug the Pi now.");
	}
	Present ();
	m_ActLED.Off ();
	m_Timer.MsDelay (1500);

#if RASPPI >= 5
	poweroff ();
#else
	if (CMachineInfo::Get ()->GetMachineModel () == MachineModel400)
	{
		// The Pi 400's keyboard controller holds the power: it cuts it when
		// the firmware's expander line 5 (GLOBAL_SHUTDOWN) is driven, as
		// Linux's gpio-poweroff does (active, inactive, active).
		static const unsigned States[] = {1, 0, 1};
		for (unsigned i = 0; i < 3; i++)
		{
			CBcmPropertyTags Tags;
			TPropertyTagGPIOState State;
			State.nGPIO = EXP_GPIO_BASE + 5;
			State.nState = States[i];
			Tags.GetTag (PROPTAG_SET_SET_GPIO_STATE, &State, sizeof State, 8);
			m_Timer.MsDelay (i < 2 ? 100 : 3000);
		}
	}
	CBcmWatchdog Watchdog;
	Watchdog.Restart (CBcmWatchdog::PartitionHalt);	// the firmware halts until GPIO 3 / power key
#endif
}

// ----------------------------------------------------------------- Wi-Fi scan

// The Broadcom firmware's scan records, as Circle's WPA driver reads them.
struct TBssInfo
{
	u32 version;
	u32 length;
	u8 BSSID[6];
	u16 beacon_period;
	u16 capability;
	u8 SSID_len;
	u8 SSID[32];
	struct { u32 count; u8 rates[16]; } rateset;
	u16 chanspec;
	u16 atim_window;
	u8 dtim_period;
	s16 RSSI;
	s8 phy_noise;
	u8 n_cap;
	u32 nbss_cap;
	u8 ctl_ch;
	u32 reserved32[1];
	u8 flags;
	u8 reserved[3];
	u8 basic_mcs[16];
	u16 ie_offset;
	u32 ie_length;
	u16 SNR;
};

struct TEscanResult
{
	u32 buflen;
	u32 version;
	u16 sync_id;
	u16 bss_count;
	TBssInfo bss_info;
};

int CKernel::ScanWiFi (char (*ppNames)[kNameLen], int nMax)
{
	if (m_pWLAN == 0)
	{
		ShowMessage ("Starting Wi-Fi...", "");
		Present ();
		m_pWLAN = new CBcm4343Device (WLAN_FIRMWARE);
		if (m_pWLAN == 0 || !m_pWLAN->Initialize ())
		{
			m_Logger.Write (FromKernel, LogWarning, "No Wi-Fi for the scan");
			delete m_pWLAN;
			m_pWLAN = 0;
			return 0;
		}
	}

	ShowMessage ("Searching for Wi-Fi networks...", "This takes about five seconds.");
	Present ();

	unsigned nLen;
	static u8 Buffer[FRAME_BUFFER_SIZE];
	while (m_pWLAN->ReceiveScanResult (Buffer, &nLen))
	{
		// old results
	}

	if (!m_pWLAN->Control ("escan 4"))
	{
		m_Logger.Write (FromKernel, LogWarning, "Wi-Fi scan failed");
		return 0;
	}

	static int Signal[128];
	int n = 0;
	hostu64 nEnd = CTimer::GetClockTicks64 () + 5500000;
	while (CTimer::GetClockTicks64 () < nEnd)
	{
		WaitUntil (CTimer::GetClockTicks64 () + 20000);
		while (m_pWLAN->ReceiveScanResult (Buffer, &nLen))
		{
			if (nLen < sizeof (TEscanResult))
			{
				continue;
			}
			TEscanResult *pResult = (TEscanResult *) Buffer;
			TBssInfo *pBss = &pResult->bss_info;
			for (unsigned i = 0; i < pResult->bss_count; i++)
			{
				if (   (u8 *) pBss + sizeof (TBssInfo) > Buffer + nLen
				    || pBss->length < sizeof (TBssInfo) - 8)
				{
					break;
				}

				char Name[kNameLen];
				unsigned nNameLen = pBss->SSID_len <= 32 ? pBss->SSID_len : 32;
				for (unsigned k = 0; k < nNameLen; k++)
				{
					u8 c = pBss->SSID[k];
					Name[k] = c < ' ' ? '?' : (char) c;
				}
				Name[nNameLen] = '\0';

				if (nNameLen > 0 && Name[0] != '\0')
				{
					int j = 0;
					while (j < n && strcmp (ppNames[j], Name) != 0) j++;
					if (j < n)
					{
						if (pBss->RSSI > Signal[j]) Signal[j] = pBss->RSSI;
					}
					else if (n < nMax && n < 128)
					{
						strCopy (ppNames[n], Name, kNameLen);
						Signal[n] = pBss->RSSI;
						n++;
					}
				}

				pBss = (TBssInfo *) ((u8 *) pBss + pBss->length);
			}
		}
	}

	// Strongest first.
	for (int i = 1; i < n; i++)
	{
		for (int j = i; j > 0 && Signal[j] > Signal[j - 1]; j--)
		{
			char Tmp[kNameLen];
			strCopy (Tmp, ppNames[j], kNameLen);
			strCopy (ppNames[j], ppNames[j - 1], kNameLen);
			strCopy (ppNames[j - 1], Tmp, kNameLen);
			int t = Signal[j]; Signal[j] = Signal[j - 1]; Signal[j - 1] = t;
		}
	}

	m_Logger.Write (FromKernel, LogNotice, "Wi-Fi scan: %d networks", n);
	for (int i = 0; i < n; i++)
	{
		m_Logger.Write (FromKernel, LogNotice, "  %s (%d dBm)", ppNames[i], Signal[i]);
	}

	return n;
}

// ----------------------------------------------------------------- trouble

// Once a second: if the main loop has not moved for eight seconds, put what
// is known on the screen, so that a photo of it tells what went wrong.
void CKernel::WatchdogTimer (TKernelTimerHandle hTimer, void *pParam, void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	if (pThis->m_nLoops != pThis->m_nWatchedLoops)
	{
		pThis->m_nWatchedLoops = pThis->m_nLoops;
		pThis->m_nStuckSeconds = 0;
	}
	else if (++pThis->m_nStuckSeconds == 8 && !pThis->m_bEmergencyShown)
	{
		pThis->ShowEmergency ("The emulator has stopped responding.");
	}

	pThis->m_Timer.StartKernelTimer (HZ, WatchdogTimer, 0, pThis);
}

void CKernel::PanicHandler (void)
{
	if (s_pThis != 0)
	{
		s_pThis->ShowEmergency ("The emulator has crashed.");
	}
}

// Straight into the frame buffer (no back buffer): may run in an interrupt.
void CKernel::ShowEmergency (const char *pTitle)
{
	m_bEmergencyShown = TRUE;
	volatile u32 *pFB = m_pFrameBufferPixels != 0 ? FrontPixels () : 0;
	if (pFB == 0)
	{
		return;
	}
	unsigned nWidth = (unsigned) m_Surface.width, nHeight = (unsigned) m_Surface.height;
	unsigned fw = (unsigned) m_Font.width (), fh = (unsigned) m_Font.height ();
	unsigned nCols = nWidth / fw, nRows = nHeight / fh;

	for (unsigned y = 0; y < nHeight; y++)
	{
		for (unsigned x = 0; x < nWidth; x++)
		{
			pFB[y * m_nFrameBufferPitch + x] = 0x00400000;
		}
	}

	static char Text[sizeof m_LogTail + 2048 + 256];
	Text[0] = '\0';
	strAppend (Text, pTitle, sizeof Text);
	strAppend (Text, "\nPlease take a photo of this screen.  Task: ", sizeof Text);
	CTask *pTask = CScheduler::Get ()->GetCurrentTask ();
	const char *pName = pTask != 0 ? pTask->GetName () : 0;
	strAppend (Text, pName != 0 && pName[0] ? pName : "main", sizeof Text);
	strAppend (Text, "\n\n", sizeof Text);
	unsigned nHead = strLen (Text);
	strAppend (Text, m_LogTail, sizeof Text);
	static char Recent[2048];
	int nRecent = m_Logger.Read (Recent, sizeof Recent - 1, FALSE);
	if (nRecent > 0)
	{
		Recent[nRecent] = '\0';
		strAppend (Text, Recent, sizeof Text);
	}

	// Lay the text out in lines, then show the title and the last lines.
	static unsigned LineStart[512];
	unsigned nLines = 0, nCol = 0;
	LineStart[nLines++] = 0;
	for (unsigned i = 0; Text[i] && nLines < 512; i++)
	{
		if (Text[i] == '\n' || nCol + 1 >= nCols)
		{
			LineStart[nLines++] = i + 1;
			nCol = 0;
		}
		else
		{
			nCol++;
		}
	}

	unsigned nFirstLog = 0;
	while (nFirstLog < nLines && LineStart[nFirstLog] < nHead) nFirstLog++;
	unsigned nRoom = nRows > nFirstLog ? nRows - nFirstLog : 0;
	unsigned nFrom = nLines - nFirstLog > nRoom ? nLines - nRoom : nFirstLog;

	unsigned nRow = 0;
	for (unsigned l = 0; l < nLines && nRow < nRows; l++)
	{
		if (l >= nFirstLog && l < nFrom)
		{
			continue;
		}
		unsigned nEnd = l + 1 < nLines ? LineStart[l + 1] : strLen (Text);
		for (unsigned i = LineStart[l], c = 0; i < nEnd && c < nCols; i++)
		{
			char ch = Text[i];
			if (ch == '\n')
			{
				break;
			}
			if (ch < ' ' || ch > '~')
			{
				ch = ' ';
			}
			for (unsigned y = 0; y < fh; y++)
			{
				for (unsigned x = 0; x < fw; x++)
				{
					if (m_Font.pixel (ch, (int) x, (int) y))
					{
						pFB[(nRow * fh + y) * m_nFrameBufferPitch + c * fw + x] = 0x00FFFFFF;
					}
				}
			}
			c++;
		}
		nRow++;
	}
}

void CKernel::UpdateUSB (void)
{
	// A USB audio device unplugged: back to HDMI (or wait for another).
	if (m_bUSBSound && m_pSound != 0 && !m_pSound->IsActive ())
	{
		m_Logger.Write (FromKernel, LogNotice, "Sound: the USB audio device was unplugged");
		m_bUSBSoundFailed = FALSE;
		if (m_Config.sound)
		{
			SetupSound ();
		}
		else
		{
			StopSound ();
		}
	}

	if (!m_USBHCI.UpdatePlugAndPlay ())
	{
		return;
	}

	// One plugged in: use it at once.
	if (   m_Config.sound && WantUSBSound () && !m_bUSBSound && !m_bUSBSoundFailed
	    && m_DeviceNameService.GetDevice ("uaudio1-1", FALSE) != 0)
	{
		SetupSound ();
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
		PostEvent (EvMouseMove, mx, my);
	}

	boolean bButton = (nButtons & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT)) != 0;
	static boolean bLastButton = FALSE;
	if (bButton != bLastButton)
	{
		bLastButton = bButton;
		PostEvent (EvMouseButton, bButton);
	}
}

void CKernel::MacKey (int nKey, boolean bDown)
{
	if (nKey < 0 || nKey >= 128 || m_bMacKeyDown[nKey] == bDown)
	{
		return;
	}

	m_bMacKeyDown[nKey] = bDown;
	PostEvent (EvKey, nKey, bDown);
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
	if (ucUsage == 0x66)				// Keyboard Power
	{
		if (bDown)
		{
			m_bPowerKey = TRUE;
		}
		return;
	}

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

		if (m_pOsd->editing ())
		{
			// Typing the Wi-Fi name or password.
			if (ucUsage == HID_ENTER || ucUsage == HID_KP_ENTER)
			{
				m_pOsd->key (Osd::NavSelect);
			}
			else if (ucUsage == HID_ESC)
			{
				m_pOsd->key (Osd::NavBack);
			}
			else
			{
				boolean bShift = (m_LastModifiers & (HIDMOD_LSHIFT | HIDMOD_RSHIFT)) != 0;
				if (m_bCapsLock && ucUsage >= 0x04 && ucUsage <= 0x1D)
				{
					bShift = !bShift;
				}
				char c = usbToAscii (ucUsage, bShift, !strEqNoCase (m_Config.keyboard, "us"));
				if (c != 0)
				{
					m_pOsd->typeChar (c);
				}
			}
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
			PostEvent (EvInterrupt, 0);
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
	if (Kind == WifiNetworks)
	{
		return ScanWiFi (ppNames, nMax);
	}

	int n = 0;
	for (int nPass = 0; nPass < 2 && n < nMax; nPass++)
	{
		boolean bDiskDir = nPass == 1;
		if (Kind == Floppies && !bDiskDir) continue;
		if (Kind == Roms && bDiskDir) continue;
		if (Kind == CdImages && bDiskDir && m_Config.diskDir[0] == '\0') continue;

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
				bUse = bUse && (nSize == 409600 || nSize == 819200 || nSize == 1474560
						|| (nSize > 409600 + 84 && nSize <= FloppyFileMax));
				break;
			case HardDisks:
				bUse = bUse && nSize > 2 * 1024 * 1024 && !endsWithNoCase (Info.fname, ".zip")
				       && !endsWithNoCase (Info.fname, ".iso") && !endsWithNoCase (Info.fname, ".toast")
				       && !endsWithNoCase (Info.fname, ".cdr");
				break;
			case CdImages:
				bUse = bUse && (endsWithNoCase (Info.fname, ".iso") || endsWithNoCase (Info.fname, ".toast")
						|| endsWithNoCase (Info.fname, ".cdr"));
				break;
			default:
				bUse = bUse && nSize >= 0x10000 && nSize <= 0x110000;	// some dumps carry extra bytes
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
	PauseEmu ();
	bool bOK = InsertDiskPaused (nDrive, pName);
	ResumeEmu ();
	return bOK;
}

bool CKernel::InsertDiskPaused (int nDrive, const char *pName)
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
	if (   !pDisk->Load (Path, FloppyFileMax)
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
	PauseEmu ();
	m_pMachine->floppy ().eject (nDrive);		// flushes the image
	delete m_pFloppy[nDrive];
	m_pFloppy[nDrive] = 0;
	m_FloppyName[nDrive][0] = '\0';
	m_Config.floppy[nDrive][0] = '\0';
	ResumeEmu ();
}

void CKernel::resetMachine (void)
{
	m_nMacOffTicks = 0;
	PauseEmu ();
	m_pMachine->flushDisks ();
	SavePRAM ();
	ReleaseAllKeys ();
	AttachHardDisks ();				// the menu may have changed them
	m_pMachine->reset ();
	m_bForceRedraw = TRUE;
	m_nEventOut = m_nEventIn;			// forget input meant for the old run
	ResumeEmu ();
}

// Model, ROM, memory and video changes need a fresh start of the whole
// emulator: save everything and reboot the Pi (it is quick).
// Sharp scaling needs the firmware's nearest-neighbour filter, which is a
// config.txt setting read at boot: add or remove "scaling_kernel=8".
void CKernel::UpdateFirmwareConfig (void)
{
	static char Text[8192];
	FIL File;
	UINT nRead = 0;
	if (f_open (&File, DRIVE "/config.txt", FA_READ) != FR_OK)
	{
		return;
	}
	f_read (&File, Text, sizeof Text - 1, &nRead);
	f_close (&File);
	Text[nRead] = '\0';

	static char Out[8192 + 64];
	Out[0] = '\0';
	boolean bHad = FALSE;
	for (char *p = Text; *p; )
	{
		char *pEnd = p;
		while (*pEnd && *pEnd != '\n') pEnd++;
		boolean bKernelLine = strncmp (p, "scaling_kernel=", 15) == 0;
		bHad |= bKernelLine;
		if (!bKernelLine)
		{
			char Save = *pEnd;
			*pEnd = '\0';
			strAppend (Out, p, sizeof Out);
			strAppend (Out, "\n", sizeof Out);
			*pEnd = Save;
		}
		p = *pEnd ? pEnd + 1 : pEnd;
	}
	if (m_Config.sharp)
	{
		strAppend (Out, "scaling_kernel=8\n", sizeof Out);
	}
	else if (!bHad)
	{
		return;					// nothing to change
	}

	WriteFileSafely (DRIVE "/config.txt", Out, strLen (Out));
}

// Insert or eject from the web page (see CWebServer::DriveRequested).
const char *CKernel::WebDrive (int nWhich, const char *pName)
{
	boolean bCdBusy = m_pCdrom != 0 && m_Config.cdromId >= 0 && m_pMachine->scsiDevice (m_Config.cdromId).locked;
	if (nWhich == 2)
	{
		if (m_Config.cdromId < 0)
		{
			return "This Mac has no CD-ROM drive (cdrom_id in macemu.cfg).";
		}
		if (bCdBusy)
		{
			return "The Mac is using the CD: drag it to the Trash on the Mac first.";
		}
		if (pName[0] == '\0')
		{
			insertCd (0);
			return "Ejected the CD.";
		}
		char Path[160];
		strCopy (Path, m_Config.diskDir, sizeof Path);
		strAppend (Path, "/", sizeof Path);
		strAppend (Path, pName, sizeof Path);
		return insertCd (Path) ? "The CD is in the CD-ROM drive." : "Cannot open that CD image.";
	}

	if (pName[0] == '\0')
	{
		if (nWhich < 0 || m_pFloppy[nWhich] == 0)
		{
			return "That drive is empty.";
		}
		ejectDisk (nWhich);
		return "Ejected the floppy.";
	}
	if (nWhich < 0)
	{
		nWhich = m_pFloppy[0] == 0 ? 0 : m_pFloppy[1] == 0 ? 1 : -1;
		if (nWhich < 0)
		{
			return "Both floppy drives are full: eject one first.";
		}
	}
	if (!insertDisk (nWhich, pName))
	{
		return "Not a usable floppy (400K, 800K; 1.44 MB on a Quadra).";
	}
	return nWhich == 0 ? "The floppy is in floppy drive 1." : "The floppy is in floppy drive 2.";
}

// A file written whole, so that losing the power never leaves half of it:
// the new contents go to NAME.new, which then replaces NAME.  RecoverFile
// finishes the job at start-up if the power went in between.
boolean CKernel::WriteFileSafely (const char *pPath, const void *pData, unsigned nLength)
{
	char New[96];
	strCopy (New, pPath, sizeof New);
	strAppend (New, ".new", sizeof New);

	FIL File;
	if (f_open (&File, New, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return FALSE;
	}
	UINT nWritten = 0;
	boolean bOK = f_write (&File, pData, nLength, &nWritten) == FR_OK && nWritten == nLength;
	bOK = f_close (&File) == FR_OK && bOK;
	if (!bOK)
	{
		f_unlink (New);
		return FALSE;
	}
	f_unlink (pPath);
	return f_rename (New, pPath) == FR_OK;
}

void CKernel::RecoverFile (const char *pPath)
{
	char New[96];
	strCopy (New, pPath, sizeof New);
	strAppend (New, ".new", sizeof New);
	FILINFO Info;
	if (f_stat (New, &Info) != FR_OK)
	{
		return;
	}
	if (f_stat (pPath, &Info) == FR_OK)
	{
		f_unlink (New);				// the old file is whole; the new one may not be
	}
	else
	{
		f_rename (New, pPath);			// it was written whole, then the power went
	}
}

// The floppies and the CD in the drives, kept in macemu.cfg for the next
// start-up (only those lines: other settings are saved by Save settings).
void CKernel::RememberDisks (void)
{
	if (   strEq (m_SavedFloppy[0], m_Config.floppy[0]) && strEq (m_SavedFloppy[1], m_Config.floppy[1])
	    && strEq (m_SavedCdrom, m_Config.cdrom))
	{
		return;
	}
	strCopy (m_SavedFloppy[0], m_Config.floppy[0], sizeof m_SavedFloppy[0]);
	strCopy (m_SavedFloppy[1], m_Config.floppy[1], sizeof m_SavedFloppy[1]);
	strCopy (m_SavedCdrom, m_Config.cdrom, sizeof m_SavedCdrom);

	static Config Saved;
	static char Buffer[8192];
	Saved.setDefaults ();
	FIL File;
	if (f_open (&File, CONFIG_FILE, FA_READ | FA_OPEN_EXISTING) == FR_OK)
	{
		UINT nRead = 0;
		f_read (&File, Buffer, sizeof Buffer - 1, &nRead);
		f_close (&File);
		Saved.parse (Buffer, nRead);
	}
	strCopy (Saved.floppy[0], m_Config.floppy[0], Config::kPath);
	strCopy (Saved.floppy[1], m_Config.floppy[1], Config::kPath);
	strCopy (Saved.cdrom, m_Config.cdrom, Config::kPath);
	u32 nLength = Saved.serialize (Buffer, sizeof Buffer);
	WriteFileSafely (CONFIG_FILE, Buffer, nLength);
}

// A new, empty, initialized floppy ("Blank 1.dsk" ...) in the disk folder,
// put in a drive (the first empty one; drive 2 when both are full).
bool CKernel::newFloppy (bool bHighDensity, char *pName, unsigned nMax)
{
	u32 nBytes = bHighDensity ? 1474560 : 819200;
	if (nBytes > MaxFloppyBytes ())
	{
		return false;
	}
	char Name[32];
	char Path[160];
	FILINFO Info;
	unsigned n = 1;
	for (; n < 1000; n++)
	{
		strCopy (Name, "Blank ", sizeof Name);
		appendUint (Name, n, sizeof Name);
		strAppend (Name, ".dsk", sizeof Name);
		strCopy (Path, DRIVE "/", sizeof Path);
		strAppend (Path, m_Config.diskDir, sizeof Path);
		strAppend (Path, "/", sizeof Path);
		strAppend (Path, Name, sizeof Path);
		if (f_stat (Path, &Info) != FR_OK)
		{
			break;
		}
	}
	char Volume[16] = "Blank ";
	appendUint (Volume, n, sizeof Volume);

	u8 *pImage = new u8[nBytes];
	if (pImage == 0)
	{
		return false;
	}
	bool bOK = macemu::hfsMakeVolume (pImage, nBytes, Volume, 0, 0, MacTime ());
	FIL File;
	if (bOK)
	{
		char Dir[96] = DRIVE "/";
		strAppend (Dir, m_Config.diskDir, sizeof Dir);
		f_mkdir (Dir);
		bOK = f_open (&File, Path, FA_WRITE | FA_CREATE_NEW) == FR_OK;
	}
	if (bOK)
	{
		UINT nWritten = 0;
		bOK = f_write (&File, pImage, nBytes, &nWritten) == FR_OK && nWritten == nBytes;
		bOK = f_close (&File) == FR_OK && bOK;
		if (!bOK)
		{
			f_unlink (Path);
		}
	}
	delete [] pImage;
	if (!bOK)
	{
		return false;
	}

	int nDrive = m_pFloppy[0] == 0 ? 0 : 1;
	if (!insertDisk (nDrive, Name))
	{
		return false;
	}
	strCopy (pName, Name, nMax);
	return true;
}

void CKernel::saveAndReboot (void)
{
	PauseEmu ();					// for good
	saveSettings ();
	UpdateFirmwareConfig ();
	m_pMachine->flushDisks ();
	SavePRAM ();
	ShowMessage ("Restarting with the new settings...", "");
	Present ();
	m_Timer.MsDelay (200);
	reboot ();
}

void CKernel::interruptButton (void)
{
	PostEvent (EvInterrupt, 0);
}

void CKernel::settingsChanged (void)
{
	ApplyPalette ();
	m_Layout = layoutFor (m_Surface, m_nVideoWidth ? m_nVideoWidth : 512, m_nVideoHeight ? m_nVideoHeight : 342, m_Config.scale);
	m_Layout.scanlines = m_Config.sharp == 2;
	fillRect (m_Surface, 0, 0, m_Surface.width, m_Surface.height, ColBorder);
	m_bForceRedraw = TRUE;

	if (m_Config.sound && (m_pSound == 0 || !strEqNoCase (m_SoundOutput, m_Config.soundDevice)))
	{
		m_bUSBSoundFailed = FALSE;
		SetupSound ();
	}
}

bool CKernel::saveSettings (void)
{
	static char Buffer[4096];
	u32 nLength = m_Config.serialize (Buffer, sizeof Buffer);

	if (!WriteFileSafely (CONFIG_FILE, Buffer, nLength))
	{
		return false;
	}
	strCopy (m_SavedFloppy[0], m_Config.floppy[0], sizeof m_SavedFloppy[0]);
	strCopy (m_SavedFloppy[1], m_Config.floppy[1], sizeof m_SavedFloppy[1]);
	strCopy (m_SavedCdrom, m_Config.cdrom, sizeof m_SavedCdrom);
	return true;
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
