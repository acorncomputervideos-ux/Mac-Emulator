// The Mac's Ethernet, carried in UDP datagrams over the Pi's own network
// (cable or Wi-Fi): every frame the Mac sends goes out as one datagram, to
// the peer it is for, or to everyone on the local network.  Other Pis
// running this emulator, and Basilisk II or SheepShaver with their "UDP
// tunnel" turned on (same port, 6066 by default), see each other in the
// Chooser and share files and printers over AppleTalk.
//
// Following the UDP tunnel of Basilisk II, the Mac's Ethernet address is
// 'B' '2' and the Pi's IPv4 address, so a peer can tell where to send a
// frame for it from the address alone.
//
// The Mac (on core 1) and the network (on core 0) meet in two rings.

#ifndef _macemu_ethertunnel_h
#define _macemu_ethertunnel_h

#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/net/ipaddress.h>
#include <circle/types.h>
#include "../core/sonic.h"
#include "../frontend/natrouter.h"
#include "../frontend/httpsproxy.h"
#include "natcircle.h"

class CEtherTunnel : public macemu::EtherLink
{
public:
	CEtherTunnel (void);
	~CEtherTunnel (void);

	// Core 0.  pLastIP: the address the Pi had last time (4 bytes) or 0,
	// for the Mac's Ethernet address before the network is up.
	void SetLastIP (const u8 *pLastIP);
	// Once the network is up.  nPort: the AppleTalk tunnel's UDP port, or 0
	// for none.  pPeer: a host that also gets every broadcast ("a.b.c.d"),
	// or 0.  bInternet: route the Mac's TCP/IP through the Pi (NatRouter).
	// bHttps: the HTTPS helper for the Mac's web browsers (with bInternet).
	boolean Start (CNetSubSystem *pNet, u16 nPort, const char *pPeer = 0, boolean bInternet = FALSE,
		       boolean bHttps = FALSE);
	unsigned LoadRoots (const char *pPEM, unsigned nLength);	// root certificates, for the HTTPS helper
	macemu::HttpsProxy *Proxy (void) const	{ return m_pProxy; }
	void Update (void);				// move frames; call often
	boolean Running (void) const		{ return m_pNet != 0; }
	boolean Internet (void) const		{ return m_pRouter != 0; }
	unsigned Connections (void) const	{ return m_pRouter != 0 ? m_pRouter->connections () : 0; }
	unsigned Sent (void) const		{ return m_nSent; }
	unsigned Received (void) const		{ return m_nReceived; }

	// EtherLink, for the Mac (core 1)
	void etherAddress (u8 mac[6]) override;
	void etherSend (const u8 *pFrame, u32 nLength) override;
	u32 etherReceive (u8 *pFrame) override;

private:
	enum
	{
		MaxFrame = 1514,		// a whole Ethernet frame
		TunnelMax = 1472,		// what fits one datagram (Circle sends no IP fragments)
		TxSlots = 32,
		RxSlots = 48,
		Peers = 16
	};

	struct TFrame
	{
		u16 nLength;
		u8 Data[MaxFrame];
	};

	struct TPeer
	{
		u8 MAC[6];
		u8 IP[4];
		unsigned nAge;
	};

	void Learn (const u8 *pMAC, const u8 *pIP);
	boolean PushRx (const u8 *pFrame, unsigned nLength);
	boolean PeerFor (const u8 *pMAC, u8 *pIP);

	CNetSubSystem *m_pNet;
	CSocket *m_pSocket;			// the AppleTalk tunnel, or 0
	CNatSockets *m_pNatSockets;
	macemu::HttpsProxy *m_pProxy;		// the HTTPS helper, or 0
	macemu::NatRouter *m_pRouter;		// internet for the Mac, or 0
	u16 m_nPort;
	u8 m_OwnIP[4];
	u8 m_PeerIP[4];
	boolean m_bHavePeer;
	volatile boolean m_bHaveIP;
	u8 m_LastIP[4];
	boolean m_bHaveLastIP;
	u8 m_Fallback[6];

	TFrame *m_pTx;				// core 1 -> core 0
	volatile unsigned m_nTxIn, m_nTxOut;
	TFrame *m_pRx;				// core 0 -> core 1
	volatile unsigned m_nRxIn, m_nRxOut;
	u8 *m_pBuffer;

	TPeer m_Peers[Peers];
	unsigned m_nClock;
	volatile unsigned m_nSent, m_nReceived;
};

#endif
