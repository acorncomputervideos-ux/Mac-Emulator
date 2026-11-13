// The shared folder's names: Mac Roman on the Mac, UTF-8 (host) or code
// page 850 (FatFs on the Pi) on the card, with %XX for what the card cannot
// take.  (The whole file system is tested through Mac OS itself: the host
// runner's SHARE_TEST=1 with --share.)
#include <cstdio>
#include <cstring>
#include "../src/core/sharedfs.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool roundTrip(const u8 *mac, int n, bool utf8)
{
    char host[512];
    SharedFolder::macToHost(mac, n, host, sizeof host, utf8);
    u8 back[32];
    int m = SharedFolder::hostToMac(host, back, utf8);
    return m == n && !memcmp(back, mac, (size_t)n);
}

int main()
{
    for (int utf8 = 0; utf8 < 2; utf8++) {
        // Every Mac Roman character but ':' (which Mac names cannot hold).
        bool all = true;
        for (int c = 0x01; c < 0x100; c++) {
            if (c == ':') continue;
            u8 name[3] = {'a', (u8)c, 'b'};
            if (!roundTrip(name, 3, utf8)) { all = false; printf("character %02X does not survive (utf8 %d)\n", c, utf8); }
        }
        CHECK(all);
        // Leading dots (hidden on the card) and trailing dots and spaces (FAT drops them).
        static const u8 dots[] = {'.', 'x', '.'};
        CHECK(roundTrip(dots, 3, utf8));
        static const u8 space[] = {'x', ' '};
        CHECK(roundTrip(space, 2, utf8));
        char host[512];
        SharedFolder::macToHost(dots, 3, host, sizeof host, utf8);
        CHECK(host[0] != '.' && host[strlen(host) - 1] != '.');
    }

    char host[512];
    static const u8 cafe[] = {'C', 'a', 'f', 0x8E};     // "Café" in Mac Roman
    SharedFolder::macToHost(cafe, 4, host, sizeof host, true);
    CHECK(!strcmp(host, "Caf\xC3\xA9"));                // UTF-8
    SharedFolder::macToHost(cafe, 4, host, sizeof host, false);
    CHECK(!strcmp(host, "Caf\x82"));                    // code page 850
    static const u8 bad[] = {'a', '/', 'b', '?', '%'};
    SharedFolder::macToHost(bad, 5, host, sizeof host, true);
    CHECK(!strcmp(host, "a%2Fb%3F%25"));
    static const u8 tm[] = {0xAA};                      // ™: not in code page 850
    SharedFolder::macToHost(tm, 1, host, sizeof host, false);
    CHECK(!strcmp(host, "%AA"));

    // Host names: UTF-8 and code page 850 into Mac Roman; unknown characters '?'.
    u8 mac[32];
    int n = SharedFolder::hostToMac("na\xC3\xAFve \xE2\x80\x94 \xE6\x97\xA5", mac, true);
    CHECK(n == 9 && mac[2] == 0x95 && mac[6] == 0xD1 && mac[8] == '?');
    n = SharedFolder::hostToMac("\x82t\x82", mac, false);
    CHECK(n == 3 && mac[0] == 0x8E && mac[2] == 0x8E);

    // Too long for the Mac: 31 characters, distinct for distinct names.
    u8 a[32], b[32];
    int la = SharedFolder::hostToMac("A very long file name from a computer, part one.txt", a, true);
    int lb = SharedFolder::hostToMac("A very long file name from a computer, part two.txt", b, true);
    CHECK(la == 31 && lb == 31 && memcmp(a, b, 31) != 0);
    CHECK(!memcmp(a, "A very long file name fro#", 26));

    // Mac names compare without regard to case.
    static const u8 x[] = {'R', 'e', 'a', 'd'}, y[] = {'r', 'E', 'A', 'D'};
    CHECK(SharedFolder::sameMacName(x, 4, y, 4));
    CHECK(!SharedFolder::sameMacName(x, 4, y, 3));

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
