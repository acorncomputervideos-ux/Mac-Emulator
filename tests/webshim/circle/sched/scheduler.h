#ifndef SHIM_SCHED_H
#define SHIM_SCHED_H
class CScheduler { public: static CScheduler *Get(); void MsSleep(unsigned ms); void Yield() {} };
#endif
