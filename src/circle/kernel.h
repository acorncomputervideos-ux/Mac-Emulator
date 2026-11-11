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
#include <circle/logger.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/types.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>

#include "storage.h"
#include "../core/mac.h"
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

class CKernel : public macemu::OsdHost
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

	// macemu::OsdHost
	int listDisks (char (*ppNames)[kNameLen], int nMax) override;
	bool insertDisk (int nDrive, const char *pName) override;
	void ejectDisk (int nDrive) override;
	const char *diskName (int nDrive) override	{ return m_FloppyName[nDrive]; }
	void resetMachine (void) override;
	void interruptButton (void) override;
	void settingsChanged (void) override;
	bool saveSettings (void) override;
	bool screenshot (void) override;
	const char *infoLine (int nLine) override	{ return m_InfoLine[nLine ? 1 : 0]; }

private:
	boolean LoadConfig (void);
	boolean LoadROM (void);
	boolean SetupDisplay (void);
	void SetupSound (void);
	void AttachDisks (void);
	void LoadPRAM (void);
	void SavePRAM (void);
	void ApplyPalette (void);
	void Fatal (const char *pLine1, const char *pLine2 = "", const char *pLine3 = "");

	void UpdateUSB (void);
	void ProcessInput (void);
	void HandleKey (u8 ucUsage, boolean bDown);
	void HandleModifier (u8 ucBit, boolean bDown);
	void MacKey (int nKey, boolean bDown);
	void ReleaseAllKeys (void);
	void QueueSound (void);
	void UpdateScreen (void);
	void Housekeeping (void);
	void UpdateInfo (void);

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
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;

	CBcmFrameBuffer		*m_pFrameBuffer;
	CSoundBaseDevice	*m_pSound;
	CUSBKeyboardDevice * volatile m_pKeyboard;
	CMouseDevice * volatile	m_pMouse;
	CCircleFont		m_Font;

	macemu::Config		m_Config;
	macemu::Machine		*m_pMachine;
	macemu::Osd		*m_pOsd;
	macemu::Surface		m_Surface;
	macemu::Layout		m_Layout;
	u8			*m_pROM;
	u32			m_nROMSize;
	u8			*m_pRAM;
	u8			*m_pPrevScreen;
	boolean			m_bForceRedraw;

	CMemoryStorage		*m_pFloppy[2];
	char			m_FloppyName[2][kNameLen];
	CFileStorage		*m_pHardDisk[macemu::Scsi::kTargets];

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

	// sound
	unsigned		m_nSampleRate;
	unsigned		m_nSoundFraction;

	// bookkeeping
	unsigned		m_nFrames;
	u32			m_nLastActivity;
	unsigned		m_nIdleFrames;
	unsigned		m_nScreenshot;
	unsigned		m_nEmulatedFrames;
	u64			m_nStatsTime;
	unsigned		m_nSpeedPercent;
	char			m_InfoLine[2][64];
	char			m_ROMName[32];

	static CKernel *s_pThis;
};

#endif
