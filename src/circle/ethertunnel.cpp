#include "ethertunnel.h"
#include <circle/bcmpropertytags.h>
#include <circle/net/in.h>
#include <circle/synchronize.h>
#include <circle/util.h>
#include "../frontend/util.h"

CEtherTunnel::CEtherTunnel (void)
:	m_pNet (0),
	m_pSocket (0),
	m_pNatSockets (0),
	m_pRouter (0),
	m_nPort (0),
	m_bHavePeer (FALSE),
	m_bHaveIP (FALSE),
	m_bHaveLastIP (FALSE),
	m_nTxIn (0),
	m_nTxOut (0),
	m_nRxIn (0),
	m_nRxOut (0),
	m_nClock (0),
	m_nSent (0),
	m_nReceived (0)
{
	m_pTx = new TFrame[TxSlots];
	m_pRx = new TFrame[RxSlots];
	m_pBuffer = new u8[FRAME_BUFFER_SIZE];
	memset (m_Peers, 0, sizeof m_Peers);
	memset (m_OwnIP, 0, sizeof m_OwnIP);
	memset (m_LastIP, 0, sizeof m_LastIP);

	// Without any IP address: Apple's prefix and three bytes of the board's
	// serial number.
	u8 Fallback[6] = {0x08, 0x00, 0x07, 0x5A, 0x00, 0x01};
	CBcmPropertyTags Tags;
	TPropertyTagSerial Serial;
	if (Tags.GetTag (PROPTAG_GET_BOARD_SERIAL, &Serial, sizeof Serial))
	{
		Fallback[3] = (u8) (Serial.Serial[0] >> 16);
		Fallback[4] = (u8) (Serial.Serial[0] >> 8);
		Fallback[5] = (u8) Serial.Serial[0];
	}
	memcpy (m_Fallback, Fallback, sizeof m_Fallback);
}

CEtherTunnel::~CEtherTunnel (void)
{
	delete m_pRouter;
	delete m_pNatSockets;
	delete m_pSocket;
	delete [] m_pTx;
	delete [] m_pRx;
	delete [] m_pBuffer;
}

void CEtherTunnel::SetLastIP (const u8 *pLastIP)
{
	if (pLastIP != 0 && (pLastIP[0] | pLastIP[1] | pLastIP[2] | pLastIP[3]) != 0)
	{
		memcpy (m_LastIP, pLastIP, 4);
		DataMemBarrier ();
		m_bHaveLastIP = TRUE;
	}
}

boolean CEtherTunnel::Start (CNetSubSystem *pNet, u16 nPort, const char *pPeer, boolean bInternet)
{
	if (m_pNet != 0 || pNet == 0)
	{
		return m_pNet != 0;
	}
	if (pPeer != 0 && *pPeer != '\0')
	{
		m_bHavePeer = macemu::parseIPv4 (pPeer, m_PeerIP);
	}

	if (nPort != 0)
	{
		CSocket *pSocket = new CSocket (pNet, IPPROTO_UDP);
		if (pSocket->Bind (nPort) < 0 || pSocket->SetOptionBroadcast (TRUE) < 0)
		{
			delete pSocket;
		}
		else
		{
			m_pSocket = pSocket;
			m_nPort = nPort;
		}
	}
	if (bInternet)
	{
		m_pNatSockets = new CNatSockets (pNet);
		m_pRouter = new macemu::NatRouter (*m_pNatSockets);
	}

	pNet->GetConfig ()->GetIPAddress ()->CopyTo (m_OwnIP);
	DataMemBarrier ();
	m_bHaveIP = TRUE;
	m_pNet = pNet;
	return m_pSocket != 0 || m_pRouter != 0;
}

// ---------------------------------------------------------------- core 1

void CEtherTunnel::etherAddress (u8 mac[6])
{
	DataMemBarrier ();
	const u8 *pIP = m_bHaveIP ? m_OwnIP : m_bHaveLastIP ? m_LastIP : 0;
	if (pIP == 0)
	{
		memcpy (mac, m_Fallback, 6);
		return;
	}
	mac[0] = 'B';
	mac[1] = '2';
	memcpy (mac + 2, pIP, 4);
}

void CEtherTunnel::etherSend (const u8 *pFrame, u32 nLength)
{
	if (nLength < 14 || nLength > MaxFrame)
	{
		return;
	}
	unsigned nNext = (m_nTxIn + 1) % TxSlots;
	if (nNext == m_nTxOut)
	{
		return;				// the network is behind: drop
	}
	TFrame &F = m_pTx[m_nTxIn];
	memcpy (F.Data, pFrame, nLength);
	F.nLength = (u16) nLength;
	DataMemBarrier ();
	m_nTxIn = nNext;
}

u32 CEtherTunnel::etherReceive (u8 *pFrame)
{
	if (m_nRxOut == m_nRxIn)
	{
		return 0;
	}
	DataMemBarrier ();
	const TFrame &F = m_pRx[m_nRxOut];
	u32 nLength = F.nLength;
	memcpy (pFrame, F.Data, nLength);
	DataMemBarrier ();
	m_nRxOut = (m_nRxOut + 1) % RxSlots;
	return nLength;
}

// ---------------------------------------------------------------- core 0

void CEtherTunnel::Learn (const u8 *pMAC, const u8 *pIP)
{
	if (pMAC[0] & 1)
	{
		return;
	}
	TPeer *pSlot = &m_Peers[0];
	for (unsigned i = 0; i < Peers; i++)
	{
		TPeer &P = m_Peers[i];
		if (memcmp (P.MAC, pMAC, 6) == 0)
		{
			pSlot = &P;
			break;
		}
		if (P.nAge < pSlot->nAge)
		{
			pSlot = &P;		// the oldest (or an empty one)
		}
	}
	memcpy (pSlot->MAC, pMAC, 6);
	memcpy (pSlot->IP, pIP, 4);
	pSlot->nAge = ++m_nClock;
}

boolean CEtherTunnel::PeerFor (const u8 *pMAC, u8 *pIP)
{
	if (pMAC[0] & 1)
	{
		return FALSE;			// broadcast or multicast: everyone
	}
	for (unsigned i = 0; i < Peers; i++)
	{
		if (m_Peers[i].nAge != 0 && memcmp (m_Peers[i].MAC, pMAC, 6) == 0)
		{
			memcpy (pIP, m_Peers[i].IP, 4);
			return TRUE;
		}
	}
	if (pMAC[0] == 'B' && pMAC[1] == '2')
	{
		memcpy (pIP, pMAC + 2, 4);	// a tunnel address says where it is
		return TRUE;
	}
	return FALSE;
}

boolean CEtherTunnel::PushRx (const u8 *pFrame, unsigned nLength)
{
	unsigned nNext = (m_nRxIn + 1) % RxSlots;
	if (nNext == m_nRxOut)
	{
		return FALSE;			// the Mac is not taking them: drop
	}
	TFrame &F = m_pRx[m_nRxIn];
	memcpy (F.Data, pFrame, nLength);
	F.nLength = (u16) nLength;
	DataMemBarrier ();
	m_nRxIn = nNext;
	return TRUE;
}

void CEtherTunnel::Update (void)
{
	if (m_pNet == 0)
	{
		return;
	}

	// Out: what the Mac sent.  TCP/IP and ARP go to the router, the rest
	// (AppleTalk) through the tunnel.
	while (m_nTxOut != m_nTxIn)
	{
		DataMemBarrier ();
		const TFrame &F = m_pTx[m_nTxOut];
		if (m_pRouter != 0 && m_pRouter->fromMac (F.Data, F.nLength))
		{
			m_nSent++;
		}
		else if (m_pSocket != 0 && F.nLength <= TunnelMax)
		{
			u8 IP[4];
			CIPAddress Dest;
			boolean bDirect = PeerFor (F.Data, IP);
			if (bDirect)
			{
				Dest.Set (IP);
			}
			else
			{
				Dest.SetBroadcast ();
			}
			if (m_pSocket->SendTo (F.Data, F.nLength, MSG_DONTWAIT, Dest, m_nPort) > 0)
			{
				m_nSent++;
			}
			if (!bDirect && m_bHavePeer)
			{
				CIPAddress Peer (m_PeerIP);	// broadcasts do not reach it
				m_pSocket->SendTo (F.Data, F.nLength, MSG_DONTWAIT, Peer, m_nPort);
			}
		}
		DataMemBarrier ();
		m_nTxOut = (m_nTxOut + 1) % TxSlots;
	}

	// The router's answers and the internet's data.
	if (m_pRouter != 0)
	{
		m_pRouter->poll ();
		u32 nLength;
		while (   (m_nRxIn + 1) % RxSlots != m_nRxOut	// room for it (the router keeps the rest)
		       && (nLength = m_pRouter->toMac (m_pBuffer)) != 0)
		{
			PushRx (m_pBuffer, nLength);
			m_nReceived++;
		}
	}

	// In: AppleTalk datagrams for the Mac.
	for (unsigned n = 0; m_pSocket != 0 && n < RxSlots; n++)
	{
		CIPAddress From;
		u16 nFromPort;
		int nLength = m_pSocket->ReceiveFrom (m_pBuffer, FRAME_BUFFER_SIZE, MSG_DONTWAIT,
						      &From, &nFromPort);
		if (nLength <= 0)
		{
			break;
		}
		if (nLength < 14 || nLength > TunnelMax || From == m_OwnIP)
		{
			continue;			// not a frame, or our own broadcast
		}
		u8 FromIP[4];
		From.CopyTo (FromIP);
		Learn (m_pBuffer + 6, FromIP);
		if (PushRx (m_pBuffer, (unsigned) nLength))
		{
			m_nReceived++;
		}
	}
}
