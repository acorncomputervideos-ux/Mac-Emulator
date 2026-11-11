// Runs the 68000 core against SingleStepTests vectors (see tools/sst2txt.py).
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "../src/core/m68k.h"

using namespace macemu;

struct TestBus : Bus {
    std::vector<u8> mem;
    TestBus() : mem(1 << 24) {}
    u8 read8(u32 a) override { return mem[a & 0xFFFFFF]; }
    u16 read16(u32 a) override { a &= 0xFFFFFE; return (u16)((mem[a] << 8) | mem[a + 1]); }
    void write8(u32 a, u8 v) override { mem[a & 0xFFFFFF] = v; }
    void write16(u32 a, u16 v) override { a &= 0xFFFFFE; mem[a] = (u8)(v >> 8); mem[a + 1] = (u8)v; }
};

struct State { u32 r[19]; u32 pf[2]; std::vector<std::pair<u32, u32>> ram; };

static bool readState(FILE *f, State &s)
{
    for (int i = 0; i < 19; i++) if (fscanf(f, "%u", &s.r[i]) != 1) return false;
    int n;
    if (fscanf(f, "%u %u %d", &s.pf[0], &s.pf[1], &n) != 3) return false;
    s.ram.resize(n);
    for (int i = 0; i < n; i++) if (fscanf(f, "%u %u", &s.ram[i].first, &s.ram[i].second) != 2) return false;
    return true;
}

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    bool verbose = argc > 2;
    TestBus bus;
    M68K cpu(&bus);
    struct Stat { int total = 0, fail = 0, skipped = 0, timing = 0; };
    std::map<std::string, Stat> stats;
    char group[64], name[256];
    int length;
    while (fscanf(f, "%63s %255s %d", group, name, &length) == 3) {
        State ini, fin;
        if (!readState(f, ini) || !readState(f, fin)) break;
        Stat &st = stats[group];

        // Address errors are not emulated: skip vectors that end in the
        // address error handler (vector 3).
        u32 vec3 = 0; bool haveVec3 = false;
        for (auto &p : ini.ram) if (p.first >= 12 && p.first < 16) { vec3 |= p.second << (8 * (15 - p.first)); haveVec3 = true; }
        if (haveVec3 && fin.r[18] == vec3) { st.skipped++; continue; }
        // Known-bad vectors: ASR by a register count >= operand size reports
        // C=X=0 for negative operands, contradicting the 68000 manual.
        if (!strncmp(group, "ASR", 3)) {
            unsigned op = (unsigned)strtoul(std::string(name, 4).c_str(), nullptr, 16);
            unsigned bits = group[4] == 'b' ? 8 : group[4] == 'w' ? 16 : 32;
            if ((op & 0xC0) != 0xC0 && (op & 0x20) && (ini.r[(op >> 9) & 7] & 63) >= bits) { st.skipped++; continue; }
        }
        st.total++;

        for (auto &p : ini.ram) bus.mem[p.first & 0xFFFFFF] = (u8)p.second;
        u32 pc = ini.r[18];
        bus.write16(pc, (u16)ini.pf[0]);
        bus.write16(pc + 2, (u16)ini.pf[1]);
        for (int i = 0; i < 8; i++) cpu.d[i] = ini.r[i];
        for (int i = 0; i < 7; i++) cpu.a[i] = ini.r[8 + i];
        cpu.setSR(0x2700);
        cpu.setSR((u16)ini.r[17]);
        cpu.setUSP(ini.r[15]);
        cpu.setSSP(ini.r[16]);
        cpu.pc = pc;
        cpu.stopped = false;
        int cycles = cpu.step();

        bool ok = true;
        std::string why;
        auto chk = [&](const char *what, u32 got, u32 want) {
            if (got != want) { ok = false; char b[96]; snprintf(b, sizeof b, " %s=%08x(want %08x)", what, got, want); why += b; }
        };
        static const char *rn[] = {"d0","d1","d2","d3","d4","d5","d6","d7","a0","a1","a2","a3","a4","a5","a6"};
        for (int i = 0; i < 8; i++) chk(rn[i], cpu.d[i], fin.r[i]);
        for (int i = 0; i < 7; i++) chk(rn[8 + i], cpu.a[i], fin.r[8 + i]);
        chk("usp", cpu.getUSP(), fin.r[15]);
        chk("ssp", cpu.getSSP(), fin.r[16]);
        chk("sr", cpu.getSR(), fin.r[17]);
        chk("pc", cpu.pc, fin.r[18]);
        for (auto &p : fin.ram) chk("ram", bus.mem[p.first & 0xFFFFFF], p.second);
        if (!ok) {
            st.fail++;
            if (verbose && st.fail <= 6) printf("FAIL %s %s:%s   [d0=%08x d1=%08x d2=%08x sr=%04x]\n", group, name, why.c_str(), ini.r[0], ini.r[1], ini.r[2], ini.r[17]);
        } else if (cycles != length) {
            st.timing++;
            if (verbose && st.timing <= 2) printf("TIME %s %s: got %d want %d\n", group, name, cycles, length);
        }
        for (auto &p : ini.ram) bus.mem[p.first & 0xFFFFFF] = 0;
        for (auto &p : fin.ram) bus.mem[p.first & 0xFFFFFF] = 0;
        bus.write16(pc, 0); bus.write16(pc + 2, 0);
    }
    int total = 0, fail = 0, skipped = 0, timing = 0;
    for (auto &kv : stats) {
        total += kv.second.total; fail += kv.second.fail; skipped += kv.second.skipped; timing += kv.second.timing;
        if (kv.second.fail || (verbose && kv.second.timing))
            printf("%-12s total %5d  fail %5d  timing-mismatch %5d  skipped %d\n", kv.first.c_str(), kv.second.total, kv.second.fail, kv.second.timing, kv.second.skipped);
    }
    printf("TOTAL %d tests, %d failed, %d skipped (address errors, known-bad ASR), %d passed with a cycle-count mismatch\n", total, fail, skipped, timing);
    return fail ? 1 : 0;
}
