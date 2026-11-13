#ifndef SHIM_NET_H
#define SHIM_NET_H
#define FRAME_BUFFER_SIZE 1600
class CNetSubSystem { public: bool IsRunning (void) const { return true; } };
class CString {
public:
	operator const char * (void) const { return m_Text; }
	char m_Text[16] = "127.0.0.1";
};
class CIPAddress { public: void Format (CString *) const {} };
#endif
