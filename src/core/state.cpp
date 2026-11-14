// Save states (see state.h): each part's fields, saved and loaded alike.
#include "state.h"
#include "quadra.h"

namespace macemu {

void M68K::state(StateIO &s)
{
    s.tag("CPU ");
    s.a(d); s.a(a); s.v(pc); s.b(stopped); s.v(totalCycles);
    s.v(vbr); s.v(sfc); s.v(dfc); s.v(cacr); s.v(caar); s.v(instPC); s.v(cyc);
    s.v(usp); s.v(isp); s.v(msp);
    s.v(fC); s.v(fV); s.v(fZ); s.v(fN); s.v(fX); s.v(fS); s.v(fM); s.v(fT); s.v(imask);
    s.v(irq); s.b(nmiSeen); s.v(opcode); s.b(traceNext);
    s.b(restartable); s.v(lastTrap); s.v(pageFaults); s.v(slowAccesses);
    if (!s.saving()) flushTlb(0xFFFFFFFFu);             // rebuilt as the code runs
}

void Fpu::state(StateIO &s)
{
    s.tag("FPU ");
    s.a(fp); s.v(fpcr); s.v(fpsr); s.v(fpiar);
    s.a(savedFp); s.v(savedCr); s.v(savedSr); s.v(savedIar);
    s.b(used);
}

void Mmu040::state(StateIO &s)
{
    s.tag("MMU ");
    s.v(tc); s.v(urp); s.v(srp); s.v(mmusr); s.a(itt); s.a(dtt);
    if (!s.saving()) flush();                           // the translation cache starts empty
}

void Via::state(StateIO &s)
{
    s.tag("VIA ");
    s.v(ora); s.v(orb); s.v(ddra); s.v(ddrb); s.v(sr); s.v(acr); s.v(pcr); s.v(ifr); s.v(ier);
    s.v(t1Latch); s.v(t2LatchLow); s.v(t1); s.v(t2); s.b(t1Armed); s.b(t2Armed);
}

void Iwm::state(StateIO &s)
{
    s.tag("IWM ");
    for (int i = 0; i < kDrives; i++) {
        Drive &d = drv[i];
        s.b(d.present); s.v(d.dataOffset); s.v(d.sides); s.b(d.writeProtect); s.b(d.motor);
        s.b(d.stepOut); s.v(d.track); s.b(d.switched); s.b(d.hd); s.b(d.mfm);
        s.a(d.nib); s.v(d.nibLen); s.b(d.nibValid[0]); s.b(d.nibValid[1]); s.v(d.nibTrack);
        s.v(d.pos); s.b(d.writing); s.v(d.writeSide); s.v(d.writeStart); s.v(d.writeCount);
        s.b(ejectedByGuest[i]);
    }
    s.b(ism); s.v(ismMode); s.v(ismSetup); s.v(ismPhases); s.v(ismError); s.a(ismParam);
    s.v(paramIdx); s.v(modeSeq);
    for (int i = 0; i < 2; i++) { s.v(fifo[i].v); s.b(fifo[i].mark); }
    s.v(fifoCount); s.v(crc); s.v(mfmPos); s.b(hunting); s.v(lastSector); s.b(lastWasMark);
    s.a(wbuf); s.v(wlen);
    s.b(ca0); s.b(ca1); s.b(ca2); s.b(lstrb); s.b(enable); s.b(select); s.b(q6); s.b(q7);
    s.b(sel); s.v(mode); s.v(underrunCountdown);
    s.v(activityCounter);
}

void Esp::state(StateIO &s)
{
    s.tag("ESP ");
    for (int i = 0; i < kTargets; i++) {
        ScsiDevice &d = dev[i];
        s.b(d.cdrom); s.b(d.present); s.b(d.locked); s.b(d.unitAttention); s.b(d.ejected); s.v(d.blockBytes);
    }
    s.a(rregs); s.a(wregs); s.a(fifo); s.v(fifoCount); s.b(tchiWritten); s.b(irqLine); s.b(dma);
    s.b(connected); s.v(target);
    int p = (int)phase;
    s.v(p);
    phase = (Phase)p;
    s.v(lastCmd); s.a(cdb); s.v(cdbLen); s.a(buf); s.v(bufLen); s.v(bufPos); s.v(lba); s.v(blocksLeft);
    s.b(reading); s.b(writing); s.v(status); s.v(senseKey); s.v(senseCode); s.b(dmaDone); s.b(fifoFromTarget);
    s.v(activityCounter);
}

void Rtc::state(StateIO &s)
{
    s.tag("RTC ");
    s.v(seconds); s.a(pram); s.b(pramDirty); s.b(clk); s.b(writeProtect);
    s.v(shiftReg); s.v(cmd_); s.v(extAddr); s.v(bits); s.v(state_); s.b(output); s.b(dataOut);
}

void Asc::state(StateIO &s)
{
    s.tag("ASC ");
    s.a(regs);
    for (int i = 0; i < 2; i++) {
        Fifo &f = fifo[i];
        s.a(f.data); s.v(f.rd); s.v(f.wr); s.v(f.count); s.v(f.status); s.a(f.ext);
    }
    s.b(irqLine); s.v(emptyCycle);
}

void AdbKeyboard::state(StateIO &s)
{
    s.tag("KBD ");
    s.v(address); s.v(handlerId); s.v(leds); s.a(queue); s.v(head); s.v(tail); s.v(modifiers); s.a(held);
}

void AdbMouse::state(StateIO &s)
{
    s.tag("MOUS");
    s.v(address); s.v(handlerId); s.v(dx); s.v(dy); s.b(button); s.b(reportedButton);
}

void AdbTransceiver::state(StateIO &s)
{
    s.tag("ADB ");
    s.v(state_); s.b(intHigh); s.b(pollBlocked); s.b(pollReply); s.b(timedOut);
    s.a(out); s.v(outLen); s.a(in); s.v(inLen); s.v(inIndex); s.v(pollCommand); s.v(pollMask);
    s.v(pending); s.v(pollIndex); s.b(quietAfterReset); s.v(ticksSinceReset);
}

void Dafb::state(StateIO &s)
{
    s.tag("DAFB");
    s.v(display);
    int m = (int)(mode - kModes);
    s.v(m);
    mode = kModes + m;
    s.a(regs); s.a(palette); s.v(lutIndex); s.a(clut); s.v(clutVersion);
    s.memory(frame, sizeof frame);
    if (!s.saving()) clutVersion++;                     // the host redraws everything
}

void Scc::state(StateIO &s)
{
    s.tag("SCC ");
    for (int i = 0; i < 2; i++) {
        Channel &c = ch[i];
        s.a(c.wr); s.v(c.ptr); s.b(c.txEmpty); s.v(c.txBusy); s.b(c.shifting); s.b(c.eom); s.b(c.hunt);
        s.v(c.rr0Latched); s.b(c.txIP); s.b(c.extIP);
    }
}

void Sonic::state(StateIO &s)
{
    s.tag("SNIC");
    s.a(regs); s.a(cam); s.a(station); s.b(stationKnown); s.b(rbaFull);
}

void Quadra::state(StateIO &s)
{
    s.tag("QUAD");
    s.v(ramSize);
    s.memory(ram, ramSize);
    s.b(powerOff); s.b(via2PowerDriven); s.b(auxMode);
    s.b(via1Irq); s.b(via2Irq); s.b(sccIrq);
    s.b(videoIrqSeen); s.b(espIrqSeen); s.b(ascIrqSeen); s.b(sonicIrqSeen);
    s.v(nextNetPoll); s.v(nmiFrames); s.v(viaFrac); s.v(frameStart); s.v(frameCount);
    s.a(djmemc); s.a(iosb);
    u32 gen = mapGeneration;
    s.v(gen);
    if (!s.saving()) { mapGeneration++; cpu.codeMayChange(); }   // the CPU's TLB and the JIT's checks start again
    cpu.state(s); fpu.state(s); mmu.state(s); via1.state(s); via2.state(s);
    iwm.state(s); esp.state(s); rtc.state(s); asc.state(s);
    adb.state(s); keyboard.state(s); mouse.state(s);
    dafb.state(s); scc.state(s); sonic.state(s);
    bool hasShared = shared != 0;
    s.b(hasShared);
    if (hasShared != (shared != 0)) { s.ok = false; return; }   // saved with a shared folder: needs one
    if (shared) shared->state(s);
    s.tag("END ");
}

} // namespace macemu
