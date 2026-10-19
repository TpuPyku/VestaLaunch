// VestaLaunch - app launcher for the Lada Vesta MMC (Windows CE 6.0, 800x480); replaces AppLauncher on the "Navigation" button.
// Started by the stock shell G3NavHMI from Config\ExternNavi.cfg (the "Navigation" button), like AppLauncher,
// or by VestaHome's "Apps" tile; "Back" hides it and brings back the window it was opened over (any shell).
// The "Apps" screen lists programs from apps.txt next to the exe; shortcuts are added, changed and removed
// right on that screen (own on-screen keyboard and file browser: the unit has neither).
// Style and status bar are VestaHome's (mockup skin/vesta-skin-v0.1-screens, "Программы").
// Colours and pictures come from Skin\skin.ini; whatever it doesn't give is drawn by the built-in code.
// Also builds as a desktop Win32 app for testing on a PC; "VestaLaunchPC.exe /export" writes the built-in
// graphics to Skin\ as PNG files plus a default skin.ini.
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"
#include "vtask.h"   // VestaCore: the task manager's processes, windows and load

#define APP_CLASS L"VestaLaunch_Wnd"

// ---------------------------------------------------------------- palette (concept v0.1 tokens; skin.ini may override)

static COLORREF kInk      = RGB(0x0b, 0x0c, 0x0e);   // background
static COLORREF kPanel    = RGB(0x17, 0x19, 0x1c);   // tiles, widgets
static const COLORREF kPanel2   = RGB(0x22, 0x25, 0x2a);   // pressed / nested
static COLORREF kLine     = RGB(0x34, 0x38, 0x3f);   // 2 px borders
static COLORREF kText     = RGB(0xf3, 0xf1, 0xec);
static COLORREF kMuted    = RGB(0xa3, 0xa8, 0xb0);
static COLORREF kDim      = RGB(0x5d, 0x63, 0x6c);
static COLORREF kOrange   = RGB(0xff, 0x6b, 0x1a);   // the one accent: action / active
static COLORREF kFacet    = RGB(0xc2, 0x4a, 0x0b);
static COLORREF kTri[3]   = { RGB(0x0e, 0x0f, 0x12), RGB(0x14, 0x16, 0x1a), RGB(0x17, 0x19, 0x1d) };
static const COLORREF kTriOr[4] = { RGB(0x8f, 0x36, 0x08), RGB(0xc2, 0x4a, 0x0b), RGB(0xa8, 0x3f, 0x09), RGB(0xe6, 0x5a, 0x12) };

// ---------------------------------------------------------------- layout

enum Icon { IC_NONE, IC_BACK, IC_NEXT, IC_EDIT, IC_ADD, IC_FOLDER, IC_FILE, IC_UP, IC_SHIFT, IC_BKSP, IC_TASKS,
            IC_COUNT };

// Picture names in skin.ini (and Skin\<name>.png), by icon; NULL = no picture, always the built-in drawing.
static const wchar_t* kIconKey[IC_COUNT] = { NULL, L"prev", L"next", L"edit", L"add", L"folder", L"file",
    L"up", L"shift", L"backspace" };

enum Action { A_NONE, A_BACK, A_RUN, A_ADD, A_PREV, A_NEXT, A_EDITMODE, A_FIELD, A_SAVE, A_DELETE, A_CANCEL,
              A_OK, A_KEY, A_SHIFT, A_LANG, A_BKSP, A_FILE, A_UP, A_NOICON, A_TASKS, A_TASKBACK, A_TASKTAB,
              A_TASKSORT, A_TASKPAGE, A_TASKKILL, A_TASKSWITCH, A_TASKCLOSE, A_KEYFIELD, A_CLIPCOPY,
              A_CLIPPASTE, A_CLIPCLEAR };

// S_EDIT: one shortcut; S_KEYS: on-screen keyboard for its name/parameters; S_FILES: picking its exe;
// S_TASKS: the task manager (processes, windows, CPU and memory).
enum Screen { S_APPS, S_EDIT, S_KEYS, S_FILES, S_TASKS };

struct Btn {
    RECT        rc;
    Action      action;
    int         arg;       // A_RUN: index in s_apps; A_FIELD: F_*; A_KEY: character; A_FILE: index in s_files;
                           // A_TASKKILL: process id; A_TASKSWITCH, A_TASKCLOSE: the window (HWND)
};

// VestaHome answers "is it night?" with 1 at night. WM_VESTA_SHOW: a second start (by the shell or VestaHome)
// asks the running copy to come to the front.
#define HOME_CLASS     L"VestaHome_Wnd"
#define WM_VESTA_NIGHT (WM_APP + 2)
#define WM_VESTA_SHOW  (WM_APP + 3)
#define WM_EXIT_NAVI   (WM_APP + 4)   // the shell pulsed "Exit_Navi"
enum { F_NAME, F_PATH, F_PARAMS, F_ICON };   // fields of the shortcut editor

static const wchar_t* kKeysRu[4] = { L"1234567890-_", L"йцукенгшщзхъ", L"фывапролджэё", L"ячсмитьбю.,!" };
static const wchar_t* kKeysEn[4] = { L"1234567890-_", L"qwertyuiop()", L"asdfghjkl:;\"", L"zxcvbnm.,/\\=" };

// Skin pictures, premultiplied RGBA; px == NULL = the built-in drawing is used.
struct Img { int w, h; unsigned char* px; };

// Programs of the "Apps" screen, from apps.txt: "Name | \path\to\program.exe | class!title | parameters | icon".
struct App {
    wchar_t name[48];
    wchar_t path[MAX_PATH];
    wchar_t wclass[64];    // optional window to raise, "class!title" like AppLauncher; empty = any window of the process
    wchar_t wtitle[96];
    wchar_t params[200];   // command line passed to the program, may be empty
    wchar_t iconPath[MAX_PATH];   // own icon: .png, .ico or an exe/dll to take it from; relative = from our folder
    HICON   icon;          // the icon from an exe/dll/ico (the program's own if no iconPath), NULL if none
    Img     img;           // the icon from a .png, at most 64 x 64
    bool    exists;
};

// The stock shell stays topmost, so a started program must be raised above it explicitly.
// After the start its window is looked for every 300 ms for up to kWaitMs.
enum { TIMER_CLOCK = 1, TIMER_WAIT = 2, kWaitMs = 20000, kMaxRaised = 16 };
static DWORD s_waitPid;
static int   s_waitApp = -1;
static DWORD s_waitTick;
static HWND  s_raised[kMaxRaised];   // windows we made topmost; dropped again when the shell is shown
static int   s_raisedCount;

// Programs started from here: their processes, so that a second tap brings the running copy to the front
// instead of starting another one.
enum { kMaxRunning = 16 };
struct Running { wchar_t path[MAX_PATH]; HANDLE proc; DWORD pid; };
static Running s_running[kMaxRunning];

// The guard: while a raised program is in front, every window of its process is kept topmost. A dialog it
// opens is a normal window and would land under VestaHome (topmost), and the program with it.
// A program can be left without a visible window while its process stays: its own "minimize" (CE has no
// minimized state - the window is hidden; CityGuide, 2026-10-19) or a hang / crash. Our black backdrop and the
// shell's black navigation screen then stay. After kBlackMs without a visible window of the process we step
// aside to the shell as on "Back" (the program keeps running).
enum { TIMER_GUARD = 3, kGuardMs = 500, kBlackMs = 2000 };
static HANDLE s_guard;        // process of the program in front; NULL = no guard
static DWORD  s_guardPid;
static DWORD  s_guardSeen;    // GetTickCount() when a visible window of the process was last seen
static HWND   s_backWnd;      // the black backdrop under a program (see PutBackdropUnder)
static bool   s_reverse;      // the reverse gear is in (the shell shows the camera): nothing of ours is raised
static void   PutBackdropUnder(HWND top);
static void   HideBackdrop();
enum { kMaxApps = 64, kAppsPerPage = 8 };
static App   s_apps[kMaxApps];
static int   s_appCount;
static int   s_page;
static Screen s_screen = S_APPS;

// Shortcut editor: works on a copy, written to apps.txt on "Save".
static bool  s_editMode;          // apps screen: a tile opens its editor instead of starting the program
static int   s_editIdx;           // index in s_apps, -1 = new shortcut
static App   s_edit;
static bool  s_delArmed;          // "Delete" was pressed once; the second press deletes

static int     s_keysField;       // F_NAME or F_PARAMS
static wchar_t s_keysText[200];
static bool    s_keysEn, s_keysShift;
static bool    s_clipMenu;       // "Copy" / "Paste" shown in place of the text field

// File browser for the program path: folders and .exe files of s_fdir.
struct FileEnt { wchar_t name[100]; bool dir; };
enum { kMaxFiles = 256, kFilesPerPage = 10 };
static FileEnt s_files[kMaxFiles];
static int     s_fileCount, s_filePage;
static wchar_t s_fdir[MAX_PATH];  // ends with a backslash
static int     s_filesFor;        // F_PATH: .exe files; F_ICON: pictures (.png, .ico) and .exe/.dll

enum { kMaxBtns = 96 };
static Btn   s_btns[kMaxBtns];    // buttons of the current screen, registered while it is drawn
static int   kBtnCount;

static Img     s_icons[IC_COUNT][2];   // [1] = pressed variant
static wchar_t s_skinBg[MAX_PATH];     // background picture, optional

static HWND    s_wnd;
static HBITMAP s_back;          // whole frame, drawn off-screen
static HBITMAP s_bg;            // the skin's background picture, only if it gives one (750 KB)
static unsigned short* s_backBits;   // their pixels: RGB565, bottom-up, 800 per row
static unsigned short* s_bgBits;
static unsigned short* s_tile[2];    // one period of the triangle mosaic (kTileW x kTileH, top-down), day / night
static int     s_pressed = -1;
static int     s_downX;         // where the press started: a horizontal swipe turns the page
static bool    s_night;         // follows VestaHome's day/night
static wchar_t s_dir[MAX_PATH];
static wchar_t s_status[96];    // last action result, shown in the status bar
static HWND    s_under;         // the window we were opened over (the shell, whichever it is): "Back" returns to it
enum { TIMER_FRONT = 4, kFrontMs = 4000 };
enum { TIMER_LONG = 6, kLongMs = 600 };   // a long press (the keyboard's text field)
static DWORD   s_frontTick;     // when we were asked to the front: kept there for kFrontMs
#ifdef UNDER_CE
static HWND    s_taskbar;
#endif
static int AppsPages()
{
    int tiles = s_appCount + (s_appCount < kMaxApps ? 1 : 0);   // + the "Add" tile
    return (tiles + kAppsPerPage - 1) / kAppsPerPage;
}

static int FilePages()
{
    int pages = (s_fileCount + kFilesPerPage - 1) / kFilesPerPage;
    return pages < 1 ? 1 : pages;
}

// Night mode only dims the menu: colours lose a third of their brightness.
static COLORREF C(COLORREF c)
{
    if (!s_night)
        return c;
    return RGB(GetRValue(c) * 2 / 3, GetGValue(c) * 2 / 3, GetBValue(c) * 2 / 3);
}

// ---------------------------------------------------------------- drawing helpers

static void Fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(C(c));
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void Poly(HDC dc, const POINT* p, int n, COLORREF fill)
{
    HBRUSH br = CreateSolidBrush(C(fill));
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    Polygon(dc, p, n);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// Equilateral-triangle mosaic, three close shades (the 2018 Vesta trim).
static void Triangles(HDC dc, int l, int t, int r, int b, const COLORREF* shades, int nshades)
{
    const int w = 48, h = 42;
    HRGN clip = CreateRectRgn(l, t, r, b);
    SelectClipRgn(dc, clip);
    for (int row = 0; t + row * h < b; row++) {
        int y0 = t + row * h, y1 = y0 + h;
        for (int col = -1; l + col * w / 2 < r; col++) {
            int x = l + col * w / 2;
            bool up = ((row + col) & 1) == 0;
            POINT p[3];
            if (up) {
                p[0].x = x;         p[0].y = y1;
                p[1].x = x + w / 2; p[1].y = y0;
                p[2].x = x + w;     p[2].y = y1;
            } else {
                p[0].x = x;         p[0].y = y0;
                p[1].x = x + w;     p[1].y = y0;
                p[2].x = x + w / 2; p[2].y = y1;
            }
            Poly(dc, p, 3, shades[(row * 7 + col * 3 + (up ? 1 : 0)) % nshades]);
        }
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

// ---------------------------------------------------------------- icons (48 x 48 design grid)

struct Pen {
    HDC dc; int x, y; double s; HPEN pen; HGDIOBJ old;
    Pen(HDC d, int px, int py, int size, COLORREF c) : dc(d), x(px), y(py), s(size / 48.0)
    {
        int w = (int)(3.5 * s + 0.5);
        pen = CreatePen(PS_SOLID, w < 2 ? 2 : w, C(c));
        old = SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
    }
    ~Pen() { SelectObject(dc, old); DeleteObject(pen); }
    int X(double v) const { return x + (int)(v * s + 0.5); }
    int Y(double v) const { return y + (int)(v * s + 0.5); }
    void Line(const double* xy, int n)
    {
        POINT p[16];
        for (int i = 0; i < n; i++) {
            p[i].x = X(xy[2 * i]);
            p[i].y = Y(xy[2 * i + 1]);
        }
        Polyline(dc, p, n);
    }
    void L2(double a, double b, double c, double d) { double v[4] = { a, b, c, d }; Line(v, 2); }
    void Circle(double cx, double cy, double r) { Ellipse(dc, X(cx - r), Y(cy - r), X(cx + r), Y(cy + r)); }
    void Box(double l, double t, double r, double b, double rad)
    {
        RoundRect(dc, X(l), Y(t), X(r), Y(b), (int)(rad * 2 * s), (int)(rad * 2 * s));
    }
    // GDI on CE has no Arc(): approximate with a polyline, angles in degrees, 0 = right, clockwise.
    void ArcP(double cx, double cy, double r, double a0, double a1)
    {
        POINT p[24];
        int n = 0;
        for (int i = 0; i <= 20; i++) {
            double a = (a0 + (a1 - a0) * i / 20) * 3.14159265 / 180;
            p[n].x = X(cx + r * cos(a));
            p[n].y = Y(cy + r * sin(a));
            n++;
        }
        Polyline(dc, p, n);
    }
};

static void PaintIcon(HDC dc, Icon ic, int x, int y, int size, COLORREF c)
{
    Pen p(dc, x, y, size, c);
    switch (ic) {
    case IC_BACK: {
        double v[] = { 30, 9, 15, 24, 30, 39 };
        p.Line(v, 3);
        break;
    }
    case IC_NEXT: {
        double v[] = { 18, 9, 33, 24, 18, 39 };
        p.Line(v, 3);
        break;
    }
    case IC_EDIT: {   // pencil (mockup i-edit)
        double v[] = { 30, 8, 40, 18, 18, 40, 8, 40, 8, 30, 30, 8 };
        p.Line(v, 6);
        p.L2(26, 12, 36, 22);
        break;
    }
    case IC_ADD:
        p.L2(24, 9, 24, 39);
        p.L2(9, 24, 39, 24);
        break;
    case IC_FOLDER: {
        double v[] = { 5, 11, 18, 11, 22, 16, 43, 16, 43, 39, 5, 39, 5, 11 };
        p.Line(v, 7);
        break;
    }
    case IC_FILE: {
        double v[] = { 11, 5, 28, 5, 37, 14, 37, 43, 11, 43, 11, 5 };
        double w[] = { 28, 5, 28, 14, 37, 14 };
        p.Line(v, 6);
        p.Line(w, 3);
        break;
    }
    case IC_UP: {
        double v[] = { 9, 31, 24, 16, 39, 31 };
        p.Line(v, 3);
        break;
    }
    case IC_SHIFT: {
        double v[] = { 24, 7, 41, 24, 31, 24, 31, 40, 17, 40, 17, 24, 7, 24, 24, 7 };
        p.Line(v, 8);
        break;
    }
    case IC_BKSP: {
        double v[] = { 16, 11, 43, 11, 43, 37, 16, 37, 4, 24, 16, 11 };
        p.Line(v, 6);
        p.L2(23, 18, 35, 30);
        p.L2(35, 18, 23, 30);
        break;
    }
    case IC_TASKS: {   // task manager: a screen with the load's pulse (as in VestaHome)
        double v[] = { 10, 26, 17, 26, 21, 17, 27, 33, 31, 23, 38, 23 };
        p.Box(5, 9, 43, 39, 4);
        p.Line(v, 6);
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- skin pictures

#define RGB565(r, g, b) ((unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// 800 x 480 RGB565 bottom-up DIB section: the unit's screen format, with pixels reachable for blending.
static HBITMAP MakeDib(int w, int h, unsigned short** bits)
{
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = w;
    bi.h.biHeight = h;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.masks[0] = 0xF800;
    bi.masks[1] = 0x07E0;
    bi.masks[2] = 0x001F;
    HDC screen = GetDC(NULL);
    HBITMAP bmp = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)bits, NULL, 0);
    ReleaseDC(NULL, screen);
    return bmp;
}

// Blends a skin picture scaled (bilinear) to w x h at (x, y) into the back buffer; dimmed at night.
// With a tint the picture is only a mask: painted in that colour.
static void DrawImg(const Img& im, int x, int y, int w, int h, COLORREF tint = CLR_INVALID)
{
#ifndef UNDER_CE
    GdiFlush();   // desktop GDI batches drawing: finish it before touching the pixels
#endif
    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= 480)
            continue;
        int fy = (2 * dy + 1) * im.h * 128 / h - 128;   // source position, 8-bit fraction
        if (fy < 0)
            fy = 0;
        int y0 = fy >> 8, wy = fy & 255, y1 = y0 + 1 < im.h ? y0 + 1 : y0;
        const unsigned char* r0 = im.px + y0 * im.w * 4;
        const unsigned char* r1 = im.px + y1 * im.w * 4;
        unsigned short* dst = s_backBits + (479 - py) * 800;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < 0 || px >= 800)
                continue;
            int fx = (2 * dx + 1) * im.w * 128 / w - 128;
            if (fx < 0)
                fx = 0;
            int x0 = fx >> 8, wx = fx & 255, x1 = x0 + 1 < im.w ? x0 + 1 : x0;
            const unsigned char *pa = r0 + x0 * 4, *pb = r0 + x1 * 4, *pc = r1 + x0 * 4, *pd = r1 + x1 * 4;
            int s[4];
            for (int k = 0; k < 4; k++)
                s[k] = ((pa[k] * (256 - wx) + pb[k] * wx) * (256 - wy) + (pc[k] * (256 - wx) + pd[k] * wx) * wy) >> 16;
            if (!s[3])
                continue;
            if (tint != CLR_INVALID) {
                s[0] = GetRValue(tint) * s[3] / 255;
                s[1] = GetGValue(tint) * s[3] / 255;
                s[2] = GetBValue(tint) * s[3] / 255;
            }
            if (s_night)
                for (int k = 0; k < 3; k++)
                    s[k] = s[k] * 2 / 3;
            unsigned p = dst[px];
            int inv = 255 - s[3];
            int r = s[0] + ((p >> 8) & 0xF8) * inv / 255;
            int g = s[1] + ((p >> 3) & 0xFC) * inv / 255;
            int b = s[2] + ((p << 3) & 0xF8) * inv / 255;
            dst[px] = RGB565(r, g, b);
        }
    }
}

// ---------------------------------------------------------------- frame

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

// Whole file into a malloc'ed buffer with a terminating zero; NULL if it can't be read.
static char* ReadAll(const wchar_t* path, DWORD* size)
{
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD n = GetFileSize(h, NULL), got = 0;
    char* buf = n == 0xFFFFFFFF ? NULL : (char*)malloc(n + 1);
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

// The skin's 800 x 480 background picture (as is: night dims it when a frame is drawn).
static bool LoadBackground()
{
    DWORD size;
    char* data = ReadAll(s_skinBg, &size);
    if (!data)
        return false;
    int w, h, comp;
    unsigned char* rgb = stbi_load_from_memory((unsigned char*)data, size, &w, &h, &comp, 3);
    free(data);
    if (!rgb)
        return false;
    for (int y = 0; y < 480; y++) {
        unsigned short* dst = s_bgBits + (479 - y) * 800;
        for (int x = 0; x < 800; x++) {
            COLORREF c = kInk;
            if (x < w && y < h) {
                const unsigned char* p = rgb + (y * w + x) * 3;
                c = RGB(p[0], p[1], p[2]);
            }
            dst[x] = RGB565(GetRValue(c), GetGValue(c), GetBValue(c));
        }
    }
    stbi_image_free(rgb);
    return true;
}

enum { kTileW = 48, kTileH = 252 };   // the mosaic repeats every two triangles across and six rows down

// Night: a third less brightness, as C() does (RGB565).
static unsigned short Dim565(unsigned p)
{
    return (unsigned short)((((p >> 11) * 2 / 3) << 11) | ((((p >> 5) & 63) * 2 / 3) << 5) | ((p & 31) * 2 / 3));
}

// As in VestaHome: one period of the mosaic is kept, day and night (24 KB each), and copied into the frame
// row by row instead of keeping an 800 x 480 background (750 KB). Only the skin's own background picture,
// if it gives one, is kept full-size. Built once at start, in day colours.
static void BuildBackground()
{
    unsigned short* bits;
    HBITMAP bmp = MakeDib(kTileW, kTileH, &bits);
    if (bmp) {
        HDC dc = CreateCompatibleDC(NULL);
        HGDIOBJ ob = SelectObject(dc, bmp);
        Triangles(dc, 0, 0, kTileW, kTileH, kTri, 3);
        SelectObject(dc, ob);
        DeleteDC(dc);
#ifndef UNDER_CE
        GdiFlush();
#endif
        s_tile[0] = (unsigned short*)malloc(kTileW * kTileH * 2);
        s_tile[1] = (unsigned short*)malloc(kTileW * kTileH * 2);
        if (s_tile[0] && s_tile[1])
            for (int y = 0; y < kTileH; y++)
                for (int x = 0; x < kTileW; x++) {
                    unsigned short p = bits[(kTileH - 1 - y) * kTileW + x];   // the DIB is bottom-up
                    s_tile[0][y * kTileW + x] = p;
                    s_tile[1][y * kTileW + x] = Dim565(p);
                }
        DeleteObject(bmp);
    }
    if (s_skinBg[0]) {
        s_bg = MakeDib(800, 480, &s_bgBits);
        if (s_bg && !LoadBackground()) {
            DeleteObject(s_bg);
            s_bg = NULL;
        }
    }
}

// ---------------------------------------------------------------- drawing (VestaHome's helpers)
// Coordinates as in the mockups' SVG: x, y, w, h, text y = baseline. Buttons are registered while drawn (Zone).

// The background into the frame, as VestaHome's screens: graphite under the status bar, the mosaic below
// it (copied from the tile, row by row); or the skin's picture, dimmed per frame at night.
static void Background(HDC dc)
{
    if (s_bg) {
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ ob = SelectObject(src, s_bg);
        BitBlt(dc, 0, 0, 800, 480, src, 0, 0, SRCCOPY);
        SelectObject(src, ob);
        DeleteDC(src);
        if (s_night) {
#ifndef UNDER_CE
            GdiFlush();
#endif
            for (int i = 0; i < 800 * 480; i++)
                s_backBits[i] = Dim565(s_backBits[i]);
        }
        return;
    }
    Fill(dc, 0, 0, 800, 56, kInk);
    const unsigned short* t = s_tile[s_night ? 1 : 0];
    if (!t)
        return;
#ifndef UNDER_CE
    GdiFlush();
#endif
    for (int y = 56; y < 480; y++) {
        const unsigned short* src = t + ((y - 56) % kTileH) * kTileW;
        unsigned short* dst = s_backBits + (479 - y) * 800;
        for (int x = 0; x < 800; x += kTileW)
            memcpy(dst + x, src, (x + kTileW <= 800 ? kTileW : 800 - x) * 2);
    }
}

struct FontEnt { int size, weight; HFONT f; };
static FontEnt s_fonts[160];
static int     s_fontCount;

// Fonts by pixel size and weight (400 / 600 / 700 as in the mockups), created once.
static HFONT Font(int size, int weight)
{
    for (int i = 0; i < s_fontCount; i++)
        if (s_fonts[i].size == size && s_fonts[i].weight == weight)
            return s_fonts[i].f;
    HFONT f = MakeFont(size, weight);
    if (s_fontCount < 160) {
        s_fonts[s_fontCount].size = size;
        s_fonts[s_fontCount].weight = weight;
        s_fonts[s_fontCount++].f = f;
    }
    return f;
}

static int TextWidth(HDC dc, const wchar_t* s, int size, int weight)
{
    SIZE sz;
    HGDIOBJ of = SelectObject(dc, Font(size, weight));
    GetTextExtentPoint32(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

enum { AL_START, AL_MID, AL_END };

// Text like SVG <text>: baseline at y, anchored at its start, middle or end. A line wider than maxw
// (0 = no limit) is set smaller until it fits. Returns its right edge.
static int T(HDC dc, int x, int y, const wchar_t* s, int size, COLORREF c, int weight = 600, int anchor = AL_START,
             int maxw = 0)
{
    int w = TextWidth(dc, s, size, weight);
    while (maxw > 0 && w > maxw && size > 10)
        w = TextWidth(dc, s, --size, weight);
    int left = anchor == AL_MID ? x - w / 2 : anchor == AL_END ? x - w : x;
    HGDIOBJ of = SelectObject(dc, Font(size, weight));
    SetTextAlign(dc, TA_LEFT | TA_BASELINE);
    SetTextColor(dc, C(c));
    SetBkMode(dc, TRANSPARENT);
    ExtTextOut(dc, left, y, 0, NULL, s, (int)wcslen(s), NULL);
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    SelectObject(dc, of);
    return left + w;
}

// A line too long for maxw shows its end after "…" (paths, typed text). Returns its right edge.
static int TTail(HDC dc, int x, int y, const wchar_t* s, int size, COLORREF c, int weight, int maxw)
{
    if (TextWidth(dc, s, size, weight) <= maxw)
        return T(dc, x, y, s, size, c, weight);
    wchar_t buf[MAX_PATH + 2];
    const wchar_t* p = s;
    do {
        p++;
        _snwprintf(buf, MAX_PATH + 2, L"…%s", p);
        buf[MAX_PATH + 1] = 0;
    } while (*p && TextWidth(dc, buf, size, weight) > maxw);
    return T(dc, x, y, buf, size, c, weight);
}

// Rounded rectangle like SVG <rect rx>: fill and 2 px stroke, CLR_INVALID = none.
static void R(HDC dc, int x, int y, int w, int h, int rx, COLORREF fill, COLORREF stroke = CLR_INVALID)
{
    if (w <= 0 || h <= 0)
        return;
    if (rx == 0 && stroke == CLR_INVALID) {
        if (fill != CLR_INVALID)
            Fill(dc, x, y, x + w, y + h, fill);
        return;
    }
    HBRUSH br = fill == CLR_INVALID ? NULL : CreateSolidBrush(C(fill));
    HPEN pen = stroke == CLR_INVALID ? CreatePen(PS_NULL, 0, 0) : CreatePen(PS_SOLID, 2, C(stroke));
    HGDIOBJ ob = SelectObject(dc, br ? (HGDIOBJ)br : GetStockObject(NULL_BRUSH)), op = SelectObject(dc, pen);
    int e = stroke == CLR_INVALID ? 1 : 0;   // without a pen GDI leaves out the right and bottom edge
    RoundRect(dc, x, y, x + w + e, y + h + e, rx * 2, rx * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (br)
        DeleteObject(br);
    DeleteObject(pen);
}

// Dashed frame of the empty "Add" slot (2 px, the corners stay open).
static void DashRect(HDC dc, int x, int y, int w, int h, int rx, COLORREF c)
{
    for (int p = rx; p < w - rx; p += 12) {
        int q = p + 6 < w - rx ? p + 6 : w - rx;
        Fill(dc, x + p, y, x + q, y + 2, c);
        Fill(dc, x + p, y + h - 2, x + q, y + h, c);
    }
    for (int p = rx; p < h - rx; p += 12) {
        int q = p + 6 < h - rx ? p + 6 : h - rx;
        Fill(dc, x, y + p, x + 2, y + q, c);
        Fill(dc, x + w - 2, y + p, x + w, y + q, c);
    }
}

// Registers a button of the current screen; true while it is pressed.
static bool Zone(int x, int y, int w, int h, Action a, int arg = 0)
{
    if (kBtnCount >= kMaxBtns)
        return false;
    Btn& b = s_btns[kBtnCount];
    b.rc.left = x;
    b.rc.top = y;
    b.rc.right = x + w;
    b.rc.bottom = y + h;
    b.action = a;
    b.arg = arg;
    return kBtnCount++ == s_pressed;
}

// Icon in any colour: the skin's picture (as is in the text colour, otherwise tinted) or the built-in drawing.
static void Ico(HDC dc, Icon ic, int x, int y, int size, COLORREF c)
{
    const Img& im = s_icons[ic][0];
    const Img& dn = s_icons[ic][1];
    if (c == kInk && dn.px)
        DrawImg(dn, x, y, size, size);
    else if (im.px)
        DrawImg(im, x, y, size, size, c == kText ? CLR_INVALID : c);
    else
        PaintIcon(dc, ic, x, y, size, c);
}

static void Dock(HDC dc)
{
    Fill(dc, 0, 398, 800, 480, kInk);
    Fill(dc, 0, 398, 800, 400, kLine);
}

// Button states as in VestaHome: outlined, selected, pressed, filled, the screen's main action (orange).
enum { B_N, B_SEL, B_ACT, B_FILL, B_PRI };

static void Button(HDC dc, int x, int y, int w, int h, const wchar_t* label, int state, Icon ic = IC_NONE,
                   int size = 18, int isz = 0, Action a = A_NONE, int arg = 0)
{
    if (Zone(x, y, w, h, a, arg))
        state = B_ACT;
    COLORREF fg;
    if (state == B_ACT || state == B_SEL || state == B_PRI) {
        R(dc, x, y, w, h, 10, kOrange);
        fg = kInk;
    } else if (state == B_FILL) {
        R(dc, x, y, w, h, 10, kPanel, kLine);
        fg = kText;
    } else {
        R(dc, x, y, w, h, 10, CLR_INVALID, kLine);
        fg = label ? kMuted : kText;
    }
    if (ic != IC_NONE && label) {
        if (!isz)
            isz = h - 20 < 32 ? h - 20 : 32;
        int tw = TextWidth(dc, label, size, 700);
        int x0 = x + (w - isz - 10 - tw) / 2;
        Ico(dc, ic, x0, y + (h - isz) / 2, isz, fg);
        T(dc, x0 + isz + 10, y + h / 2 + size * 36 / 100, label, size, fg, 700);
    } else if (ic != IC_NONE) {
        if (!isz)
            isz = h - 14 < 44 ? h - 14 : 44;
        Ico(dc, ic, x + (w - isz) / 2, y + (h - isz) / 2, isz, fg);
    } else if (label) {
        T(dc, x + w / 2, y + h / 2 + size * 36 / 100, label, size, fg, 700, AL_MID, w - 12);
    }
}

// Status bar as on VestaHome's screens: back button and title, the clock; the last action's result (if any)
// right before the clock. No Bluetooth / USB marks: their state is not known here yet.
static void Frame(HDC dc, const wchar_t* title, Action back)
{
    Fill(dc, 0, 54, 800, 56, kLine);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t s[8];
    _snwprintf(s, 8, L"%02d:%02d", t.wHour, t.wMinute);
    T(dc, 784, 38, s, 26, kText, 600, AL_END);
    bool d = Zone(0, 0, 64, 56, back);
    R(dc, 8, 8, 48, 40, 8, d ? kOrange : kPanel);
    Ico(dc, IC_BACK, 14, 10, 36, d ? kInk : kText);
    int end = T(dc, 72, 37, title, 22, kText, 600, AL_START, s_status[0] ? 340 : 620);
    if (s_status[0])
        T(dc, 696, 36, s_status, 15, kMuted, 400, AL_END, 696 - end - 16);
}

// ---------------------------------------------------------------- screens

// Program tiles 184 x 150, 4 x 2 per page (mockup "Программы"), the last one "Add" as a dashed empty slot;
// in edit mode the tiles are framed orange and open the editor. Dock: page dots, edit; back is in the status bar.
static void DrawApps(HDC dc)
{
    Frame(dc, s_editMode ? L"ПРИЛОЖЕНИЯ · ИЗМЕНЕНИЕ" : L"ПРИЛОЖЕНИЯ", A_BACK);   // back = minimize
    int first = s_page * kAppsPerPage;
    int tiles = s_appCount + (s_appCount < kMaxApps ? 1 : 0);
    for (int k = 0; k < kAppsPerPage && first + k < tiles; k++) {
        int i = first + k, x = 16 + (k % 4) * 194, y = 72 + (k / 4) * 162;
        if (i == s_appCount) {   // "Add"
            bool d = Zone(x, y, 184, 150, A_ADD);
            if (d)
                R(dc, x, y, 184, 150, 14, kOrange);
            else
                DashRect(dc, x, y, 184, 150, 14, kLine);
            Ico(dc, IC_ADD, x + 68, y + 30, 48, d ? kInk : kDim);
            T(dc, x + 92, y + 124, L"ДОБАВИТЬ", 15, d ? kInk : kDim, 600, AL_MID);
            continue;
        }
        const App& a = s_apps[i];
        bool d = Zone(x, y, 184, 150, A_RUN, i);
        R(dc, x, y, 184, 150, 14, d ? kOrange : kPanel, d ? CLR_INVALID : s_editMode ? kOrange : kLine);
        if (!d) {   // orange facet in the top right corner, following the rounded corner (radius 14)
            POINT f[12];
            int n = 0;
            f[n].x = x + 184 - 38; f[n].y = y + 1; n++;
            for (int j = 0; j <= 8; j++, n++) {
                double an = (-90 + j * 90 / 8.0) * 3.14159265 / 180;
                f[n].x = x + 184 - 14 + (int)(13 * cos(an) + 0.5);
                f[n].y = y + 14 + (int)(13 * sin(an) + 0.5);
            }
            f[n].x = x + 184 - 1; f[n].y = y + 38; n++;
            Poly(dc, f, n, kFacet);
        }
        if (a.img.px) {
            DrawImg(a.img, x + 60, y + 24, 64, 64);
        } else if (a.icon) {
            DrawIconEx(dc, x + 60, y + 24, a.icon, 64, 64, 0, NULL, DI_NORMAL);
        } else {   // no icon in the program: its first letter in a ring
            wchar_t letter[2] = { a.name[0], 0 };
            Pen ring(dc, x + 60, y + 24, 64, d ? kInk : kText);
            ring.Circle(24, 24, 20);
            T(dc, x + 92, y + 66, letter, 26, d ? kInk : kText, 700, AL_MID);
        }
        T(dc, x + 92, y + 124, a.exists ? a.name : L"нет файла", 15, d ? kInk : a.exists ? kMuted : kDim,
          d ? 700 : 600, AL_MID, 168);
    }
    if (s_appCount == 0) {
        T(dc, 400, 300, L"Список программ пуст", 22, kText, 700, AL_MID);
        T(dc, 400, 332, L"Нажмите «Добавить» или впишите программы в apps.txt", 17, kMuted, 400, AL_MID, 760);
    }
    Dock(dc);
    int pages = AppsPages();
    if (pages > 1) {   // page dots; tapping them (or swiping the tiles) turns the page
        Zone(212, 410, 376, 58, A_NEXT);
        int w = 28 + (pages - 1) * 28, dx = 400 - w / 2;
        for (int p = 0; p < pages; p++) {
            int dw = p == s_page ? 28 : 20;
            R(dc, dx, 436, dw, 6, 3, p == s_page ? kOrange : kDim);
            dx += dw + 8;
        }
    }
    Button(dc, 16, 410, 184, 58, L"ЗАДАЧИ", B_N, IC_TASKS, 17, 0, A_TASKS);
    Button(dc, 600, 410, 184, 58, L"ИЗМЕНИТЬ", s_editMode ? B_SEL : B_N, IC_EDIT, 17, 0, A_EDITMODE);
}

static const wchar_t* kFieldCaption[4] = { L"НАЗВАНИЕ", L"ПРОГРАММА", L"ПАРАМЕТРЫ ЗАПУСКА", L"ИКОНКА" };
static const wchar_t* kFieldEmpty[4] = { L"нажмите, чтобы ввести", L"нажмите, чтобы выбрать exe", L"нет",
    L"своя у программы; нажмите, чтобы выбрать png, ico или exe" };

// One shortcut: four fields (keyboard or file browser behind each), cancel / delete / save.
// A chosen icon has a "reset" button at the end of its field.
static void DrawEdit(HDC dc)
{
    Frame(dc, s_editIdx < 0 ? L"НОВЫЙ ЯРЛЫК" : L"ИЗМЕНИТЬ ЯРЛЫК", A_CANCEL);
    for (int k = 0; k < 4; k++) {
        int y = 72 + k * 80;
        const wchar_t* v = k == F_NAME ? s_edit.name : k == F_PATH ? s_edit.path :
                           k == F_PARAMS ? s_edit.params : s_edit.iconPath;
        bool reset = k == F_ICON && v[0];
        int w = reset ? 640 : 768;
        bool d = Zone(16, y, w, 70, A_FIELD, k);
        R(dc, 16, y, w, 70, 10, d ? kOrange : kPanel, d ? CLR_INVALID : kLine);
        T(dc, 36, y + 26, kFieldCaption[k], 13, d ? kInk : kMuted, 700);
        if (v[0])
            TTail(dc, 36, y + 55, v, 22, d ? kInk : kText, 700, w - 88);
        else
            T(dc, 36, y + 55, kFieldEmpty[k], 18, d ? kInk : kDim, 400, AL_START, w - 88);
        Ico(dc, IC_NEXT, 16 + w - 48, y + 19, 32, d ? kInk : kMuted);
        if (reset)
            Button(dc, 668, y, 116, 70, L"СБРОС", B_N, IC_NONE, 17, 0, A_NOICON);
    }
    Dock(dc);
    Button(dc, 16, 410, 220, 58, L"ОТМЕНА", B_N, IC_NONE, 18, 0, A_CANCEL);
    if (s_editIdx >= 0)   // the second press deletes
        Button(dc, 290, 410, 220, 58, s_delArmed ? L"ТОЧНО УДАЛИТЬ?" : L"УДАЛИТЬ", s_delArmed ? B_FILL : B_N,
               IC_NONE, 18, 0, A_DELETE);
    Button(dc, 564, 410, 220, 58, L"СОХРАНИТЬ", B_PRI, IC_NONE, 18, 0, A_SAVE);
}

// On-screen keyboard (like the mockup's Wi-Fi keyboard): the field and "Done" on top, 4 rows of 12 keys,
// then Shift, language, space, backspace.
static void DrawKeys(HDC dc)
{
    Frame(dc, s_keysField == F_NAME ? L"НАЗВАНИЕ" : L"ПАРАМЕТРЫ ЗАПУСКА", A_CANCEL);
    if (s_clipMenu) {   // after a long press on the field, in its place; any other tap closes it
        Button(dc, 16, 68, 194, 60, L"КОПИРОВАТЬ", s_keysText[0] ? B_FILL : B_N, IC_NONE, 17, 0,
               s_keysText[0] ? A_CLIPCOPY : A_NONE);
        Button(dc, 219, 68, 194, 60, L"ВСТАВИТЬ", B_FILL, IC_NONE, 17, 0, A_CLIPPASTE);
        Button(dc, 422, 68, 194, 60, L"ОЧИСТИТЬ", s_keysText[0] ? B_FILL : B_N, IC_NONE, 17, 0,
               s_keysText[0] ? A_CLIPCLEAR : A_NONE);
    } else {
        Zone(16, 68, 600, 60, A_KEYFIELD);   // a long press: copy / paste (TIMER_LONG)
        R(dc, 16, 68, 600, 60, 10, kPanel, kLine);
        int end = TTail(dc, 32, 106, s_keysText, 26, kText, 700, 560);
        R(dc, end + 4, 84, 3, 28, 0, kOrange);
    }
    Button(dc, 628, 68, 156, 60, L"ГОТОВО", B_PRI, IC_NONE, 18, 0, A_OK);
    const wchar_t** rows = s_keysEn ? kKeysEn : kKeysRu;
    for (int r = 0; r < 4; r++)
        for (int c = 0; rows[r][c]; c++) {
            int x = 13 + c * 65, y = 140 + r * 66;
            wchar_t ch[2] = { rows[r][c], 0 };
            if (s_keysShift)
                CharUpperBuff(ch, 1);
            bool d = Zone(x, y, 60, 60, A_KEY, rows[r][c]);
            R(dc, x, y, 60, 60, 10, d ? kOrange : kPanel, d ? CLR_INVALID : kLine);
            T(dc, x + 30, y + 40, ch, 24, d ? kInk : kText, 700, AL_MID);
        }
    Button(dc, 13, 404, 125, 60, NULL, s_keysShift ? B_SEL : B_FILL, IC_SHIFT, 18, 32, A_SHIFT);
    Button(dc, 143, 404, 125, 60, s_keysEn ? L"ENG" : L"РУС", B_FILL, IC_NONE, 20, 0, A_LANG);
    bool d = Zone(273, 404, 385, 60, A_KEY, L' ');
    R(dc, 273, 404, 385, 60, 10, d ? kOrange : kPanel, d ? CLR_INVALID : kLine);
    R(dc, 397, 438, 136, 4, 2, d ? kInk : kMuted);
    Button(dc, 663, 404, 125, 60, NULL, B_FILL, IC_BKSP, 18, 36, A_BKSP);
}

// Choosing the program: the folder's path, folders and .exe files 2 x 5 per page.
static void DrawFiles(HDC dc)
{
    Frame(dc, s_filesFor == F_ICON ? L"ВЫБОР ИКОНКИ" : L"ВЫБОР ПРОГРАММЫ", A_CANCEL);
    TTail(dc, 20, 84, s_fdir, 15, kMuted, 400, 760);
    int first = s_filePage * kFilesPerPage;
    for (int k = 0; k < kFilesPerPage && first + k < s_fileCount; k++) {
        const FileEnt& f = s_files[first + k];
        int x = 16 + (k / 5) * 388, y = 96 + (k % 5) * 60;
        bool d = Zone(x, y, 380, 54, A_FILE, first + k);
        R(dc, x, y, 380, 54, 10, d ? kOrange : kPanel);
        Ico(dc, f.dir ? IC_FOLDER : IC_FILE, x + 14, y + 12, 30, d ? kInk : kMuted);
        T(dc, x + 58, y + 34, f.name, 18, d ? kInk : kText, 700, AL_START, 310);
    }
    if (s_fileCount == 0)
        T(dc, 400, 240, s_filesFor == F_ICON ? L"Здесь нет папок и иконок" : L"Здесь нет папок и программ", 22,
          kText, 700, AL_MID);
    Dock(dc);
    Button(dc, 16, 410, 120, 58, NULL, B_N, IC_UP, 18, 0, A_UP);
    Button(dc, 148, 410, 120, 58, NULL, B_N, IC_BACK, 18, 0, A_PREV);
    wchar_t s[16];
    _snwprintf(s, 16, L"%d / %d", s_filePage + 1, FilePages());
    T(dc, 400, 448, s, 22, kText, 700, AL_MID);
    Button(dc, 532, 410, 120, 58, NULL, B_N, IC_NEXT, 18, 0, A_NEXT);
    Button(dc, 664, 410, 120, 58, L"ОТМЕНА", B_N, IC_NONE, 18, 0, A_CANCEL);
}

static void DrawTasks(HDC dc);

static void DrawFrame(HDC dc)
{
    Background(dc);
    kBtnCount = 0;
    switch (s_screen) {
    case S_APPS:  DrawApps(dc); break;
    case S_EDIT:  DrawEdit(dc); break;
    case S_KEYS:  DrawKeys(dc); break;
    case S_FILES: DrawFiles(dc); break;
    case S_TASKS: DrawTasks(dc); break;
    }
}

static int s_drawnMinute = -1;   // the minute on the clock of the last frame

static void Redraw()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    s_drawnMinute = t.wMinute;
    HDC hdc = GetDC(s_wnd);
    HDC dc = CreateCompatibleDC(hdc);
    HGDIOBJ ob = SelectObject(dc, s_back);
    DrawFrame(dc);
    BitBlt(hdc, 0, 0, 800, 480, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteDC(dc);
    ReleaseDC(s_wnd, hdc);
}

// A page turn, as in VestaHome: the new page slides in from the right (dir 1) or the left (dir -1) and pushes
// the old one out, in the band between the top bar and the dock (they stay). The old frame is what s_back
// holds now; its copy lives only for the animation (768 KB).
static void RedrawSlide(int dir)
{
    unsigned short* oldBits = NULL;
    HBITMAP old = MakeDib(800, 480, &oldBits);
    if (!old || !oldBits) {
        if (old)
            DeleteObject(old);
        Redraw();
        return;
    }
    memcpy(oldBits, s_backBits, 800 * 480 * 2);
    SYSTEMTIME t;
    GetLocalTime(&t);
    s_drawnMinute = t.wMinute;
    HDC hdc = GetDC(s_wnd);
    HDC dc = CreateCompatibleDC(hdc), odc = CreateCompatibleDC(hdc);
    HGDIOBJ ob = SelectObject(dc, s_back), oo = SelectObject(odc, old);
    DrawFrame(dc);
    const int top = 56, bottom = 408, h = bottom - top;
    BitBlt(hdc, 0, 0, 800, top, dc, 0, 0, SRCCOPY);
    BitBlt(hdc, 0, bottom, 800, 480 - bottom, dc, 0, bottom, SRCCOPY);
    const DWORD kMs = 220;
    DWORD t0 = GetTickCount();
    for (;;) {
        DWORD el = GetTickCount() - t0;
        double p = el >= kMs ? 1.0 : (double)el / kMs;
        p = 1 - (1 - p) * (1 - p) * (1 - p);   // ease-out
        int off = (int)(800 * p + 0.5);
        if (dir > 0) {
            BitBlt(hdc, 0, top, 800 - off, h, odc, off, top, SRCCOPY);
            BitBlt(hdc, 800 - off, top, off, h, dc, 0, top, SRCCOPY);
        } else {
            BitBlt(hdc, off, top, 800 - off, h, odc, 0, top, SRCCOPY);
            BitBlt(hdc, 0, top, off, h, dc, 800 - off, top, SRCCOPY);
        }
        if (el >= kMs)
            break;
        Sleep(10);
    }
    SelectObject(odc, oo);
    SelectObject(dc, ob);
    DeleteDC(odc);
    DeleteDC(dc);
    ReleaseDC(s_wnd, hdc);
    DeleteObject(old);
}

static int s_slideDir;   // set by an action that turned a page: the next frame slides in

// ---------------------------------------------------------------- actions

static bool Exists(const wchar_t* path)
{
    return GetFileAttributes(path) != 0xFFFFFFFF;
}

static void Trim(wchar_t* s)
{
    wchar_t* p = s;
    while (*p == L' ' || *p == L'\t')
        p++;
    memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    int n = (int)wcslen(s);
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r'))
        s[--n] = 0;
}

#define APPS_HEADER \
    "# Programs of the \"Apps\" screen: one per line, \"Name | full path to the exe\".\r\n" \
    "# Lines starting with # are ignored. Save the file as UTF-8.\r\n" \
    "# The started program is raised above the stock shell. If it starts another program (a launcher,\r\n" \
    "# a MortScript), name the window to raise as the third field: \"Name | path | class!title\".\r\n" \
    "# The fourth field is the program's command line: \"Name | path | | parameters\" (third field may be empty).\r\n" \
    "# The fifth field is an own icon: \"Name | path | | | Icons\\name.png\" - a .png (up to 256 x 256), .ico or\r\n" \
    "# an exe/dll to take the icon from; a path without a drive/root is from the launcher's folder.\r\n" \
    "# Shortcuts changed on the \"Apps\" screen rewrite this file; comments other than these are not kept.\r\n"

static const char kDefaultApps[] =
    APPS_HEADER
    "\xD0\xAF\xD0\xBD\xD0\xB4\xD0\xB5\xD0\xBA\xD1\x81 \xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD1\x8B | \\SDMMC\\YaMapsCE\\YaMapsCE.exe\r\n"
    "OSM \xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD1\x8B | \\SDMMC\\OSMapsCE\\OSMapsCE.exe\r\n"
    "CityGuide | \\SDMMC\\Navi\\CityGuide\\CityGuideCE.exe\r\n";

static bool LoadImg(const wchar_t* path, Img* im);

static void FreeAppIcon(App& a)
{
    if (a.icon)
        DestroyIcon(a.icon);
    if (a.img.px)
        free(a.img.px);
    a.icon = NULL;
    a.img.px = NULL;
}

// A shortcut's own icon is kept at most 64 x 64 (the tile's size): averaged down from a bigger picture.
static void ShrinkImg(Img* im)
{
    if (im->w <= 64 && im->h <= 64)
        return;
    int w = im->w > 64 ? 64 : im->w, h = im->h > 64 ? 64 : im->h;
    unsigned char* out = (unsigned char*)malloc(w * h * 4);
    if (!out)
        return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int x0 = x * im->w / w, x1 = (x + 1) * im->w / w, y0 = y * im->h / h, y1 = (y + 1) * im->h / h;
            int s[4] = { 0, 0, 0, 0 }, n = (x1 - x0) * (y1 - y0);
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++)
                    for (int k = 0; k < 4; k++)
                        s[k] += im->px[(sy * im->w + sx) * 4 + k];
            for (int k = 0; k < 4; k++)
                out[(y * w + x) * 4 + k] = (unsigned char)(s[k] / n);
        }
    free(im->px);
    im->px = out;
    im->w = w;
    im->h = h;
}

// The tile's icon: the one chosen for the shortcut (if it loads), otherwise the program's own.
static void LoadAppIcon(App& a)
{
    a.icon = NULL;
    a.img.px = NULL;
    if (a.iconPath[0]) {
        wchar_t file[MAX_PATH];
        if (a.iconPath[0] == L'\\' || wcschr(a.iconPath, L':'))
            wcscpy(file, a.iconPath);
        else
            _snwprintf(file, MAX_PATH, L"%s%s", s_dir, a.iconPath);
        file[MAX_PATH - 1] = 0;
        int n = (int)wcslen(file);
        if (n > 4 && !_wcsicmp(file + n - 4, L".png")) {
            if (LoadImg(file, &a.img)) {
                ShrinkImg(&a.img);
                return;
            }
        } else if (ExtractIconEx(file, 0, &a.icon, NULL, 1) && a.icon) {
            return;
        }
        a.icon = NULL;
    }
    if (a.exists)
        ExtractIconEx(a.path, 0, &a.icon, NULL, 1);
}

// Reads apps.txt (UTF-8, optional BOM); writes an example list on the first start.
static void LoadApps()
{
    for (int i = 0; i < s_appCount; i++)
        FreeAppIcon(s_apps[i]);
    s_appCount = 0;

    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%sapps.txt", s_dir);
    DWORD got;
    char* buf = ReadAll(path, &got);
    if (!buf) {
        HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, kDefaultApps, sizeof(kDefaultApps) - 1, &w, NULL);
            CloseHandle(h);
        }
        buf = ReadAll(path, &got);
        if (!buf)
            return;
    }
    char* p = buf;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    while (*p && s_appCount < kMaxApps) {
        char* end = strchr(p, '\n');
        if (end)
            *end = 0;
        wchar_t line[400];
        int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, line, 400);
        p = end ? end + 1 : p + strlen(p);
        if (n <= 0)
            continue;
        Trim(line);
        wchar_t* bar = wcschr(line, L'|');
        if (!line[0] || line[0] == L'#' || line[0] == L';' || !bar)
            continue;
        *bar = 0;
        App& a = s_apps[s_appCount];
        wcsncpy(a.name, line, 47);
        a.name[47] = 0;
        Trim(a.name);
        // optional third field: window "class!title" (a leading '!' as in AppLauncher profiles is allowed),
        // optional fourth: the program's command line; optional fifth: its own icon
        a.wclass[0] = a.wtitle[0] = a.params[0] = a.iconPath[0] = 0;
        wchar_t* bar2 = wcschr(bar + 1, L'|');
        if (bar2) {
            *bar2 = 0;
            wchar_t* w = bar2 + 1;
            wchar_t* bar3 = wcschr(w, L'|');
            if (bar3) {
                *bar3 = 0;
                wchar_t* bar4 = wcschr(bar3 + 1, L'|');
                if (bar4) {
                    *bar4 = 0;
                    wcsncpy(a.iconPath, bar4 + 1, MAX_PATH - 1);
                    a.iconPath[MAX_PATH - 1] = 0;
                    Trim(a.iconPath);
                }
                wcsncpy(a.params, bar3 + 1, 199);
                a.params[199] = 0;
                Trim(a.params);
            }
            Trim(w);
            if (*w == L'!')
                w++;
            wchar_t* ex = wcschr(w, L'!');
            if (ex) {
                *ex = 0;
                wcsncpy(a.wtitle, ex + 1, 95);
                a.wtitle[95] = 0;
            }
            wcsncpy(a.wclass, w, 63);
            a.wclass[63] = 0;
            Trim(a.wclass);
            Trim(a.wtitle);
        }
        wcsncpy(a.path, bar + 1, MAX_PATH - 1);
        a.path[MAX_PATH - 1] = 0;
        Trim(a.path);
        if (!a.name[0] || !a.path[0])
            continue;
        a.exists = Exists(a.path);
        LoadAppIcon(a);
        s_appCount++;
    }
    free(buf);
}

// Writes s_apps back to apps.txt, through a temporary file so a power cut can't leave half a list.
static bool SaveApps()
{
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%sapps.txt", s_dir);
    _snwprintf(tmp, MAX_PATH, L"%sapps.tmp", s_dir);
    HANDLE h = CreateFile(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD w;
    bool ok = WriteFile(h, APPS_HEADER, sizeof(APPS_HEADER) - 1, &w, NULL) != 0;
    for (int i = 0; i < s_appCount && ok; i++) {
        const App& a = s_apps[i];
        wchar_t win[170], line[1000];
        if (a.wtitle[0])
            _snwprintf(win, 170, L"!%s!%s", a.wclass, a.wtitle);
        else
            wcscpy(win, a.wclass);
        if (a.iconPath[0])
            _snwprintf(line, 1000, L"%s | %s | %s | %s | %s\r\n", a.name, a.path, win, a.params, a.iconPath);
        else if (a.params[0])
            _snwprintf(line, 1000, L"%s | %s | %s | %s\r\n", a.name, a.path, win, a.params);
        else if (win[0])
            _snwprintf(line, 1000, L"%s | %s | %s\r\n", a.name, a.path, win);
        else
            _snwprintf(line, 1000, L"%s | %s\r\n", a.name, a.path);
        line[999] = 0;
        char out[3000];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, out, sizeof(out), NULL, NULL);
        ok = n > 1 && WriteFile(h, out, n - 1, &w, NULL);
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFile(tmp);
        return false;
    }
    DeleteFile(path);
    return MoveFile(tmp, path) != 0;
}

static bool Alive(HANDLE proc)
{
    return proc && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
}

// The process of a program started from here that still runs, 0 if none. Finished ones are forgotten.
static DWORD RunningPid(const wchar_t* path)
{
    DWORD pid = 0;
    for (int k = 0; k < kMaxRunning; k++) {
        Running& r = s_running[k];
        if (r.proc && !Alive(r.proc)) {
            CloseHandle(r.proc);
            r.proc = NULL;
        }
        if (r.proc && !_wcsicmp(r.path, path))
            pid = r.pid;
    }
    return pid;
}

static void Remember(const wchar_t* path, HANDLE proc, DWORD pid)
{
    int k = 0;
    while (k < kMaxRunning - 1 && s_running[k].proc)
        k++;
    if (s_running[k].proc)   // all slots busy: the last one is forgotten
        CloseHandle(s_running[k].proc);
    wcscpy(s_running[k].path, path);
    s_running[k].proc = proc;
    s_running[k].pid = pid;
}

struct FindApp { HWND found; };
static BOOL CALLBACK EnumApp(HWND h, LPARAM lp);

static void RunApp(int i)
{
    const App& a = s_apps[i];
    // Already running (started from here, or its window "class!title" is open): bring it to the front.
    s_waitPid = RunningPid(a.path);
    s_waitApp = i;
    FindApp f = { NULL };
    if (!s_waitPid && (a.wclass[0] || a.wtitle[0]))
        EnumWindows(EnumApp, (LPARAM)&f);
    if (s_waitPid || f.found) {
        s_waitTick = GetTickCount();
        SetTimer(s_wnd, TIMER_WAIT, 300, NULL);
        _snwprintf(s_status, 96, L"уже запущена: %s", a.name);
        return;
    }
    s_waitApp = -1;
    PROCESS_INFORMATION pi;
#ifdef UNDER_CE
    LPSTARTUPINFO si = NULL;          // must be NULL on Windows CE
    wchar_t cmd[200];                 // CE passes just the arguments
    wcscpy(cmd, a.params);
    wchar_t* args = cmd[0] ? cmd : NULL;
#else
    STARTUPINFO sib;
    memset(&sib, 0, sizeof(sib));
    sib.cb = sizeof(sib);
    LPSTARTUPINFO si = &sib;          // desktop Windows requires it
    wchar_t cmd[MAX_PATH + 204];      // desktop: the program name comes first
    _snwprintf(cmd, MAX_PATH + 204, L"\"%s\" %s", a.path, a.params);
    wchar_t* args = cmd;
#endif
    if (a.exists && CreateProcess(a.path, args, NULL, NULL, FALSE, 0, NULL, NULL, si, &pi)) {
        CloseHandle(pi.hThread);
        Remember(a.path, pi.hProcess, pi.dwProcessId);
        s_waitPid = pi.dwProcessId;
        s_waitApp = i;
        s_waitTick = GetTickCount();
        SetTimer(s_wnd, TIMER_WAIT, 300, NULL);
        _snwprintf(s_status, 96, L"запускается: %s", a.name);
    } else {
        _snwprintf(s_status, 96, L"не запускается: %s", a.name);
    }
}

// Window of the program being started: by class/title from apps.txt, otherwise any visible
// top-level window of its process.

static BOOL CALLBACK EnumApp(HWND h, LPARAM lp)
{
    if (h == s_wnd || !IsWindowVisible(h) || GetParent(h))
        return TRUE;
    const App& a = s_apps[s_waitApp];
    bool match;
    if (a.wclass[0] || a.wtitle[0]) {
        wchar_t cls[64] = L"", title[96] = L"";
        GetClassName(h, cls, 64);
        GetWindowText(h, title, 96);
        match = (!a.wclass[0] || !wcscmp(cls, a.wclass)) && (!a.wtitle[0] || !wcscmp(title, a.wtitle));
    } else {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        match = pid == s_waitPid;
    }
    if (match) {
        ((FindApp*)lp)->found = h;
        return FALSE;
    }
    return TRUE;
}

static bool s_guardOwned;   // s_guard was opened here (otherwise it is s_running's handle, not ours to close)

static void StopGuard()
{
    KillTimer(s_wnd, TIMER_GUARD);
    if (s_guard && s_guardOwned)
        CloseHandle(s_guard);
    s_guard = NULL;
}

static void StartGuard(DWORD pid)
{
    StopGuard();
#ifdef UNDER_CE
    s_guard = OpenProcess(0, FALSE, pid);
#else
    s_guard = OpenProcess(SYNCHRONIZE, FALSE, pid);
#endif
    s_guardOwned = s_guard != NULL;
    for (int k = 0; !s_guard && k < kMaxRunning; k++)   // no OpenProcess: the handle we started it with
        if (s_running[k].proc && s_running[k].pid == pid)
            s_guard = s_running[k].proc;
    s_guardPid = pid;
    s_guardSeen = GetTickCount();
    if (s_guard)
        SetTimer(s_wnd, TIMER_GUARD, kGuardMs, NULL);
}

static BOOL CALLBACK EnumGuardShown(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == s_guardPid && IsWindowVisible(h)) {
        *(bool*)lp = true;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK EnumGuardTop(HWND h, LPARAM lp)   // the first (topmost) visible window of the program
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == s_guardPid && IsWindowVisible(h)) {
        *(HWND*)lp = h;
        return FALSE;
    }
    return TRUE;
}

struct Guarded { HWND h[16]; int n; };

static BOOL CALLBACK EnumGuard(HWND h, LPARAM lp)
{
    Guarded* g = (Guarded*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == s_guardPid && IsWindowVisible(h) && !(GetWindowLong(h, GWL_EXSTYLE) & WS_EX_TOPMOST) && g->n < 16)
        g->h[g->n++] = h;
    return TRUE;
}

// Every 0.5 s while a raised program runs: its windows that are not topmost (a new dialog) are raised,
// bottom one first so that their order stays. Not while the shell we were opened over, VestaHome or we are
// in front (the user went back).
static void ShowFront();
static void DropRaised();
static void Minimize();
static void Log(const wchar_t* fmt, ...);

static void OnGuardTimer()
{
    if (IsWindowVisible(s_wnd)) {
        StopGuard();
        return;
    }
    if (!Alive(s_guard)) {
        // The program has closed. For the shell the navigation (us) is still on, its navigation screen is
        // black: come back like AppLauncher does, otherwise the unit is left on that black screen.
        // (When the shell ends the navigation itself, Exit_Navi has already stopped this guard.)
        StopGuard();
        DropRaised();
        HideBackdrop();
        Log(L"program closed: back to the launcher");
        ShowFront();
        return;
    }
    bool shown = false;
    EnumWindows(EnumGuardShown, (LPARAM)&shown);
    if (shown) {
        s_guardSeen = GetTickCount();
    } else if (GetTickCount() - s_guardSeen > kBlackMs) {
        Log(L"program (pid %u) without a window for %u ms (minimized?): back to the shell", (unsigned)s_guardPid,
            (unsigned)kBlackMs);
        Minimize();
        return;
    }
    HWND front = GetForegroundWindow();
    if (front && (front == s_under || front == FindWindow(HOME_CLASS, NULL)))
        return;   // the user went back to the shell / VestaHome
    if (s_reverse)
        return;   // the camera is up: nothing is raised over it
    Guarded g;
    g.n = 0;
    EnumWindows(EnumGuard, (LPARAM)&g);   // top to bottom
    for (int k = g.n - 1; k >= 0; k--) {
        SetWindowPos(g.h[k], HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (s_raisedCount < kMaxRaised)
            s_raised[s_raisedCount++] = g.h[k];
    }
}

// VestaHome keeps itself in front and its "Minimize" sends it under explorer: it gets neither the backdrop nor
// the guard (with them the black backdrop stayed over explorer, 2026-10-16). The launcher just hides.
static bool IsHome(HWND h)
{
    wchar_t cls[32] = L"";
    GetClassName(h, cls, 32);
    return !wcscmp(cls, HOME_CLASS);
}

static void FrontHome(HWND h)
{
    StopGuard();
    HideBackdrop();
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
}

static void OnWaitTimer()
{
    if (s_waitApp < 0) {
        KillTimer(s_wnd, TIMER_WAIT);
        return;
    }
    FindApp f = { NULL };
    EnumWindows(EnumApp, (LPARAM)&f);
    if (f.found && IsHome(f.found)) {
        KillTimer(s_wnd, TIMER_WAIT);
        FrontHome(f.found);
        Log(L"program in front: %s (VestaHome: no guard)", s_apps[s_waitApp].name);
        s_status[0] = 0;
        s_waitApp = -1;
        ShowWindow(s_wnd, SW_HIDE);
    } else if (f.found) {
        KillTimer(s_wnd, TIMER_WAIT);
        SetWindowPos(f.found, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        PutBackdropUnder(f.found);   // a tap beside a smaller window must not reach the shell
        SetForegroundWindow(f.found);
        if (s_raisedCount < kMaxRaised)
            s_raised[s_raisedCount++] = f.found;
        DWORD pid = 0;
        GetWindowThreadProcessId(f.found, &pid);
        StartGuard(pid);
        Log(L"program in front: %s (pid %u), guard %s", s_apps[s_waitApp].name, (unsigned)pid, s_guard ? L"on" : L"OFF");
        s_status[0] = 0;
        s_waitApp = -1;
        ShowWindow(s_wnd, SW_HIDE);   // the program is on top now; we come back when it closes (guard)
    } else if (GetTickCount() - s_waitTick > kWaitMs) {
        KillTimer(s_wnd, TIMER_WAIT);
        _snwprintf(s_status, 96, L"окно не найдено: %s", s_apps[s_waitApp].name);
        s_waitApp = -1;
    }
}

// ---------------------------------------------------------------- task manager

// "Tasks" in the dock: processes and windows (VestaCore vtask), the CPU and memory graphs over the last
// 2 minutes; the same screen as VestaHome's. Sampled every 2 s while the screen is in front, never in the
// background. A process ends on the second press of its button; system ones and the launcher itself can't.

enum { kTaskRows = 5 };
static TaskInfo s_task;
static int      s_taskTab;       // 0 processes, 1 windows
static int      s_taskPage;
static DWORD    s_taskArm;       // the process whose "End" was pressed once
static int      s_taskTicks;

static void TasksOpen()
{
    if (!s_task.procCount && !s_task.histCount) {   // the first time: the busiest first
        s_task.sortKey = TS_CPU;
        s_task.sortDesc = true;
    }
    s_taskPage = 0;
    s_taskArm = 0;
    TaskSample(&s_task, s_wnd);
}

static int TaskPages()
{
    int n = s_taskTab ? s_task.winCount : s_task.procCount;
    return n ? (n + kTaskRows - 1) / kTaskRows : 1;
}

static void MemText(wchar_t* s, int n, DWORD kb)
{
    if (kb < 1024)
        _snwprintf(s, n, L"%u КБ", (unsigned)kb);
    else
        _snwprintf(s, n, L"%u,%u МБ", (unsigned)(kb / 1024), (unsigned)(kb % 1024 * 10 / 1024));
    s[n - 1] = 0;
}

static void TaskEnd(DWORD pid)
{
    int i = 0;
    while (i < s_task.procCount && s_task.proc[i].pid != pid)
        i++;
    if (i == s_task.procCount)
        return;
    wchar_t name[40];
    wcscpy(name, s_task.proc[i].name);
    if (s_taskArm != pid) {
        s_taskArm = pid;
        _snwprintf(s_status, 96, L"ещё раз, чтобы завершить %s", name);
        return;
    }
    s_taskArm = 0;
    if (TaskKill(pid, name)) {
        _snwprintf(s_status, 96, L"завершён: %s", name);
        memmove(&s_task.proc[i], &s_task.proc[i + 1], (s_task.procCount - i - 1) * sizeof(TaskProc));
        s_task.procCount--;
    } else {
        _snwprintf(s_status, 96, L"не завершается: %s", name);
    }
}

static int TaskWinIndex(HWND h)
{
    for (int i = 0; i < s_task.winCount; i++)
        if (s_task.win[i].h == h)
            return i;
    return -1;
}

static void TaskSwitch(HWND h)
{
    int i = TaskWinIndex(h);
    if (i < 0 || !IsWindow(h)) {
        wcscpy(s_status, L"окна уже нет");
        return;
    }
    if (h == s_under) {   // the window we were opened over (the shell, VestaHome): plain "Back"
        Minimize();
        return;
    }
    s_status[0] = 0;
    if (IsHome(h)) {
        FrontHome(h);
        Log(L"task manager: switched to %s (VestaHome: no guard)", s_task.win[i].title);
        ShowWindow(s_wnd, SW_HIDE);
        return;
    }
    // as a program started from here: on top with the backdrop under it, guarded, the launcher hides
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    PutBackdropUnder(h);
    SetForegroundWindow(h);
    if (s_raisedCount < kMaxRaised)
        s_raised[s_raisedCount++] = h;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    StartGuard(pid);
    Log(L"task manager: switched to %s (pid %u)", s_task.win[i].title, (unsigned)pid);
    ShowWindow(s_wnd, SW_HIDE);
}

static void TaskClose(HWND h)
{
    int i = TaskWinIndex(h);
    if (i < 0 || !IsWindow(h)) {
        wcscpy(s_status, L"окна уже нет");
        return;
    }
    PostMessage(h, WM_CLOSE, 0, 0);
    _snwprintf(s_status, 96, L"закрывается: %s", s_task.win[i].title);
    memmove(&s_task.win[i], &s_task.win[i + 1], (s_task.winCount - i - 1) * sizeof(TaskWin));
    s_task.winCount--;
}

// A graph panel: caption, value, the history (percent, newest at the right edge), a line at 50 %.
static void TaskGraph(HDC dc, int x, int y, int w, const wchar_t* caption, const wchar_t* value,
                      const unsigned char* hist, COLORREF c)
{
    R(dc, x, y, w, 104, 14, kPanel, kLine);
    T(dc, x + 14, y + 26, caption, 15, kMuted, 700);
    T(dc, x + w - 14, y + 26, value, 15, kText, 700, AL_END, w - 120);
    int gx = x + 14, gy = y + 36, gw = w - 28, gh = 56;
    Fill(dc, gx, gy + gh / 2, gx + gw, gy + gh / 2 + 1, kLine);
    Fill(dc, gx, gy + gh, gx + gw, gy + gh + 2, kLine);
    int n = s_task.histCount;
    if (n < 2)
        return;
    POINT p[TASK_HIST];
    for (int i = 0; i < n; i++) {
        p[i].x = gx + (TASK_HIST - n + i) * gw / (TASK_HIST - 1);
        p[i].y = gy + gh - hist[i] * gh / 100;
    }
    HPEN pen = CreatePen(PS_SOLID, 2, C(c));
    HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, p, n);
    SelectObject(dc, old);
    DeleteObject(pen);
}

// A sort button; the current one is orange with a small triangle: down = the biggest first.
static void TaskSortBtn(HDC dc, int x, const wchar_t* label, int key)
{
    bool cur = s_task.sortKey == key;
    Button(dc, x, 176, 104, 48, label, cur ? B_SEL : B_N, IC_NONE, 16, 0, A_TASKSORT, key);
    if (!cur)
        return;
    int cx = x + 92, cy = 200, d = s_task.sortDesc ? 1 : -1;
    POINT t[3] = { { cx - 5, cy - 3 * d }, { cx + 5, cy - 3 * d }, { cx, cy + 3 * d } };
    Poly(dc, t, 3, kInk);
}

static void DrawTasks(HDC dc)
{
    wchar_t s[96], a[24], b[24];
    Frame(dc, L"ДИСПЕТЧЕР ЗАДАЧ", A_TASKBACK);
    if (s_task.cpu10 >= 0)
        _snwprintf(s, 96, L"%d %%", (s_task.cpu10 + 5) / 10);
    else
        wcscpy(s, L"...");
    TaskGraph(dc, 16, 64, 380, L"ПРОЦЕССОР", s, s_task.cpuHist, kOrange);
    DWORD total = s_task.memTotalKB, used = total - s_task.memAvailKB;
    MemText(a, 24, used);
    MemText(b, 24, total);
    _snwprintf(s, 96, L"%s из %s · %u %%", a, b, total ? (unsigned)(used * 100 / total) : 0);
    TaskGraph(dc, 404, 64, 380, L"ПАМЯТЬ", s, s_task.memHist, kText);

    _snwprintf(s, 96, L"ПРОЦЕССЫ %d", s_task.procCount);
    Button(dc, 16, 176, 172, 48, s, s_taskTab == 0 ? B_SEL : B_N, IC_NONE, 16, 0, A_TASKTAB, 0);
    _snwprintf(s, 96, L"ОКНА %d", s_task.winCount);
    Button(dc, 196, 176, 128, 48, s, s_taskTab == 1 ? B_SEL : B_N, IC_NONE, 16, 0, A_TASKTAB, 1);
    if (s_taskTab == 0) {
        TaskSortBtn(dc, 336, L"ИМЯ", TS_NAME);
        TaskSortBtn(dc, 446, L"ЦП", TS_CPU);
        TaskSortBtn(dc, 556, L"ПАМЯТЬ", TS_MEM);
    }
    int pages = TaskPages();
    if (s_taskPage >= pages)
        s_taskPage = pages - 1;
    Button(dc, 668, 176, 40, 48, NULL, B_N, IC_BACK, 18, 24, A_TASKPAGE, -1);
    _snwprintf(s, 96, L"%d/%d", s_taskPage + 1, pages);
    T(dc, 726, 207, s, 15, kMuted, 600, AL_MID);
    Button(dc, 744, 176, 40, 48, NULL, B_N, IC_NEXT, 18, 24, A_TASKPAGE, 1);

    if (s_taskTab == 0 && !s_task.procOk) {
        T(dc, 400, 350, L"Список процессов недоступен: нет toolhelp.dll", 18, kMuted, 600, AL_MID, 760);
        return;
    }
    for (int k = 0; k < kTaskRows; k++) {
        int i = s_taskPage * kTaskRows + k, y = 232 + k * 48;
        if (s_taskTab == 0) {
            if (i >= s_task.procCount)
                break;
            const TaskProc& p = s_task.proc[i];
            bool self = p.pid == GetCurrentProcessId(), sys = TaskIsSystem(p.name);
            R(dc, 16, y, 768, 44, 10, kPanel);
            T(dc, 32, y + 29, p.name, 18, self ? kOrange : kText, 700, AL_START, 250);
            _snwprintf(s, 96, L"%d пот.", p.threads);
            T(dc, 296, y + 28, s, 15, kMuted, 400);
            if (p.cpu10 >= 0)
                _snwprintf(s, 96, L"%d,%d %%", p.cpu10 / 10, p.cpu10 % 10);
            else
                wcscpy(s, L"...");
            T(dc, 470, y + 29, s, 17, p.cpu10 >= 200 ? kOrange : kText, 700, AL_END);
            if (p.memKB)
                MemText(s, 96, p.memKB);
            else
                wcscpy(s, L"-");
            T(dc, 604, y + 29, s, 17, kText, 700, AL_END);
            if (sys || self)
                T(dc, 700, y + 28, sys ? L"система" : L"эта программа", 14, kDim, 600, AL_MID);
            else
                Button(dc, 624, y + 4, 152, 36, s_taskArm == p.pid ? L"ТОЧНО?" : L"ЗАВЕРШИТЬ",
                       s_taskArm == p.pid ? B_PRI : B_N, IC_NONE, 15, 0, A_TASKKILL, (int)p.pid);
        } else {
            if (i >= s_task.winCount)
                break;
            const TaskWin& w = s_task.win[i];
            R(dc, 16, y, 768, 44, 10, kPanel);
            T(dc, 32, y + 29, w.title, 18, kText, 700, AL_START, 300);
            T(dc, 346, y + 28, w.proc, 15, kMuted, 400, AL_START, 190);
            Button(dc, 544, y + 4, 112, 36, L"ПЕРЕЙТИ", B_N, IC_NONE, 15, 0, A_TASKSWITCH, (int)(INT_PTR)w.h);
            Button(dc, 664, y + 4, 112, 36, L"ЗАКРЫТЬ", B_N, IC_NONE, 15, 0, A_TASKCLOSE, (int)(INT_PTR)w.h);
        }
    }
    if (s_taskTab == 1 && !s_task.winCount)
        T(dc, 400, 350, L"Других окон нет", 18, kMuted, 600, AL_MID);
}

// ---------------------------------------------------------------- shortcut editor

static void OpenEditor(int i)
{
    s_editIdx = i;
    if (i >= 0)
        s_edit = s_apps[i];
    else
        memset(&s_edit, 0, sizeof(s_edit));
    s_edit.icon = NULL;   // the icons stay owned by s_apps
    s_edit.img.px = NULL;
    s_delArmed = false;
    s_status[0] = 0;
    s_screen = S_EDIT;
}

// Writes the list, reads it back (icons, "no file" marks) and returns to the apps screen.
static void FinishEdit()
{
    bool ok = SaveApps();
    LoadApps();
    s_screen = S_APPS;
    if (s_page >= AppsPages())
        s_page = AppsPages() - 1;
    if (ok)
        s_status[0] = 0;
    else
        wcscpy(s_status, L"не удалось записать apps.txt");
}

static void SaveEdit()
{
    if (!s_edit.name[0] || !s_edit.path[0]) {
        wcscpy(s_status, L"нужны название и программа");
        return;
    }
    if (s_editIdx < 0) {
        s_apps[s_appCount++] = s_edit;
    } else {
        HICON icon = s_apps[s_editIdx].icon;   // freed by the reload in FinishEdit
        Img img = s_apps[s_editIdx].img;
        s_apps[s_editIdx] = s_edit;
        s_apps[s_editIdx].icon = icon;
        s_apps[s_editIdx].img = img;
    }
    FinishEdit();
}

static void DeleteEdit()
{
    FreeAppIcon(s_apps[s_editIdx]);
    memmove(&s_apps[s_editIdx], &s_apps[s_editIdx + 1], (s_appCount - s_editIdx - 1) * sizeof(App));
    s_appCount--;
    s_waitApp = -1;   // indices moved: stop waiting for a window of a started program
    s_delArmed = false;
    FinishEdit();
}

static void OpenKeys(int field)
{
    s_keysField = field;
    wcscpy(s_keysText, field == F_NAME ? s_edit.name : s_edit.params);
    s_keysShift = false;
    s_clipMenu = false;
    s_screen = S_KEYS;
}

static void KeyType(wchar_t ch)
{
    int n = (int)wcslen(s_keysText);
    if (n >= (s_keysField == F_NAME ? 47 : 199))
        return;
    if (s_keysShift) {   // one-shot, like a phone keyboard
        CharUpperBuff(&ch, 1);
        s_keysShift = false;
    }
    s_keysText[n] = ch;
    s_keysText[n + 1] = 0;
}

static void KeyBack()
{
    int n = (int)wcslen(s_keysText);
    if (n > 0)
        s_keysText[n - 1] = 0;
}

// ---- the clipboard on the keyboard screen: a long press on the text field (TIMER_LONG) shows "Copy",
// "Paste" and "Clear" in its place. The text is copied whole; pasting adds to its end (the cursor is always there),
// without line breaks and tabs, up to the field's length.

static void ClipCopy()
{
    int n = (int)wcslen(s_keysText) + 1;
    bool ok = false;
    if (OpenClipboard(s_wnd)) {
        EmptyClipboard();
#ifdef UNDER_CE
        HANDLE h = LocalAlloc(LMEM_FIXED, n * sizeof(wchar_t));   // CE: the clipboard takes local memory as is
        if (h)
            memcpy(h, s_keysText, n * sizeof(wchar_t));
#else
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
        if (h) {
            memcpy(GlobalLock(h), s_keysText, n * sizeof(wchar_t));
            GlobalUnlock(h);
        }
#endif
        ok = h && SetClipboardData(CF_UNICODETEXT, h);
        CloseClipboard();
    }
    wcscpy(s_status, ok ? L"скопировано" : L"не скопировано");
}

static void ClipPaste()
{
    int n = (int)wcslen(s_keysText), cap = (s_keysField == F_NAME ? 47 : 199);
    bool got = false;
    if (OpenClipboard(s_wnd)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
#ifdef UNDER_CE
        const wchar_t* p = (const wchar_t*)h;
#else
        const wchar_t* p = h ? (const wchar_t*)GlobalLock(h) : NULL;
#endif
        for (; p && *p && n < cap; p++)
            if (*p >= L' ') {
                s_keysText[n++] = *p;
                got = true;
            }
        s_keysText[n] = 0;
#ifndef UNDER_CE
        if (h)
            GlobalUnlock(h);
#endif
        CloseClipboard();
    }
    wcscpy(s_status, got ? L"вставлено" : L"в буфере нет текста");
}

static void KeysDone()
{
    wchar_t* dst = s_keysField == F_NAME ? s_edit.name : s_edit.params;
    wcscpy(dst, s_keysText);
    Trim(dst);
    s_screen = S_EDIT;
}

static int CmpFiles(const void* pa, const void* pb)
{
    const FileEnt* a = (const FileEnt*)pa;
    const FileEnt* b = (const FileEnt*)pb;
    if (a->dir != b->dir)
        return a->dir ? -1 : 1;
    return _wcsicmp(a->name, b->name);
}

static bool HasExt(const wchar_t* name, const wchar_t* ext)
{
    int n = (int)wcslen(name);
    return n > 4 && !_wcsicmp(name + n - 4, ext);
}

// Folders and .exe files of s_fdir (for an icon: .png, .ico, .exe, .dll), folders first.
static void ListDir()
{
    s_fileCount = 0;
    s_filePage = 0;
    wchar_t mask[MAX_PATH];
    _snwprintf(mask, MAX_PATH, L"%s*", s_fdir);
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(mask, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int n = (int)wcslen(fd.cFileName);
        if (n >= 100 || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (!dir && !HasExt(fd.cFileName, L".exe") && (s_filesFor != F_ICON || (!HasExt(fd.cFileName, L".png") &&
            !HasExt(fd.cFileName, L".ico") && !HasExt(fd.cFileName, L".dll"))))
            continue;
        s_files[s_fileCount].dir = dir;
        wcscpy(s_files[s_fileCount].name, fd.cFileName);
        s_fileCount++;
    } while (s_fileCount < kMaxFiles && FindNextFile(h, &fd));
    FindClose(h);
    qsort(s_files, s_fileCount, sizeof(FileEnt), CmpFiles);
}

// Points the browser at the folder of file, if it has one that exists.
static bool BrowseFolderOf(const wchar_t* file)
{
    const wchar_t* slash = wcsrchr(file, L'\\');
    if (!slash || slash == file)
        return false;
    wchar_t dir[MAX_PATH];
    int n = (int)(slash - file);
    wcsncpy(dir, file, n);
    dir[n] = 0;
    DWORD attr = GetFileAttributes(dir);
    if (attr == 0xFFFFFFFF || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    wcscpy(s_fdir, dir);
    wcscat(s_fdir, L"\\");
    return true;
}

// For the program the browser opens in its folder, otherwise at the root. For the icon: in the current
// icon's folder, otherwise our Icons\ folder, otherwise the program's folder.
static void OpenFiles(int field)
{
    s_filesFor = field;
    wcscpy(s_fdir, L"\\");
    wchar_t icons[MAX_PATH];
    _snwprintf(icons, MAX_PATH, L"%sIcons\\", s_dir);
    if (field == F_ICON) {
        wchar_t cur[MAX_PATH];
        if (s_edit.iconPath[0] == L'\\' || wcschr(s_edit.iconPath, L':') || !s_edit.iconPath[0])
            wcscpy(cur, s_edit.iconPath);
        else
            _snwprintf(cur, MAX_PATH, L"%s%s", s_dir, s_edit.iconPath);
        if (!(cur[0] && BrowseFolderOf(cur)) && !BrowseFolderOf(icons))
            BrowseFolderOf(s_edit.path);
    } else {
        BrowseFolderOf(s_edit.path);
    }
    ListDir();
    s_screen = S_FILES;
}

static void UpDir()
{
    int n = (int)wcslen(s_fdir);
    if (n <= 1)
        return;   // already at the root
    s_fdir[n - 1] = 0;
    wchar_t* slash = wcsrchr(s_fdir, L'\\');
    if (slash)
        slash[1] = 0;
    else
        wcscpy(s_fdir, L"\\");
    ListDir();
}

// A folder opens; an exe becomes the program, and names the shortcut if it has no name yet.
static void PickFile(int i)
{
    const FileEnt& f = s_files[i];
    if (wcslen(s_fdir) + wcslen(f.name) + 2 >= MAX_PATH)
        return;
    if (f.dir) {
        wcscat(s_fdir, f.name);
        wcscat(s_fdir, L"\\");
        ListDir();
        return;
    }
    if (s_filesFor == F_ICON) {   // inside our folder kept relative: survives a move of the card's folders
        int n = (int)wcslen(s_dir);
        if (!_wcsnicmp(s_fdir, s_dir, n))
            _snwprintf(s_edit.iconPath, MAX_PATH, L"%s%s", s_fdir + n, f.name);
        else
            _snwprintf(s_edit.iconPath, MAX_PATH, L"%s%s", s_fdir, f.name);
        s_edit.iconPath[MAX_PATH - 1] = 0;
        s_screen = S_EDIT;
        return;
    }
    wcscpy(s_edit.path, s_fdir);
    wcscat(s_edit.path, f.name);
    if (!s_edit.name[0]) {
        wcsncpy(s_edit.name, f.name, 47);
        s_edit.name[47] = 0;
        int n = (int)wcslen(s_edit.name);
        if (n > 4 && !_wcsicmp(s_edit.name + n - 4, L".exe"))
            s_edit.name[n - 4] = 0;
    }
    s_screen = S_EDIT;
}

// The stock shell keeps running underneath: bring its window back and step aside.
// Its window is the biggest visible top-level window that is not ours.
struct FindShell { HWND best; int area; };

static BOOL CALLBACK EnumShell(HWND h, LPARAM lp)
{
    FindShell* f = (FindShell*)lp;
    if (h == s_wnd || !IsWindowVisible(h))
        return TRUE;
    for (int i = 0; i < s_raisedCount; i++)
        if (h == s_raised[i])
            return TRUE;
#ifdef UNDER_CE
    if (h == s_taskbar)
        return TRUE;
#endif
    RECT rc;
    GetWindowRect(h, &rc);
    int area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > f->area) {
        f->area = area;
        f->best = h;
    }
    return TRUE;
}

static void ShowShell()
{
    // programs we raised would stay above the shell: put them back into the normal z-order
    for (int i = 0; i < s_raisedCount; i++)
        if (IsWindow(s_raised[i]))
            SetWindowPos(s_raised[i], HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    FindShell f = { NULL, 0 };
    EnumWindows(EnumShell, (LPARAM)&f);
    s_raisedCount = 0;
    if (f.best && f.area >= 640 * 400) {
        ShowWindow(s_wnd, SW_HIDE);
        SetForegroundWindow(f.best);
        s_status[0] = 0;
    } else {
        wcscpy(s_status, L"оболочка не найдена");
    }
}

// Programs we raised stay above everything: put them back into the normal z-order.
static void DropRaised()
{
    for (int i = 0; i < s_raisedCount; i++)
        if (IsWindow(s_raised[i]))
            SetWindowPos(s_raised[i], HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    s_raisedCount = 0;
}

// Where the log goes. On the unit only removable media - the SD card or a USB disk, folder VestaLogs - never
// the built-in flash (\SDMMC, \ResidentFlash, \StorageFlash wear out); without a card or a disk there is no
// log. Looked up again every 30 s: a card can be put in later. PC: next to the exe.
static bool LogDir(wchar_t* dir)
{
#ifdef UNDER_CE
    static wchar_t s_logDir[MAX_PATH];
    static DWORD   s_logDirTick;
    static bool    s_logDirOk, s_logDirTried;
    if (!s_logDirTried || GetTickCount() - s_logDirTick > 30000) {
        static const wchar_t* vols[4] = { L"\\SDMemory", L"\\USB Disk", L"\\USBDisk", L"\\USBDevice" };
        s_logDirTried = true;
        s_logDirTick = GetTickCount();
        s_logDirOk = false;
        for (int i = 0; i < 4 && !s_logDirOk; i++) {
            DWORD a = GetFileAttributes(vols[i]);
            if (a == 0xFFFFFFFF || !(a & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            _snwprintf(s_logDir, MAX_PATH, L"%s\\VestaLogs\\", vols[i]);
            CreateDirectory(s_logDir, NULL);
            DWORD b = GetFileAttributes(s_logDir);
            s_logDirOk = b != 0xFFFFFFFF && (b & FILE_ATTRIBUTE_DIRECTORY);
        }
    }
    if (s_logDirOk)
        wcscpy(dir, s_logDir);
    return s_logDirOk;
#else
    wcscpy(dir, s_dir);
    return true;
#endif
}

// A short log (shows on the unit who covered us), on removable media only; cut when it grows over 32 KB.
static void Log(const wchar_t* fmt, ...)
{
    wchar_t path[MAX_PATH], line[200];
    if (!LogDir(path))
        return;
    wcscat(path, L"VestaLaunch.log");
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(line, 200, fmt, a);
    va_end(a);
    line[199] = 0;
    HANDLE h = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (GetFileSize(h, NULL) > 32 * 1024)
        SetEndOfFile(h);   // at offset 0: start over
    SetFilePointer(h, 0, NULL, FILE_END);
    char buf[640];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, buf, sizeof(buf) - 2, NULL, NULL);
    if (n > 0) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[16];
        _snprintf(stamp, 16, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
        DWORD w;
        WriteFile(h, stamp, (DWORD)strlen(stamp), &w, NULL);
        WriteFile(h, buf, (DWORD)(n - 1), &w, NULL);
        WriteFile(h, "\r\n", 2, &w, NULL);
    }
    CloseHandle(h);
}

static const wchar_t* ClassOf(HWND h)
{
    static wchar_t cls[64];
    cls[0] = 0;
    if (h)
        GetClassName(h, cls, 64);
    else
        wcscpy(cls, L"(none)");
    return cls;
}

// The stock shell (G3NavHMI, CExternNaviMgr) starts the first program of ExternNavi.cfg as its "external
// navigation" and keeps track of it only through named events the program sets (MonitorEventThread):
// "Nav_Starting" / "Nav_Started" = it runs: the shell closes its "Starting..." dialog and shows its navigation
// screen; "Nav_Terminating" / "Nav_Terminated" = it has gone: the shell returns to its own screens and will
// start the program again on the next "Navigation" press. Without them the shell stays in "Starting..." for
// good and never starts the program again (it believes it is still starting) - what happened on the unit.
// So: shown = started, hidden ("Back") = terminated; the next press starts a second copy of us, which hands
// over to this one (WM_VESTA_SHOW). The shell pulses "Exit_Navi" when it ends the navigation itself.
static void NavEvent(const wchar_t* name)
{
    HANDLE h = CreateEvent(NULL, FALSE, FALSE, name);   // the shell's event if it exists, otherwise a new one
    if (h) {
        SetEvent(h);
        CloseHandle(h);
    }
}

static bool s_navOn;   // the shell believes the navigation (we, or a program started from here) is running

static void NavStarted()
{
    NavEvent(L"Nav_Starting");
    Sleep(50);   // the shell takes them one by one, in its own thread
    NavEvent(L"Nav_Started");
    s_navOn = true;
}

static void NavTerminated()
{
    NavEvent(L"Nav_Terminating");
    Sleep(50);
    NavEvent(L"Nav_Terminated");
    s_navOn = false;
}

static DWORD WINAPI ExitNaviThread(LPVOID)
{
    HANDLE h = CreateEvent(NULL, FALSE, FALSE, L"Exit_Navi");
    if (!h)
        return 0;
    for (;;)
        if (WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0 && s_wnd)
            PostMessage(s_wnd, WM_EXIT_NAVI, 0, 0);
}

// To the front, asked by a second start: remember what we cover, raise, and keep in front for kFrontMs
// (whoever started the second copy may raise itself again when it sees that copy end).
static void ShowFront()
{
    HWND front = GetForegroundWindow();
    if (front && front != s_wnd)
        s_under = front;
    StopGuard();
    HideBackdrop();
    ShowWindow(s_wnd, SW_SHOW);
#ifdef UNDER_CE
    SetWindowPos(s_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
#endif
    SetForegroundWindow(s_wnd);
    s_frontTick = GetTickCount();
    SetTimer(s_wnd, TIMER_FRONT, 250, NULL);
    NavStarted();
    Log(L"show: over %s, front now %s; Nav_Started set", ClassOf(s_under), ClassOf(GetForegroundWindow()));
}

static void OnFrontTimer()
{
    HWND front = GetForegroundWindow();
    if (front != s_wnd && IsWindowVisible(s_wnd) && !s_reverse) {
        Log(L"covered by %s: raise again", ClassOf(front));
#ifdef UNDER_CE
        SetWindowPos(s_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
#endif
        SetForegroundWindow(s_wnd);
    }
    if (GetTickCount() - s_frontTick > kFrontMs || !IsWindowVisible(s_wnd))
        KillTimer(s_wnd, TIMER_FRONT);
}

// "Back": we hide and the window we were opened over comes back - the stock shell, VestaHome, any other;
// without one the biggest window (ShowShell). The next start shows us again.
static void Minimize()
{
    s_screen = S_APPS;
    s_page = 0;
    s_editMode = false;
    s_status[0] = 0;
    StopGuard();
    HideBackdrop();
    KillTimer(s_wnd, TIMER_FRONT);
    if (s_under && IsWindow(s_under) && IsWindowVisible(s_under) && s_under != s_wnd) {
        DropRaised();
        ShowWindow(s_wnd, SW_HIDE);
        SetForegroundWindow(s_under);
    } else {
        ShowShell();
    }
    NavTerminated();
    Log(L"back: hidden, front now %s; Nav_Terminated set", ClassOf(GetForegroundWindow()));
}

// The shell's empty navigation screen in front. While the navigation is "on" for the shell, every time the
// shell shows its external-navigation dialog (0x32B, CExternNaviDlg) - after the rear camera, on "Navigation"
// pressed again, on a tap on its screen behind a smaller program window - it only tries to show the
// navigator's window, and this firmware never gives it one (SetExternMainWindowHWND is never called, so
// ExtShowNavi does nothing): the unit stays on a black screen until a reboot. AppLauncher watches the screen
// for it (its "RadioBlackScreen" check); so do we. That dialog's background is exactly RGB(1,1,0) (set in its
// constructor) and the display is full colour, so the colour is a mark: nothing else - neither a dark camera
// picture nor a black window - paints 6 of 8 sample points in it. Seen twice in a row (2 s): the program
// started from here comes back to the front, without one - the launcher.
static int   s_shellNavHits;
static DWORD s_shellNavLogTick;

// ---- the backdrop: a black window of ours right under a program that does not fill the screen. Without it
// a tap beside the program lands on the shell's (empty) navigation screen, the shell comes up over the program
// and stays there. A tap on the backdrop brings the program back on top.

static void HideBackdrop()
{
    if (s_backWnd && IsWindowVisible(s_backWnd))
        ShowWindow(s_backWnd, SW_HIDE);
}

// The backdrop topmost, then the program's window topmost over it.
static void PutBackdropUnder(HWND top)
{
    if (!s_backWnd || s_reverse)
        return;
    SetWindowPos(s_backWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

// The program started from here back on top (with the backdrop under it); false = there is none.
static bool RaiseProgram()
{
    if (!s_guard || !Alive(s_guard))
        return false;
    HWND top = NULL;
    EnumWindows(EnumGuardTop, (LPARAM)&top);   // its topmost visible window (they are listed top first)
    if (!top)
        return false;
    PutBackdropUnder(top);
    SetForegroundWindow(top);
    return true;
}

// The navigation (we) back in front after the shell came up over it: the program, without one the launcher.
static void BringBack(const wchar_t* why)
{
    if (s_reverse)
        return;
    if (RaiseProgram()) {
        Log(L"%s: program (pid %u) back in front", why, (unsigned)s_guardPid);
        return;
    }
    Log(L"%s: launcher back in front", why);
    ShowFront();
}

static LRESULT CALLBACK BackProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP:   // a tap beside the program: it comes back on top; none to raise - back to the shell
        if (!RaiseProgram()) {
            Log(L"backdrop tapped, no program window: back to the shell");
            Minimize();
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---- the reverse gear: desay_sdk_api.dll (the unit's ROM, \Windows) SV_VehicleGetReverseStatus(DWORD*) reads
// GPIO 0x5d; no notifications ("not support"), so it is polled twice a second. The value at start is taken as
// "not in reverse" (the polarity is not known). While in reverse nothing of ours is raised (the camera is up);
// a second after it ends, the navigation comes back in front - the shell leaves its empty navigation screen
// behind after the camera.
enum { TIMER_REVERSE = 5, kReverseMs = 500, kReverseBackMs = 1000 };
typedef DWORD (*ReverseFn)(DWORD*);
static ReverseFn s_reverseFn;
static int   s_reverseBase = -1;  // GPIO value at start = not in reverse
static DWORD s_reverseOffTick;
static bool  s_reverseErrLogged;

static void InitReverse()
{
#ifdef UNDER_CE
    HMODULE lib = LoadLibrary(L"desay_sdk_api.dll");
    if (lib)
        s_reverseFn = (ReverseFn)GetProcAddress(lib, L"SV_VehicleGetReverseStatus");
#endif
    Log(L"reverse: %s", s_reverseFn ? L"SV_VehicleGetReverseStatus polled" : L"not available");
}

static void PollReverse()
{
    if (!s_reverseFn)
        return;
    DWORD v = 0;
    if (s_reverseFn(&v) != 0) {
        if (!s_reverseErrLogged)
            Log(L"reverse: SV_VehicleGetReverseStatus failed");
        s_reverseErrLogged = true;
        return;
    }
    if (s_reverseBase < 0) {
        s_reverseBase = (int)v;
        Log(L"reverse: GPIO 0x5d = %u at start, taken as \"not in reverse\"", (unsigned)v);
        return;
    }
    bool rev = v != (DWORD)s_reverseBase;
    if (rev != s_reverse) {
        s_reverse = rev;
        Log(L"reverse: %s (GPIO 0x5d = %u)", rev ? L"ON - nothing is raised" : L"off", (unsigned)v);
        if (rev) {
            HideBackdrop();   // never over the camera
            s_reverseOffTick = 0;
        } else {
            s_reverseOffTick = GetTickCount();
        }
    }
    if (!rev && s_reverseOffTick && GetTickCount() - s_reverseOffTick >= kReverseBackMs) {
        s_reverseOffTick = 0;
        if (s_navOn && GetForegroundWindow() != s_wnd)
            BringBack(L"reverse ended");
    }
}

static void CheckShellNavScreen()
{
    if (!s_navOn || s_reverse) {   // in reverse the shell shows the camera: hands off
        s_shellNavHits = 0;
        return;
    }
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front)
        GetWindowThreadProcessId(front, &pid);
    wchar_t title[32] = L"";
    if (front)
        GetWindowText(front, title, 32);
    if (!front || front == s_wnd || (s_guard && pid == s_guardPid) ||
        (wcscmp(title, L"RadioCommWindow") && front != s_under)) {
        s_shellNavHits = 0;   // we, the program, or anything but the shell in front
        return;
    }
    static const POINT pts[8] = { { 100, 140 }, { 400, 140 }, { 700, 140 }, { 100, 260 },
                                  { 400, 260 }, { 700, 260 }, { 250, 360 }, { 550, 360 } };
    COLORREF c[8];
    int marks = 0;
    HDC dc = GetDC(NULL);
    for (int i = 0; i < 8; i++) {
        c[i] = GetPixel(dc, pts[i].x, pts[i].y);
        if (c[i] == RGB(1, 1, 0))
            marks++;
    }
    ReleaseDC(NULL, dc);
    // the colours go to the log when it matches, and now and then otherwise (to check the mark on the unit)
    if (marks >= 6 || GetTickCount() - s_shellNavLogTick > 15000) {
        s_shellNavLogTick = GetTickCount();
        Log(L"shell in front (%s): %d/8 nav-screen colour; %06X %06X %06X %06X %06X %06X %06X %06X",
            ClassOf(front), marks, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    }
    if (marks < 6) {
        s_shellNavHits = 0;
        return;
    }
    if (++s_shellNavHits < 2)
        return;
    s_shellNavHits = 0;
    BringBack(L"shell's black navigation screen");
}

// Day or night as in VestaHome (asked on every clock tick; without VestaHome it stays day).
static void SyncNight()
{
    HWND home = FindWindow(HOME_CLASS, NULL);
    s_night = home && SendMessage(home, WM_VESTA_NIGHT, 0, 0) != 0;
}

// The buttons are registered while the screen is drawn; a changed screen starts with none.
static void BuildButtons()
{
    kBtnCount = 0;
}

static void DoAction(const Btn& b)
{
    if (b.action != A_DELETE)
        s_delArmed = false;
    if (b.action != A_TASKKILL)
        s_taskArm = 0;
    s_clipMenu = false;
    switch (b.action) {
    case A_BACK:     Minimize(); break;
    case A_TASKS:      TasksOpen(); s_screen = S_TASKS; s_status[0] = 0; break;
    case A_TASKBACK:   s_screen = S_APPS; s_status[0] = 0; break;
    case A_TASKTAB:    s_taskTab = b.arg; s_taskPage = 0; s_status[0] = 0; break;
    case A_TASKSORT:   TaskSort(&s_task, b.arg); s_taskPage = 0; break;
    case A_TASKPAGE:
        s_taskPage += b.arg;
        s_taskPage = s_taskPage < 0 ? 0 : s_taskPage >= TaskPages() ? TaskPages() - 1 : s_taskPage;
        break;
    case A_TASKKILL:   TaskEnd((DWORD)b.arg); break;
    case A_TASKSWITCH: TaskSwitch((HWND)(INT_PTR)b.arg); break;
    case A_TASKCLOSE:  TaskClose((HWND)(INT_PTR)b.arg); break;
    case A_CLIPCOPY:   ClipCopy(); break;
    case A_CLIPPASTE:  ClipPaste(); break;
    case A_CLIPCLEAR:  s_keysText[0] = 0; break;
    case A_RUN:      if (s_editMode) OpenEditor(b.arg); else RunApp(b.arg); break;
    case A_ADD:      OpenEditor(-1); break;
    case A_PREV:
        if (s_screen == S_FILES) {
            if (s_filePage > 0) {
                s_filePage--;
                s_slideDir = -1;
            }
        } else if (s_page > 0) {
            s_page--;
            s_slideDir = -1;
        }
        break;
    case A_NEXT:   // files: next page; programs (page dots): next page, round
        if (s_screen == S_FILES) {
            if (s_filePage + 1 < FilePages()) {
                s_filePage++;
                s_slideDir = 1;
            }
        } else if (AppsPages() > 1) {
            s_page = (s_page + 1) % AppsPages();
            s_slideDir = 1;
        }
        break;
    case A_EDITMODE: s_editMode = !s_editMode; break;
    case A_FIELD:    if (b.arg == F_PATH || b.arg == F_ICON) OpenFiles(b.arg); else OpenKeys(b.arg); break;
    case A_NOICON:   s_edit.iconPath[0] = 0; break;
    case A_SAVE:     SaveEdit(); break;
    case A_DELETE:   if (s_delArmed) DeleteEdit(); else s_delArmed = true; break;
    case A_CANCEL:   // editor -> apps, keyboard/browser -> editor
        if (s_screen == S_EDIT) {
            s_screen = S_APPS;
            s_status[0] = 0;
        } else {
            s_screen = S_EDIT;
        }
        break;
    case A_OK:       KeysDone(); break;
    case A_KEY:      KeyType((wchar_t)b.arg); break;
    case A_SHIFT:    s_keysShift = !s_keysShift; break;
    case A_LANG:     s_keysEn = !s_keysEn; break;
    case A_BKSP:     KeyBack(); break;
    case A_FILE:     PickFile(b.arg); break;
    case A_UP:       UpDir(); break;
    default:         break;
    }
    BuildButtons();
}

static int HitTest(int x, int y)
{
    for (int i = 0; i < kBtnCount; i++) {
        const RECT& r = s_btns[i].rc;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------- skin

struct SkinColor { const wchar_t* key; COLORREF* c; const char* note; };
static const SkinColor kSkinColors[] = {
    { L"ink",     &kInk,     "background" },
    { L"panel",   &kPanel,   "tiles, buttons, widgets" },
    { L"line",    &kLine,    "borders" },
    { L"text",    &kText,    "main text, built-in icons" },
    { L"muted",   &kMuted,   "secondary text" },
    { L"dim",     &kDim,     "inactive text" },
    { L"accent",  &kOrange,  "pressed button, active mode, headings" },
    { L"facet",   &kFacet,   "tile corner" },
    { L"mosaic1", &kTri[0],  "built-in background triangles (unused with a background picture)" },
    { L"mosaic2", &kTri[1],  "" },
    { L"mosaic3", &kTri[2],  "" },
};

// PNG icon into premultiplied RGBA. Up to 256 x 256: icons only, keeps memory bounded.
static bool LoadImg(const wchar_t* path, Img* im)
{
    DWORD size;
    char* data = ReadAll(path, &size);
    if (!data)
        return false;
    int comp;
    im->px = stbi_load_from_memory((unsigned char*)data, size, &im->w, &im->h, &comp, 4);
    free(data);
    if (im->px && (im->w > 256 || im->h > 256)) {
        stbi_image_free(im->px);
        im->px = NULL;
    }
    if (!im->px)
        return false;
    for (int i = 0; i < im->w * im->h; i++) {
        unsigned char* p = im->px + i * 4;
        for (int k = 0; k < 3; k++)
            p[k] = (unsigned char)(p[k] * p[3] / 255);
    }
    return true;
}

// Skin\skin.ini: [colors] name=RRGGBB, [images] name=file.png (in the Skin folder). Read once at start.
static void LoadSkin()
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%sSkin\\skin.ini", s_dir);
    DWORD size;
    char* buf = ReadAll(path, &size);
    if (!buf)
        return;
    bool images = false;
    char* p = buf;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    while (*p) {
        char* end = strchr(p, '\n');
        if (end)
            *end = 0;
        wchar_t line[300];
        int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, line, 300);
        p = end ? end + 1 : p + strlen(p);
        if (n <= 0)
            continue;
        wchar_t* semi = wcschr(line, L';');
        if (semi)
            *semi = 0;
        Trim(line);
        if (line[0] == L'[') {
            images = !_wcsicmp(line, L"[images]");
            continue;
        }
        wchar_t* eq = wcschr(line, L'=');
        if (!eq)
            continue;
        *eq = 0;
        wchar_t* val = eq + 1;
        Trim(line);
        Trim(val);
        if (!images) {
            if (*val == L'#')
                val++;
            wchar_t* e;
            unsigned long v = wcstoul(val, &e, 16);
            if (e - val != 6)
                continue;
            for (int k = 0; k < (int)(sizeof(kSkinColors) / sizeof(kSkinColors[0])); k++)
                if (!_wcsicmp(line, kSkinColors[k].key))
                    *kSkinColors[k].c = RGB(v >> 16, (v >> 8) & 255, v & 255);
            continue;
        }
        if (!val[0])
            continue;
        wchar_t file[MAX_PATH];
        _snwprintf(file, MAX_PATH, L"%sSkin\\%s", s_dir, val);
        file[MAX_PATH - 1] = 0;
        if (!_wcsicmp(line, L"background")) {
            wcscpy(s_skinBg, file);
            continue;
        }
        int len = (int)wcslen(line);
        int down = len > 5 && !_wcsicmp(line + len - 5, L"_down");
        if (down)
            line[len - 5] = 0;
        for (int ic = 0; ic < IC_COUNT; ic++)
            if (kIconKey[ic] && !_wcsicmp(line, kIconKey[ic]) && !s_icons[ic][down].px)
                LoadImg(file, &s_icons[ic][down]);
    }
    free(buf);
}

#ifndef UNDER_CE
// ---------------------------------------------------------------- skin export (PC: VestaLaunchPC.exe /export)

static unsigned long Crc32(unsigned long c, const unsigned char* p, unsigned long n)
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320UL & (0 - (c & 1)));
    }
    return ~c;
}

static void PutBE(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void PngChunk(FILE* f, const char* type, const unsigned char* data, unsigned long n)
{
    unsigned char b[4];
    PutBE(b, n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    fwrite(data, 1, n, f);
    PutBE(b, Crc32(Crc32(0, (const unsigned char*)type, 4), data, n));
    fwrite(b, 1, 4, f);
}

// Minimal PNG writer, 8-bit RGB (ch = 3) or RGBA (ch = 4), deflate "stored" blocks: no compression.
static void WritePng(const wchar_t* path, const unsigned char* px, int w, int h, int ch)
{
    unsigned long line = w * ch + 1, raw = line * h;
    unsigned char* rows = (unsigned char*)malloc(raw);
    unsigned char* z = (unsigned char*)malloc(raw + raw / 65535 * 5 + 16);
    for (int y = 0; y < h; y++) {
        rows[y * line] = 0;   // filter: none
        memcpy(rows + y * line + 1, px + y * w * ch, w * ch);
    }
    unsigned char* q = z;
    *q++ = 0x78;
    *q++ = 0x01;
    for (unsigned long i = 0; i < raw; i += 65535) {
        unsigned long n = raw - i < 65535 ? raw - i : 65535;
        *q++ = i + n == raw ? 1 : 0;
        *q++ = (unsigned char)n;
        *q++ = (unsigned char)(n >> 8);
        *q++ = (unsigned char)~n;
        *q++ = (unsigned char)(~n >> 8);
        memcpy(q, rows + i, n);
        q += n;
    }
    unsigned long a = 1, b = 0;
    for (unsigned long i = 0; i < raw; i++) {
        a = (a + rows[i]) % 65521;
        b = (b + a) % 65521;
    }
    PutBE(q, (b << 16) | a);
    q += 4;
    unsigned char hdr[13];
    PutBE(hdr, w);
    PutBE(hdr + 4, h);
    hdr[8] = 8;
    hdr[9] = ch == 4 ? 6 : 2;
    hdr[10] = hdr[11] = hdr[12] = 0;
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        PngChunk(f, "IHDR", hdr, 13);
        PngChunk(f, "IDAT", z, (unsigned long)(q - z));
        PngChunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(rows);
    free(z);
}

// Writes the built-in icons to Skin\ as <name>.png (64 x 64, drawn 4x larger white on black and averaged
// into the alpha channel: antialiased lines) and skin.ini with the built-in colours. No pressed variants and
// no background picture: they cost memory on the unit (the pressed state tints the icon dark, the background
// is the built-in mosaic). Doesn't read the existing skin.ini.
static void ExportSkin()
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%sSkin\\", s_dir);
    CreateDirectory(dir, NULL);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 800;
    bi.bmiHeader.biHeight = -480;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ ob = SelectObject(dc, bmp);
    const unsigned* px = (const unsigned*)bits;   // 0x00RRGGBB

    static unsigned char out[64 * 64 * 4];
    for (int ic = 0; ic < IC_COUNT; ic++) {
        if (!kIconKey[ic])
            continue;
        Fill(dc, 0, 0, 256, 256, RGB(0, 0, 0));
        PaintIcon(dc, (Icon)ic, 0, 0, 256, RGB(255, 255, 255));
        GdiFlush();
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                int sum = 0;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++)
                        sum += (px[(y * 4 + j) * 800 + x * 4 + i] >> 16) & 255;
                unsigned char* o = out + (y * 64 + x) * 4;
                o[0] = GetRValue(kText);
                o[1] = GetGValue(kText);
                o[2] = GetBValue(kText);
                o[3] = (unsigned char)(sum / 16);
            }
        _snwprintf(path, MAX_PATH, L"%s%s.png", dir, kIconKey[ic]);
        WritePng(path, out, 64, 64, 4);
    }
    SelectObject(dc, ob);
    DeleteDC(dc);
    DeleteObject(bmp);

    _snwprintf(path, MAX_PATH, L"%sskin.ini", dir);
    FILE* f = _wfopen(path, L"w");
    if (!f)
        return;
    fprintf(f, "; VestaLaunch skin: the Skin folder next to VestaLaunch.exe. Read once at start.\n"
               "; [colors]: RRGGBB. [images]: PNG files in this folder, transparency supported.\n"
               "; Icons are square and drawn scaled, in the colour of their place unless it is the text colour.\n"
               "; Optional: <name>_down = a separate pressed picture (otherwise the icon is tinted dark),\n"
               ";   background = an 800 x 480 picture instead of the built-in mosaic.\n"
               "; Each picture costs memory: a 64 x 64 icon 16 KB, the background 750 KB.\n"
               "; A missing line or file falls back to the built-in drawing.\n\n[colors]\n");
    for (int k = 0; k < (int)(sizeof(kSkinColors) / sizeof(kSkinColors[0])); k++) {
        COLORREF c = *kSkinColors[k].c;
        fprintf(f, "%-8S= %02X%02X%02X%s%s\n", kSkinColors[k].key, GetRValue(c), GetGValue(c), GetBValue(c),
                kSkinColors[k].note[0] ? "   ; " : "", kSkinColors[k].note);
    }
    fprintf(f, "\n[images]\n");
    for (int ic = 0; ic < IC_COUNT; ic++)
        if (kIconKey[ic])
            fprintf(f, "%S = %S.png\n", kIconKey[ic], kIconKey[ic]);
    fclose(f);
}
#endif

// ---------------------------------------------------------------- window

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        s_wnd = hwnd;
        s_back = MakeDib(800, 480, &s_backBits);
        LoadSkin();
        BuildBackground();
#ifdef UNDER_CE
        s_taskbar = FindWindow(L"HHTaskBar", NULL);
        if (s_taskbar)
            ShowWindow(s_taskbar, SW_HIDE);
#endif
        LoadApps();
        SyncNight();
        SetTimer(hwnd, TIMER_CLOCK, 1000, NULL);
        InitReverse();
        SetTimer(hwnd, TIMER_REVERSE, kReverseMs, NULL);
        NavStarted();
        CloseHandle(CreateThread(NULL, 0, ExitNaviThread, NULL, 0, NULL));
        Log(L"start, opened over %s; Nav_Started set", ClassOf(s_under));
        return 0;
    case WM_TIMER:
        if (wp == TIMER_LONG) {   // held on the keyboard's text field: copy / paste
            KillTimer(hwnd, TIMER_LONG);
            if (s_pressed >= 0 && s_btns[s_pressed].action == A_KEYFIELD) {
                s_pressed = -1;   // the release does nothing
                s_clipMenu = true;
                Redraw();
            }
            return 0;
        }
        if (wp == TIMER_REVERSE) {
            PollReverse();
        } else if (wp == TIMER_GUARD) {
            OnGuardTimer();
        } else if (wp == TIMER_FRONT) {
            OnFrontTimer();
        } else if (wp == TIMER_WAIT) {
            OnWaitTimer();
            if (IsWindowVisible(hwnd))
                Redraw();   // the status line may have changed
        } else if (IsWindowVisible(hwnd) && GetForegroundWindow() == hwnd) {
            // a whole frame costs: redraw only for a new minute or a day/night change, or the task manager's
            // new sample (every 2 s)
            bool night = s_night;
            SYSTEMTIME t;
            GetLocalTime(&t);
            SyncNight();
            bool tasks = wp == TIMER_CLOCK && s_screen == S_TASKS && (++s_taskTicks & 1) == 0;
            if (tasks)
                TaskSample(&s_task, hwnd);
            if (night != s_night || t.wMinute != s_drawnMinute || tasks)
                Redraw();
        }
        if (wp == TIMER_CLOCK)
            CheckShellNavScreen();   // every second, shown or hidden
        return 0;
    case WM_SHOWWINDOW:   // shown again: VestaHome's day/night may have changed meanwhile, the guard stops
        if (wp) {
            SyncNight();
            StopGuard();
        }
        return 0;
    case WM_VESTA_SHOW:
        ShowFront();
        return 0;
    case WM_EXIT_NAVI:   // the shell ended the navigation (another source, its own screen): step aside
        Log(L"Exit_Navi from the shell");
        if (IsWindowVisible(hwnd))
            Minimize();
        return 0;
    case WM_ACTIVATE:   // back in front: the clock may be behind
        if (LOWORD(wp) != WA_INACTIVE)
            Redraw();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        Redraw();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        s_downX = (short)LOWORD(lp);
        s_pressed = HitTest((short)LOWORD(lp), (short)HIWORD(lp));
        if (s_pressed >= 0 && s_btns[s_pressed].action == A_KEYFIELD)
            SetTimer(hwnd, TIMER_LONG, kLongMs, NULL);
        SetCapture(hwnd);
        Redraw();
        return 0;
    case WM_LBUTTONUP: {
        KillTimer(hwnd, TIMER_LONG);
        ReleaseCapture();
        int x = (short)LOWORD(lp), dx = x - s_downX;
        int i = HitTest(x, (short)HIWORD(lp));
        int was = s_pressed;
        s_pressed = -1;
        if ((s_screen == S_APPS || s_screen == S_FILES) && (dx > 120 || dx < -120)) {   // swipe: turn the page
            Btn turn = { { 0, 0, 0, 0 }, dx < 0 ? A_NEXT : A_PREV, 0 };
            if (s_screen == S_APPS && dx < 0 && s_page + 1 >= AppsPages())
                turn.action = A_NONE;   // swiping doesn't go round, the dots do
            Redraw();   // the old page without the pressed look: the frame the new one pushes out
            s_slideDir = 0;
            DoAction(turn);
            if (s_slideDir)
                RedrawSlide(s_slideDir);
            s_slideDir = 0;
            return 0;
        }
        Redraw();
        if (i >= 0 && i == was) {
            s_slideDir = 0;
            DoAction(s_btns[i]);
            if (IsWindow(hwnd) && IsWindowVisible(hwnd)) {
                if (s_slideDir)
                    RedrawSlide(s_slideDir);
                else
                    Redraw();
            }
            s_slideDir = 0;
        }
        return 0;
    }
#ifndef UNDER_CE
    case WM_CHAR:      // PC test: typing on the keyboard screen; Enter = done, Esc = cancel
        if (s_screen != S_KEYS)
            return 0;
        if (wp == VK_ESCAPE)
            s_screen = S_EDIT;
        else if (wp == VK_RETURN)
            KeysDone();
        else if (wp == VK_BACK)
            KeyBack();
        else if (wp >= 32)
            KeyType((wchar_t)wp);
        BuildButtons();
        Redraw();
        return 0;
    case WM_KEYDOWN:   // PC test: Esc = exit
        if (s_screen != S_KEYS && wp == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;
#endif
    case WM_DESTROY:
        NavTerminated();   // the shell may start us again
        KillTimer(hwnd, TIMER_CLOCK);
        KillTimer(hwnd, TIMER_REVERSE);
        if (s_backWnd)
            DestroyWindow(s_backWnd);
#ifdef UNDER_CE
        if (s_taskbar)
            ShowWindow(s_taskbar, SW_SHOW);
#endif
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

#ifdef UNDER_CE
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
#else
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int)
#endif
{
    GetModuleFileName(NULL, s_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(s_dir, L'\\');
    if (slash)
        slash[1] = 0;
#ifndef UNDER_CE
    if (wcsstr(cmd, L"/export")) {
        ExportSkin();
        return 0;
    }
#endif

    // One copy: the shell starts us again on every "Navigation" press, VestaHome on its "Apps" tile; a second
    // start only shows the running copy. The named mutex also catches a start racing the first one before its
    // window exists (a double tap while the first copy is still loading).
    // The running copy raises itself (WM_VESTA_SHOW): raising it from here did not hold on the unit - the shell
    // that started this copy came back on top. This copy stays 1.5 s, the running one keeps the front for 4 s.
    CreateMutex(NULL, FALSE, L"VestaLaunch_Once");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 50; i++) {   // the first copy may still be loading: wait for its window up to 5 s
            HWND prev = FindWindow(APP_CLASS, NULL);
            if (prev) {
                PostMessage(prev, WM_VESTA_SHOW, 0, 0);
                Sleep(1500);
                break;
            }
            Sleep(100);
        }
        return 0;
    }
    s_under = GetForegroundWindow();   // what we open over: "Back" returns to it

    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_CLASS;
#ifndef UNDER_CE
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
#endif
    RegisterClass(&wc);

#ifdef UNDER_CE
    DWORD style = WS_POPUP | WS_VISIBLE;
    int x = 0, y = 0, w = 800, h = 480;
#else
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    RECT rc = { 0, 0, 800, 480 };
    AdjustWindowRect(&rc, style, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = rc.right - rc.left, h = rc.bottom - rc.top;
#endif
#ifdef UNDER_CE
    DWORD exStyle = WS_EX_TOPMOST;   // the stock shell is topmost, a normal window would stay under it
#else
    DWORD exStyle = 0;
#endif
    HWND hwnd = CreateWindowEx(exStyle, APP_CLASS, L"VestaLaunch", style, x, y, w, h, NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    {   // the backdrop under programs (hidden until one is in front)
        WNDCLASS bc;
        memset(&bc, 0, sizeof(bc));
        bc.lpfnWndProc = BackProc;
        bc.hInstance = inst;
        bc.lpszClassName = L"VestaLaunchBackdrop";
        RegisterClass(&bc);
#ifdef UNDER_CE
        s_backWnd = CreateWindowEx(WS_EX_TOPMOST, L"VestaLaunchBackdrop", L"", WS_POPUP, 0, 0, 800, 480, NULL, NULL,
                                   inst, NULL);
#else
        RECT wr;   // PC: over the launcher's own window only
        GetWindowRect(hwnd, &wr);
        s_backWnd = CreateWindowEx(0, L"VestaLaunchBackdrop", L"", WS_POPUP, wr.left, wr.top, wr.right - wr.left,
                                   wr.bottom - wr.top, NULL, NULL, inst, NULL);
#endif
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
