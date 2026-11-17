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
