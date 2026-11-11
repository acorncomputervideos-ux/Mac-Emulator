#ifndef SHIM_LOGGER_H
#define SHIM_LOGGER_H
enum TLogSeverity { LogPanic, LogError, LogWarning, LogNotice, LogDebug };
class CLogger { public: static CLogger *Get(); void Write(const char *src, TLogSeverity, const char *fmt, ...); };
#endif
