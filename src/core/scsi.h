// NCR 5380 SCSI controller with up to seven hard disk targets.

#ifndef MACEMU_SCSI_H
#define MACEMU_SCSI_H

#include "types.h"

namespace macemu {

class Scsi {
public:
    enum { kTargets = 7 };

    Scsi();
    void reset();
    void attach(int id, Storage *disk) { disks[id] = disk; }
    Storage *disk(int id) const { return disks[id]; }

    u8 read(int reg, bool dack);
    void write(int reg, u8 v, bool dack);
    void flush();

    u32 activityCounter;

private:
    enum Phase { Free, Selected, Command, DataIn, DataOut, Status, MsgIn, MsgOut };

    Storage *disks[kTargets];
    u8 odr, icr, mode, tcr;
    bool aip, irq, dmaSend, dmaRecv;

    Phase phase;
    bool req;
    bool atnAtSelect;
    int target;
    u8 cmd[12];
    int cmdLen, cmdPos;
    u8 buf[512];
    u32 bufLen, bufPos;
    u32 lba, blocksLeft;
    bool blockIo;
    u8 status;
    u8 senseKey, senseCode;

    u8 busPhaseBits() const;
    bool phaseMatch() const { return (tcr & 7) == busPhaseBits(); }
    u8 currentIn() const;
    void setPhase(Phase p);
    void byteOut(u8 v);         // initiator -> target
    void byteInTaken();         // target -> initiator acknowledged
    void execute();
    void finish(u8 st, u8 key = 0, u8 code = 0);
    void reply(u32 len, u32 alloc);
    bool loadBlock();
    void storeBlock();
};

} // namespace macemu

#endif
