| Display driver for the emulator's NuBus video card ("DRVR" resource that
| lives in the card's declaration ROM).
|
| It is a thin stub: Control and Status calls are handed to the card through
| a register window at dCtlDevBase + $FFFF0, and the card (C++ code in
| nubusvideo.cpp) carries them out.  The stub installs the VBL slot
| interrupt handler and answers GetGamma itself.
|
| Assemble with tools/build_videodrv.py, which writes videodrv.h.

        .equ    REGS,       0xFFFF0     | register window, offset from dCtlDevBase
        .equ    R_CMD,      0           | write: 1 open, 2 control, 3 status, 4 close,
                                        |        0xA0 acknowledge VBL; read: slot number
        .equ    R_PB,       4           | parameter block pointer
        .equ    R_DCE,      8           | DCE pointer
        .equ    R_RESULT,   12          | read: OSErr of the last command

        .equ    dCtlStorage, 20
        .equ    dCtlSlot,   40
        .equ    dCtlDevBase, 42
        .equ    ioTrap,     6
        .equ    csCode,     26
        .equ    csParam,    28
        .equ    JIODone,    0x08FC
        .equ    JVBLTask,   0x0D28

        .text
DrvrStart:
        .word   0x4C00                  | dNeedLock, dStatEnable, dCtlEnable
        .word   0, 0, 0                 | delay, event mask, menu
        .word   Open-DrvrStart
        .word   Prime-DrvrStart
        .word   Control-DrvrStart
        .word   Status-DrvrStart
        .word   Close-DrvrStart
DrvrName:
        .byte   18
        .ascii  ".Display_Video_Emu"
        .even

| ------------------------------------------------------------------ Open
| A0 = parameter block, A1 = DCE
Open:
        movem.l d1-d2/a0-a3,-(sp)
        move.l  a1,a3
        move.l  dCtlDevBase(a3),a2
        adda.l  #REGS,a2
        move.l  a0,R_PB(a2)
        move.l  a3,R_DCE(a2)
        move.l  #1,R_CMD(a2)            | the card sets its default mode and colours

        moveq   #16,d0                  | slot interrupt queue element
        .word   0xA71E                  | _NewPtr ,SYS,CLEAR
        move.l  a0,d0
        beq.s   OpenFail
        move.l  a0,dCtlStorage(a3)
        move.w  #6,4(a0)                | sqType = sIQType
        lea     VBLHandler(pc),a1
        move.l  a1,8(a0)                | sqAddr
        move.l  a2,12(a0)               | sqParm = register window
        moveq   #0,d0
        move.b  dCtlSlot(a3),d0
        .word   0xA075                  | _SIntInstall
        moveq   #0,d0
        bra.s   OpenDone
OpenFail:
        moveq   #-23,d0                 | openErr
OpenDone:
        movem.l (sp)+,d1-d2/a0-a3
        rts

| ------------------------------------------------------------------ Prime
Prime:
        moveq   #0,d0
        rts

| ------------------------------------------------------------------ Control
Control:
        move.l  a2,-(sp)
        move.l  dCtlDevBase(a1),a2
        adda.l  #REGS,a2
        move.l  a0,R_PB(a2)
        move.l  a1,R_DCE(a2)
        move.l  #2,R_CMD(a2)
        move.l  R_RESULT(a2),d0
        move.l  (sp)+,a2
        bra.s   Done

| ------------------------------------------------------------------ Status
Status:
        move.l  a2,-(sp)
        cmpi.w  #8,csCode(a0)           | cscGetGamma: point at our table
        bne.s   1f
        move.l  csParam(a0),a2
        pea     GammaTable(pc)
        move.l  (sp)+,(a2)
        moveq   #0,d0
        move.l  (sp)+,a2
        bra.s   Done
1:      move.l  dCtlDevBase(a1),a2
        adda.l  #REGS,a2
        move.l  a0,R_PB(a2)
        move.l  a1,R_DCE(a2)
        move.l  #3,R_CMD(a2)
        move.l  R_RESULT(a2),d0
        move.l  (sp)+,a2
        | fall through

| Return through IODone unless the call was immediate.
Done:
        move.w  ioTrap(a0),d1
        btst    #9,d1                   | noQueueBit
        bne.s   2f
        move.l  JIODone,-(sp)
2:      rts

| ------------------------------------------------------------------ Close
Close:
        movem.l d1-d2/a0-a3,-(sp)
        move.l  a1,a3
        move.l  dCtlDevBase(a3),a2
        adda.l  #REGS,a2
        move.l  a0,R_PB(a2)
        move.l  a3,R_DCE(a2)
        move.l  #4,R_CMD(a2)            | stop interrupts
        move.l  dCtlStorage(a3),d1
        beq.s   3f
        move.l  d1,a0
        moveq   #0,d0
        move.b  dCtlSlot(a3),d0
        .word   0xA076                  | _SIntRemove
        move.l  d1,a0
        .word   0xA01F                  | _DisposePtr
        clr.l   dCtlStorage(a3)
3:      moveq   #0,d0
        movem.l (sp)+,d1-d2/a0-a3
        rts

| ------------------------------------------------------------------ VBL
| Called by the Slot Manager with A1 = sqParm (the register window).
VBLHandler:
        move.l  #0xA0,R_CMD(a1)         | acknowledge the card's interrupt
        move.l  R_CMD(a1),d0            | slot number
        move.l  JVBLTask,a0
        jsr     (a0)
        moveq   #1,d0                   | interrupt serviced
        rts

| ------------------------------------------------------------------ Gamma
        .even
GammaTable:
        .word   0                       | gVersion
        .word   0                       | gType
        .word   0                       | gFormulaSize
        .word   1                       | gChanCnt
        .word   256                     | gDataCnt
        .word   8                       | gDataWidth
        .irp    i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        .byte   \i*16+0,\i*16+1,\i*16+2,\i*16+3,\i*16+4,\i*16+5,\i*16+6,\i*16+7
        .byte   \i*16+8,\i*16+9,\i*16+10,\i*16+11,\i*16+12,\i*16+13,\i*16+14,\i*16+15
        .endr
DrvrEnd:
