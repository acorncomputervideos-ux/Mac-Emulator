// Mac Emulator for Raspberry Pi (Circle bare-metal kernel).

#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/bcmframebuffer.h>
#include <circle/chargenerator.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/cputhrottle.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/net/netsubsystem.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/types.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>

#include "storage.h"
#include "webserver.h"
#include "ethertunnel.h"
#include "usbpowerkey.h"
#include "../core/mac.h"
#include "../core/macii.h"
#include "../core/quadra.h"
#include "../frontend/config.h"
#include "../frontend/osd.h"
#include "../frontend/video.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CCircleFont : public macemu::Font
{
public:
	int width (void) const override		{ return (int) m_Generator.GetCharWidth (); }
	int height (void) const override	{ return (int) m_Generator.GetCharHeight (); }
	bool pixel (char chChar, int nPosX, int nPosY) const override
	{
		return m_Generator.GetPixel (chChar, (unsigned) nPosX, (unsigned) nPosY);
	}

private:
	CCharGenerator m_Generator;
};

#ifdef ARM_ALLOW_MULTI_CORE
class CKernel;
// Core 1 runs the Mac; cores 2 and 3 are not used yet.
class CEmuCores : public CMultiCoreSupport
{
public:
	CEmuCores (CKernel *pKernel) : CMultiCoreSupport (CMemorySystem::Get ()), m_pKernel (pKernel) {}
	void Run (unsigned nCore) override;
private:
	CKernel *m_pKernel;
};
#endif

class CKernel : public macemu::OsdHost, public CWebServerHost
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

	void EmuCoreRun (void);			// the Mac's own core (core 1)

	// macemu::OsdHost
	int listFiles (FileKind Kind, char (*ppNames)[kNameLen], int nMax) override;
	void saveAndReboot (void) override;
	const char *networkLine (int nLine) override	{ return m_NetLine[nLine < 0 || nLine > 2 ? 0 : nLine]; }
	bool insertDisk (int nDrive, const char *pName) override;
	bool InsertDiskPaused (int nDrive, const char *pName);
	void ejectDisk (int nDrive) override;
	const char *diskName (int nDrive) override	{ return m_FloppyName[nDrive]; }
	bool insertCd (const char *pName) override;
	const char *cdName (void) override		{ return m_pCdrom != 0 ? m_Config.cdrom : ""; }
	void resetMachine (void) override;
	void interruptButton (void) override;
	void settingsChanged (void) override;
	bool saveSettings (void) override;
	bool screenshot (void) override;
	void powerOff (void) override;
	const char *infoLine (int nLine) override	{ return m_InfoLine[nLine ? 1 : 0]; }

	// CWebServerHost
	boolean FileInUse (const char *pPath) override;
	const char *DiskDir (void) override		{ return m_Config.diskDir; }
	const char *Version (void) override		{ return "built " __DATE__ " " __TIME__; }
	macemu::u32 MacTime (void) override		{ return m_pMachine->clockChip ().seconds; }
	macemu::u32 MaxFloppyBytes (void) override	{ return m_pQuadra != 0 ? 1474560 : 819200; }

private:
	boolean LoadConfig (void);
	boolean LoadROM (void);
	boolean SetupDisplay (int nMacWidth, int nMacHeight);
	void SetupSound (void);
	void UpdateFirmwareConfig (void);
	void AttachDisks (void);
	void AttachHardDisks (void);
	void LoadPRAM (void);
	void SavePRAM (void);
	void StartMacEthernet (void);
	void LoadRootCertificates (void);
	void NewNetSubSystem (boolean bWiFi);
	void ApplyPalette (void);
	void Fatal (const char *pLine1, const char *pLine2 = "", const char *pLine3 = "");

	void UpdateUSB (void);
	void ProcessInput (void);
	void HandleKey (u8 ucUsage, boolean bDown);
	void HandleModifier (u8 ucBit, boolean bDown);
	void MacKey (int nKey, boolean bDown);
	void ReleaseAllKeys (void);
	void QueueSound (void);
	void ExecuteFrame (void);		// one emulated frame, with queued input first
	void PauseEmu (void);			// stop the Mac's core at a frame boundary
	void ResumeEmu (void);
	enum TEmuEvent { EvKey, EvMouseMove, EvMouseButton, EvInterrupt };
	void PostEvent (TEmuEvent Type, int a, int b = 0);
	void DrainEvents (void);
	void UpdateScreen (void);
	void Present (void);
	void BootLog (boolean bForce = FALSE);
	void DrawMac (boolean bForce);
	void ShowMessage (const char *pLine1, const char *pLine2);
	void HandlePowerKey (void);
	boolean HasPowerButton (void);
	int ScanWiFi (char (*ppNames)[kNameLen], int nMax);
	void ShowEmergency (const char *pTitle);
	static void PowerKeyHandler (void);
	static void WatchdogTimer (TKernelTimerHandle hTimer, void *pParam, void *pContext);
	static void PanicHandler (void);
	void Housekeeping (void);
	void UpdateInfo (void);
	void StartNetwork (void);
	void StartEthernet (void);
	void StartWiFi (void);
	boolean WriteWiFiConfig (const char *pPath);
	void UpdateNetwork (void);
	void WaitUntil (::u64 nUntil);

	static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);
	static void MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY,
					int nWheelMove);
	static void MouseRemovedHandler (CDevice *pDevice, void *pContext);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CScheduler		m_Scheduler;
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;
	CCPUThrottle		m_CPUThrottle;
	FATFS			m_FileSystem;

	CBcmFrameBuffer		*m_pFrameBuffer;
	volatile u32		*m_pFrameBufferPixels;	// device memory: aligned 32-bit stores only
	unsigned		m_nFrameBufferPitch;	// in pixels
	unsigned		m_nDisplayWidth;	// the monitor's mode
	unsigned		m_nDisplayHeight;
	CSoundBaseDevice	*m_pSound;
	CUSBKeyboardDevice * volatile m_pKeyboard;
	CMouseDevice * volatile	m_pMouse;
	CCircleFont		m_Font;

	macemu::Config		m_Config;
	macemu::Emulator	*m_pMachine;
	macemu::Machine		*m_pPlus;
	macemu::MacII		*m_pMacII;
	macemu::Quadra		*m_pQuadra;
	macemu::Osd		*m_pOsd;
	macemu::Surface		m_Surface;
	macemu::Layout		m_Layout;
	u8			*m_pROM;
	u32			m_nROMSize;
	u8			*m_pRAM;
	u8			*m_pPrevScreen;
	boolean			m_bForceRedraw;
	boolean			m_bHaveVideo;
	int			m_nVideoWidth;
	int			m_nVideoHeight;
	macemu::u32		m_nClutVersion;

	CMemoryStorage		*m_pFloppy[2];
	char			m_FloppyName[2][kNameLen];
	CFileStorage		*m_pHardDisk[macemu::Scsi::kTargets];
	char			m_HardDiskPath[macemu::Scsi::kTargets][160];	// without the drive
	CFileStorage		*m_pCdrom;		// the CD-ROM image in the drive


	// input, filled in interrupt context
	enum { KeyQueueSize = 64 };
	volatile u16		m_KeyQueue[KeyQueueSize];	// bit 15 down, bit 14 modifier
	volatile unsigned	m_nKeyIn;
	volatile unsigned	m_nKeyOut;
	u8			m_LastModifiers;
	u8			m_LastKeys[6];
	volatile int		m_nMouseX;
	volatile int		m_nMouseY;
	volatile unsigned	m_nMouseButtons;
	int			m_nMouseRemX;
	int			m_nMouseRemY;
	boolean			m_bMacKeyDown[128];
	boolean			m_bCapsLock;
	boolean			m_bTurbo;
	volatile boolean	m_bPowerKey;		// a power key went down
	unsigned		m_nMacPowerKeyTicks;	// when the Mac was last sent its power key
	volatile unsigned	m_nLoops;		// main loop heartbeat, for the freeze detector
	unsigned		m_nWatchedLoops;
	unsigned		m_nStuckSeconds;
	boolean			m_bEmergencyShown;
	char			m_LogTail[2048];	// end of what went to macemu.log

	// sound
	unsigned		m_nSampleRate;
	unsigned		m_nSoundFraction;

	// bookkeeping
	unsigned		m_nFrames;
	u32			m_nLastActivity;
	unsigned		m_nIdleFrames;
	unsigned		m_nScreenshot;
	volatile unsigned	m_nEmulatedFrames;	// counted on the Mac's core
	unsigned		m_nEmulatedSeen;	// value at the last speed reading

	// The Mac on its own core: pause handshake, input queue, sound ring
#ifdef ARM_ALLOW_MULTI_CORE
	CEmuCores		*m_pCores;
#endif
	volatile boolean	m_bEmuCoreRunning;
	volatile unsigned	m_nPauseRequests;
	volatile unsigned	m_nPauseSeq;
	volatile unsigned	m_nPauseAck;
	enum { EventQueueSize = 256, SoundRingSize = 16384 };
	struct TEvent { u8 nType; s16 a, b; };
	TEvent			m_Events[EventQueueSize];
	volatile unsigned	m_nEventIn, m_nEventOut;
	s16			m_SoundRing[SoundRingSize];
	volatile unsigned	m_nSoundIn, m_nSoundOut;
	volatile unsigned	m_nSoundRate;
	u64			m_nStatsTime;
	unsigned		m_nSpeedPercent;
	char			m_InfoLine[2][64];
	char			m_NetLine[3][64];
	char			m_ROMName[32];

	// network
	CNetSubSystem		*m_pNet;
	CWebServer		*m_pWebServer;
	CBcm4343Device		*m_pWLAN;
	CWPASupplicant		*m_pWPASupplicant;
	boolean			m_bWiFi;		// Wi-Fi chosen
	boolean			m_bWiFiPending;		// start it once the Mac is running
	unsigned		m_nNetStartTicks;
	boolean			m_bWrongPassword;	// seen in the WPA log
	char			m_WpaLast[64];		// the WPA supplicant's latest message, for the menu
	unsigned		m_nLogBytes;		// written to macemu.log so far
	boolean			m_bNetRunning;
	unsigned		m_nNetTicks;
	CEtherTunnel		*m_pEther;		// the Mac's Ethernet, or 0

	static CKernel *s_pThis;
};

#endif
