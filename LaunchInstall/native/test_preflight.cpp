// test_preflight.cpp - preflight.h on the PC: what LaunchInstall checks before it writes anything (a firmware that
// encrypts \ResidentFlash - the user 2026-11-17: refused at once, the reason in the log).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "preflight.h"

static int s_failed, s_checks;

static void Check(bool ok, const char* what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAILED: %s\n", what);
    }
}

int main()
{
    unsigned char pe[512];
    memset(pe, 0xCC, sizeof(pe));   // (not zeros: a wrong byte looked at is not taken for the PE's)
    memset(pe + 0x3C, 0, 4);
    pe[0] = 'M';
    pe[1] = 'Z';
    pe[0x3C] = 0x80;   // e_lfanew
    memcpy(pe + 0x80, "PE\0\0", 4);
    Check(PeImage(pe, sizeof(pe)) != 0, "a program: MZ and PE where e_lfanew says");
    Check(!PeImage(pe, 0x83), "PE cut short: not a program");
    Check(PeImage(pe, 0x84) != 0, "PE just fits: a program");
    pe[0x81] = 'X';
    Check(!PeImage(pe, sizeof(pe)), "no PE signature: not a program");
    pe[0x81] = 'E';
    pe[0] = 0x3A;   // encrypted bytes
    Check(!PeImage(pe, sizeof(pe)), "no MZ (encrypted): not a program");
    pe[0] = 'M';
    pe[1] = 'Q';
    Check(!PeImage(pe, sizeof(pe)), "MZ's second byte wrong: not a program");
    pe[1] = 'Z';
    pe[0x3C] = 0xFE;
    pe[0x3D] = 0xFF;   // e_lfanew far past the end
    Check(!PeImage(pe, sizeof(pe)), "e_lfanew past the end: not a program");
    Check(!PeImage(pe, 2) && !PeImage(NULL, 0), "too short: not a program");
    pe[0x3C] = 0x38;   // the shortest program: PE at 0x38, e_lfanew right after it, 0x40 bytes in all
    pe[0x3D] = 0;
    memcpy(pe + 0x38, "PE\0\0", 4);
    Check(PeImage(pe, 0x40) != 0, "0x40 bytes with PE inside: a program");
    Check(!PeImage(pe, 0x3F), "0x3F bytes: too short");
    memset(pe + 0x38, 0xCC, 4);
    pe[0x3C] = 0x80;
    pe[0x3D] = 0x01;   // e_lfanew 0x180: its second byte counts
    memcpy(pe + 0x180, "PE\0\0", 4);
    Check(PeImage(pe, 0x200) != 0, "e_lfanew over 255: a program");
    Check(!PeImage(pe, 0x183), "e_lfanew over 255, cut short: not a program");

    const char cfg[] = "\\SDMMC\\VestaLaunch\\VestaLaunch.exe\r\n\\SDMMC\\Navitel\\Navitel.exe\r\n\tx";
    Check(CfgIsText(cfg, (DWORD)strlen(cfg)) != 0, "the cfg's paths: text");
    Check(CfgIsText("", 0) != 0, "an empty cfg: text");
    const char utf8[] = "\\SDMMC\\\xD0\x9D\xD0\xB0\xD0\xB2\\a.exe";
    Check(CfgIsText(utf8, (DWORD)strlen(utf8)) != 0, "UTF-8 letters: text");
    const char nul[] = { 'a', 0, 'b' };
    Check(!CfgIsText(nul, 3), "a zero byte: not text");
    const char ctl[] = { 'a', 0x1B, 'b' };
    Check(!CfgIsText(ctl, 3), "a control byte: not text");
    const char del[] = { 'a', 0x7F };
    Check(!CfgIsText(del, 2), "DEL: not text");
    const char last[] = { 'a', 'b', 0x01 };
    Check(!CfgIsText(last, 3), "a control byte at the end: not text");
    const char first[] = { 0x02, 'a' };
    Check(!CfgIsText(first, 2), "a control byte first: not text");
    Check(CfgIsText(" a b ", 5) == 1, "spaces: text (1)");

    wchar_t hex[64];
    const unsigned char head[] = { 0x4D, 0x5A, 0x90, 0x00, 0xAB };
    HexHead(head, 5, hex, 64);
    Check(!wcscmp(hex, L"4D 5A 90 00 AB"), "the head in hex");
    unsigned char many[40];
    for (int i = 0; i < 40; i++)
        many[i] = (unsigned char)i;
    HexHead(many, 40, hex, 64);
    Check(wcslen(hex) == 16 * 3 - 1 && !wcsncmp(hex, L"00 01 02", 8), "at most 16 bytes");
    HexHead(many, 40, hex, 12);
    Check(!wcscmp(hex, L"00 01 02 03"), "12 places: 4 bytes and the zero, exactly");
    HexHead(many, 40, hex, 11);
    Check(!wcscmp(hex, L"00 01 02"), "11 places: 3 bytes");
    HexHead(many, 40, hex, 3);
    Check(!wcscmp(hex, L""), "3 places: none (the first takes 3 with its zero... kept empty)");
    HexHead(NULL, 0, hex, 64);
    Check(!wcscmp(hex, L"-"), "nothing: a dash");
    HexHead(many, 0, hex, 64);
    Check(!wcscmp(hex, L"-"), "no bytes: a dash");
    Check(ProbeRefusal(0) == NULL, "the trial write fine: no refusal");
    Check(ProbeRefusal(1) && wcsstr(ProbeRefusal(1), L"нельзя записать"), "not created: the partition protected");
    Check(ProbeRefusal(2) && wcsstr(ProbeRefusal(2), L"шифрует"), "read back otherwise: it encrypts");
    Check(ProbeRefusal(3) && wcsstr(ProbeRefusal(3), L"не удаляется"), "not deleted: protected");
    // the probe (a firmware that encrypts its partitions): how a file's first bytes look
    unsigned char same[256], all[256];
    memset(same, 'A', sizeof(same));
    for (int i = 0; i < 256; i++)
        all[i] = (unsigned char)i;
    Check(EntropyX100(same, 256) == 0, "one byte value: entropy 0");
    Check(EntropyX100(all, 256) == 800, "every byte value once: 8 bits");
    Check(EntropyX100(all, 128) == 700, "128 values once: 7 bits");
    Check(EntropyX100(NULL, 0) == 0, "nothing: 0");
    Check(!wcscmp(Looks(L"a.dll", pe, 0x200), L"программа"), "a program read as one");
    Check(!wcscmp(Looks(L"G3NavHMI.EXE", all, 256), L"ПРОГРАММА НЕ ЧИТАЕТСЯ"), "an exe without its header: told");
    Check(!wcscmp(Looks(L"x.mui", all, 256), L"ПРОГРАММА НЕ ЧИТАЕТСЯ"), "a .mui (resources) too");
    Check(!wcscmp(Looks(L"ExternNavi.cfg", (const unsigned char*)"\\SDMMC\\a.exe\r\n", 14), L"текст"), "a cfg: text");
    Check(!wcscmp(Looks(L"x.bin", all, 256), L"шум (шифр или сжатие)"), "noise");
    unsigned char low[256];
    memset(low, 0x01, sizeof(low));
    Check(!wcscmp(Looks(L"x.bin", low, 256), L"данные"), "plain data (not text, not noise)");
    Check(!wcscmp(Looks(L"x.bin", all, 200), L"шум (шифр или сжатие)"), "200 values: 7.64 bits - noise");
    Check(!wcscmp(Looks(L"x.bin", all + 1, 150), L"данные"), "150 values: 7.23 bits - data");
    Check(!wcscmp(Looks(L"empty.txt", NULL, 0), L"пустой"), "an empty file");
    Check(!wcscmp(Looks(L"empty.txt", all, 0), L"пустой"), "no bytes read: empty");
    unsigned char edge[256];   // 0..63 twice, 64..191 once: exactly 7.50 bits (and not text: control bytes in it)
    for (int i = 0; i < 128; i++)
        edge[i] = (unsigned char)(i / 2);
    for (int i = 0; i < 128; i++)
        edge[128 + i] = (unsigned char)(64 + i);
    Check(EntropyX100(edge, 256) == 750, "the edge: 7.50 bits");
    Check(!wcscmp(Looks(L"x.bin", edge, 256), L"шум (шифр или сжатие)"), "7.50 bits: noise");
    Check(Crc32(0, (const unsigned char*)"123456789", 9) == 0xCBF43926UL, "CRC32 of the check string");
    Check(Crc32(Crc32(0, (const unsigned char*)"1234", 4), (const unsigned char*)"56789", 5) == 0xCBF43926UL,
          "CRC32 in two pieces: the same");
    Check(Crc32(0, NULL, 0) == 0, "CRC32 of nothing: 0");
    // an update and the user's own look (the user 17.11: keep Icons and the Skin edits)
    Check(KeepWhich(false, false, 1, 0) == KEEP_OLD, "not in the new version (the user's own picture): kept");
    Check(KeepWhich(true, true, 0x11, 0x11) == KEEP_NEW, "as the installer put it: the new version's");
    Check(KeepWhich(true, true, 0x22, 0x11) == KEEP_OLD, "changed by the user: theirs kept");
    Check(KeepWhich(true, false, 0x22, 0) == KEEP_ASIDE, "no list of what was put (older installs): the new one, "
                                                         "theirs kept aside");
    unsigned long crc = 0;
    const char* list = "# shipped\r\nSkin\\skin.ini 0A0B0C0D\r\nskin\\add.png deadbeef\n";
    Check(ShippedCrc(list, "Skin\\skin.ini", &crc) && crc == 0x0A0B0C0DUL, "the list: a file's CRC");
    Check(ShippedCrc(list, "Skin\\ADD.png", &crc) && crc == 0xDEADBEEFUL, "the list: names in any case, the last line");
    Check(!ShippedCrc(list, "Skin\\next.png", &crc), "the list: a file not in it");
    Check(!ShippedCrc(list, "Skin", &crc), "the list: a name's start alone is not it");
    Check(!ShippedCrc(NULL, "Skin\\skin.ini", &crc), "no list");
    // VestaHome's loader in G3NavHMI.exe (the shell renamed G3NavHMI_orig.exe): our backup goes to the shell, never
    // over the loader (2026-11-25: an uninstall left VestaHome unstarted)
    Check(ShellIsLoader(112128, true), "a small G3NavHMI.exe with the shell beside it: the loader");
    Check(!ShellIsLoader(4077056, true), "the shell's own size: the shell");
    Check(!ShellIsLoader(112128, false), "no G3NavHMI_orig.exe: whatever it is, it is the shell's place");
    Check(!ShellIsLoader(0, true), "no G3NavHMI.exe: not the loader");
    printf("test_preflight: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
