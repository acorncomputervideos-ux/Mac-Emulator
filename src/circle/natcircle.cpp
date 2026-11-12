#include "natcircle.h"
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/netdevice.h>
#include <circle/sched/task.h>
#include <circle/timer.h>
#include <circle/bcmrandom.h>
#include <circle/util.h>

// Opens one TCP connection, then ends (the scheduler deletes ended tasks).
class CConnectTask : public CTask
{
public:
	CConnectTask (CNatSockets::TTcp *pTcp, const u8 *pIP, u16 nPort)
	:	m_pTcp (pTcp), m_IP (pIP), m_nPort (nPort)
	{
	}

	void Run (void) override
	{
		int nResult = m_pTcp->pSocket->Connect (m_IP, m_nPort);
		if (m_pTcp->bAbandoned)
		{
			delete m_pTcp->pSocket;		// the router gave up on it meanwhile
			delete [] m_pTcp->pPending;
			delete m_pTcp;
			return;
		}
		m_pTcp->nState = nResult == 0 ? macemu::NatSockets::Connected : macemu::NatSockets::Failed;
	}

private:
	CNatSockets::TTcp *m_pTcp;
	CIPAddress m_IP;
	u16 m_nPort;
};

CNatSockets::CNatSockets (CNetSubSystem *pNet)
:	m_pNet (pNet),
	m_nNextPort (49152)
{
	memset (m_pUdp, 0, sizeof m_pUdp);
	memset (m_pTcp, 0, sizeof m_pTcp);
	m_pBuffer = new u8[FRAME_BUFFER_SIZE];
}

CNatSockets::~CNatSockets (void)
{
	for (unsigned i = 0; i < MaxUdp; i++)
	{
		delete m_pUdp[i];
	}
	for (unsigned i = 0; i < MaxTcp; i++)
	{
		if (m_pTcp[i] != 0)
		{
			tcpClose ((int) i);
		}
	}
	delete [] m_pBuffer;
}

// ---------------------------------------------------------------- UDP

int CNatSockets::udpOpen (void)
{
	for (unsigned i = 0; i < MaxUdp; i++)
	{
		if (m_pUdp[i] != 0)
		{
			continue;
		}
		CSocket *pSocket = new CSocket (m_pNet, IPPROTO_UDP);
		for (unsigned nTry = 0; nTry < 8; nTry++)
		{
			u16 nPort = m_nNextPort++;
			if (m_nNextPort > 65000)
			{
				m_nNextPort = 49152;
			}
			if (pSocket->Bind (nPort) >= 0)
			{
				m_pUdp[i] = pSocket;
				return (int) i;
			}
		}
		delete pSocket;
		return -1;
	}
	return -1;
}

bool CNatSockets::udpSend (int h, const u8 *ip, u16 port, const u8 *data, u32 len)
{
	if (h < 0 || h >= MaxUdp || m_pUdp[h] == 0 || len == 0)
	{
		return false;
	}
	CIPAddress Dest (ip);
	return m_pUdp[h]->SendTo (data, len, MSG_DONTWAIT, Dest, port) == (int) len;
}

int CNatSockets::udpReceive (int h, u8 *buf, u32 max, u8 *ip, u16 &port)
{
	if (h < 0 || h >= MaxUdp || m_pUdp[h] == 0)
	{
		return 0;
	}
	CIPAddress From;
	u16 nPort = 0;
	int n = m_pUdp[h]->ReceiveFrom (m_pBuffer, FRAME_BUFFER_SIZE, MSG_DONTWAIT, &From, &nPort);
	if (n <= 0)
	{
		return 0;
	}
	if ((u32) n > max)
	{
		n = (int) max;
	}
	memcpy (buf, m_pBuffer, n);
	From.CopyTo (ip);
	port = nPort;
	return n;
}

void CNatSockets::udpClose (int h)
{
	if (h >= 0 && h < MaxUdp)
	{
		delete m_pUdp[h];
		m_pUdp[h] = 0;
	}
}

// ---------------------------------------------------------------- TCP

int CNatSockets::tcpOpen (const u8 *ip, u16 port)
{
	for (unsigned i = 0; i < MaxTcp; i++)
	{
		if (m_pTcp[i] != 0)
		{
			continue;
		}
		TTcp *pTcp = new TTcp;
		pTcp->pSocket = new CSocket (m_pNet, IPPROTO_TCP);
		pTcp->nState = Connecting;
		pTcp->bAbandoned = FALSE;
		pTcp->bUsed = TRUE;
		pTcp->pPending = new u8[FRAME_BUFFER_SIZE];
		pTcp->nPendingLength = pTcp->nPendingOffset = 0;
		m_pTcp[i] = pTcp;
		new CConnectTask (pTcp, ip, port);		// starts running at the next yield
		return (int) i;
	}
	return -1;
}

int CNatSockets::tcpState (int h)
{
	if (h < 0 || h >= MaxTcp || m_pTcp[h] == 0)
	{
		return Failed;
	}
	return m_pTcp[h]->nState;
}

int CNatSockets::tcpSend (int h, const u8 *data, u32 len)
{
	if (h < 0 || h >= MaxTcp || m_pTcp[h] == 0 || m_pTcp[h]->nState != Connected)
	{
		return -1;
	}
	if (len == 0)
	{
		return 0;
	}
	int n = m_pTcp[h]->pSocket->Send (data, len, MSG_DONTWAIT);
	return n < 0 ? -1 : n;
}

int CNatSockets::tcpReceive (int h, u8 *buf, u32 max)
{
	if (h < 0 || h >= MaxTcp || m_pTcp[h] == 0 || m_pTcp[h]->nState != Connected)
	{
		return -1;
	}
	TTcp *p = m_pTcp[h];
	if (p->nPendingOffset >= p->nPendingLength)
	{
		// Circle gives a whole segment at a time: take it into our own
		// buffer, so that none is lost when `max` is smaller.
		int n = p->pSocket->Receive (p->pPending, FRAME_BUFFER_SIZE, MSG_DONTWAIT);
		if (n < 0)
		{
			return -1;			// closed by the server (or broken)
		}
		if (n == 0)
		{
			return 0;
		}
		p->nPendingLength = (unsigned) n;
		p->nPendingOffset = 0;
	}
	unsigned n = p->nPendingLength - p->nPendingOffset;
	if (n > max)
	{
		n = max;
	}
	memcpy (buf, p->pPending + p->nPendingOffset, n);
	p->nPendingOffset += n;
	return (int) n;
}

void CNatSockets::tcpClose (int h)
{
	if (h < 0 || h >= MaxTcp || m_pTcp[h] == 0)
	{
		return;
	}
	TTcp *p = m_pTcp[h];
	m_pTcp[h] = 0;
	if (p->nState == Connecting)
	{
		p->bAbandoned = TRUE;			// its task is inside Connect()
		return;
	}
	delete p->pSocket;
	delete [] p->pPending;
	delete p;
}

// ---------------------------------------------------------------- other

bool CNatSockets::dnsServer (u8 *ip)
{
	const CIPAddress *pDNS = m_pNet->GetConfig ()->GetDNSServer ();
	if (pDNS == 0 || pDNS->IsNull ())
	{
		static const u8 Fallback[4] = {8, 8, 8, 8};
		memcpy (ip, Fallback, 4);
		return true;
	}
	pDNS->CopyTo (ip);
	return true;
}

macemu::u32 CNatSockets::milliseconds (void)
{
	return (macemu::u32) (CTimer::GetClockTicks64 () / 1000);
}

// Set by the NTP daemon once the network is up; before that the Pi does not
// know the date (it has no battery-backed clock).
bool CNatSockets::wallClock (macemu::u32 &unixTime)
{
	unsigned nTime = CTimer::Get ()->GetUniversalTime ();
	unixTime = nTime;
	return nTime > 1700000000u;
}

// The SoC's hardware random number generator, mixed with the cycle clock.
void CNatSockets::randomBytes (macemu::u8 *buf, macemu::u32 len)
{
	static CBcmRandomNumberGenerator Random;
	for (macemu::u32 i = 0; i < len; i += 4)
	{
		u32 v = Random.GetNumber () ^ (u32) CTimer::GetClockTicks64 () * 2654435761u;
		for (unsigned k = 0; k < 4 && i + k < len; k++)
		{
			buf[i + k] = (u8) (v >> (8 * k));
		}
	}
}
