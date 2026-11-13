// Save states: the whole machine written out and read back, so a Mac can
// be frozen (hibernated) and carry on later exactly where it was.
//
// Every part has a state(StateIO &) that lists its fields; the same
// function saves and loads.  Pointers (to the ROM, the disks, the host)
// are not saved: the machine is set up as before, then its state loaded
// over it.
//
// kStateFormat names the format: a state saved by any build with the same
// number loads.  Change any state() (a field more, one fewer, another
// size), or the 68k code the emulator puts in the Mac, and it must go up:
// tests/hwtest.cpp checks the layout and fails until it does.

#ifndef MACEMU_STATE_H
#define MACEMU_STATE_H

#include "types.h"

namespace macemu {

enum { kStateFormat = 1 };

class StateIO {
public:
    virtual ~StateIO() {}
    bool ok = true;

    bool saving() const { return isSaving; }
    // Only the layout (field sizes, in order), no data: for the check above.
    virtual bool describing() const { return false; }

    // The bytes of a plain value or array (no pointers in it).
    template <class T> void v(T &x) { raw(&x, sizeof x); }
    template <class T, unsigned N> void a(T (&x)[N]) { raw(x, sizeof x); }
    void b(bool &x) { u8 t = x ? 1 : 0; raw(&t, 1); x = t != 0; }

    // A section marker: loading checks it is where it should be.
    void tag(const char *name)
    {
        char t[4] = {name[0], name[1], name[2], name[3]};
        if (isSaving) raw(t, 4);
        else {
            char r[4] = {0, 0, 0, 0};
            raw(r, 4);
            if (r[0] != t[0] || r[1] != t[1] || r[2] != t[2] || r[3] != t[3]) ok = false;
        }
    }

    // Memory (RAM, video RAM): 4 KB pages, the all-zero ones as one byte.
    void memory(u8 *p, u32 n)
    {
        if (describing()) { raw(p, n); return; }
        for (u32 at = 0; at < n && ok; at += 4096) {
            u32 len = n - at < 4096 ? n - at : 4096;
            u8 zero = 1;
            if (isSaving) {
                for (u32 i = 0; i < len; i++) if (p[at + i]) { zero = 0; break; }
                raw(&zero, 1);
                if (!zero) raw(p + at, len);
            } else {
                raw(&zero, 1);
                if (zero) for (u32 i = 0; i < len; i++) p[at + i] = 0;
                else raw(p + at, len);
            }
        }
    }

    // Saving writes these bytes out, loading reads them in; a failure
    // clears ok.
    virtual void raw(void *p, u32 n) = 0;

protected:
    explicit StateIO(bool save) : isSaving(save) {}

private:
    bool isSaving;
};

} // namespace macemu

#endif
