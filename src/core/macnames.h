// Mac file names on a host's file system: Mac Roman on the Mac, UTF-8 (a
// Unix host) or code page 850 (FatFs on the Pi) on the card, with %XX for
// what the card cannot hold.  The shared folder and the web page both use it.

#ifndef MACEMU_MACNAMES_H
#define MACEMU_MACNAMES_H

#include "types.h"

namespace macemu {

// A Mac name (`len` Mac Roman bytes) as a host name; returns its length.
int macToHostName(const u8 *mac, int len, char *out, int max, bool utf8);
// A host name as a Mac name (at most 31 bytes; longer ones are shortened
// with '#' and a hash, so different names stay different); returns its length.
int hostToMacName(const char *host, u8 *mac, bool utf8);
// Mac names compare without regard to (ASCII) case.
bool sameMacName(const u8 *a, int la, const u8 *b, int lb);

// Text (the clipboard): Mac Roman with CR line ends <-> UTF-8 with LF.
// Characters Mac Roman lacks become '?'.  Both return the length written.
u32 macTextToUtf8(const u8 *mac, u32 len, char *out, u32 max);
u32 utf8ToMacText(const char *utf8, u32 len, u8 *out, u32 max);

} // namespace macemu

#endif
