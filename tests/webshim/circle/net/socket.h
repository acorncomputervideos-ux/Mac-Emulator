#ifndef SHIM_SOCKET_H
#define SHIM_SOCKET_H
#include <circle/types.h>
#include <circle/net/netsubsystem.h>
class CSocket {
public:
    CSocket(CNetSubSystem *, int);
    explicit CSocket(int fd) : fd(fd) {}
    ~CSocket();
    int Bind(u16 port);
    int Listen(unsigned backlog = 4);
    CSocket *Accept(CIPAddress *, u16 *);
    int Send(const void *p, unsigned n, int flags);
    int Receive(void *p, unsigned n, int flags);
    int SetOptionReceiveTimeout(unsigned us);
    int SetOptionSendTimeout(unsigned us);
private:
    int fd;
};
#endif
