// probe.inc - LaunchInstall's "Диагностика" (the user 2026-11-17: a firmware with a modified CAN encrypts \ResidentFlash
// and the system partition; people want VestaHome / VestaLaunch there - first learn how it does it). Reads only: the
// versions, the registry's storage / driver / startup keys, the stores and partitions, the volumes, the processes and
// their modules, \Windows' files not in ROM (a filter DLL the firmware brings), how the partitions' files look to us,
// a trial file in each partition (written, read back, deleted) and whether the system's loader reads a DLL we can't.
// Into LaunchInstall_probe.log next to this exe. Part of launch_install.cpp (one translation unit).

static HANDLE s_probe = INVALID_HANDLE_VALUE;
static int    s_probeLines;

static void PLine(const wchar_t* fmt, ...)
{
    if (s_probe == INVALID_HANDLE_VALUE)
        return;
    wchar_t line[600];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(line, 600, fmt, a);
    va_end(a);
    line[599] = 0;
    char buf[1900];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, buf, sizeof(buf) - 2, NULL, NULL);
    DWORD w;
    if (n > 0) {
        WriteFile(s_probe, buf, (DWORD)(n - 1), &w, NULL);
        WriteFile(s_probe, "\r\n", 2, &w, NULL);
    }
    s_probeLines++;
}

// ---------------------------------------------------------------- the versions
static void ProbeVersions()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    PLine(L"== LaunchInstall: диагностика %04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
          t.wSecond);
    OSVERSIONINFO v;
    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    if (GetVersionEx(&v))
        PLine(L"ОС: %lu.%lu сборка %lu, платформа %lu, %s", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
              v.dwPlatformId, v.szCSDVersion);
#ifdef UNDER_CE
    wchar_t s[128];
    if (SystemParametersInfo(SPI_GETPLATFORMTYPE, sizeof(s), s, 0))
        PLine(L"тип платформы: %s", s);
    if (SystemParametersInfo(SPI_GETOEMINFO, sizeof(s), s, 0))
        PLine(L"OEM: %s", s);
#endif
    MEMORYSTATUS m;
    m.dwLength = sizeof(m);
    GlobalMemoryStatus(&m);
    PLine(L"память: свободно %lu из %lu КБ", (DWORD)(m.dwAvailPhys / 1024), (DWORD)(m.dwTotalPhys / 1024));
}

// ---------------------------------------------------------------- the registry
static void ProbeValue(const wchar_t* name, DWORD type, const BYTE* data, DWORD size, int depth)
{
    wchar_t pad[24];
    int k = depth * 2 < 22 ? depth * 2 : 22;
    for (int i = 0; i < k; i++)
        pad[i] = L' ';
    pad[k] = 0;
    const wchar_t* nm = name[0] ? name : L"(по умолчанию)";
    if ((type == REG_SZ || type == REG_EXPAND_SZ) && size >= 2) {
        PLine(L"%s  %s = \"%.*s\"", pad, nm, (int)(size / 2), (const wchar_t*)data);
    } else if (type == REG_MULTI_SZ) {
        wchar_t joined[300];
        int j = 0;
        for (DWORD i = 0; i + 1 < size / 2 && j < 298; i++) {
            wchar_t c = ((const wchar_t*)data)[i];
            joined[j++] = c ? c : L'|';
        }
        joined[j] = 0;
        PLine(L"%s  %s = [%s]", pad, nm, joined);
    } else if (type == REG_DWORD && size == 4) {
        DWORD d;
        memcpy(&d, data, 4);
        PLine(L"%s  %s = 0x%08X (%lu)", pad, nm, d, d);
    } else {
        wchar_t hex[64];
        HexHead(data, size, hex, 64);
        PLine(L"%s  %s = тип %lu, %lu Б: %s", pad, nm, type, size, hex);
    }
}

static void ProbeKey(HKEY root, const wchar_t* path, int depth, int maxDepth)
{
    if (s_probeLines > 6000)
        return;
    HKEY h;
    if (RegOpenKeyEx(root, path, 0, KEY_READ, &h) != ERROR_SUCCESS) {
        if (!depth)
            PLine(L"[%s] нет", path);
        return;
    }
    PLine(L"%*s[%s]", depth * 2, L"", path);
    BYTE data[1024];
    for (DWORD i = 0; i < 200; i++) {
        wchar_t name[128];
        DWORD nl = 128, type = 0, size = sizeof(data);
        if (RegEnumValue(h, i, name, &nl, NULL, &type, data, &size) != ERROR_SUCCESS)
            break;
        ProbeValue(name, type, data, size, depth);
    }
    for (DWORD i = 0; i < 200 && s_probeLines <= 6000; i++) {
        wchar_t sub[128];
        DWORD sl = 128;
        if (RegEnumKeyEx(h, i, sub, &sl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        wchar_t full[MAX_PATH];
        _snwprintf(full, MAX_PATH, L"%s\\%s", path, sub);
        full[MAX_PATH - 1] = 0;
        if (depth < maxDepth)
            ProbeKey(root, full, depth + 1, maxDepth);
        else
            PLine(L"%*s[%s] ...", depth * 2 + 2, L"", full);
    }
    RegCloseKey(h);
}

static void ProbeRegistry()
{
    PLine(L"== реестр");
    ProbeKey(HKEY_LOCAL_MACHINE, L"Ident", 0, 1);
    ProbeKey(HKEY_LOCAL_MACHINE, L"System\\StorageManager", 0, 5);   // the file systems and their filters
    ProbeKey(HKEY_LOCAL_MACHINE, L"System\\Filesys", 0, 3);
    ProbeKey(HKEY_LOCAL_MACHINE, L"Init", 0, 1);                      // what starts at boot
    ProbeKey(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 1);           // the drivers loaded now
    ProbeKey(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn", 0, 2);
    ProbeKey(HKEY_LOCAL_MACHINE, L"Software", 0, 0);                  // the vendors
}

// ---------------------------------------------------------------- the stores, partitions and volumes
#ifdef UNDER_CE
#include <storemgr.h>
typedef HANDLE (WINAPI* FirstStoreFn)(PSTOREINFO);
typedef BOOL   (WINAPI* NextStoreFn)(HANDLE, PSTOREINFO);
typedef BOOL   (WINAPI* CloseFindFn)(HANDLE);
typedef HANDLE (WINAPI* OpenStoreFn)(LPCTSTR);
typedef HANDLE (WINAPI* FirstPartFn)(HANDLE, PPARTINFO);
typedef BOOL   (WINAPI* NextPartFn)(HANDLE, PPARTINFO);
struct VolInfo { DWORD cbSize, dwAttributes, dwFlags, dwBlockSize; wchar_t szStoreName[32], szPartitionName[32]; };
typedef BOOL   (WINAPI* VolInfoFn)(LPCWSTR, int, VolInfo*);

static void ProbeStores()
{
    PLine(L"== хранилища и разделы");
    HMODULE core = GetModuleHandle(L"coredll.dll");
    FirstStoreFn first = (FirstStoreFn)GetProcAddress(core, L"FindFirstStore");
    NextStoreFn next = (NextStoreFn)GetProcAddress(core, L"FindNextStore");
    CloseFindFn closeStore = (CloseFindFn)GetProcAddress(core, L"FindCloseStore");
    OpenStoreFn open = (OpenStoreFn)GetProcAddress(core, L"OpenStore");
    FirstPartFn firstPart = (FirstPartFn)GetProcAddress(core, L"FindFirstPartition");
    NextPartFn nextPart = (NextPartFn)GetProcAddress(core, L"FindNextPartition");
    CloseFindFn closePart = (CloseFindFn)GetProcAddress(core, L"FindClosePartition");
    if (!first || !next || !closeStore || !open || !firstPart || !nextPart || !closePart) {
        PLine(L"функций менеджера хранилищ нет в coredll");
        return;
    }
    STOREINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    HANDLE f = first(&si);
    for (BOOL ok = f != INVALID_HANDLE_VALUE; ok; ok = next(f, &si)) {
        PLine(L"хранилище %s «%s»: профиль %s, класс 0x%X, тип 0x%X, флаги 0x%X, %lu разделов, сектор %lu Б, "
              L"атрибуты 0x%X", si.szDeviceName, si.szStoreName, si.sdi.szProfile, si.dwDeviceClass, si.dwDeviceType,
              si.dwDeviceFlags, si.dwPartitionCount, si.dwBytesPerSector, si.dwAttributes);
        HANDLE st = open(si.szDeviceName);
        if (st != INVALID_HANDLE_VALUE) {
            PARTINFO pi;
            memset(&pi, 0, sizeof(pi));
            pi.cbSize = sizeof(pi);
            HANDLE pf = firstPart(st, &pi);
            for (BOOL pok = pf != INVALID_HANDLE_VALUE; pok; pok = nextPart(pf, &pi))
                PLine(L"  раздел «%s»: файловая система %s, том %s, тип 0x%02X, атрибуты 0x%X, %lu секторов",
                      pi.szPartitionName, pi.szFileSys, pi.szVolumeName, pi.bPartType, pi.dwAttributes,
                      (DWORD)pi.snNumSectors);
            if (pf != INVALID_HANDLE_VALUE)
                closePart(pf);
            CloseHandle(st);
        }
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
    }
    if (f != INVALID_HANDLE_VALUE)
        closeStore(f);
}
#endif

static void ProbeVolumes()
{
    PLine(L"== папки корня (тома)");
#ifdef UNDER_CE
    VolInfoFn volInfo = (VolInfoFn)GetProcAddress(GetModuleHandle(L"coredll.dll"), L"CeGetVolumeInfoW");
#endif
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(L"\\*", &fd);
    for (BOOL ok = f != INVALID_HANDLE_VALUE; ok; ok = FindNextFile(f, &fd)) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"\\%s", fd.cFileName);
        path[MAX_PATH - 1] = 0;
        ULARGE_INTEGER freeB, total, totalFree;
        DWORD freeMb = 0, totalMb = 0;
        if (GetDiskFreeSpaceEx(path, &freeB, &total, &totalFree)) {
            freeMb = (DWORD)(freeB.QuadPart >> 20);
            totalMb = (DWORD)(total.QuadPart >> 20);
        }
        wchar_t vol[160] = L"";
#ifdef UNDER_CE
        VolInfo vi;
        memset(&vi, 0, sizeof(vi));
        vi.cbSize = sizeof(vi);
        if (volInfo && volInfo(path, 0, &vi))
            _snwprintf(vol, 160, L", том: атрибуты 0x%X, флаги 0x%X, блок %lu, хранилище «%s», раздел «%s»",
                       vi.dwAttributes, vi.dwFlags, vi.dwBlockSize, vi.szStoreName, vi.szPartitionName);
        vol[159] = 0;
#endif
        PLine(L"%s: атрибуты 0x%X, %lu МБ из %lu свободно%s", path, fd.dwFileAttributes, freeMb, totalMb, vol);
    }
    if (f != INVALID_HANDLE_VALUE)
        FindClose(f);
}

// ---------------------------------------------------------------- the processes and their modules
static void ProbeProcesses()
{
    PLine(L"== процессы и модули");
#ifdef UNDER_CE
    HMODULE lib = LoadLibrary(L"toolhelp.dll");
    if (!lib) {
        PLine(L"нет toolhelp.dll");
        return;
    }
    SnapshotFn snapshot = (SnapshotFn)GetProcAddress(lib, L"CreateToolhelp32Snapshot");
    ProcessFn first = (ProcessFn)GetProcAddress(lib, L"Process32First");
    ProcessFn next = (ProcessFn)GetProcAddress(lib, L"Process32Next");
    CloseFn close = (CloseFn)GetProcAddress(lib, L"CloseToolhelp32Snapshot");
    typedef BOOL (WINAPI* ModuleFn)(HANDLE, MODULEENTRY32*);
    ModuleFn mfirst = (ModuleFn)GetProcAddress(lib, L"Module32First");
    ModuleFn mnext = (ModuleFn)GetProcAddress(lib, L"Module32Next");
#else
    HMODULE lib = LoadLibrary(L"kernel32.dll");
    if (!lib)
        return;
    SnapshotFn snapshot = (SnapshotFn)GetProcAddress(lib, "CreateToolhelp32Snapshot");
    ProcessFn first = (ProcessFn)GetProcAddress(lib, "Process32FirstW");
    ProcessFn next = (ProcessFn)GetProcAddress(lib, "Process32NextW");
    CloseFn close = (CloseFn)GetProcAddress(lib, "CloseHandle");
    typedef BOOL (WINAPI* ModuleFn)(HANDLE, MODULEENTRY32*);
    ModuleFn mfirst = (ModuleFn)GetProcAddress(lib, "Module32FirstW");
    ModuleFn mnext = (ModuleFn)GetProcAddress(lib, "Module32NextW");
#endif
    if (snapshot && first && next && close) {
        HANDLE snap = snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32 pe;
            memset(&pe, 0, sizeof(pe));
            pe.dwSize = sizeof(pe);
            int n = 0;
            for (BOOL ok = first(snap, &pe); ok && n < 80; ok = next(snap, &pe), n++) {
                PLine(L"%s: pid 0x%X, потоков %lu", pe.szExeFile, pe.th32ProcessID, pe.cntThreads);
                if (!mfirst || !mnext)
                    continue;
                HANDLE ms = snapshot(TH32CS_SNAPMODULE, pe.th32ProcessID);
                if (ms == INVALID_HANDLE_VALUE)
                    continue;
                MODULEENTRY32 me;
                memset(&me, 0, sizeof(me));
                me.dwSize = sizeof(me);
                int k = 0;
                for (BOOL mok = mfirst(ms, &me); mok && k < 120; mok = mnext(ms, &me), k++)
                    PLine(L"    %s  %lu Б  %s", me.szModule, me.modBaseSize, me.szExePath);
                close(ms);
            }
            close(snap);
        }
    }
    FreeLibrary(lib);
}

// ---------------------------------------------------------------- the files as we see them
struct FileTally { int files, prog, unreadable, text, noise, other; };

static void ProbeFile(const wchar_t* path, const WIN32_FIND_DATA& fd, FileTally* t, int list)
{
    unsigned char head[4096];
    DWORD size = 0;
    int got = ReadHead(path, head, sizeof(head), &size);
    const wchar_t* looks = got < 0 ? L"не открывается" : Looks(fd.cFileName, head, (DWORD)got);
    t->files++;
    if (!wcscmp(looks, L"программа"))
        t->prog++;
    else if (!wcscmp(looks, L"ПРОГРАММА НЕ ЧИТАЕТСЯ") || got < 0)
        t->unreadable++;
    else if (!wcscmp(looks, L"текст"))
        t->text++;
    else if (!wcscmp(looks, L"шум (шифр или сжатие)"))
        t->noise++;
    else
        t->other++;
    if (!list)
        return;
    wchar_t hex[64];
    HexHead(got > 0 ? head : NULL, got > 0 ? (DWORD)got : 0, hex, 25);
    PLine(L"  %s  %lu Б, атрибуты 0x%X, энтропия %d.%02d, %s  [%s]", path, fd.nFileSizeLow, fd.dwFileAttributes,
          EntropyX100(head, got > 0 ? (DWORD)got : 0) / 100, EntropyX100(head, got > 0 ? (DWORD)got : 0) % 100, looks,
          hex);
}

// The tree's files (at most budget listed), the tally; romOnly 0: in \Windows only the files not in ROM are listed.
static void ProbeTree(const wchar_t* dir, int depth, int* budget, FileTally* t, int notInRom)
{
    wchar_t mask[MAX_PATH];
    _snwprintf(mask, MAX_PATH, L"%s\\*", dir);
    mask[MAX_PATH - 1] = 0;
    WIN32_FIND_DATA fd;
    HANDLE f = FindFirstFile(mask, &fd);
    for (BOOL ok = f != INVALID_HANDLE_VALUE; ok; ok = FindNextFile(f, &fd)) {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        path[MAX_PATH - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0)
                ProbeTree(path, depth - 1, budget, t, notInRom);
            continue;
        }
#ifdef FILE_ATTRIBUTE_INROM
        if (notInRom && (fd.dwFileAttributes & FILE_ATTRIBUTE_INROM))
            continue;
#endif
        ProbeFile(path, fd, t, *budget > 0);
        if (*budget > 0)
            (*budget)--;
    }
    if (f != INVALID_HANDLE_VALUE)
        FindClose(f);
}

static void ProbeFiles()
{
    PLine(L"== файлы, как их читает установщик");
    struct Where { const wchar_t* dir; int depth, budget, notInRom; } where[] = {
        { L"\\Windows", 0, 150, 1 },   // only what the firmware added at boot (a filter DLL would be here)
        { g_appDir, 2, 200, 0 },
        { L"\\ResidentFlash", 1, 120, 0 },
        { L"\\StorageFlash", 2, 80, 0 },
        { L"\\SDMMC", 1, 60, 0 },
    };
    for (int i = 0; i < (int)(sizeof(where) / sizeof(where[0])); i++) {
        FileTally t;
        memset(&t, 0, sizeof(t));
        int budget = where[i].budget;
        PLine(L"%s%s:", where[i].dir, where[i].notInRom ? L" (только не из ПЗУ)" : L"");
        ProbeTree(where[i].dir, where[i].depth, &budget, &t, where[i].notInRom);
        PLine(L"  итого %d файлов: программ %d, программ не читается %d, текст %d, шум %d, прочее %d", t.files, t.prog,
              t.unreadable, t.text, t.noise, t.other);
    }
}

// ---------------------------------------------------------------- writing, and the system's loader
static void ProbeWrites()
{
    PLine(L"== пробная запись (0 — ок, 1 — не создан, 2 — прочитано другое, 3 — не удалён, -1 — нет папки)");
    const wchar_t* dirs[] = { g_appDir, L"\\ResidentFlash", L"\\StorageFlash", L"\\SDMMC", L"\\Windows", L"\\Temp" };
    for (int i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++)
        PLine(L"%s: %d", dirs[i], DirExists(dirs[i]) ? ProbeWrite(dirs[i]) : -1);
}

// A DLL of the shell's folder that we read without its header, loaded by the system as data (no code run): if the
// loader gets a program's header and resources, the partition is decrypted for the system, not for us.
static void ProbeLoader()
{
    PLine(L"== загрузчик системы");
    wchar_t mask[MAX_PATH], pick[MAX_PATH] = L"", plain[MAX_PATH] = L"";
    _snwprintf(mask, MAX_PATH, L"%s\\*.dll", g_appDir);
    mask[MAX_PATH - 1] = 0;
    WIN32_FIND_DATA fd;
    DWORD best = 0xFFFFFFFF;
    HANDLE f = FindFirstFile(mask, &fd);
    for (BOOL ok = f != INVALID_HANDLE_VALUE; ok; ok = FindNextFile(f, &fd)) {
        wchar_t path[MAX_PATH];
        Join(path, g_appDir, fd.cFileName);
        unsigned char head[1024];
        DWORD size;
        int got = ReadHead(path, head, sizeof(head), &size);
        int pe = got > 0 && PeImage(head, (DWORD)got);
        if (!pe && size < best) {   // the smallest one we can't read
            best = size;
            wcscpy(pick, path);
        } else if (pe && !plain[0]) {
            wcscpy(plain, path);
        }
    }
    if (f != INVALID_HANDLE_VALUE)
        FindClose(f);
    const wchar_t* tries[2] = { pick, plain };
    for (int i = 0; i < 2; i++) {
        if (!tries[i][0]) {
            PLine(i ? L"читаемой DLL в папке оболочки нет" : L"нечитаемой DLL в папке оболочки нет");
            continue;
        }
        HMODULE h = LoadLibraryEx(tries[i], NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (!h) {
            PLine(L"%s: как данные не загружена, ошибка %lu", tries[i], GetLastError());
            continue;
        }
        const unsigned char* base = (const unsigned char*)((DWORD)h & ~3u);
        wchar_t hex[64] = L"-";
        int mz = 0;
        if (!IsBadReadPtr(base, 16)) {
            HexHead(base, 16, hex, 64);
            mz = base[0] == 'M' && base[1] == 'Z';
        }
        HRSRC ver = FindResource(h, MAKEINTRESOURCE(1), RT_VERSION);
        PLine(L"%s (%s): загружена как данные, начало в памяти [%s]%s, ресурс версии: %s", tries[i],
              i ? L"читается нами" : L"нами не читается", hex, mz ? L" — MZ" : L"",
              ver ? L"есть" : L"нет");
        FreeLibrary(h);
    }
}

// The whole probe; the window gets the file's name.
static void DoProbe()
{
    g_status[0] = 0;
    Detect();
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    ExeDir(dir);
    Join(path, dir, L"LaunchInstall_probe.log");
    s_probe = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (s_probe == INVALID_HANDLE_VALUE) {
        Say(L"ОШИБКА: не создать %s", path);
        return;
    }
    s_probeLines = 0;
    Say(L"Диагностика: собираю сведения о прошивке...");
    ProbeVersions();
    LogFindings();
    ProbeRegistry();
#ifdef UNDER_CE
    ProbeStores();
#endif
    ProbeVolumes();
    ProbeProcesses();
    ProbeFiles();
    ProbeWrites();
    ProbeLoader();
    PLine(L"== конец, %d строк", s_probeLines);
    CloseHandle(s_probe);
    s_probe = INVALID_HANDLE_VALUE;
    Say(L"Готово: %s (%d строк). Ничего в системе не изменено. Пришлите этот файл.", path, s_probeLines);
}
