// NatSockets on Circle's network stack, for the Mac's internet router.
// Circle's TCP Connect() blocks, so each connection is opened by a small
// task of its own; everything else is non-blocking.

#ifndef _macemu_natcircle_h
#define _macemu_natcircle_h

#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/types.h>
#include "../frontend/natrouter.h"

class CNatSockets : public macemu::NatSockets
{
public:
	CNatSockets (CNetSubSystem *pNet);
	~CNatSockets (void);

	int udpOpen (void) override;
	bool udpSend (int h, const macemu::u8 *ip, macemu::u16 port, const macemu::u8 *data, macemu::u32 len) override;
	int udpReceive (int h, macemu::u8 *buf, macemu::u32 max, macemu::u8 *ip, macemu::u16 &port) override;
	void udpClose (int h) override;

	int tcpOpen (const macemu::u8 *ip, macemu::u16 port) override;
	int tcpState (int h) override;
	int tcpSend (int h, const macemu::u8 *data, macemu::u32 len) override;
	int tcpReceive (int h, macemu::u8 *buf, macemu::u32 max) override;
	void tcpClose (int h) override;

	bool dnsServer (macemu::u8 *ip) override;
	macemu::u32 milliseconds (void) override;
	bool wallClock (macemu::u32 &unixTime) override;
	void randomBytes (macemu::u8 *buf, macemu::u32 len) override;

	struct TTcp
	{
		CSocket *pSocket;
		volatile int nState;		// NatSockets::Connecting, Connected or Failed
		volatile boolean bAbandoned;	// closed while still connecting: the task cleans up
		boolean bUsed;
		u8 *pPending;			// received but not yet taken (Circle hands out whole segments)
		unsigned nPendingLength, nPendingOffset;
	};

private:
	enum { MaxUdp = 16, MaxTcp = 24 };

	CNetSubSystem *m_pNet;
	CSocket *m_pUdp[MaxUdp];
	TTcp *m_pTcp[MaxTcp];
	u16 m_nNextPort;
	u8 *m_pBuffer;
};

#endif
