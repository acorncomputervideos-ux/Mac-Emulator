// SCSI target commands, shared by the NCR 5380 (Mac Plus, Mac II) and the
// 53C96 (Quadra) models: a hard disk, or a CD-ROM drive holding an image.
// The chip models run the bus phases; this decides what each command does.

#ifndef MACEMU_SCSICMD_H
#define MACEMU_SCSICMD_H

#include "types.h"

namespace macemu {

struct ScsiDevice {
    Storage *storage = 0;
    bool cdrom = false;
    bool present = true;                // a CD is loaded
    bool locked = false;                // PREVENT MEDIUM REMOVAL
    bool unitAttention = false;         // medium changed: tell the next command
    bool ejected = false;               // the Mac ejected the CD (the host may react)
    u32 blockBytes = 512;               // CD: 2048, or 512 after MODE SELECT

    u32 blocks() const { return storage ? storage->size() / blockBytes : 0; }
    void insertCd(Storage *s)
    {
        storage = s;
        cdrom = true;
        present = s != 0;
        blockBytes = 2048;
        unitAttention = s != 0;
        ejected = false;
    }
};

enum ScsiAction {
    ScsiStatus,                         // no data: go to the status phase
    ScsiDataIn,                         // `len` bytes in buf for the initiator
    ScsiDataOut,                        // take `len` parameter bytes (MODE SELECT)
    ScsiReadBlocks,                     // stream `count` blocks from `lba`
    ScsiWriteBlocks,                    // take `count` blocks for `lba`
};

struct ScsiResult {
    ScsiAction action;
    u32 len, lba, count;
    u8 status, key, asc;                // status 2: check condition with this sense
};

enum { kScsiBufBytes = 2048 };

// Run a command.  `buf` (kScsiBufBytes) receives reply data; senseKey and
// senseAsc are the sense data from the previous command (REQUEST SENSE).
ScsiResult scsiExecute(ScsiDevice &dev, const u8 *cdb, u8 *buf, u8 senseKey, u8 senseAsc);

// The parameter data of a ScsiDataOut command has arrived.
void scsiDataOutDone(ScsiDevice &dev, const u8 *cdb, const u8 *buf, u32 len);

// Length of a CDB from its first byte.
inline int scsiCdbLength(u8 op) { int g = op >> 5; return g == 0 ? 6 : g == 5 ? 12 : 10; }

} // namespace macemu

#endif
