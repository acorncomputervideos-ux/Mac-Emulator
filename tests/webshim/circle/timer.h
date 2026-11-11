#ifndef SHIM_TIMER_H
#define SHIM_TIMER_H
#define HZ 100
class CTimer { public: static CTimer *Get(); unsigned GetTicks(); };
#endif
