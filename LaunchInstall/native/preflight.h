// preflight.h - what LaunchInstall checks before it writes anything (the user 2026-11-17: a firmware with a modified
// CAN encrypts \ResidentFlash and the system partition to protect its files - there the install must be refused at
// once, with the reason in LaunchInstall.log). Pure: the bytes in, the answer out.
#ifndef PREFLIGHT_H
#define PREFLIGHT_H

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// A Windows program's bytes: "MZ", then "PE\0\0" where its e_lfanew (the little-endian u32 at 0x3C) points. An
// encrypted file has neither.
static int PeImage(const unsigned char* d, DWORD n)
{
    if (!d || n < 0x40 || d[0] != 'M' || d[1] != 'Z')
        return 0;
    DWORD at;
    memcpy(&at, d + 0x3C, sizeof(at));// (both the unit and the PC are little-endian)
    return at <= n - 4 && !memcmp(d + at, "PE\0\0", 4);
}

// VestaHome's loader stands in G3NavHMI.exe (2026-11-25): it renames the shell G3NavHMI_orig.exe and is far smaller
// (~0.1 MB, the shell ~4 MB). Then the shell's backup (our patch's) goes back to G3NavHMI_orig.exe, never over the
// loader - else VestaHome no longer starts.
static int ShellIsLoader(DWORD exeSize, bool origThere)
{
    return origThere && exeSize > 0 && exeSize < 1024 * 1024;
}

// ExternNavi.cfg as the shell reads it: lines of paths - no zero byte, no control byte but tab and line ends.
static int CfgIsText(const char* d, DWORD n)
{
    for (DWORD i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        if (c == 0x7F || (c < 0x20 && c != '\t' && c != '\r' && c != '\n'))
            return 0;
    }
    return 1;
}

// The trial write in the shell's folder (LaunchInstall's ProbeWrite: 0 ok, 1 not created, 2 read back other bytes,
// 3 not deleted) as the refusal's reason; NULL - fine.
static const wchar_t* ProbeRefusal(int probe)
{
    switch (probe) {
    case 0:  return NULL;
    case 1:  return L"в папку оболочки нельзя записать файл — раздел защищён";
    case 2:  return L"записанный в папку оболочки файл читается иначе — раздел шифрует файлы";
    default: return L"пробный файл в папке оболочки не удаляется — раздел защищён";
    }
}

// Shannon entropy of the bytes, bits per byte x 100 (0 .. 800): an encrypted or compressed file is near 800, a program's
// header and text far below (the probe, 2026-11-17).
static int EntropyX100(const unsigned char* d, DWORD n)
{
    DWORD count[256];
    memset(count, 0, sizeof(count));
    for (DWORD i = 0; i < n; i++)
        count[d[i]]++;
    double h = 0;
    for (int v = 0; v < 256; v++)
        if (count[v]) {
            double p = (double)count[v] / n;
            h -= p * log(p) / log(2.0);
        }
    return (int)(h * 100 + 0.5);
}

// How a file's first bytes look to us (the probe): a program, a program we can't read (an .exe / .dll / .mui
// without its header - encrypted for us), text, noise (encrypted or compressed: PNG, ZIP look so too), plain data.
static const wchar_t* Looks(const wchar_t* name, const unsigned char* d, DWORD n)
{
    if (!d || !n)
        return L"пустой";
    if (PeImage(d, n))
        return L"программа";
    const wchar_t* dot = wcsrchr(name, L'.');
    if (dot && (!_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".dll") || !_wcsicmp(dot, L".mui")))
        return L"ПРОГРАММА НЕ ЧИТАЕТСЯ";
    if (CfgIsText((const char*)d, n))
        return L"текст";
    return EntropyX100(d, n) >= 750 ? L"шум (шифр или сжатие)" : L"данные";
}

// CRC-32 (zlib's) of n more bytes after crc (0 to start): a whole file read in pieces (the probe's "целостность",
// the user 17.11: files copied off \ResidentFlash of the CAN panel firmware came out broken on the PC).
static unsigned long Crc32(unsigned long crc, const unsigned char* d, DWORD n)
{
    crc = ~crc & 0xFFFFFFFFUL;
    for (DWORD i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
    }
    return ~crc & 0xFFFFFFFFUL;
}

// An update and the user's own look (the user 17.11: Icons and the Skin edits kept). The installer leaves a list of
// what it put (shipped.crc: "Skin\name CRC" a line); a file of the old install's Icons / Skin goes:
//   not in the new version - the old one (the user's own); as the list says - the new one; changed - the old one;
//   no list (an install before it) - the new one, the old kept aside (Skin.previous): edits can't be told apart.
enum { KEEP_NEW, KEEP_OLD, KEEP_ASIDE };
static int KeepWhich(bool inNew, bool listed, unsigned long oldCrc, unsigned long shippedCrc)
{
    if (!inNew)
        return KEEP_OLD;
    if (!listed)
        return KEEP_ASIDE;
    return oldCrc == shippedCrc ? KEEP_NEW : KEEP_OLD;
}

// The CRC the list gives a file (its relative name, any case); false - not listed, or no list.
static bool ShippedCrc(const char* list, const char* name, unsigned long* crc)
{
    size_t nl = strlen(name);
    for (const char* p = list; p && *p;) {
        const char* e = strpbrk(p, "\r\n");
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > nl + 1 && p[nl] == ' ' && !_strnicmp(p, name, nl)) {
            *crc = strtoul(p + nl + 1, NULL, 16);
            return true;
        }
        p = e ? e + 1 : NULL;
    }
    return false;
}

// The first bytes (at most 16) in hex for the log: "4D 5A 90 00"; "-" when none. cap: at least 2.
static void HexHead(const unsigned char* d, DWORD n, wchar_t* out, int cap)
{
    if (!d || !n) {
        wcscpy(out, L"-");
        return;
    }
    int k = 0;
    out[0] = 0;
    for (DWORD i = 0; i < n && i < 16 && k + 4 <= cap; i++)   // (" XX" and the zero still fit)
        k += _snwprintf(out + k, cap - k, i ? L" %02X" : L"%02X", d[i]);
}

#endif
