// Power keys on USB keyboards: the Raspberry Pi 400's Fn+F10, and the
// power/sleep keys that many keyboards report on a second "system control"
// or "consumer control" HID interface.  Circle has no driver for those
// interfaces; a small hook in its USB device factory (tools/circle-usb-hook.sh)
// lets this one claim them.

#ifndef _macemu_usbpowerkey_h
#define _macemu_usbpowerkey_h

#include <circle/usb/usbhiddevice.h>
#include <circle/types.h>

typedef void TPowerKeyHandler (void);

class CUSBPowerKeyDevice : public CUSBHIDDevice
{
public:
	CUSBPowerKeyDevice (CUSBFunction *pFunction);
	~CUSBPowerKeyDevice (void);

	boolean Configure (void);

	// Called in interrupt context when a power key goes down.
	static void RegisterHandler (TPowerKeyHandler *pHandler)	{ s_pHandler = pHandler; }

	// Does this report descriptor have a power key and nothing that
	// another driver should have (mouse, joystick, gamepad)?
	static boolean Wanted (const u8 *pDesc, unsigned nLength);

private:
	void ReportHandler (const u8 *pReport, unsigned nReportSize);
	boolean Decode (const u8 *pDesc, unsigned nLength);

	enum { MaxFields = 8, ReportBytes = 64 };
	struct TField
	{
		u8 nReportID;
		boolean bArray;
		unsigned nBitOffset;	// after the report ID byte
		unsigned nBitSize;
		unsigned nCount;	// arrays: number of slots
		u32 nUsageMin;		// arrays: page << 16 | usage for value nLogicalMin
		int nLogicalMin;
	};
	TField m_Field[MaxFields];
	unsigned m_nFields;
	boolean m_bReportIDs;
	boolean m_bDown;

	static TPowerKeyHandler *s_pHandler;
};

#endif
