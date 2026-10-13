// LaunchInstall - one-file installer of VestaLaunch for the Lada Vesta MMC (Windows CE 6.0, ARM, 800x480),
// with the G3NavHMI patch, the "Navigation" button caption and tile patches and automatic rollback. Also
// builds as a desktop app for testing.
//
// What "Install" does (each step is written to a journal first and verified after):
//   1. unpacks the embedded VestaLaunch (exe + Skin) into \SDMMC\VestaLaunch.new and compares every
//      written file with the embedded bytes; the user's apps.txt of an older install is carried over;
//   2. G3NavHMI.exe: the AppLauncher loopback patch "127.0.0.1" -> "127.0.0.2" (one byte; the shell then
//      opens the first entry of ExternNavi.cfg on "Navigation"). Only if the exe holds exactly one
//      "127.0.0.1" and no "127.0.0.2"; one "127.0.0.2" and no "127.0.0.1" = already patched, left as is;
//      anything else = unknown firmware, refused. The exe is backed up (next to it and on \SDMMC) and,
//      after the write, compared with the backup: exactly that byte may differ;
//   3. Config\ExternNavi.cfg: \SDMMC\VestaLaunch\VestaLaunch.exe as the first line (backup, written
//      through a temporary file, read back);
//   4. language_cn.qm: the "Navigation" button caption "Навигация" -> "Программы" in place (the entry
//      whose source is "NAVI"; both captions are 9 letters, so the file keeps its size). Only if that
//      one entry is there; a file already saying "Программы" is left alone;
//   5. the "Navigation" button tile main_symbol_navigation.png -> ours (an app grid), only when the file
//      is byte for byte the factory one (another skin's tile is left alone);
//   6. swaps VestaLaunch.new in (an older VestaLaunch is kept as VestaLaunch.old until the end);
//   7. if the shell had to be closed for step 2: starts it again and watches it for 30 s.
// Any failed step, a failed check or a shell that dies in those 30 s rolls everything back in reverse order
// from the journal and starts the original shell. A journal left by an interrupted run (power off) is
// rolled back by the next "Install" or "Uninstall".
//
// The registry does not survive a reboot, so the state lives in files: the backups, the journal
// \SDMMC\VestaLaunch_install.jrn and the log LaunchInstall.log next to this exe.
// Closing the shell: the head unit reboots about 2 minutes after the shell is gone (the MCU misses its
// heartbeat), so the shell is started again right after the patch. Main.exe does not watch a running shell.
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <tlhelp32.h>

#define APP_CLASS       L"LaunchInstall_Wnd"
#define IDR_PAYLOAD     101
#define IDR_ICONFACTORY 102
#define IDR_ICONVESTA   103

static const char*    kLoopOld  = "127.0.0.1";
static const char*    kLoopNew  = "127.0.0.2";
static const wchar_t* kShell    = L"G3NavHMI.exe";
static const wchar_t* kLauncher = L"VestaLaunch.exe";
static const char*    kCfgLine  = "\\SDMMC\\VestaLaunch\\VestaLaunch.exe";   // what ExternNavi.cfg points to
// The shell's "Navigation" button caption in language\language_cn.qm: the entry whose source is "NAVI" and
// whose translation is "Навигация" (the only "Навигация" followed by that source — three others sit inside
// longer sentences). We put "Программы" there: also 9 letters, so the file keeps its size and the shell's
// lookup stays valid. .qm strings are UTF-16, big-endian, without a terminator.
enum { kCapLen = 18, kTailLen = 14 };
static const unsigned char kNavCap[kCapLen] = { 0x04,0x1D,0x04,0x30,0x04,0x32,0x04,0x38,0x04,0x33,0x04,0x30,0x04,0x46,0x04,0x38,0x04,0x4F };
static const unsigned char kPrgCap[kCapLen] = { 0x04,0x1F,0x04,0x40,0x04,0x3E,0x04,0x33,0x04,0x40,0x04,0x30,0x04,0x3C,0x04,0x3C,0x04,0x4B };
static const unsigned char kQmTail[kTailLen] = { 0x08,0,0,0, 0x00, 0x06,0,0,0, 0x04, 'N','A','V','I' };
#ifdef UNDER_CE
enum { kWatchMs = 30000 };
#else
enum { kWatchMs = 3000 };   // PC test: the "shell" is the program named in LI_SHELL
#endif

static wchar_t g_appDir[MAX_PATH];   // ...\navigation\app
static wchar_t g_sd[MAX_PATH];       // \SDMMC (PC test: <appDir>\..\SDMMC)
static wchar_t g_exe[MAX_PATH], g_exeBak[MAX_PATH], g_exeBak2[MAX_PATH];
static wchar_t g_cfg[MAX_PATH], g_cfgBak[MAX_PATH], g_cfgTmp[MAX_PATH];
static wchar_t g_launch[MAX_PATH], g_new[MAX_PATH], g_old[MAX_PATH];
static wchar_t g_journal[MAX_PATH];
static wchar_t g_qm[MAX_PATH], g_qmBak[MAX_PATH];   // the shell's translation: the "Navigation" button caption
static int   g_qmState;                             // QM_*: what that caption is now
static wchar_t g_icon[MAX_PATH], g_iconBak[MAX_PATH];   // the shell's "Navigation" button tile
static int   g_iconState;                               // ICON_*: which art that tile is

// detection
enum { EXE_MISSING, EXE_CLEAN, EXE_PATCHED, EXE_UNKNOWN };
enum { QM_MISSING, QM_FACTORY, QM_PATCHED, QM_UNKNOWN };
enum { ICON_MISSING, ICON_FACTORY, ICON_VESTA, ICON_OTHER };
static int   g_exeState;
static DWORD g_loopOff;              // offset of "127.0.0.1" in the exe (EXE_CLEAN)
static int   g_cfgOurs;              // ExternNavi.cfg starts with our line
static int   g_hasLaunch;            // \SDMMC\VestaLaunch\VestaLaunch.exe exists
static int   g_hasJournal;           // an interrupted run left its journal
static int   g_bootFlag;             // \ResidentFlash\SwUpdate.flag is armed: a boot update is pending
static int   g_payloadOk;
static int   g_ui;                   // started with the window (asks before closing the shell)

static wchar_t g_status[2048];       // what the last action did, one line per step
static HWND    g_hwnd;

// ---------------------------------------------------------------- paths, log, status

static void Join(wchar_t* out, const wchar_t* a, const wchar_t* b)
{
    int la = (int)wcslen(a);
    _snwprintf(out, MAX_PATH, L"%s%s%s", a, (la && a[la - 1] == L'\\') ? L"" : L"\\", b);
    out[MAX_PATH - 1] = 0;
}

static void ExeDir(wchar_t* out)
{
    GetModuleFileName(NULL, out, MAX_PATH);
    wchar_t* p = wcsrchr(out, L'\\');
    if (p)
        *p = 0;
}

static void LogMsg(const wchar_t* s)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    ExeDir(dir);
    Join(path, dir, L"LaunchInstall.log");
    HANDLE h = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(h, 0, NULL, FILE_END);
    char buf[1200];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 2, NULL, NULL);
    if (n > 0) {
        DWORD w;
        WriteFile(h, buf, (DWORD)(n - 1), &w, NULL);
        WriteFile(h, "\r\n", 2, &w, NULL);
    }
    CloseHandle(h);
}

// One line of the result: shown in the window and written to the log.
static void Say(const wchar_t* fmt, ...)
{
    wchar_t line[400];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(line, 400, fmt, a);
    va_end(a);
    line[399] = 0;
    LogMsg(line);
    int n = (int)wcslen(g_status);
    if (n + (int)wcslen(line) + 2 < 2048) {
        if (n)
            wcscat(g_status, L"\n");
        wcscat(g_status, line);
    }
    if (g_hwnd) {   // show the progress while the steps run
        InvalidateRect(g_hwnd, NULL, FALSE);
        UpdateWindow(g_hwnd);
    }
}

static void SetPaths()
{
#ifdef UNDER_CE
    wcscpy(g_sd, L"\\SDMMC");
#else
    Join(g_sd, g_appDir, L"..\\SDMMC");
    CreateDirectory(g_sd, NULL);
#endif
    Join(g_exe, g_appDir, kShell);
    wcscpy(g_exeBak, g_exe);
    wcscat(g_exeBak, L".vlaunch.bak");
    Join(g_exeBak2, g_sd, L"VestaLaunch_backup\\G3NavHMI.exe");
    Join(g_cfg, g_appDir, L"Config\\ExternNavi.cfg");
    wcscpy(g_cfgBak, g_cfg);
    wcscat(g_cfgBak, L".vlaunch.bak");
    wcscpy(g_cfgTmp, g_cfg);
    wcscat(g_cfgTmp, L".vlaunch.tmp");
    Join(g_launch, g_sd, L"VestaLaunch");
    Join(g_new, g_sd, L"VestaLaunch.new");
    Join(g_old, g_sd, L"VestaLaunch.old");
    Join(g_journal, g_sd, L"VestaLaunch_install.jrn");
    Join(g_qm, g_appDir, L"language\\language_cn.qm");
    wcscpy(g_qmBak, g_qm);
    wcscat(g_qmBak, L".vlaunch.bak");
    Join(g_icon, g_appDir, L"res\\img\\Itelma\\MMC27\\Temp\\main\\main_symbol_navigation.png");
    wcscpy(g_iconBak, g_icon);
    wcscat(g_iconBak, L".vlaunch.bak");
}

// ---------------------------------------------------------------- files

static int Exists(const wchar_t* p)
{
    return GetFileAttributes(p) != 0xFFFFFFFF;
}

static int DirExists(const wchar_t* p)
{
    DWORD a = GetFileAttributes(p);
    return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void EnsureDirTree(const wchar_t* dir)
{
    wchar_t tmp[MAX_PATH];
    wcscpy(tmp, dir);
    for (wchar_t* p = tmp + 1; *p; p++)
        if (*p == L'\\') {
            *p = 0;
            CreateDirectory(tmp, NULL);
            *p = L'\\';
        }
    CreateDirectory(tmp, NULL);
}

static int WriteBytes(const wchar_t* path, const void* data, DWORD len)
{
    wchar_t dir[MAX_PATH];
    wcscpy(dir, path);
    wchar_t* sl = wcsrchr(dir, L'\\');
    if (sl) {
        *sl = 0;
        EnsureDirTree(dir);
    }
    HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    DWORD w = 0;
    int ok = WriteFile(h, data, len, &w, NULL) && w == len;
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

// The file holds exactly these bytes.
static int SameAs(const wchar_t* path, const void* data, DWORD len)
{
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int ok = GetFileSize(h, NULL) == len;
    char buf[4096];
    for (DWORD pos = 0; ok && pos < len; ) {
        DWORD want = len - pos < sizeof(buf) ? len - pos : sizeof(buf), got = 0;
        ok = ReadFile(h, buf, want, &got, NULL) && got == want && !memcmp(buf, (const char*)data + pos, want);
        pos += want;
    }
    CloseHandle(h);
    return ok;
}

// Compares two files; returns the number of differing bytes (-1: unreadable or different sizes) and the
// offset of the first one.
static int Diff(const wchar_t* a, const wchar_t* b, DWORD* first)
{
    HANDLE ha = CreateFile(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hb = CreateFile(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    int n = -1;
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE && GetFileSize(ha, NULL) == GetFileSize(hb, NULL)) {
        static char ba[8192], bb[8192];
        DWORD pos = 0, ga, gb;
        n = 0;
        while (ReadFile(ha, ba, sizeof(ba), &ga, NULL) && ReadFile(hb, bb, sizeof(bb), &gb, NULL) && ga && ga == gb) {
            for (DWORD i = 0; i < ga; i++)
                if (ba[i] != bb[i]) {
                    if (!n && first)
                        *first = pos + i;
                    n++;
                }
            pos += ga;
        }
    }
    if (ha != INVALID_HANDLE_VALUE)
        CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE)
        CloseHandle(hb);
    return n;
}

// Copy and check that the copy is identical.
static int CopyChecked(const wchar_t* src, const wchar_t* dst)
{
    wchar_t dir[MAX_PATH];
    wcscpy(dir, dst);
    wchar_t* sl = wcsrchr(dir, L'\\');
    if (sl) {
        *sl = 0;
        EnsureDirTree(dir);
    }
    SetFileAttributes(dst, FILE_ATTRIBUTE_NORMAL);
    HANDLE hi = CreateFile(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hi == INVALID_HANDLE_VALUE)
        return 0;
    HANDLE ho = CreateFile(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (ho == INVALID_HANDLE_VALUE) {
        CloseHandle(hi);
        return 0;
    }
    static char buf[8192];
    DWORD got;
    int ok = 1;
    while (ok && ReadFile(hi, buf, sizeof(buf), &got, NULL) && got) {
        DWORD w;
        ok = WriteFile(ho, buf, got, &w, NULL) && w == got;
    }
    FlushFileBuffers(ho);
    CloseHandle(ho);
    CloseHandle(hi);
    return ok && Diff(src, dst, NULL) == 0;
}

static void RemoveTree(const wchar_t* path)
{
    wchar_t pat[MAX_PATH];
    Join(pat, path, L"*");
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            wchar_t c[MAX_PATH];
            Join(c, path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTree(c);
            } else {
                SetFileAttributes(c, FILE_ATTRIBUTE_NORMAL);
                DeleteFile(c);
            }
        } while (FindNextFile(f, &fd));
        FindClose(f);
    }
    SetFileAttributes(path, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectory(path);
}

static int CanWrite(const wchar_t* p)
{
    HANDLE h = CreateFile(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    CloseHandle(h);
    return 1;
}

// Whole small file (cfg) into memory with a terminating zero; NULL if it can't be read.
static char* ReadAll(const wchar_t* path, DWORD* size)
{
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD n = GetFileSize(h, NULL), got = 0;
    char* buf = n > (1 << 20) ? NULL : (char*)malloc(n + 1);
    if (buf && !ReadFile(h, buf, n, &got, NULL)) {
        free(buf);
        buf = NULL;
    }
    CloseHandle(h);
    if (buf) {
        buf[got] = 0;
        *size = got;
    }
    return buf;
}

// Counts "127.0.0.1" / "127.0.0.2" in the exe (streamed), remembers where the first "127.0.0.1" is.
static void ScanExe(int* nOld, int* nNew, DWORD* offOld)
{
    *nOld = *nNew = 0;
    *offOld = 0;
    HANDLE h = CreateFile(g_exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    const int BUF = 1 << 16, KEEP = 16, L = 9;
    char* b = (char*)malloc(BUF + KEEP);
    if (!b) {
        CloseHandle(h);
        return;
    }
    DWORD base = 0;   // file offset of b[0]
    int carry = 0;
    for (;;) {
        DWORD got = 0;
        ReadFile(h, b + carry, BUF, &got, NULL);
        int total = carry + (int)got;
        int lim = got ? total - (L - 1) : total;
        for (int i = 0; i < lim && i + L <= total; i++) {
            if (!memcmp(b + i, kLoopOld, L)) {
                if (!*nOld)
                    *offOld = base + i;
                (*nOld)++;
            } else if (!memcmp(b + i, kLoopNew, L)) {
                (*nNew)++;
            }
        }
        if (!got)
            break;
        int rem = total - lim;
        memmove(b, b + lim, rem);
        base += lim;
        carry = rem;
    }
    free(b);
    CloseHandle(h);
}

// ---------------------------------------------------------------- the shell's button caption (language_cn.qm)
// The shell draws the translation of "NAVI" on its "Navigation" button. Counting occurrences of a caption
// directly followed by that source tells the button entry from the three "Навигация" inside sentences.

static int CountCap(const unsigned char* d, DWORD size, const unsigned char* cap)
{
    int n = 0;
    for (DWORD i = 0; i + kCapLen + kTailLen <= size; i++)
        if (!memcmp(d + i, cap, kCapLen) && !memcmp(d + i + kCapLen, kQmTail, kTailLen))
            n++;
    return n;
}

static void DetectQm()
{
    if (!Exists(g_qm)) {
        g_qmState = QM_MISSING;
        return;
    }
    DWORD size;
    unsigned char* d = (unsigned char*)ReadAll(g_qm, &size);
    g_qmState = QM_UNKNOWN;
    if (d) {
        int nOld = CountCap(d, size, kNavCap), nNew = CountCap(d, size, kPrgCap);
        g_qmState = nOld == 1 && nNew == 0 ? QM_FACTORY : nOld == 0 && nNew == 1 ? QM_PATCHED : QM_UNKNOWN;
        free(d);
    }
}

// "Навигация" -> "Программы" in place; the file is read back to check the result. The shell loads its
// translations at start-up and does not keep the file open, so this also works while it runs (on a fresh
// install it is closed here anyway: step 2 closed it).
static int PatchQm()
{
    DWORD size;
    unsigned char* d = (unsigned char*)ReadAll(g_qm, &size);
    if (!d)
        return 0;
    DWORD at = 0;
    int found = 0;
    for (DWORD i = 0; !found && i + kCapLen + kTailLen <= size; i++)
        if (!memcmp(d + i, kNavCap, kCapLen) && !memcmp(d + i + kCapLen, kQmTail, kTailLen)) {
            at = i;
            found = 1;
        }
    if (found)
        memcpy(d + at, kPrgCap, kCapLen);
    int ok = found && WriteBytes(g_qm, d, size) && SameAs(g_qm, d, size);
    free(d);
    return ok;
}

// ---------------------------------------------------------------- payload (VestaLaunch.exe + Skin\...)
// [u32 count][count x (u16 nameLen, name UTF-8 with '/', u32 dataLen, data)][u32 CRC-32 of all before]

struct Payload { const unsigned char* base; DWORD size; };

static unsigned long Crc32(const unsigned char* p, DWORD n)
{
    unsigned long c = 0xFFFFFFFFUL;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320UL & (0 - (c & 1)));
    }
    return ~c;
}

static unsigned ReadU16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static unsigned long ReadU32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long)p[3] << 24); }

// An embedded RCDATA block (payload.bin and the two button tiles).
static int GetRes(int id, Payload* pl)
{
    HRSRC r = FindResource(NULL, MAKEINTRESOURCE(id), RT_RCDATA);
    if (!r)
        return 0;
    pl->size = SizeofResource(NULL, r);
    pl->base = (const unsigned char*)LockResource(LoadResource(NULL, r));
    return pl->base && pl->size > 0;
}

static int GetPayload(Payload* pl)
{
    return GetRes(IDR_PAYLOAD, pl) && pl->size >= 8 && Crc32(pl->base, pl->size - 4) == ReadU32(pl->base + pl->size - 4);
}

// Writes every file under dir and reads it back; returns the number of files, -1 on any error.
static int Unpack(const Payload& pl, const wchar_t* dir)
{
    const unsigned char* p = pl.base + 4;
    const unsigned char* end = pl.base + pl.size - 4;
    unsigned long n = ReadU32(pl.base);
    for (unsigned long i = 0; i < n; i++) {
        if (p + 2 > end)
            return -1;
        unsigned nl = ReadU16(p);
        p += 2;
        char name[256];
        if (nl >= sizeof(name) || p + nl + 4 > end)
            return -1;
        memcpy(name, p, nl);
        name[nl] = 0;
        p += nl;
        unsigned long dl = ReadU32(p);
        p += 4;
        if (p + dl > end)
            return -1;
        wchar_t wname[256], path[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
        for (wchar_t* s = wname; *s; s++)
            if (*s == L'/')
                *s = L'\\';
        Join(path, dir, wname);
        if (!WriteBytes(path, p, dl) || !SameAs(path, p, dl)) {
            Say(L"  ошибка записи: %s", path);
            return -1;
        }
        p += dl;
    }
    return (int)n;
}

// ---------------------------------------------------------------- processes

typedef HANDLE (WINAPI *SnapshotFn)(DWORD, DWORD);
typedef BOOL   (WINAPI *ProcessFn)(HANDLE, PROCESSENTRY32*);
typedef BOOL   (WINAPI *CloseFn)(HANDLE);

// kill = 1: terminate every process of that exe name. Returns how many were found; -1: no toolhelp.
// toolhelp.dll is loaded dynamically: without it the program still starts.
static int ForProcess(const wchar_t* exeName, int kill)
{
#ifdef UNDER_CE
    HMODULE lib = LoadLibrary(L"toolhelp.dll");
    if (!lib)
        return -1;
    SnapshotFn snapshot = (SnapshotFn)GetProcAddress(lib, L"CreateToolhelp32Snapshot");
    ProcessFn  first    = (ProcessFn)GetProcAddress(lib, L"Process32First");
    ProcessFn  next     = (ProcessFn)GetProcAddress(lib, L"Process32Next");
    CloseFn    close    = (CloseFn)GetProcAddress(lib, L"CloseToolhelp32Snapshot");
#else
    HMODULE lib = LoadLibrary(L"kernel32.dll");
    if (!lib)
        return -1;
    SnapshotFn snapshot = (SnapshotFn)GetProcAddress(lib, "CreateToolhelp32Snapshot");
    ProcessFn  first    = (ProcessFn)GetProcAddress(lib, "Process32FirstW");
    ProcessFn  next     = (ProcessFn)GetProcAddress(lib, "Process32NextW");
    CloseFn    close    = (CloseFn)GetProcAddress(lib, "CloseHandle");
#endif
    int found = -1;
    if (snapshot && first && next && close) {
        HANDLE snap = snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            found = 0;
            PROCESSENTRY32 pe;
            memset(&pe, 0, sizeof(pe));
            pe.dwSize = sizeof(pe);
            for (BOOL ok = first(snap, &pe); ok; ok = next(snap, &pe)) {
                if (_wcsicmp(pe.szExeFile, exeName))
                    continue;
                found++;
                if (kill) {
                    HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (proc) {
                        TerminateProcess(proc, 0);
                        CloseHandle(proc);
                    }
                }
            }
            close(snap);
        }
    }
    FreeLibrary(lib);
    return found;
}

// Closes the shell and waits until its exe can be written (up to 6 s).
static int CloseShell()
{
    ForProcess(kShell, 1);
    for (int i = 0; i < 30; i++) {
        if (CanWrite(g_exe))
            return 1;
        Sleep(200);
    }
    return 0;
}

static HANDLE s_shellProc;   // the shell we started (the watchdog's fallback without toolhelp)

static void StartShell()
{
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    if (s_shellProc) {
        CloseHandle(s_shellProc);
        s_shellProc = NULL;
    }
#ifdef UNDER_CE
    if (ForProcess(kShell, 0) > 0)   // already back (started by someone else)
        return;
    if (CreateProcess(g_exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
#else
    // PC test: the "shell" is the program named in the environment variable LI_SHELL
    wchar_t cmd[MAX_PATH];
    if (!GetEnvironmentVariable(L"LI_SHELL", cmd, MAX_PATH))
        return;
    STARTUPINFO si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
#endif
        CloseHandle(pi.hThread);
        s_shellProc = pi.hProcess;
    }
    if (g_hwnd)   // the shell is topmost: keep the result visible
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

static int ShellAlive()
{
#ifdef UNDER_CE
    int n = ForProcess(kShell, 0);
    if (n >= 0)
        return n > 0;
#endif
    return s_shellProc && WaitForSingleObject(s_shellProc, 0) == WAIT_TIMEOUT;
}

// The shell must stay up for kWatchMs. 1 = it does.
static int Watch()
{
    for (int t = 0; t < kWatchMs; t += 500) {
        if (!ShellAlive())
            return 0;
        Sleep(500);
    }
    return ShellAlive();
}

// ---------------------------------------------------------------- journal
// One word per line, written before each step ("exe" / "cfg" / "qm" / "icon" once their backup is verified):
// "shell", "exe", "cfg", "cfg-none", "qm", "icon", "launch-old", "launch".

static void Journal(const char* step)
{
    HANDLE h = CreateFile(g_journal, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD w;
    WriteFile(h, step, (DWORD)strlen(step), &w, NULL);
    WriteFile(h, "\r\n", 2, &w, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static int Journaled(const char* text, const char* step)
{
    int n = (int)strlen(step);
    for (const char* p = text; (p = strstr(p, step)) != NULL; p += n)
        if ((p == text || p[-1] == '\n') && (p[n] == '\r' || p[n] == '\n' || !p[n]))
            return 1;
    return 0;
}

// Undoes the journaled steps in reverse order and starts the original shell. Returns 1 if all is back.
static int Rollback()
{
    DWORD size;
    char* j = ReadAll(g_journal, &size);
    if (!j)
        return 1;
    Say(L"Откат:");
    int ok = 1;
    if (Journaled(j, "launch") || Journaled(j, "launch-old")) {   // the new launcher out, an older one back
        ForProcess(kLauncher, 1);
        Sleep(300);
        if (Journaled(j, "launch")) {   // only then the folder is ours (the old one was moved aside before)
            RemoveTree(g_launch);
            Say(L"  новый VestaLaunch удалён");
        }
        if (Journaled(j, "launch-old") && DirExists(g_old)) {
            if (MoveFile(g_old, g_launch)) {
                Say(L"  прежний VestaLaunch возвращён");
            } else {
                Say(L"  НЕ удалось вернуть прежний VestaLaunch (%s)", g_old);
                ok = 0;
            }
        }
    }
    RemoveTree(g_new);
    if (Journaled(j, "cfg")) {
        if (Exists(g_cfgBak)) {
            if (CopyChecked(g_cfgBak, g_cfg)) {
                DeleteFile(g_cfgBak);
                Say(L"  ExternNavi.cfg восстановлен");
            } else {
                Say(L"  НЕ удалось восстановить ExternNavi.cfg (копия: %s)", g_cfgBak);
                ok = 0;
            }
        }
        DeleteFile(g_cfgTmp);
    }
    if (Journaled(j, "cfg-none")) {
        DeleteFile(g_cfg);
        Say(L"  ExternNavi.cfg удалён (его не было)");
    }
    if (Journaled(j, "qm") && Exists(g_qmBak) && Diff(g_qmBak, g_qm, NULL) != 0) {
        if (CopyChecked(g_qmBak, g_qm)) {
            DeleteFile(g_qmBak);
            Say(L"  language_cn.qm восстановлен");
        } else {
            Say(L"  НЕ удалось восстановить language_cn.qm (копия: %s)", g_qmBak);
            ok = 0;
        }
    }
    if (Journaled(j, "icon") && Exists(g_iconBak) && Diff(g_iconBak, g_icon, NULL) != 0) {
        if (CopyChecked(g_iconBak, g_icon)) {
            DeleteFile(g_iconBak);
            Say(L"  main_symbol_navigation.png восстановлен");
        } else {
            Say(L"  НЕ удалось восстановить main_symbol_navigation.png (копия: %s)", g_iconBak);
            ok = 0;
        }
    }
    if (Journaled(j, "exe") && Exists(g_exeBak) && Diff(g_exeBak, g_exe, NULL) != 0) {
        if (CloseShell() && CopyChecked(g_exeBak, g_exe)) {
            DeleteFile(g_exeBak);
            Say(L"  G3NavHMI.exe восстановлен из копии");
        } else {
            Say(L"  НЕ удалось восстановить G3NavHMI.exe (копии: %s, %s)", g_exeBak, g_exeBak2);
            ok = 0;
        }
    } else if (Journaled(j, "exe")) {
        DeleteFile(g_exeBak);   // the patch was never written
    }
    if (Journaled(j, "shell")) {
        StartShell();
        Say(L"  оболочка запущена");
    }
    free(j);
    if (ok)
        DeleteFile(g_journal);
    else
        Say(L"  журнал оставлен: %s", g_journal);
    return ok;
}

// ---------------------------------------------------------------- state

// The shell's button tile: on the factory firmware the 179x182 art; ours is the same tile with an app grid
// instead of the arrow. A tile that is neither (another skin's art) is left alone.
static void DetectIcon()
{
    Payload orig, vesta;
    if (!Exists(g_icon)) {
        g_iconState = ICON_MISSING;
    } else if (!GetRes(IDR_ICONFACTORY, &orig)) {
        g_iconState = ICON_OTHER;   // nothing to compare with: better leave the file alone
    } else if (SameAs(g_icon, orig.base, orig.size)) {
        g_iconState = ICON_FACTORY;
    } else {
        g_iconState = GetRes(IDR_ICONVESTA, &vesta) && SameAs(g_icon, vesta.base, vesta.size) ? ICON_VESTA : ICON_OTHER;
    }
}

static void Detect()
{
    SetPaths();
    int nOld, nNew;
    if (!Exists(g_exe)) {
        g_exeState = EXE_MISSING;
    } else {
        ScanExe(&nOld, &nNew, &g_loopOff);
        g_exeState = nOld == 1 && nNew == 0 ? EXE_CLEAN : nOld == 0 && nNew == 1 ? EXE_PATCHED : EXE_UNKNOWN;
    }
    DWORD size;
    char* cfg = ReadAll(g_cfg, &size);
    g_cfgOurs = cfg && !strncmp(cfg, kCfgLine, strlen(kCfgLine)) && (cfg[strlen(kCfgLine)] == '\r' || cfg[strlen(kCfgLine)] == '\n');
    free(cfg);
    DetectQm();
    DetectIcon();
    wchar_t exe[MAX_PATH];
    Join(exe, g_launch, kLauncher);
    g_hasLaunch = Exists(exe);
    g_hasJournal = Exists(g_journal);
    DWORD a = GetFileAttributes(L"\\ResidentFlash\\SwUpdate.flag");
    g_bootFlag = a != 0xFFFFFFFF && !(a & FILE_ATTRIBUTE_DIRECTORY);
    Payload pl;
    g_payloadOk = GetPayload(&pl);
}

// New ExternNavi.cfg: our line first, then the other lines byte for byte (ours removed from further down),
// so that removing our first line gives back the original file exactly.
static char* NewCfg(const char* old, DWORD* len)
{
    DWORD n = old ? (DWORD)strlen(old) : 0;
    char* out = (char*)malloc(strlen(kCfgLine) + 2 + n + 3);
    if (!out)
        return NULL;
    strcpy(out, kCfgLine);
    strcat(out, "\r\n");
    for (const char* p = old; p && *p; ) {
        const char* e = strchr(p, '\n');
        int ln = e ? (int)(e - p + 1) : (int)strlen(p);
        int body = ln;
        while (body > 0 && (p[body - 1] == '\n' || p[body - 1] == '\r'))
            body--;
        if (!(body == (int)strlen(kCfgLine) && !_strnicmp(p, kCfgLine, body)))
            strncat(out, p, ln);
        p += ln;
    }
    *len = (DWORD)strlen(out);
    return out;
}

// ExternNavi.cfg replaced through a temporary file, read back.
static int WriteCfg(const char* text, DWORD len)
{
    if (!WriteBytes(g_cfgTmp, text, len) || !SameAs(g_cfgTmp, text, len))
        return 0;
    SetFileAttributes(g_cfg, FILE_ATTRIBUTE_NORMAL);
    if (Exists(g_cfg) && !DeleteFile(g_cfg))
        return 0;
    return MoveFile(g_cfgTmp, g_cfg) && SameAs(g_cfg, text, len);
}

static int AskCloseShell()
{
    if (!g_ui)
        return 1;
    return MessageBox(g_hwnd,
        L"Для патча G3NavHMI.exe оболочку надо закрыть.\n"
        L"Установщик сразу запустит её снова и 30 секунд проследит, что она работает;\n"
        L"если нет — вернёт всё как было.\n\n"
        L"Закрыть оболочку и продолжить?", L"VestaLaunch", MB_YESNO | MB_ICONWARNING) == IDYES;
}

// ---------------------------------------------------------------- install / uninstall

static int Fail(const wchar_t* what)
{
    Say(L"ОШИБКА: %s", what);
    Rollback();
    Say(L"Установка не выполнена, система в прежнем состоянии.");
    return 0;
}

static int DoInstall()
{
    g_status[0] = 0;
    Detect();
    if (g_hasJournal) {
        Say(L"Найдена прерванная установка.");
        if (!Rollback()) {
            Say(L"Откат не завершён — установка отменена.");
            return 0;
        }
        Detect();
    }
    if (!g_payloadOk) { Say(L"ОШИБКА: встроенные файлы повреждены (CRC)."); return 0; }
    if (g_bootFlag)   { Say(L"Отказ: заряжено загрузочное обновление (\\ResidentFlash\\SwUpdate.flag)."); return 0; }
    if (g_exeState == EXE_MISSING) { Say(L"ОШИБКА: нет %s", g_exe); return 0; }
    if (g_exeState == EXE_UNKNOWN) {
        Say(L"Отказ: в G3NavHMI.exe не найден ровно один адрес 127.0.0.1 — незнакомая прошивка, патч не делается.");
        return 0;
    }
    int patch = g_exeState == EXE_CLEAN;
    if (patch) {
        HANDLE h = CreateFile(g_exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        DWORD need = h != INVALID_HANDLE_VALUE ? GetFileSize(h, NULL) : 0;
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        ULARGE_INTEGER freeApp, total, totalFree;
        if (GetDiskFreeSpaceEx(g_appDir, &freeApp, &total, &totalFree) && freeApp.QuadPart < (ULONGLONG)need + 512 * 1024) {
            Say(L"Отказ: в %s мало места для копии G3NavHMI.exe (%u КБ).", g_appDir, need / 1024);
            return 0;
        }
        if (!AskCloseShell()) { Say(L"Отменено."); return 0; }
    }
    Say(L"Установка VestaLaunch:");

    // 1. the launcher, unpacked aside and checked
    Payload pl;
    GetPayload(&pl);
    ForProcess(kLauncher, 1);   // a running copy holds its folder
    RemoveTree(g_new);
    int files = Unpack(pl, g_new);
    if (files < 0) {
        RemoveTree(g_new);
        Say(L"ОШИБКА: VestaLaunch не распакован, ничего не изменено.");
        return 0;
    }
    wchar_t apps[MAX_PATH], newApps[MAX_PATH];
    Join(apps, g_launch, L"apps.txt");
    Join(newApps, g_new, L"apps.txt");
    if (Exists(apps) && !CopyChecked(apps, newApps)) {
        RemoveTree(g_new);
        Say(L"ОШИБКА: не удалось перенести apps.txt, ничего не изменено.");
        return 0;
    }
    Say(L"  1. VestaLaunch распакован и проверен: %d файлов%s", files, Exists(apps) ? L", apps.txt перенесён" : L"");

    // 2. G3NavHMI.exe: one byte
    if (patch) {
        Journal("shell");
        if (!CloseShell())
            return Fail(L"оболочка не закрылась, G3NavHMI.exe занят");
        if (!CopyChecked(g_exe, g_exeBak)) {
            DeleteFile(g_exeBak);
            return Fail(L"не удалось сделать копию G3NavHMI.exe");
        }
        Journal("exe");   // from here on the verified copy restores the exe
        CopyChecked(g_exe, g_exeBak2);   // second copy on \SDMMC for manual recovery; not required
        HANDLE h = CreateFile(g_exe, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        DWORD w = 0;
        int ok = h != INVALID_HANDLE_VALUE && SetFilePointer(h, g_loopOff + 8, NULL, FILE_BEGIN) == g_loopOff + 8 &&
                 WriteFile(h, "2", 1, &w, NULL) && w == 1;
        if (h != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(h);
            CloseHandle(h);
        }
        DWORD at = 0;
        int nOld, nNew;
        DWORD off;
        ScanExe(&nOld, &nNew, &off);
        if (!ok || Diff(g_exeBak, g_exe, &at) != 1 || at != g_loopOff + 8 || nOld != 0 || nNew != 1)
            return Fail(L"патч G3NavHMI.exe не прошёл проверку");
        Say(L"  2. G3NavHMI.exe: 127.0.0.1 -> 127.0.0.2 (смещение 0x%X), проверено по копии", g_loopOff + 8);
    } else {
        Say(L"  2. G3NavHMI.exe уже пропатчен (127.0.0.2) — не трогаю");
    }

    // 3. ExternNavi.cfg
    DWORD size = 0, len;
    char* old = ReadAll(g_cfg, &size);
    if (old) {
        if (!CopyChecked(g_cfg, g_cfgBak)) {
            DeleteFile(g_cfgBak);
            free(old);
            return Fail(L"не удалось сделать копию ExternNavi.cfg");
        }
        Journal("cfg");
    } else {
        Journal("cfg-none");
    }
    char* text = NewCfg(old, &len);
    free(old);
    int cfgOk = text && WriteCfg(text, len);
    free(text);
    if (!cfgOk)
        return Fail(L"ExternNavi.cfg не записан");
    Say(L"  3. ExternNavi.cfg: VestaLaunch первым пунктом");

    // 4. the shell's button caption in language_cn.qm
    if (g_qmState == QM_FACTORY) {
        if (!CopyChecked(g_qm, g_qmBak)) {
            DeleteFile(g_qmBak);
            return Fail(L"не удалось сделать копию language_cn.qm");
        }
        Journal("qm");   // from here on the verified copy restores the caption
        if (!PatchQm())
            return Fail(L"надпись кнопки в language_cn.qm не изменена");
        Say(L"  4. language_cn.qm: надпись кнопки «Навигация» → «Программы»");
    } else if (g_qmState == QM_PATCHED) {
        Say(L"  4. language_cn.qm уже с надписью «Программы» — не трогаю");
    } else if (g_qmState == QM_MISSING) {
        Say(L"  4. language_cn.qm нет — надпись кнопки не правится");
    } else {
        Say(L"  4. language_cn.qm: надпись кнопки не распознана — не трогаю");
    }

    // 5. the shell's "Navigation" button tile
    if (g_iconState == ICON_FACTORY) {
        Payload vesta;
        if (!GetRes(IDR_ICONVESTA, &vesta))
            return Fail(L"в установщике нет значка кнопки");
        if (!CopyChecked(g_icon, g_iconBak)) {
            DeleteFile(g_iconBak);
            return Fail(L"не удалось сделать копию main_symbol_navigation.png");
        }
        Journal("icon");   // from here on the verified copy restores the tile
        if (!WriteBytes(g_icon, vesta.base, vesta.size) || !SameAs(g_icon, vesta.base, vesta.size))
            return Fail(L"значок кнопки не записан");
        Say(L"  5. main_symbol_navigation.png: значок кнопки заменён");
    } else if (g_iconState == ICON_VESTA) {
        Say(L"  5. main_symbol_navigation.png уже наш — не трогаю");
    } else if (g_iconState == ICON_MISSING) {
        Say(L"  5. main_symbol_navigation.png нет — значок кнопки не меняется");
    } else {
        Say(L"  5. main_symbol_navigation.png: не заводской — не трогаю");
    }

    // 6. the new launcher in place, an older one aside
    if (DirExists(g_launch)) {
        Journal("launch-old");
        RemoveTree(g_old);
        if (!MoveFile(g_launch, g_old))
            return Fail(L"прежняя папка VestaLaunch занята");
    }
    Journal("launch");
    wchar_t exe[MAX_PATH];
    Join(exe, g_launch, kLauncher);
    if (!MoveFile(g_new, g_launch) || !Exists(exe))
        return Fail(L"VestaLaunch не встал на место");
    Say(L"  6. VestaLaunch на месте: %s", g_launch);

    // 7. the patched shell must come up and stay up
    if (patch) {
        Say(L"  7. запуск оболочки, проверка %d с…", kWatchMs / 1000);
        StartShell();
        if (!Watch())
            return Fail(L"оболочка не работает после патча");
        Say(L"     оболочка работает");
    }

    RemoveTree(g_old);
    DeleteFile(g_cfgBak);   // the exe backup stays: "Uninstall" restores the factory exe from it
    DeleteFile(g_journal);
    Detect();
    Say(g_qmState == QM_PATCHED ? L"ГОТОВО. Кнопка «Программы» открывает VestaLaunch."
                                : L"ГОТОВО. Кнопка оболочки открывает VestaLaunch.");
    return 1;
}

static int DoUninstall()
{
    g_status[0] = 0;
    Detect();
    if (g_hasJournal) {
        Say(L"Найдена прерванная установка.");
        Rollback();
        Detect();
    }
    if (Exists(g_exeBak) && !AskCloseShell()) { Say(L"Отменено."); return 0; }
    Say(L"Удаление VestaLaunch:");
    ForProcess(kLauncher, 1);
    Sleep(300);
    int ok = 1;
    DWORD size, len;
    char* old = ReadAll(g_cfg, &size);
    if (old) {   // our line out, the rest as it is
        char* text = NewCfg(old, &len);
        int skip = (int)strlen(kCfgLine) + 2;   // NewCfg puts ours first: drop it
        if (text && WriteCfg(text + skip, len - skip)) {
            Say(L"  ExternNavi.cfg: строка VestaLaunch убрана");
        } else {
            Say(L"  НЕ удалось исправить ExternNavi.cfg");
            ok = 0;
        }
        free(text);
        free(old);
    }
    if (Exists(g_exeBak)) {   // we patched the exe: back to the factory one
        if (CloseShell() && CopyChecked(g_exeBak, g_exe)) {
            DeleteFile(g_exeBak);
            Say(L"  G3NavHMI.exe восстановлен из копии");
        } else {
            Say(L"  НЕ удалось восстановить G3NavHMI.exe (копия: %s)", g_exeBak);
            ok = 0;
        }
        StartShell();
        Say(L"  оболочка запущена");
    } else {
        Say(L"  G3NavHMI.exe не патчился этим установщиком — не трогаю");
    }
    if (Exists(g_qmBak)) {   // we changed the button caption: back to the factory one
        if (CopyChecked(g_qmBak, g_qm)) {
            DeleteFile(g_qmBak);
            Say(L"  language_cn.qm восстановлен из копии");
        } else {
            Say(L"  НЕ удалось восстановить language_cn.qm (копия: %s)", g_qmBak);
            ok = 0;
        }
    }
    if (Exists(g_iconBak)) {   // we changed the button tile: back to the factory one
        if (CopyChecked(g_iconBak, g_icon)) {
            DeleteFile(g_iconBak);
            Say(L"  main_symbol_navigation.png восстановлен из копии");
        } else {
            Say(L"  НЕ удалось восстановить main_symbol_navigation.png (копия: %s)", g_iconBak);
            ok = 0;
        }
    }
    RemoveTree(g_launch);
    RemoveTree(g_old);
    RemoveTree(g_new);
    if (DirExists(g_launch)) {
        Say(L"  НЕ удалось удалить %s", g_launch);
        ok = 0;
    } else {
        Say(L"  папка VestaLaunch удалена");
    }
    Say(ok ? L"ГОТОВО." : L"Удаление выполнено не полностью — см. строки выше.");
    Detect();
    return ok;
}

static void DoStatus()
{
    g_status[0] = 0;
    Detect();
    static const wchar_t* exe[4] = { L"нет", L"не пропатчен", L"пропатчен (127.0.0.2)", L"незнакомая прошивка" };
    static const wchar_t* qm[4] = { L"нет", L"надпись «Навигация»", L"надпись «Программы»", L"не распознан" };
    static const wchar_t* icon[4] = { L"нет", L"заводской", L"наш", L"другой" };
    Say(L"G3NavHMI.exe: %s; ExternNavi.cfg: %s; VestaLaunch: %s; language_cn.qm: %s; значок кнопки: %s%s%s",
        exe[g_exeState], g_cfgOurs ? L"VestaLaunch первым" : L"без VestaLaunch",
        g_hasLaunch ? L"установлен" : L"нет", qm[g_qmState], icon[g_iconState],
        g_hasJournal ? L"; прерванная установка!" : L"", g_bootFlag ? L"; заряжено загрузочное обновление!" : L"");
}

// ---------------------------------------------------------------- UI

#define C_INK    RGB(0x0b, 0x0c, 0x0e)
#define C_PANEL  RGB(0x17, 0x19, 0x1c)
#define C_LINE   RGB(0x34, 0x38, 0x3f)
#define C_TEXT   RGB(0xf3, 0xf1, 0xec)
#define C_MUTED  RGB(0xa3, 0xa8, 0xb0)
#define C_ORANGE RGB(0xff, 0x6b, 0x1a)
#define C_FACET  RGB(0xc2, 0x4a, 0x0b)

struct Btn { RECT rc; const wchar_t* label; int id; };
enum { B_INSTALL, B_UNINSTALL, B_EXIT };
static const Btn g_btns[3] = {
    { {  16, 410, 256, 468 }, L"УСТАНОВИТЬ", B_INSTALL },
    { { 280, 410, 520, 468 }, L"УДАЛИТЬ",    B_UNINSTALL },
    { { 544, 410, 784, 468 }, L"ВЫХОД",      B_EXIT },
};
static HFONT g_font, g_fontS, g_fontB;
static int   g_press = -1, g_busy;

static HFONT MakeFont(int height, int weight)
{
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    // no smoothing: on the unit ANTIALIASED_QUALITY comes out as ClearType - coloured fringes in the 16-bit
    // frame, small text blurred; Tahoma is hinted for crisp unsmoothed pixels
    lf.lfQuality = NONANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, L"Tahoma");
    return CreateFontIndirect(&lf);
}

static void Fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void Text(HDC dc, int l, int t, int r, int b, const wchar_t* s, COLORREF c, HFONT f, UINT fmt)
{
    RECT rc = { l, t, r, b };
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawText(dc, s, -1, &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
}

static void Paint(HDC hdc)
{
    HDC dc = CreateCompatibleDC(hdc);   // off-screen: no flicker while the steps report
    HBITMAP bmp = CreateCompatibleBitmap(hdc, 800, 480);
    HGDIOBJ ob = SelectObject(dc, bmp);
    Fill(dc, 0, 0, 800, 480, C_INK);
    Fill(dc, 0, 54, 800, 56, C_LINE);
    Text(dc, 20, 0, 780, 54, L"УСТАНОВКА VESTALAUNCH", C_TEXT, g_fontB, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    static const wchar_t* exe[4] = { L"не найден", L"не пропатчен", L"пропатчен (127.0.0.2)", L"НЕЗНАКОМАЯ ПРОШИВКА" };
    wchar_t s[600];
    static const wchar_t* qm[4] = { L"нет", L"«Навигация»", L"«Программы»", L"не распознана" };
    static const wchar_t* icon[4] = { L"нет", L"заводской", L"наш", L"другой" };
    _snwprintf(s, 600,
               L"Оболочка: %s\\G3NavHMI.exe — %s\n"
               L"ExternNavi.cfg: %s     VestaLaunch: %s\n"
               L"Кнопка «Навигация»: %s, значок: %s\n"
               L"%s%s",
               g_appDir, exe[g_exeState], g_cfgOurs ? L"VestaLaunch первым" : L"без VestaLaunch",
               g_hasLaunch ? L"установлен" : L"не установлен", qm[g_qmState], icon[g_iconState],
               g_hasJournal ? L"Прерванная установка — будет откачена.  " : L"",
               g_bootFlag ? L"Заряжено загрузочное обновление — установка невозможна." : L"");
    Text(dc, 20, 66, 780, 150, s, C_MUTED, g_fontS, DT_LEFT | DT_WORDBREAK);
    Fill(dc, 16, 152, 784, 398, C_PANEL);
    Text(dc, 30, 160, 770, 392, g_status[0] ? g_status : L"Ставит VestaLaunch на \\SDMMC, патчит G3NavHMI.exe и ExternNavi.cfg. "
         L"Каждый шаг проверяется; при ошибке или если оболочка не поднимется — всё возвращается как было.",
         C_TEXT, g_fontS, DT_LEFT | DT_WORDBREAK);
    for (int i = 0; i < 3; i++) {
        const RECT& r = g_btns[i].rc;
        int orange = g_press == i || i == B_INSTALL;   // "Install" is the screen's one main action
        Fill(dc, r.left, r.top, r.right, r.bottom, g_press == i && i == B_INSTALL ? C_FACET : orange ? C_ORANGE : C_INK);
        if (!orange) {   // outlined
            Fill(dc, r.left, r.top, r.right, r.top + 2, C_LINE);
            Fill(dc, r.left, r.bottom - 2, r.right, r.bottom, C_LINE);
            Fill(dc, r.left, r.top, r.left + 2, r.bottom, C_LINE);
            Fill(dc, r.right - 2, r.top, r.right, r.bottom, C_LINE);
        }
        Text(dc, r.left, r.top, r.right, r.bottom, g_btns[i].label, orange ? C_INK : C_TEXT, g_font,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    BitBlt(hdc, 0, 0, 800, 480, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static int HitTest(int x, int y)
{
    for (int i = 0; i < 3; i++) {
        const RECT& r = g_btns[i].rc;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return i;
    }
    return -1;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_font = MakeFont(20, FW_BOLD);
        g_fontS = MakeFont(17, FW_NORMAL);
        g_fontB = MakeFont(22, FW_BOLD);
        Detect();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (!g_busy) {
            g_press = HitTest((short)LOWORD(lp), (short)HIWORD(lp));
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        int i = HitTest((short)LOWORD(lp), (short)HIWORD(lp)), was = g_press;
        g_press = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        if (g_busy || i < 0 || i != was)
            return 0;
        if (g_btns[i].id == B_EXIT) {
            DestroyWindow(hwnd);
            return 0;
        }
        g_busy = 1;   // one action at a time
        if (g_btns[i].id == B_INSTALL)
            DoInstall();
        else
            DoUninstall();
        g_busy = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !g_busy)
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- entry
// Args (for scripts and tests): ["<appdir>"] [status|install|uninstall]; without a verb: the window.

#ifdef UNDER_CE
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int)
#else
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int)
#endif
{
    int verb = 0;
    wcscpy(g_appDir, L"\\ResidentFlash\\navigation\\app");
    for (wchar_t* p = cmd; *p; ) {
        while (*p == L' ')
            p++;
        if (!*p)
            break;
        wchar_t tok[MAX_PATH];
        int n = 0, q = *p == L'"';
        if (q)
            p++;
        while (*p && (q ? *p != L'"' : *p != L' '))
            if (n < MAX_PATH - 1)
                tok[n++] = *p++;
            else
                p++;
        if (q && *p)
            p++;
        tok[n] = 0;
        if (!_wcsicmp(tok, L"install"))
            verb = 1;
        else if (!_wcsicmp(tok, L"uninstall"))
            verb = 2;
        else if (!_wcsicmp(tok, L"status"))
            verb = 3;
        else if (tok[0])
            wcscpy(g_appDir, tok);
    }
    LogMsg(L"---- LaunchInstall");
    if (verb) {
        int ok = verb == 1 ? DoInstall() : verb == 2 ? DoUninstall() : (DoStatus(), 1);
        return ok ? 0 : 1;
    }

    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_CLASS;
#ifndef UNDER_CE
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
#endif
    RegisterClass(&wc);
    g_ui = 1;
#ifdef UNDER_CE
    HWND hwnd = CreateWindowEx(WS_EX_TOPMOST, APP_CLASS, L"VestaLaunch", WS_POPUP | WS_VISIBLE, 0, 0, 800, 480,
                               NULL, NULL, inst, NULL);
#else
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    RECT rc = { 0, 0, 800, 480 };
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindow(APP_CLASS, L"LaunchInstall", style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                             rc.bottom - rc.top, NULL, NULL, inst, NULL);
#endif
    if (!hwnd)
        return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
