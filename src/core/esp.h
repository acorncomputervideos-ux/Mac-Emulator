// NCR 53C96 ("ESP") SCSI controller of the Quadra 610/650/700/800/900.
// Registers are 16 bytes apart; pseudo-DMA data goes through a separate
// window, paced by DRQ (VIA2 CA2).  Targets are disk images (Storage).

#ifndef MACEMU_ESP_H
#define MACEMU_ESP_H

#include "types.h"

namespace macemu {

class Esp {
public:
    enum { kTargets = 7 };

    Esp();
    void reset();
    void attach(int id, Storage *s) { disks[id] = s; }
    Storage *disk(int id) const { return disks[id]; }

    u8 read(int reg);
    void write(int reg, u8 v);
    u8 dmaRead();                       // pseudo-DMA data window
    void dmaWrite(u8 v);
    bool irq() const { return irqLine; }
    bool drq() const;
    void flush();

    u32 activityCounter;
    // Debug: phase, last command, CDB opcode, FIFO count, TC.
    void describe(char *out) const;

private:
    enum Phase { DataOut = 0, DataIn = 1, Command = 2, Status = 3, MsgOut = 6, MsgIn = 7, BusFree = 8 };

    Storage *disks[kTargets];
    u8 rregs[16], wregs[16];
    u8 fifo[16];
    int fifoCount;
    bool tchiWritten;
    bool irqLine;
    bool dma;
    bool connected;
    int target;
    Phase phase;
    u8 lastCmd;

    // Target side of the current command.
    u8 cdb[16];
    int cdbLen;
    u8 buf[512];
    u32 bufLen, bufPos;
    u32 lba, blocksLeft;
    bool reading, writing;
    u8 status, senseKey, senseCode;

    u32 tc() const { return rregs[0] | (rregs[1] << 8) | (rregs[14] << 16); }
    void setTc(u32 v);
    void push(u8 v) { if (fifoCount < 16) fifo[fifoCount++] = v; }
    u8 pop();
    void raise(u8 intr);
    void command(u8 cmd);
    void select(bool atn, bool stop);
    void execute();
    void finish(u8 st, u8 key = 0, u8 code = 0);
    void reply(u32 len, u32 alloc);
    bool targetByteIn(u8 &v);           // next byte the target sends (data in)
    void targetByteOut(u8 v);           // a byte to the target (data out)
    void transferInfo();
    void commandBytes();                // move FIFO bytes into the CDB
    void fillFifo();                    // DMA data in: target -> FIFO, counting TC down
    bool dmaDone;
    bool fifoFromTarget;                // FIFO holds data-in bytes (DRQ until drained)
    bool loadBlock();
};

} // namespace macemu

#endif
