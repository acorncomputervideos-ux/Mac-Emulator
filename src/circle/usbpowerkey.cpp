// Power keys on USB keyboards (see usbpowerkey.h).

#include "usbpowerkey.h"
#include <circle/usb/usbhid.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/logger.h>
#include <circle/util.h>

static const char From[] = "powerkey";

TPowerKeyHandler *CUSBPowerKeyDevice::s_pHandler = 0;

// Usages that mean "power": System Power Down, Keyboard Power, Consumer Power.
static boolean IsPowerUsage (u32 nUsage)
{
	return nUsage == 0x10081 || nUsage == 0x70066 || nUsage == 0xC0030;
}

// Walks the short items of a HID report descriptor.
struct THIDItem { u8 nTag; u8 nType; u32 nData; unsigned nSize; };

static boolean NextItem (const u8 *&p, const u8 *pEnd, THIDItem &Item)
{
	if (p >= pEnd)
	{
		return FALSE;
	}
	u8 b = *p++;
	if (b == 0xFE)				// long item: skip it
	{
		if (p + 2 > pEnd) return FALSE;
		unsigned n = p[0];
		p += 2 + n;
		Item.nTag = 0xFF; Item.nType = 3; Item.nData = 0; Item.nSize = 0;
		return p <= pEnd;
	}
	static const unsigned Sizes[] = {0, 1, 2, 4};
	Item.nSize = Sizes[b & 3];
	Item.nType = (b >> 2) & 3;
	Item.nTag = b >> 4;
	if (p + Item.nSize > pEnd) return FALSE;
	Item.nData = 0;
	for (unsigned i = 0; i < Item.nSize; i++)
	{
		Item.nData |= (u32) p[i] << (8 * i);
	}
	p += Item.nSize;
	return TRUE;
}

boolean CUSBPowerKeyDevice::Wanted (const u8 *pDesc, unsigned nLength)
{
	const u8 *p = pDesc, *pEnd = pDesc + nLength;
	THIDItem Item;
	u32 nPage = 0;
	boolean bPower = FALSE;
	while (NextItem (p, pEnd, Item))
	{
		if (Item.nType == 1 && Item.nTag == 0)		// Usage Page
		{
			nPage = Item.nData;
		}
		else if (Item.nType == 2 && (Item.nTag == 0 || Item.nTag == 1 || Item.nTag == 2))
		{
			u32 nUsage = Item.nSize == 4 ? Item.nData : (nPage << 16 | Item.nData);
			if (nUsage == 0x10002 || nUsage == 0x10004 || nUsage == 0x10005)
			{
				return FALSE;			// mouse, joystick, gamepad
			}
			if (Item.nTag == 0 && IsPowerUsage (nUsage))
			{
				bPower = TRUE;
			}
			if (   Item.nTag == 1			// Usage Minimum
			    && (nPage == 1 || nPage == 7 || nPage == 0x0C))
			{
				bPower = TRUE;			// a range that may hold one: check in Decode
			}
		}
	}
	return bPower;
}

CUSBPowerKeyDevice::CUSBPowerKeyDevice (CUSBFunction *pFunction)
:	CUSBHIDDevice (pFunction, ReportBytes),
	m_nFields (0),
	m_bReportIDs (FALSE),
	m_bDown (FALSE)
{
}

CUSBPowerKeyDevice::~CUSBPowerKeyDevice (void)
{
}

boolean CUSBPowerKeyDevice::Configure (void)
{
	TUSBHIDDescriptor *pHIDDesc = (TUSBHIDDescriptor *) GetDescriptor (DESCRIPTOR_HID);
	if (pHIDDesc == 0 || pHIDDesc->wReportDescriptorLength == 0)
	{
		ConfigurationError (From);
		return FALSE;
	}

	unsigned nLength = pHIDDesc->wReportDescriptorLength;
	u8 *pDesc = new u8[nLength];
	if (   pDesc == 0
	    || GetHost ()->GetDescriptor (GetEndpoint0 (), pHIDDesc->bReportDescriptorType,
					  DESCRIPTOR_INDEX_DEFAULT, pDesc, nLength,
					  REQUEST_IN | REQUEST_TO_INTERFACE, GetInterfaceNumber ())
	       != (int) nLength)
	{
		delete [] pDesc;
		return FALSE;
	}

	boolean bOK = Decode (pDesc, nLength);
	delete [] pDesc;
	if (!bOK)
	{
		return FALSE;
	}

	if (!CUSBHIDDevice::ConfigureHID (ReportBytes))
	{
		return FALSE;
	}

	for (unsigned i = 0; i < m_nFields; i++)
	{
		CLogger::Get ()->Write (From, LogNotice, "Power key: report %u, bit %u, %s",
					m_Field[i].nReportID, m_Field[i].nBitOffset,
					m_Field[i].bArray ? "array" : "button");
	}

	return StartRequest ();
}

// Find the input fields that can carry a power usage.
boolean CUSBPowerKeyDevice::Decode (const u8 *pDesc, unsigned nLength)
{
	const u8 *p = pDesc, *pEnd = pDesc + nLength;
	THIDItem Item;

	u32 nPage = 0, nReportSize = 0, nReportCount = 0;
	int nLogicalMin = 0;
	u8 nReportID = 0;
	enum { MaxUsages = 32, MaxIDs = 256 };
	u32 Usages[MaxUsages];
	unsigned nUsages = 0;
	u32 nUsageMin = 0, nUsageMax = 0;
	boolean bRange = FALSE;
	static unsigned BitPos[MaxIDs];
	memset (BitPos, 0, sizeof BitPos);

	while (NextItem (p, pEnd, Item))
	{
		if (Item.nType == 1)				// global
		{
			switch (Item.nTag)
			{
			case 0: nPage = Item.nData; break;
			case 1: nLogicalMin = Item.nSize == 1 ? (int) (s8) Item.nData
					     : Item.nSize == 2 ? (int) (s16) Item.nData : (int) Item.nData; break;
			case 7: nReportSize = Item.nData; break;
			case 8: nReportID = (u8) Item.nData; m_bReportIDs = TRUE; break;
			case 9: nReportCount = Item.nData; break;
			default: break;
			}
		}
		else if (Item.nType == 2)			// local
		{
			u32 nUsage = Item.nSize == 4 ? Item.nData : (nPage << 16 | Item.nData);
			if (Item.nTag == 0 && nUsages < MaxUsages) Usages[nUsages++] = nUsage;
			if (Item.nTag == 1) { nUsageMin = nUsage; bRange = TRUE; }
			if (Item.nTag == 2) { nUsageMax = nUsage; }
		}
		else if (Item.nType == 0)			// main
		{
			if (Item.nTag == 8)			// Input
			{
				boolean bConstant = Item.nData & 1;
				boolean bVariable = Item.nData & 2;
				unsigned nStart = BitPos[nReportID];
				if (!bConstant && bVariable)
				{
					for (unsigned i = 0; i < nReportCount; i++)
					{
						u32 nUsage = i < nUsages ? Usages[i]
							   : bRange ? nUsageMin + i
							   : nUsages ? Usages[nUsages - 1] : 0;
						if (IsPowerUsage (nUsage) && m_nFields < MaxFields)
						{
							TField &F = m_Field[m_nFields++];
							F.nReportID = nReportID;
							F.bArray = FALSE;
							F.nBitOffset = nStart + i * nReportSize;
							F.nBitSize = nReportSize;
							F.nCount = 1;
						}
					}
				}
				else if (!bConstant && bRange)	// array of usages
				{
					boolean bHas = FALSE;
					for (u32 u = nUsageMin; u <= nUsageMax && u - nUsageMin < 0x10000; u++)
					{
						bHas |= IsPowerUsage (u);
					}
					if (bHas && m_nFields < MaxFields)
					{
						TField &F = m_Field[m_nFields++];
						F.nReportID = nReportID;
						F.bArray = TRUE;
						F.nBitOffset = nStart;
						F.nBitSize = nReportSize;
						F.nCount = nReportCount;
						F.nUsageMin = nUsageMin;
						F.nLogicalMin = nLogicalMin;
					}
				}
				BitPos[nReportID] = nStart + nReportSize * nReportCount;
			}
			nUsages = 0;				// local items end here
			bRange = FALSE;
			nUsageMin = nUsageMax = 0;
		}
	}

	return m_nFields > 0;
}

static unsigned GetBits (const u8 *pData, unsigned nBytes, unsigned nOffset, unsigned nSize)
{
	unsigned nValue = 0;
	for (unsigned i = 0; i < nSize && i < 32; i++)
	{
		unsigned nBit = nOffset + i;
		if (nBit / 8 < nBytes && (pData[nBit / 8] >> (nBit % 8) & 1))
		{
			nValue |= 1u << i;
		}
	}
	return nValue;
}

void CUSBPowerKeyDevice::ReportHandler (const u8 *pReport, unsigned nReportSize)
{
	if (pReport == 0 || nReportSize == 0)
	{
		return;
	}

	u8 nID = 0;
	if (m_bReportIDs)
	{
		nID = pReport[0];
		pReport++;
		nReportSize--;
	}

	boolean bSeen = FALSE, bDown = FALSE;
	for (unsigned i = 0; i < m_nFields; i++)
	{
		const TField &F = m_Field[i];
		if (F.nReportID != nID)
		{
			continue;
		}
		bSeen = TRUE;
		for (unsigned j = 0; j < F.nCount; j++)
		{
			unsigned v = GetBits (pReport, nReportSize, F.nBitOffset + j * F.nBitSize, F.nBitSize);
			if (F.bArray)
			{
				bDown |= v != 0 && IsPowerUsage (F.nUsageMin + (u32) ((int) v - F.nLogicalMin));
			}
			else
			{
				bDown |= v != 0;
			}
		}
	}

	if (!bSeen)
	{
		return;				// another report of this interface
	}

	if (bDown && !m_bDown && s_pHandler != 0)
	{
		(*s_pHandler) ();
	}
	m_bDown = bDown;
}

// Called by Circle's USB device factory (hook added by tools/circle-usb-hook.sh)
// for HID interfaces that are not boot keyboards or mice.
CUSBFunction *macemu_usb_hid_hook (CUSBFunction *pParent)
{
	CUSBFunction TempFunction (pParent);
	TUSBHIDDescriptor *pHIDDesc = (TUSBHIDDescriptor *) TempFunction.GetDescriptor (DESCRIPTOR_HID);
	if (pHIDDesc == 0 || pHIDDesc->wReportDescriptorLength == 0)
	{
		return 0;
	}

	unsigned nLength = pHIDDesc->wReportDescriptorLength;
	u8 *pDesc = new u8[nLength];
	boolean bWanted =    pDesc != 0
			  && TempFunction.GetHost ()->GetDescriptor (
				TempFunction.GetEndpoint0 (), pHIDDesc->bReportDescriptorType,
				DESCRIPTOR_INDEX_DEFAULT, pDesc, nLength,
				REQUEST_IN | REQUEST_TO_INTERFACE, TempFunction.GetInterfaceNumber ())
			     == (int) nLength
			  && CUSBPowerKeyDevice::Wanted (pDesc, nLength);
	delete [] pDesc;

	return bWanted ? new CUSBPowerKeyDevice (pParent) : 0;
}
