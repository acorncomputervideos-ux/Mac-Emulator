#ifndef SHIM_TASK_H
#define SHIM_TASK_H
#include <circle/types.h>
class CTask { public: CTask(unsigned = 0, boolean = FALSE) {} virtual ~CTask() {} virtual void Run() {} void SetName(const char *) {} };
#endif
