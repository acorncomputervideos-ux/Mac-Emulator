#include "hfs.h"
#include "util.h"

namespace macemu {

static inline u16 rd16(const u8 *p) { return be16(p); }
static inline u32 rd32(const u8 *p) { return be32(p); }

// ---------------------------------------------------------------- reading

bool HfsVolume::open(Storage *image)
{
    img = image;
    base = 0;
    u8 b[512];

    // Apple partition map: block 1 onwards, "PM" entries.
    if (img->read(512, b, 512) && b[0] == 'P' && b[1] == 'M') {
        u32 count = rd32(b + 4);
        for (u32 i = 1; i <= count && i < 64; i++) {
            if (!img->read(i * 512, b, 512) || b[0] != 'P' || b[1] != 'M') break;
            char type[33];
            memCopy(type, b + 48, 32);
            type[32] = 0;
            if (strEq(type, "Apple_HFS")) { base = rd32(b + 8) * 512; break; }
        }
    } else if (img->read(0, b, 512) && b[0] <= 63 && (rd32(b + 64) == 409600 || rd32(b + 64) == 819200
               || rd32(b + 64) == 737280 || rd32(b + 64) == 1474560) && rd16(b + 82) == 0x0100) {
        base = 84;                                      // DiskCopy 4.2
    }

    u8 mdb[512];
    if (!readAt(1024, mdb, 512) || mdb[0] != 'B' || mdb[1] != 'D') return false;
    if (mdb[124] == 'H' && mdb[125] == '+') return false;   // HFS+ inside an HFS wrapper
    blockSize = rd32(mdb + 20);
    firstBlock = rd16(mdb + 28);
    freeBlocks = rd16(mdb + 34);
    u32 n = mdb[36] < 27 ? mdb[36] : 27;
    memCopy(volName, mdb + 37, n);
    volName[n] = 0;
    extSize = rd32(mdb + 130);
    catSize = rd32(mdb + 146);
    for (int i = 0; i < 6; i++) { extExt[i] = rd16(mdb + 134 + 2 * i); catExt[i] = rd16(mdb + 150 + 2 * i); }
    if (!blockSize || (blockSize & 511)) return false;

    u8 hdr[512];
    if (!readNode(catExt, catSize, 0, hdr)) return false;
    nodeSize = rd16(hdr + 14 + 18);
    if (nodeSize != 512) return false;
    catFirstLeaf = rd32(hdr + 14 + 10);
    if (readNode(extExt, extSize, 0, hdr)) extFirstLeaf = rd32(hdr + 14 + 10);
    return true;
}

// A node of the catalog or extents file (their first three extents).
bool HfsVolume::readNode(const u16 *ext, u32 fileSize, u32 node, u8 *out)
{
    u32 off = node * nodeSize;
    if (off + nodeSize > fileSize) return false;
    u32 fileBlock = off / blockSize, acc = 0;
    for (int i = 0; i < 3; i++) {
        u32 start = ext[2 * i], count = ext[2 * i + 1];
        if (fileBlock < acc + count) {
            u32 abs = firstBlock * 512 + (start + fileBlock - acc) * blockSize + off % blockSize;
            return readAt(abs, out, nodeSize);
        }
        acc += count;
    }
    return false;
}

static int recordCount(const u8 *node) { return rd16(node + 10); }
static const u8 *recordAt(const u8 *node, int i, u32 &len)
{
    u32 a = rd16(node + 512 - 2 * (i + 1)), b = rd16(node + 512 - 2 * (i + 2));
    if (a < 14 || b > 512 - 2 || b < a) { len = 0; return node; }
    len = b - a;
    return node + a;
}

bool HfsVolume::decode(const u8 *rec, u32 len, HfsEntry &e, u16 *dataExt, u16 *rsrcExt)
{
    if (len < 8) return false;
    u32 keyLen = rec[0];
    u32 d = (keyLen + 2) & ~1u;
    if (d + 1 > len) return false;
    const u8 *p = rec + d;
    memFill(&e, 0, sizeof e);
    e.parent = rd32(rec + 2);
    u32 nl = rec[6] < 31 ? rec[6] : 31;
    memCopy(e.name, rec + 7, nl);
    e.name[nl] = 0;
    if (p[0] == 1 && d + 70 <= len) {                   // folder
        e.folder = true;
        e.items = rd16(p + 4);
        e.id = rd32(p + 6);
        e.created = rd32(p + 10);
        e.modified = rd32(p + 14);
        e.finderFlags = rd16(p + 30);
        return true;
    }
    if (p[0] == 2 && d + 102 <= len) {                  // file
        e.type = rd32(p + 4);
        e.creator = rd32(p + 8);
        e.finderFlags = rd16(p + 12);
        e.id = rd32(p + 20);
        e.dataLen = rd32(p + 26);
        e.rsrcLen = rd32(p + 36);
        e.created = rd32(p + 44);
        e.modified = rd32(p + 48);
        for (int i = 0; i < 6; i++) { e.dataExt[i] = rd16(p + 74 + 2 * i); e.rsrcExt[i] = rd16(p + 86 + 2 * i); }
        if (dataExt) memCopy(dataExt, e.dataExt, sizeof e.dataExt);
        if (rsrcExt) memCopy(rsrcExt, e.rsrcExt, sizeof e.rsrcExt);
        return true;
    }
    return false;                                       // thread records
}

int HfsVolume::list(u32 folder, HfsEntry *out, int max)
{
    u8 node[512];
    int n = 0;
    u32 limit = catSize / nodeSize + 1;
    for (u32 nd = catFirstLeaf, visited = 0; nd && visited < limit && n < max; visited++) {
        if (!readNode(catExt, catSize, nd, node) || (s8)node[8] != -1) break;
        bool past = false;
        for (int i = 0; i < recordCount(node) && n < max; i++) {
            u32 len;
            const u8 *r = recordAt(node, i, len);
            if (len < 8) continue;
            u32 parent = rd32(r + 2);
            if (parent > folder) { past = true; break; }
            if (parent == folder && decode(r, len, out[n], 0, 0)) n++;
        }
        if (past) break;
        nd = rd32(node);
    }
    return n;
}

bool HfsVolume::find(u32 id, HfsEntry &out)
{
    u8 node[512];
    u32 limit = catSize / nodeSize + 1;
    for (u32 nd = catFirstLeaf, visited = 0; nd && visited < limit; visited++) {
        if (!readNode(catExt, catSize, nd, node) || (s8)node[8] != -1) return false;
        for (int i = 0; i < recordCount(node); i++) {
            u32 len;
            const u8 *r = recordAt(node, i, len);
            HfsEntry e;
            if (len >= 8 && decode(r, len, e, 0, 0) && e.id == id) { out = e; return true; }
        }
        nd = rd32(node);
    }
    return false;
}

// Extents beyond the first three: the extents overflow file, whose leaf
// records are keyed by fork, file ID and the first file block they map.
bool HfsVolume::forkExtents(u32 id, bool rsrc, u32 startBlock, u16 *ext)
{
    u8 node[512];
    u32 limit = extSize / nodeSize + 1;
    for (u32 nd = extFirstLeaf, visited = 0; nd && visited < limit; visited++) {
        if (!readNode(extExt, extSize, nd, node) || (s8)node[8] != -1) return false;
        for (int i = 0; i < recordCount(node); i++) {
            u32 len;
            const u8 *r = recordAt(node, i, len);
            if (len < 20 || r[0] != 7) continue;
            if (r[1] == (rsrc ? 0xFF : 0x00) && rd32(r + 2) == id && rd16(r + 6) == startBlock) {
                for (int k = 0; k < 6; k++) ext[k] = rd16(r + 8 + 2 * k);
                return true;
            }
        }
        nd = rd32(node);
    }
    return false;
}

bool HfsVolume::readFork(const HfsEntry &f, bool rsrc, u32 offset, void *buf, u32 len)
{
    u32 size = rsrc ? f.rsrcLen : f.dataLen;
    if (offset + len > size || offset + len < offset) return false;
    u8 *out = (u8 *)buf;
    u16 ext[6];
    memCopy(ext, rsrc ? f.rsrcExt : f.dataExt, sizeof ext);
    u32 extStart = 0;                                   // file block the current record starts at
    while (len) {
        u32 fileBlock = offset / blockSize, acc = extStart;
        u32 abs = 0;
        bool found = false;
        for (int i = 0; i < 3; i++) {
            u32 start = ext[2 * i], count = ext[2 * i + 1];
            if (count && fileBlock < acc + count) {
                abs = firstBlock * 512 + (start + fileBlock - acc) * blockSize;
                found = true;
                break;
            }
            acc += count;
        }
        if (!found) {
            if (fileBlock < acc || !forkExtents(f.id, rsrc, (u16)acc, ext)) return false;
            extStart = acc;
            continue;
        }
        u32 within = offset % blockSize, n = blockSize - within;
        if (n > len) n = len;
        if (!readAt(abs + within, out, n)) return false;
        out += n; offset += n; len -= n;
    }
    return true;
}

// ---------------------------------------------------------------- names

static inline char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

int hfsCompareNames(const char *a, const char *b)
{
    for (;; a++, b++) {
        u8 x = (u8)upper(*a), y = (u8)upper(*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

void hfsCleanName(char *name)
{
    u32 n = 0;
    for (const char *p = name; *p && n < 31; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-';
        name[n++] = ok ? c : '-';
    }
    while (n && name[n - 1] == ' ') n--;
    name[n] = 0;
    if (!n || name[0] == '.') {                         // ".name" is a driver name on the Mac
        char t[32] = "Untitled";
        if (n) { strCopy(t, "File", sizeof t); strAppend(t, name, sizeof t); }
        strCopy(name, t, 32);
    }
}

// ---------------------------------------------------------------- building a floppy

namespace {

struct Rec {
    u32 parent;
    char name[32];
    u8 data[102];
    u32 dataLen;
};

const u32 kSectors = 1600, kAlBlSt = 4, kAllocBlocks = kSectors - kAlBlSt - 2, kXtBlocks = 4;

u32 keyBytes(const Rec &r) { return (u32)(1 + 6 + strLen(r.name) + 1) & ~1u; }   // even
u32 recBytes(const Rec &r) { return keyBytes(r) + r.dataLen; }

void putKey(u8 *p, const Rec &r, bool index)
{
    u32 nl = strLen(r.name);
    p[0] = (u8)(index ? 37 : 6 + nl);
    p[1] = 0;
    putBe32(p + 2, r.parent);
    p[6] = (u8)nl;
    memCopy(p + 7, r.name, nl);
}

void setOffsets(u8 *node, const u32 *offs, int n)
{
    for (int i = 0; i <= n; i++) putBe16(node + 512 - 2 * (i + 1), (u16)offs[i]);
    putBe16(node + 10, (u16)n);
}

void setBit(u8 *map, u32 bit) { map[bit >> 3] |= (u8)(0x80 >> (bit & 7)); }

} // namespace

bool hfsMakeFloppy(u8 *out, const char *volumeName, const HfsNewFile *files, int count, u32 now)
{
    if (count < 0 || count > kHfsFloppyMaxFiles) return false;
    memFill(out, 0, kHfsFloppyBytes);

    char vol[28];
    strCopy(vol, volumeName, sizeof vol);
    hfsCleanName(vol);

    // Sort the files by name (insertion sort on indexes) and make names unique.
    static int order[kHfsFloppyMaxFiles];
    static char names[kHfsFloppyMaxFiles][32];
    for (int i = 0; i < count; i++) {
        strCopy(names[i], files[i].name, 32);
        hfsCleanName(names[i]);
        if (strLen(names[i]) > 27) names[i][27] = 0;
    }
    for (int i = 0; i < count; i++) {
        for (int k = 2; ; k++) {
            bool clash = false;
            for (int j = 0; j < i; j++) clash |= hfsCompareNames(names[i], names[j]) == 0;
            if (!clash) break;
            char base[32];
            strCopy(base, names[i], sizeof base);
            u32 dash = strLen(base);
            while (dash && base[dash - 1] != '-') dash--;
            if (dash && k > 2) base[dash - 1] = 0;
            strAppend(base, "-", sizeof base);
            appendUint(base, (u32)k, sizeof base);
            strCopy(names[i], base, 32);
        }
    }
    for (int i = 0; i < count; i++) {
        int j = i;
        while (j > 0 && hfsCompareNames(names[i], names[order[j - 1]]) < 0) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }

    // Allocation: extents file, catalog file, then each file's forks.
    static Rec recs[kHfsFloppyMaxFiles + 2];
    int nrec = 0;
    memFill(recs, 0, sizeof recs);
    Rec &root = recs[nrec++];                           // the root folder: (1, volume name)
    root.parent = 1;
    strCopy(root.name, vol, 32);
    root.dataLen = 70;
    root.data[0] = 1;
    putBe16(root.data + 4, (u16)count);
    putBe32(root.data + 6, 2);
    putBe32(root.data + 10, now);
    putBe32(root.data + 14, now);
    Rec &thread = recs[nrec++];                         // its thread: (2, "")
    thread.parent = 2;
    thread.dataLen = 46;
    thread.data[0] = 3;
    putBe32(thread.data + 10, 1);
    thread.data[14] = (u8)strLen(vol);
    memCopy(thread.data + 15, vol, strLen(vol));

    // Leaves: pack records in order.
    static u8 cat[64][512];
    int leafStart = 1, nodes = 1;
    {
        int first = 0;
        while (first < nrec + count) {
            u8 *node = cat[nodes];
            memFill(node, 0, 512);
            u32 offs[64];
            int n = 0;
            u32 pos = 14;
            offs[0] = pos;
            for (int i = first; i < nrec + count; i++) {
                Rec tmp;
                const Rec *r;
                if (i < nrec) r = &recs[i];
                else {
                    const int fi = order[i - nrec];
                    memFill(&tmp, 0, sizeof tmp);
                    tmp.parent = 2;
                    strCopy(tmp.name, names[fi], 32);
                    tmp.dataLen = 102;
                    r = &tmp;
                }
                u32 size = recBytes(*r);
                if (pos + size + 2 * (n + 2) > 512) break;
                n++;
                pos += size;
                offs[n] = pos;
            }
            if (!n) return false;
            first += n;
            node[8] = 0xFF;                             // leaf
            node[9] = 1;
            setOffsets(node, offs, n);
            nodes++;
            if (nodes >= 60) return false;
        }
    }
    int leaves = nodes - leafStart;
    u32 catNodesPlanned = (u32)(leaves * 2 + 8);        // room for the index nodes, and free nodes
    if (catNodesPlanned > 60) catNodesPlanned = 60;
    u32 catBlocks = catNodesPlanned;
    u32 next = kXtBlocks + catBlocks;                   // first allocation block for file data

    // File records, now that the data's place is known.
    static Rec frec[kHfsFloppyMaxFiles];
    for (int k = 0; k < count; k++) {
        const int fi = order[k];
        const HfsNewFile &f = files[fi];
        Rec &r = frec[k];
        memFill(&r, 0, sizeof r);
        r.parent = 2;
        strCopy(r.name, names[fi], 32);
        r.dataLen = 102;
        u8 *p = r.data;
        p[0] = 2;
        putBe32(p + 4, f.type);
        putBe32(p + 8, f.creator);
        putBe32(p + 20, 16 + (u32)k);
        u32 db = (f.dataLen + 511) / 512, rb = (f.rsrcLen + 511) / 512;
        if (next + db + rb > kAllocBlocks) return false;
        putBe32(p + 26, f.dataLen); putBe32(p + 30, db * 512);
        putBe32(p + 36, f.rsrcLen); putBe32(p + 40, rb * 512);
        putBe32(p + 44, now); putBe32(p + 48, now);
        if (db) { putBe16(p + 74, (u16)next); putBe16(p + 76, (u16)db); }
        u8 *dst = out + kAlBlSt * 512 + next * 512;
        if (f.dataLen) memCopy(dst, f.data, f.dataLen);
        next += db;
        if (rb) { putBe16(p + 86, (u16)next); putBe16(p + 88, (u16)rb); }
        dst = out + kAlBlSt * 512 + next * 512;
        if (f.rsrcLen) memCopy(dst, f.rsrc, f.rsrcLen);
        next += rb;
    }

    // Fill the leaves for real (same packing as above).
    {
        int idx = 0;
        for (int nd = leafStart; nd < leafStart + leaves; nd++) {
            u8 *node = cat[nd];
            int n = recordCount(node);
            for (int i = 0; i < n; i++, idx++) {
                const Rec &r = idx < nrec ? recs[idx] : frec[idx - nrec];
                u32 len;
                u8 *dst = (u8 *)recordAt(node, i, len);
                putKey(dst, r, false);
                memCopy(dst + keyBytes(r), r.data, r.dataLen);
            }
            putBe32(node, nd + 1 < leafStart + leaves ? (u32)nd + 1 : 0);
            putBe32(node + 4, nd > leafStart ? (u32)nd - 1 : 0);
        }
    }

    // Index levels, bottom-up: one record (first key, child) per node below.
    static u32 level[64], above[64];
    int lc = 0;
    for (int nd = leafStart; nd < leafStart + leaves; nd++) level[lc++] = (u32)nd;
    int depth = 1;
    while (lc > 1) {
        int ac = 0;
        for (int i = 0; i < lc; ) {
            if (nodes >= (int)catNodesPlanned) return false;
            u8 *node = cat[nodes];
            memFill(node, 0, 512);
            u32 offs[16];
            int n = 0;
            offs[0] = 14;
            while (i < lc && n < 11) {
                const u8 *child = cat[level[i]];
                u32 len;
                const u8 *first = recordAt(child, 0, len);
                u8 *dst = node + offs[n];
                memFill(dst, 0, 38);
                dst[0] = 37;
                memCopy(dst + 1, first + 1, first[0]);   // parent ID and name, padded
                putBe32(dst + 38, level[i]);
                offs[n + 1] = offs[n] + 42;
                n++; i++;
            }
            node[8] = 0;                                // index
            node[9] = (u8)(depth + 1);
            setOffsets(node, offs, n);
            if (ac) { putBe32(cat[above[ac - 1]], (u32)nodes); putBe32(node + 4, above[ac - 1]); }
            above[ac++] = (u32)nodes;
            nodes++;
        }
        for (int i = 0; i < ac; i++) level[i] = above[i];
        lc = ac;
        depth++;
    }
    u32 rootNode = level[0];

    // Catalog header node.
    {
        u8 *h = cat[0];
        memFill(h, 0, 512);
        h[8] = 1;                                       // header
        u8 *r = h + 14;
        putBe16(r + 0, (u16)depth);
        putBe32(r + 2, rootNode);
        putBe32(r + 6, (u32)(nrec + count));
        putBe32(r + 10, (u32)leafStart);
        putBe32(r + 14, (u32)(leafStart + leaves - 1));
        putBe16(r + 18, 512);
        putBe16(r + 20, 37);
        putBe32(r + 22, catNodesPlanned);
        putBe32(r + 26, catNodesPlanned - (u32)nodes);
        for (int i = 0; i < nodes; i++) setBit(h + 248, (u32)i);
        const u32 offs[4] = {14, 120, 248, 504};
        setOffsets(h, offs, 3);
    }

    // Extents file: an empty B-tree (header node only).
    u8 *xt = out + kAlBlSt * 512;
    {
        u8 *r = xt + 14;
        xt[8] = 1;
        putBe16(r + 18, 512);
        putBe16(r + 20, 7);
        putBe32(r + 22, kXtBlocks);
        putBe32(r + 26, kXtBlocks - 1);
        setBit(xt + 248, 0);
        const u32 offs[4] = {14, 120, 248, 504};
        setOffsets(xt, offs, 3);
    }
    for (u32 i = 0; i < catNodesPlanned; i++)
        if ((int)i < nodes) memCopy(out + kAlBlSt * 512 + (kXtBlocks + i) * 512, cat[i], 512);

    // Volume bitmap and master directory block (and its copy near the end).
    for (u32 b = 0; b < next; b++) setBit(out + 3 * 512, b);
    u8 *m = out + 2 * 512;
    m[0] = 'B'; m[1] = 'D';
    putBe32(m + 2, now); putBe32(m + 6, now);
    putBe16(m + 10, 0x0100);                            // unmounted cleanly
    putBe16(m + 12, (u16)count);
    putBe16(m + 14, 3);                                 // bitmap at sector 3
    putBe16(m + 16, (u16)next);
    putBe16(m + 18, (u16)kAllocBlocks);
    putBe32(m + 20, 512);
    putBe32(m + 24, 2048);
    putBe16(m + 28, (u16)kAlBlSt);
    putBe32(m + 30, 16 + (u32)count);
    putBe16(m + 34, (u16)(kAllocBlocks - next));
    m[36] = (u8)strLen(vol);
    memCopy(m + 37, vol, strLen(vol));
    putBe32(m + 70, 1);                                 // write count
    putBe32(m + 74, kXtBlocks * 512);
    putBe32(m + 78, catBlocks * 512);
    putBe32(m + 84, (u32)count);
    putBe32(m + 130, kXtBlocks * 512);
    putBe16(m + 134, 0); putBe16(m + 136, (u16)kXtBlocks);
    putBe32(m + 146, catBlocks * 512);
    putBe16(m + 150, (u16)kXtBlocks); putBe16(m + 152, (u16)catBlocks);
    memCopy(out + (kSectors - 2) * 512, m, 512);
    return true;
}

// ---------------------------------------------------------------- MacBinary

static u16 crc16(const u8 *p, u32 n)
{
    u16 crc = 0;
    for (u32 i = 0; i < n; i++) {
        crc ^= (u16)(p[i] << 8);
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
    }
    return crc;
}

bool macBinaryParse(const u8 *f, u32 len, HfsNewFile &out)
{
    if (len < 128 || f[0] != 0 || f[74] != 0 || f[82] != 0 || f[1] < 1 || f[1] > 63) return false;
    u32 dl = rd32(f + 83), rl = rd32(f + 87);
    if (dl > 0x7FFFFF || rl > 0x7FFFFF) return false;
    if (128 + macBinaryPad(dl) + rl > len) return false;
    bool crcOk = rd16(f + 124) == crc16(f, 124);
    if (!crcOk && f[122] != 0 && f[122] != 129 && f[122] != 130) return false;   // MacBinary I has no CRC
    memFill(&out, 0, sizeof out);
    u32 nl = f[1] < 31 ? f[1] : 31;
    memCopy(out.name, f + 2, nl);
    out.name[nl] = 0;
    out.type = rd32(f + 65);
    out.creator = rd32(f + 69);
    out.data = f + 128;
    out.dataLen = dl;
    out.rsrc = f + 128 + macBinaryPad(dl);
    out.rsrcLen = rl;
    return true;
}

void macBinaryHeader(const HfsEntry &e, u8 *h)
{
    memFill(h, 0, 128);
    u32 nl = strLen(e.name);
    h[1] = (u8)nl;
    memCopy(h + 2, e.name, nl);
    putBe32(h + 65, e.type);
    putBe32(h + 69, e.creator);
    h[73] = (u8)(e.finderFlags >> 8);
    putBe32(h + 83, e.dataLen);
    putBe32(h + 87, e.rsrcLen);
    putBe32(h + 91, e.created);
    putBe32(h + 95, e.modified);
    h[101] = (u8)e.finderFlags;
    h[122] = 129;
    h[123] = 129;
    putBe16(h + 124, crc16(h, 124));
}

} // namespace macemu
