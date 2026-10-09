/*
 * FolderHeat - folder icons that show how often you open them.
 *
 * A tray app that watches which folders you open in File Explorer and tints
 * each folder's own icon by how often you go there: blue, green, orange,
 * red. Visits fade with a half-life, so a folder you stop using cools down
 * and goes back to the normal icon.
 *
 * Plain C on Win32, one translation unit. Build with build.bat.
 *
 * Copyright (c) 2026 iALTURKi. MIT License.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define COBJMACROS

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"

/* ========================================================================
 * Constants
 * ====================================================================== */

#define APP_NAME        L"FolderHeat"
#define APP_VERSION     L"1.0.0"
#define APP_URL         L"https://github.com/iAlturki/FolderHeat"
#define REG_KEY         L"Software\\FolderHeat"
#define RUN_KEY         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define MAIN_CLASS      L"FolderHeat_MainWnd"
#define MUTEX_NAME      L"FolderHeat_SingleInstance"
#define EXPLORER_CLASS  L"CabinetWClass"

#define PATH_CHARS      1024
#define ICON_SET        1               /* bump when the tint changes, so Explorer's
                                           icon cache can't keep showing old colors */
#define TIER_COUNT      4               /* blue, green, orange, red */
#define HOT_ROWS        12              /* rows in "Hottest folders" */
#define PRUNE_DAYS      90              /* forget cold folders after this */

#define TIMER_SCAN      1               /* debounced scan after an Explorer event */
#define TIMER_POLL      2               /* slow safety-net scan */
#define TIMER_COOL      3               /* periodic decay of every folder */
#define TIMER_SAVE      4               /* debounced save of the history */
#define TIMER_TRAY      5               /* retry adding the tray icon */
#define TIMER_RECHECK   6               /* second look after a scan */
#define SCAN_DELAY_MS   350
#define POLL_MS         15000
#define COOL_MS         (30 * 60 * 1000)
#define SAVE_DELAY_MS   5000
#define TRAY_RETRY_MS   2000
#define RECHECK_MS      1500

/* Each tier starts at an estimated number of visits per day. The tint keeps
 * the shading of the Windows folder icon and swaps its hue. */
typedef struct {
    const WCHAR *name;
    double       perDay;    /* lower bound of the tier */
    float        hue;       /* degrees */
    float        sat;       /* saturation multiplier */
    float        light;     /* lightness multiplier */
} Tier;

static const Tier kTier[TIER_COUNT + 1] = {
    { L"none",   0.00,   0.0f, 1.00f, 1.00f },
    { L"blue",   0.12, 212.0f, 1.00f, 0.92f },
    { L"green",  0.45, 128.0f, 0.75f, 0.80f },
    { L"orange", 1.20,  24.0f, 1.10f, 0.86f },
    { L"red",    2.50, 358.0f, 1.10f, 0.80f },
};

/* Sizes written into every generated .ico */
static const int kIcoSizes[] = { 16, 20, 24, 32, 40, 48, 64, 96, 128, 256 };

/* Cooling speed choices (half-life in days) */
static const DWORD kSpeeds[] = { 7, 14, 30 };
static const WCHAR *kSpeedNames[] = { L"Fast (1 week)", L"Normal (2 weeks)", L"Slow (1 month)" };

/* COM ids, spelled out so the build doesn't depend on which ones a
 * toolchain's uuid library happens to carry. */
static const CLSID kCLSID_ShellWindows   = {0x9BA05972,0xF6A8,0x11CF,{0xA4,0x42,0x00,0xA0,0xC9,0x0A,0x8F,0x39}};
static const IID   kIID_IShellWindows    = {0x85CB6900,0x4D95,0x11CF,{0x96,0x0C,0x00,0x80,0xC7,0xF4,0xEE,0x85}};
static const IID   kIID_IServiceProvider = {0x6D5140C1,0x7436,0x11CE,{0x80,0x34,0x00,0xAA,0x00,0x60,0x09,0xFA}};
static const GUID  kSID_STopLevelBrowser = {0x4C96BE40,0x915C,0x11CF,{0x99,0xD3,0x00,0xAA,0x00,0x4A,0xE8,0x37}};
static const IID   kIID_IShellBrowser    = {0x000214E2,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const IID   kIID_IFolderView      = {0xCDE725B0,0xCCC9,0x4519,{0x91,0x7E,0x32,0x5D,0x72,0xFA,0xB4,0xCE}};
static const IID   kIID_IPersistFolder2  = {0x1AC3D9F0,0x175C,0x11D1,{0x95,0xBE,0x00,0x60,0x97,0x97,0xEA,0x4F}};

/* ========================================================================
 * Types and globals
 * ====================================================================== */

enum {
    F_APPLIED  = 1,     /* our icon is on the folder */
    F_ORIG_RO  = 2,     /* folder was read-only before we touched it */
    F_ORIG_SYS = 4,     /* folder had the system attribute before */
    F_CREATED  = 8      /* desktop.ini did not exist before we wrote it */
};

typedef struct {
    WCHAR *path;
    double score;       /* visits, decayed to 'stamp' */
    INT64  stamp;       /* unix time the score was last decayed to */
    INT64  last;        /* unix time of the latest visit */
    DWORD  visits;      /* lifetime visits */
    int    tier;        /* tier of the icon on the folder (when F_APPLIED) */
    DWORD  flags;
    BOOL   skip;        /* not ours to color this run (own icon, no access) */
} FolderEntry;

typedef struct { WCHAR **items; int count, cap; } PathList;
typedef struct { WCHAR *s; size_t len, cap; } Text;

static struct {
    BOOL  paused;
    BOOL  colors;
    BOOL  autostart;
    BOOL  welcomed;
    DWORD halfLife;     /* days */
} g_cfg;

static HINSTANCE       g_hInst;
static HWND            g_hMain;
static UINT            g_msgTaskbarCreated;
static NOTIFYICONDATAW g_nid;
static BOOL            g_trayAdded;
static HICON           g_trayIcon;
static HWINEVENTHOOK   g_hook;

static FolderEntry *g_folders;
static int      g_folderCount, g_folderCap;
static PathList g_seen;             /* folders open in Explorer at the last scan */
static BOOL     g_haveBaseline;
static BOOL     g_scanning;
static BOOL     g_dirty;

static WCHAR    g_dataDir[MAX_PATH];
static WCHAR    g_dbPath[MAX_PATH];
static WCHAR    g_excludePath[MAX_PATH];
static WCHAR    g_iconDir[MAX_PATH];
static WCHAR    g_iconPath[TIER_COUNT + 1][MAX_PATH];
static HBITMAP  g_menuBmp[TIER_COUNT + 1];

static PathList g_excl;             /* built-in and user exclusions */
static FILETIME g_exclStamp;
static ULONGLONG g_exclChecked;

static WCHAR   *g_hotPath[HOT_ROWS]; /* paths behind the open "Hottest" menu */

/* ========================================================================
 * Small helpers
 * ====================================================================== */

static INT64 NowUnix(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (INT64)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

static WCHAR *DupStr(const WCHAR *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    WCHAR *d = (WCHAR *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static BOOL SamePath(const WCHAR *a, const WCHAR *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

/* TRUE when 'path' is 'dir' itself or somewhere inside it */
static BOOL PathUnder(const WCHAR *path, const WCHAR *dir)
{
    size_t n = wcslen(dir);
    while (n > 0 && dir[n - 1] == L'\\') n--;
    if (n == 0 || wcslen(path) < n) return FALSE;
    if (CompareStringOrdinal(path, (int)n, dir, (int)n, TRUE) != CSTR_EQUAL) return FALSE;
    return path[n] == 0 || path[n] == L'\\';
}

static void ListAdd(PathList *l, const WCHAR *s)
{
    WCHAR *d;
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        WCHAR **items = (WCHAR **)realloc(l->items, cap * sizeof(WCHAR *));
        if (!items) return;
        l->items = items;
        l->cap = cap;
    }
    d = DupStr(s);
    if (d) l->items[l->count++] = d;
}

static void ListFree(PathList *l)
{
    int i;
    for (i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    ZeroMemory(l, sizeof(*l));
}

static void TextAdd(Text *t, const WCHAR *s)
{
    size_t n = wcslen(s);
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        WCHAR *p;
        while (cap < t->len + n + 1) cap *= 2;
        p = (WCHAR *)realloc(t->s, cap * sizeof(WCHAR));
        if (!p) return;
        t->s = p;
        t->cap = cap;
    }
    memcpy(t->s + t->len, s, (n + 1) * sizeof(WCHAR));
    t->len += n;
}

/* Whole file as UTF-16 (UTF-16LE with BOM, or UTF-8 with or without BOM).
 * Caller frees. */
static WCHAR *ReadText(const WCHAR *file)
{
    HANDLE h = CreateFileW(file, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    WCHAR *text = NULL;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &size) && size.QuadPart < 64 * 1024 * 1024) {
        DWORD n = (DWORD)size.QuadPart, got = 0;
        BYTE *raw = (BYTE *)malloc(n + 1);
        if (raw && ReadFile(h, raw, n, &got, NULL) && got == n) {
            if (n >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
                int chars = (int)((n - 2) / 2);
                text = (WCHAR *)malloc((chars + 1) * sizeof(WCHAR));
                if (text) {
                    memcpy(text, raw + 2, chars * sizeof(WCHAR));
                    text[chars] = 0;
                }
            } else {
                int skip = (n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
                int chars = n - skip ? MultiByteToWideChar(CP_UTF8, 0, (char *)raw + skip,
                                                           (int)(n - skip), NULL, 0) : 0;
                text = (WCHAR *)malloc((chars + 1) * sizeof(WCHAR));
                if (text) {
                    if (chars) MultiByteToWideChar(CP_UTF8, 0, (char *)raw + skip,
                                                   (int)(n - skip), text, chars);
                    text[chars] = 0;
                }
            }
        }
        free(raw);
    }
    CloseHandle(h);
    return text;
}

/* Write through a temp file so a crash never leaves half a file behind */
static BOOL WriteFileAtomic(const WCHAR *file, const void *data, DWORD size)
{
    WCHAR tmp[PATH_CHARS];
    HANDLE h;
    DWORD put = 0;
    BOOL ok;
    swprintf(tmp, PATH_CHARS, L"%ls.tmp", file);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(h, data, size, &put, NULL) && put == size;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

static BOOL WriteUtf8(const WCHAR *file, const WCHAR *text, BOOL bom)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *buf;
    BOOL ok = FALSE;
    if (n <= 0) return FALSE;
    buf = (char *)malloc(n + 3);
    if (!buf) return FALSE;
    buf[0] = (char)0xEF; buf[1] = (char)0xBB; buf[2] = (char)0xBF;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buf + 3, n, NULL, NULL);
    ok = WriteFileAtomic(file, bom ? buf : buf + 3, (DWORD)(n - 1 + (bom ? 3 : 0)));
    free(buf);
    return ok;
}

static BOOL IsDir(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ========================================================================
 * Config (HKCU\Software\FolderHeat)
 * ====================================================================== */

static DWORD RegGetDword(HKEY key, const WCHAR *name, DWORD def)
{
    DWORD v = def, size = sizeof(v), type = 0;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&v, &size) != ERROR_SUCCESS ||
        type != REG_DWORD)
        v = def;
    return v;
}

static void LoadConfig(void)
{
    HKEY key = NULL;
    size_t i;
    BOOL known = FALSE;
    RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_READ, NULL, &key, NULL);
    g_cfg.paused    = RegGetDword(key, L"Paused", 0) != 0;
    g_cfg.colors    = RegGetDword(key, L"Colors", 1) != 0;
    g_cfg.autostart = RegGetDword(key, L"Autostart", 0) != 0;
    g_cfg.welcomed  = RegGetDword(key, L"Welcomed", 0) != 0;
    g_cfg.halfLife  = RegGetDword(key, L"HalfLifeDays", 14);
    for (i = 0; i < ARRAYSIZE(kSpeeds); i++)
        if (g_cfg.halfLife == kSpeeds[i]) known = TRUE;
    if (!known) g_cfg.halfLife = 14;
    if (key) RegCloseKey(key);
}

static void SaveConfig(void)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL,
                        &key, NULL) != ERROR_SUCCESS)
        return;
#define PUT(name, v) do { DWORD d_ = (DWORD)(v); \
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&d_, sizeof(d_)); } while (0)
    PUT(L"Paused", g_cfg.paused);
    PUT(L"Colors", g_cfg.colors);
    PUT(L"Autostart", g_cfg.autostart);
    PUT(L"Welcomed", g_cfg.welcomed);
    PUT(L"HalfLifeDays", g_cfg.halfLife);
#undef PUT
    RegCloseKey(key);
}

/* Point the Run entry at the exe that is running now, so moving the exe
 * never leaves a stale autostart behind. */
static void ApplyAutostart(void)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (g_cfg.autostart) {
        WCHAR exe[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", exe);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *)cmd,
                       (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}

/* ========================================================================
 * Paths
 * ====================================================================== */

static BOOL InitPaths(void)
{
    WCHAR base[MAX_PATH];
    int t;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) return FALSE;
    swprintf(g_dataDir, MAX_PATH, L"%ls\\FolderHeat", base);
    swprintf(g_dbPath, MAX_PATH, L"%ls\\folders.tsv", g_dataDir);
    swprintf(g_excludePath, MAX_PATH, L"%ls\\exclude.txt", g_dataDir);
    swprintf(g_iconDir, MAX_PATH, L"%ls\\icons", g_dataDir);
    CreateDirectoryW(g_dataDir, NULL);
    CreateDirectoryW(g_iconDir, NULL);
    for (t = 1; t <= TIER_COUNT; t++)
        swprintf(g_iconPath[t], MAX_PATH, L"%ls\\%ls-%d.ico", g_iconDir, kTier[t].name, ICON_SET);
    return IsDir(g_iconDir);
}

/* ========================================================================
 * Icons: the Windows folder icon, re-tinted per tier
 * ====================================================================== */

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static void RgbToHsl(float r, float g, float b, float *h, float *s, float *l)
{
    float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
    *l = (mx + mn) / 2.0f;
    if (d <= 0.0f) { *h = *s = 0.0f; return; }
    *s = *l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r)      *h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) *h = (b - r) / d + 2.0f;
    else              *h = (r - g) / d + 4.0f;
    *h /= 6.0f;
}

static float HueChannel(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f)        return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static void HslToRgb(float h, float s, float l, float *r, float *g, float *b)
{
    float q, p;
    if (s <= 0.0f) { *r = *g = *b = l; return; }
    q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    p = 2.0f * l - q;
    *r = HueChannel(p, q, h + 1.0f / 3.0f);
    *g = HueChannel(p, q, h);
    *b = HueChannel(p, q, h - 1.0f / 3.0f);
}

/* Swap the hue of every coloured pixel (straight BGRA), keep its shading.
 * Greys and whites (the paper in older folder icons) are left alone. */
static void Tint(DWORD *px, int count, int tier)
{
    const Tier *t = &kTier[tier];
    int i;
    for (i = 0; i < count; i++) {
        DWORD c = px[i], a = c >> 24;
        float r, g, b, h, s, l;
        if (!a) continue;
        r = ((c >> 16) & 255) / 255.0f;
        g = ((c >> 8) & 255) / 255.0f;
        b = (c & 255) / 255.0f;
        RgbToHsl(r, g, b, &h, &s, &l);
        if (s < 0.12f) continue;
        HslToRgb(t->hue / 360.0f, Clamp01(s * t->sat), Clamp01(l * t->light), &r, &g, &b);
        px[i] = (a << 24) | ((DWORD)(r * 255.0f + 0.5f) << 16) |
                ((DWORD)(g * 255.0f + 0.5f) << 8) | (DWORD)(b * 255.0f + 0.5f);
    }
}

/* Straight-alpha BGRA pixels of an icon, top-down, size x size */
static BOOL IconToPixels(HICON icon, int size, DWORD *out)
{
    ICONINFO ii;
    BITMAPINFO bi;
    BITMAP bm;
    HDC dc;
    BOOL ok = FALSE;
    int i, n = size * size;
    if (!GetIconInfo(icon, &ii)) return FALSE;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = GetDC(NULL);
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) &&
        bm.bmWidth == size && bm.bmHeight == size &&
        GetDIBits(dc, ii.hbmColor, 0, size, out, &bi, DIB_RGB_COLORS) == size) {
        BOOL alpha = FALSE;
        ok = TRUE;
        for (i = 0; i < n && !alpha; i++) alpha = (out[i] >> 24) != 0;
        if (!alpha) {   /* old-style icon: take transparency from the mask */
            DWORD *mask = (DWORD *)malloc(n * sizeof(DWORD));
            if (mask && GetDIBits(dc, ii.hbmMask, 0, size, mask, &bi, DIB_RGB_COLORS) == size)
                for (i = 0; i < n; i++)
                    if (!(mask[i] & 0xFFFFFF)) out[i] |= 0xFF000000;
            free(mask);
        }
    }
    ReleaseDC(NULL, dc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

/* A plain folder, only used if the system icon can't be read */
static void DrawPlainFolder(int size, DWORD *px)
{
    float u = size / 16.0f;
    int x, y;
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            float fx = (x + 0.5f) / u, fy = (y + 0.5f) / u;
            BOOL front = fx >= 1.0f && fx < 15.0f && fy >= 5.0f && fy < 14.0f;
            BOOL back = (fx >= 1.0f && fx < 15.0f && fy >= 3.5f && fy < 14.0f) ||
                        (fx >= 1.0f && fx < 7.0f && fy >= 2.0f && fy < 3.5f);
            px[y * size + x] = front ? 0xFFF8C443 : (back ? 0xFFDDA21E : 0);
        }
}

static void FolderPixels(int size, DWORD *px)
{
    SHSTOCKICONINFO sii;
    HICON icon = NULL;
    ZeroMemory(&sii, sizeof(sii));
    sii.cbSize = sizeof(sii);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICONLOCATION, &sii)))
        PrivateExtractIconsW(sii.szPath, sii.iIcon, size, size, &icon, NULL, 1, 0);
    if (!icon || !IconToPixels(icon, size, px)) DrawPlainFolder(size, px);
    if (icon) DestroyIcon(icon);
}

/* One .ico with every size in kIcoSizes, 32-bit BMP entries */
static BOOL WriteTierIcon(const WCHAR *file, int tier)
{
    const int count = ARRAYSIZE(kIcoSizes);
    DWORD total = 6 + 16 * count, offset, i;
    BYTE *buf, *dir;
    BOOL ok;
    for (i = 0; i < (DWORD)count; i++) {
        DWORD s = kIcoSizes[i];
        total += 40 + s * s * 4 + ((s + 31) / 32 * 4) * s;
    }
    buf = (BYTE *)calloc(1, total);
    if (!buf) return FALSE;
    buf[2] = 1;                                 /* type: icon */
    buf[4] = (BYTE)count;
    dir = buf + 6;
    offset = 6 + 16 * count;
    for (i = 0; i < (DWORD)count; i++) {
        int s = kIcoSizes[i], x, y;
        DWORD maskRow = (s + 31) / 32 * 4;
        DWORD bytes = 40 + s * s * 4 + maskRow * s;
        DWORD *px = (DWORD *)malloc(s * s * sizeof(DWORD));
        BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)(buf + offset);
        BYTE *color = buf + offset + 40, *mask = color + s * s * 4;
        if (!px) { free(buf); return FALSE; }
        FolderPixels(s, px);
        Tint(px, s * s, tier);
        dir[0] = (BYTE)(s >= 256 ? 0 : s);
        dir[1] = (BYTE)(s >= 256 ? 0 : s);
        dir[4] = 1;                             /* planes */
        dir[6] = 32;                            /* bit count */
        memcpy(dir + 8, &bytes, 4);
        memcpy(dir + 12, &offset, 4);
        bih->biSize = 40;
        bih->biWidth = s;
        bih->biHeight = s * 2;                  /* colour + mask */
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        for (y = 0; y < s; y++) {               /* rows are stored bottom-up */
            const DWORD *row = px + (s - 1 - y) * s;
            memcpy(color + y * s * 4, row, s * 4);
            for (x = 0; x < s; x++)
                if (!(row[x] >> 24)) mask[y * maskRow + x / 8] |= (BYTE)(0x80 >> (x % 8));
        }
        free(px);
        dir += 16;
        offset += bytes;
    }
    ok = WriteFileAtomic(file, buf, total);
    free(buf);
    return ok;
}

static void EnsureIcons(void)
{
    int t;
    for (t = 1; t <= TIER_COUNT; t++)
        if (GetFileAttributesW(g_iconPath[t]) == INVALID_FILE_ATTRIBUTES)
            WriteTierIcon(g_iconPath[t], t);
}

/* Premultiplied 32-bit bitmaps for the "Hottest folders" menu */
static HBITMAP MakeMenuBitmap(int tier, int size)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    DWORD *px;
    int i;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) return NULL;
    px = (DWORD *)bits;
    FolderPixels(size, px);
    if (tier) Tint(px, size * size, tier);
    for (i = 0; i < size * size; i++) {
        DWORD c = px[i], a = c >> 24;
        px[i] = (a << 24) | ((((c >> 16) & 255) * a / 255) << 16) |
                ((((c >> 8) & 255) * a / 255) << 8) | ((c & 255) * a / 255);
    }
    return bmp;
}

static void MakeMenuBitmaps(void)
{
    int t, size = GetSystemMetrics(SM_CXSMICON);
    for (t = 0; t <= TIER_COUNT; t++) g_menuBmp[t] = MakeMenuBitmap(t, size);
}

/* ========================================================================
 * Exclusions
 * ====================================================================== */

static void AddExclusion(const WCHAR *raw)
{
    WCHAR path[PATH_CHARS], *p;
    size_t n;
    while (*raw == L' ' || *raw == L'\t' || *raw == L'"') raw++;
    if (!ExpandEnvironmentStringsW(raw, path, PATH_CHARS)) return;
    n = wcslen(path);
    while (n > 0 && (path[n - 1] == L' ' || path[n - 1] == L'\t' || path[n - 1] == L'\r' ||
                     path[n - 1] == L'"' || path[n - 1] == L'\\' || path[n - 1] == L'/'))
        path[--n] = 0;
    for (p = path; *p; p++) if (*p == L'/') *p = L'\\';
    if (n >= 2) ListAdd(&g_excl, path);
}

static void LoadExclusions(void)
{
    static const WCHAR *envs[] = {
        L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramData",
        L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"
    };
    WCHAR buf[MAX_PATH], *text;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    size_t i;
    ListFree(&g_excl);
    if (GetWindowsDirectoryW(buf, MAX_PATH)) AddExclusion(buf);
    for (i = 0; i < ARRAYSIZE(envs); i++)
        if (GetEnvironmentVariableW(envs[i], buf, MAX_PATH)) AddExclusion(buf);
    AddExclusion(g_dataDir);

    ZeroMemory(&g_exclStamp, sizeof(g_exclStamp));
    if (GetFileAttributesExW(g_excludePath, GetFileExInfoStandard, &fad))
        g_exclStamp = fad.ftLastWriteTime;
    text = ReadText(g_excludePath);
    if (text) {
        WCHAR *line = text;
        while (line && *line) {
            WCHAR *next = wcschr(line, L'\n');
            if (next) *next++ = 0;
            while (*line == L' ' || *line == L'\t') line++;
            if (*line && *line != L'#') AddExclusion(line);
            line = next;
        }
        free(text);
    }
}

static void EnsureExcludeFile(void)
{
    if (GetFileAttributesW(g_excludePath) != INVALID_FILE_ATTRIBUTES) return;
    WriteUtf8(g_excludePath,
        L"# FolderHeat never colors the folders listed here, or anything inside them.\r\n"
        L"# One folder per line, for example:\r\n"
        L"#   D:\\Archive\r\n"
        L"#   %USERPROFILE%\\Dropbox\r\n"
        L"# Lines starting with # are ignored. Save the file and FolderHeat picks it up.\r\n"
        L"\r\n", TRUE);
}

/* Reload exclude.txt if it changed (checked at most every few seconds) */
static BOOL ExclusionsChanged(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    FILETIME stamp;
    ULONGLONG now = GetTickCount64();
    if (now - g_exclChecked < 3000) return FALSE;
    g_exclChecked = now;
    ZeroMemory(&stamp, sizeof(stamp));
    if (GetFileAttributesExW(g_excludePath, GetFileExInfoStandard, &fad))
        stamp = fad.ftLastWriteTime;
    if (CompareFileTime(&stamp, &g_exclStamp) == 0) return FALSE;
    LoadExclusions();
    return TRUE;
}

/* Only plain folders on fixed local drives, never a drive root, never
 * Windows / Program Files / OneDrive / anything the user excluded. */
static BOOL IsColorable(const WCHAR *path)
{
    WCHAR root[4];
    int i;
    if (wcslen(path) <= 3 || path[1] != L':' || path[2] != L'\\') return FALSE;
    root[0] = path[0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    if (GetDriveTypeW(root) != DRIVE_FIXED) return FALSE;
    for (i = 0; i < g_excl.count; i++)
        if (PathUnder(path, g_excl.items[i])) return FALSE;
    return TRUE;
}

/* ========================================================================
 * Folder history
 * ====================================================================== */

static FolderEntry *FindFolder(const WCHAR *path)
{
    int i;
    for (i = 0; i < g_folderCount; i++)
        if (SamePath(g_folders[i].path, path)) return &g_folders[i];
    return NULL;
}

/* Note: may move g_folders, so don't hold FolderEntry pointers across it */
static FolderEntry *AddFolder(const WCHAR *path)
{
    FolderEntry *f;
    WCHAR *dup;
    if (g_folderCount == g_folderCap) {
        int cap = g_folderCap ? g_folderCap * 2 : 64;
        FolderEntry *p = (FolderEntry *)realloc(g_folders, cap * sizeof(FolderEntry));
        if (!p) return NULL;
        g_folders = p;
        g_folderCap = cap;
    }
    dup = DupStr(path);
    if (!dup) return NULL;
    f = &g_folders[g_folderCount++];
    ZeroMemory(f, sizeof(*f));
    f->path = dup;
    f->stamp = NowUnix();
    return f;
}

static void Cool(FolderEntry *f, INT64 now)
{
    if (now > f->stamp) {
        f->score *= pow(0.5, (double)(now - f->stamp) / (g_cfg.halfLife * 86400.0));
        f->stamp = now;
    }
}

/* Estimated visits per day. With a half-life of H days a folder opened k
 * times a day settles at k / (1 - 0.5^(1/H)), so this undoes that. */
static double PerDay(const FolderEntry *f)
{
    return f->score * (1.0 - pow(0.5, 1.0 / g_cfg.halfLife));
}

static int TierOf(const FolderEntry *f)
{
    double r = PerDay(f);
    int t;
    for (t = TIER_COUNT; t > 0; t--)
        if (r >= kTier[t].perDay) return t;
    return 0;
}

static int ColoredCount(void)
{
    int i, n = 0;
    for (i = 0; i < g_folderCount; i++)
        if (g_folders[i].flags & F_APPLIED) n++;
    return n;
}

static BOOL ParseLine(WCHAR *line, FolderEntry *f, WCHAR **path)
{
    WCHAR *p = line, *end;
    ZeroMemory(f, sizeof(*f));
    f->score = wcstod(p, &end);   if (end == p || *end != L'\t') return FALSE; p = end + 1;
    f->stamp = wcstoll(p, &end, 10); if (end == p || *end != L'\t') return FALSE; p = end + 1;
    f->last = wcstoll(p, &end, 10);  if (end == p || *end != L'\t') return FALSE; p = end + 1;
    f->visits = wcstoul(p, &end, 10); if (end == p || *end != L'\t') return FALSE; p = end + 1;
    f->tier = (int)wcstol(p, &end, 10); if (end == p || *end != L'\t') return FALSE; p = end + 1;
    f->flags = wcstoul(p, &end, 10); if (end == p || *end != L'\t') return FALSE; p = end + 1;
    if (!*p || !(f->score >= 0.0) || f->tier < 0 || f->tier > TIER_COUNT) return FALSE;
    *path = p;
    return TRUE;
}

static void LoadDb(void)
{
    WCHAR *text = ReadText(g_dbPath), *line;
    if (!text) return;
    line = text;
    while (line && *line) {
        WCHAR *next = wcschr(line, L'\n'), *path;
        size_t n;
        FolderEntry f, *slot;
        if (next) *next++ = 0;
        n = wcslen(line);
        if (n && line[n - 1] == L'\r') line[n - 1] = 0;
        if (wcsncmp(line, L"FolderHeat\t", 11) != 0 && ParseLine(line, &f, &path) &&
            (slot = AddFolder(path)) != NULL) {
            WCHAR *keep = slot->path;
            *slot = f;
            slot->path = keep;
        }
        line = next;
    }
    free(text);
}

static void SaveDb(void)
{
    Text t;
    WCHAR row[PATH_CHARS + 160];
    int i;
    ZeroMemory(&t, sizeof(t));
    TextAdd(&t, L"FolderHeat\t1\tscore\tstamp\tlast\tvisits\ttier\tflags\tpath\n");
    for (i = 0; i < g_folderCount; i++) {
        const FolderEntry *f = &g_folders[i];
        swprintf(row, ARRAYSIZE(row), L"%.6f\t%lld\t%lld\t%lu\t%d\t%lu\t%ls\n",
                 f->score, (long long)f->stamp, (long long)f->last,
                 (unsigned long)f->visits, f->tier, (unsigned long)f->flags, f->path);
        TextAdd(&t, row);
    }
    if (t.s && WriteUtf8(g_dbPath, t.s, FALSE)) g_dirty = FALSE;
    free(t.s);
}

static void UpdateTip(void);

static void MarkDirty(void)
{
    g_dirty = TRUE;
    if (g_hMain) SetTimer(g_hMain, TIMER_SAVE, SAVE_DELAY_MS, NULL);
    UpdateTip();
}

/* ========================================================================
 * desktop.ini: putting the icon on a folder and taking it off again
 * ====================================================================== */

static void IniPathOf(const WCHAR *dir, WCHAR *out)
{
    swprintf(out, PATH_CHARS, L"%ls\\desktop.ini", dir);
}

static void ReadIconValue(const WCHAR *ini, WCHAR *out, DWORD n)
{
    GetPrivateProfileStringW(L".ShellClassInfo", L"IconResource", L"", out, n, ini);
    if (!out[0]) GetPrivateProfileStringW(L".ShellClassInfo", L"IconFile", L"", out, n, ini);
}

static BOOL IsOurIcon(const WCHAR *value)
{
    return value[0] && PathUnder(value, g_iconDir);
}

static BOOL IniIsEmpty(const WCHAR *ini)
{
    WCHAR names[2048], body[2048], *s;
    if (!GetPrivateProfileSectionNamesW(names, ARRAYSIZE(names), ini)) return TRUE;
    for (s = names; *s; s += wcslen(s) + 1)
        if (GetPrivateProfileSectionW(s, body, ARRAYSIZE(body), ini) > 0) return FALSE;
    return TRUE;
}

static void ResolveRelative(const WCHAR *base, WCHAR *path)
{
    WCHAR out[PATH_CHARS], *p;
    for (p = path; *p; p++) if (*p == L'/') *p = L'\\';
    if (PathIsRelativeW(path) && PathCombineW(out, base, path)) lstrcpynW(path, out, PATH_CHARS);
}

/* First line of a small text file, trimmed */
static BOOL ReadFirstLine(const WCHAR *file, WCHAR *out)
{
    WCHAR *text = ReadText(file), *e;
    if (!text) return FALSE;
    e = wcspbrk(text, L"\r\n");
    if (e) *e = 0;
    lstrcpynW(out, text, PATH_CHARS);
    free(text);
    return out[0] != 0;
}

/* A desktop.ini inside a git work tree would show up as an untracked file,
 * so list it in that repo's local .git/info/exclude (never committed). */
static void HideFromGit(const WCHAR *folder)
{
    WCHAR dir[PATH_CHARS], probe[PATH_CHARS], gitdir[PATH_CHARS], line[PATH_CHARS];
    WCHAR *slash, *text;
    DWORD a = 0;
    lstrcpynW(dir, folder, PATH_CHARS);
    for (;;) {
        swprintf(probe, PATH_CHARS, L"%ls\\.git", dir);
        a = GetFileAttributesW(probe);
        if (a != INVALID_FILE_ATTRIBUTES) break;
        slash = wcsrchr(dir, L'\\');
        if (!slash) return;
        *slash = 0;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        lstrcpynW(gitdir, probe, PATH_CHARS);
    } else {                                    /* worktree or submodule: "gitdir: <path>" */
        if (!ReadFirstLine(probe, line) || wcsncmp(line, L"gitdir:", 7) != 0) return;
        text = line + 7;
        while (*text == L' ') text++;
        lstrcpynW(gitdir, text, PATH_CHARS);
        ResolveRelative(dir, gitdir);
    }
    swprintf(probe, PATH_CHARS, L"%ls\\commondir", gitdir);   /* worktrees share info/ */
    if (ReadFirstLine(probe, line)) {
        ResolveRelative(gitdir, line);
        lstrcpynW(gitdir, line, PATH_CHARS);
    }
    if (!IsDir(gitdir)) return;

    swprintf(probe, PATH_CHARS, L"%ls\\info", gitdir);
    CreateDirectoryW(probe, NULL);
    swprintf(probe, PATH_CHARS, L"%ls\\info\\exclude", gitdir);
    text = ReadText(probe);
    if (text) {
        WCHAR *l = text;
        BOOL found = FALSE;
        while (l && *l && !found) {
            WCHAR *next = wcschr(l, L'\n');
            size_t n;
            if (next) *next++ = 0;
            n = wcslen(l);
            while (n && (l[n - 1] == L'\r' || l[n - 1] == L' ')) l[--n] = 0;
            found = _wcsicmp(l, L"desktop.ini") == 0 || _wcsicmp(l, L"/desktop.ini") == 0;
            l = next;
        }
        free(text);
        if (found) return;
    }
    {
        HANDLE h = CreateFileW(probe, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            static const char add[] = "\n# folder icon colors (FolderHeat)\ndesktop.ini\n";
            DWORD put;
            WriteFile(h, add, (DWORD)(sizeof(add) - 1), &put, NULL);
            CloseHandle(h);
        }
    }
}

/* Tell Explorer the folder's icon changed. Synchronous on purpose: when
 * several folders change at once, fire-and-forget notifications can get
 * merged and Explorer keeps showing a stale icon. */
static void RefreshIcon(const WCHAR *path)
{
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, path, NULL);
}

static BOOL ApplyIcon(FolderEntry *f, int tier)
{
    WCHAR ini[PATH_CHARS], cur[PATH_CHARS];
    SHFOLDERCUSTOMSETTINGS fcs;
    DWORD attr = GetFileAttributesW(f->path);
    BOOL fresh;
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    IniPathOf(f->path, ini);
    ReadIconValue(ini, cur, PATH_CHARS);
    if (cur[0] && !IsOurIcon(cur)) {            /* the folder has an icon of its own */
        f->flags = 0;
        f->tier = 0;
        return FALSE;
    }
    fresh = GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES;
    if (!(f->flags & F_APPLIED)) {              /* remember what to put back later */
        f->flags = 0;
        if (!cur[0]) {
            if (attr & FILE_ATTRIBUTE_READONLY) f->flags |= F_ORIG_RO;
            if (attr & FILE_ATTRIBUTE_SYSTEM)   f->flags |= F_ORIG_SYS;
            if (fresh)                          f->flags |= F_CREATED;
        }
    }
    ZeroMemory(&fcs, sizeof(fcs));
    fcs.dwSize = sizeof(fcs);
    fcs.dwMask = FCSM_ICONFILE;
    fcs.pszIconFile = g_iconPath[tier];
    fcs.iIconIndex = 0;
    if (FAILED(SHGetSetFolderCustomSettings(&fcs, f->path, FCS_FORCEWRITE))) return FALSE;
    if (fresh) HideFromGit(f->path);
    RefreshIcon(f->path);
    f->flags |= F_APPLIED;
    f->tier = tier;
    return TRUE;
}

static void RemoveIcon(FolderEntry *f)
{
    WCHAR ini[PATH_CHARS], cur[PATH_CHARS];
    DWORD attr = GetFileAttributesW(f->path), iniAttr, want;
    DWORD flags = f->flags;
    f->flags = 0;
    f->tier = 0;
    if (!(flags & F_APPLIED) || attr == INVALID_FILE_ATTRIBUTES ||
        !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;
    IniPathOf(f->path, ini);
    ReadIconValue(ini, cur, PATH_CHARS);
    if (!IsOurIcon(cur)) return;                /* changed since; not ours any more */

    iniAttr = GetFileAttributesW(ini);
    SetFileAttributesW(ini, FILE_ATTRIBUTE_NORMAL);
    WritePrivateProfileStringW(L".ShellClassInfo", L"IconResource", NULL, ini);
    WritePrivateProfileStringW(L".ShellClassInfo", L"IconFile", NULL, ini);
    WritePrivateProfileStringW(L".ShellClassInfo", L"IconIndex", NULL, ini);
    WritePrivateProfileStringW(NULL, NULL, NULL, ini);     /* flush the cache */
    if (IniIsEmpty(ini)) DeleteFileW(ini);
    else if (iniAttr != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(ini, iniAttr);

    /* Put the folder's read-only/system bits back the way they were, unless
     * a desktop.ini that someone else wrote is still relying on them. */
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES || !(flags & F_CREATED)) {
        const DWORD settable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                               FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                               FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        want = attr & settable & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM);
        if (flags & F_ORIG_RO)  want |= FILE_ATTRIBUTE_READONLY;
        if (flags & F_ORIG_SYS) want |= FILE_ATTRIBUTE_SYSTEM;
        if (want != (attr & settable))
            SetFileAttributesW(f->path, want ? want : FILE_ATTRIBUTE_NORMAL);
    }
    RefreshIcon(f->path);
}

/* After an update that changed ICON_SET, move colored folders over to the
 * new icon files */
static void RepointIcons(void)
{
    WCHAR ini[PATH_CHARS], cur[PATH_CHARS];
    int i;
    for (i = 0; i < g_folderCount; i++) {
        FolderEntry *f = &g_folders[i];
        size_t n;
        if (!(f->flags & F_APPLIED) || f->tier < 1 || f->tier > TIER_COUNT) continue;
        IniPathOf(f->path, ini);
        ReadIconValue(ini, cur, PATH_CHARS);
        n = wcslen(g_iconPath[f->tier]);
        if (IsOurIcon(cur) && (_wcsnicmp(cur, g_iconPath[f->tier], n) != 0 ||
                               (cur[n] != 0 && cur[n] != L',')))
            ApplyIcon(f, f->tier);
    }
}

/* Bring one folder's icon in line with its current heat */
static void Settle(FolderEntry *f, INT64 now)
{
    int want = 0, have;
    Cool(f, now);
    if (g_cfg.colors && IsColorable(f->path)) want = TierOf(f);
    have = (f->flags & F_APPLIED) ? f->tier : 0;
    if (want == have) return;
    if (want) {
        if (f->skip) return;
        if (!ApplyIcon(f, want)) { f->skip = TRUE; return; }
    } else {
        RemoveIcon(f);
    }
    MarkDirty();
}

static void SettleAll(void)
{
    INT64 now = NowUnix();
    int i, kept = 0;
    ExclusionsChanged();
    for (i = 0; i < g_folderCount; i++) Settle(&g_folders[i], now);

    /* forget folders that went cold long ago */
    for (i = 0; i < g_folderCount; i++) {
        FolderEntry *f = &g_folders[i];
        if (!(f->flags & F_APPLIED) && TierOf(f) == 0 &&
            now - f->last > (INT64)PRUNE_DAYS * 86400) {
            free(f->path);
            g_dirty = TRUE;
            continue;
        }
        g_folders[kept++] = *f;
    }
    g_folderCount = kept;
}

static void RemoveAllIcons(void)
{
    int i;
    for (i = 0; i < g_folderCount; i++)
        if (g_folders[i].flags & F_APPLIED) RemoveIcon(&g_folders[i]);
    MarkDirty();
}

static void Visit(const WCHAR *path)
{
    FolderEntry *f;
    INT64 now = NowUnix();
    if (!IsDir(path)) return;
    if (ExclusionsChanged()) SettleAll();
    f = FindFolder(path);
    if (!f) f = AddFolder(path);
    if (!f) return;
    Cool(f, now);
    f->score += 1.0;
    f->visits++;
    f->last = now;
    Settle(f, now);
    MarkDirty();
}

/* ========================================================================
 * Watching Explorer
 * ====================================================================== */

/* File-system folder shown by one Explorer window or tab */
static BOOL FolderOfBrowser(IDispatch *disp, WCHAR *out)
{
    IServiceProvider *sp = NULL;
    IShellBrowser *sb = NULL;
    IShellView *sv = NULL;
    IFolderView *fv = NULL;
    IPersistFolder2 *pf = NULL;
    BOOL ok = FALSE;
    out[0] = 0;
    if (SUCCEEDED(IDispatch_QueryInterface(disp, &kIID_IServiceProvider, (void **)&sp)) &&
        SUCCEEDED(IServiceProvider_QueryService(sp, &kSID_STopLevelBrowser,
                                                &kIID_IShellBrowser, (void **)&sb)) &&
        SUCCEEDED(IShellBrowser_QueryActiveShellView(sb, &sv)) && sv &&
        SUCCEEDED(IShellView_QueryInterface(sv, &kIID_IFolderView, (void **)&fv)) &&
        SUCCEEDED(IFolderView_GetFolder(fv, &kIID_IPersistFolder2, (void **)&pf))) {
        PIDLIST_ABSOLUTE pidl = NULL;
        if (SUCCEEDED(IPersistFolder2_GetCurFolder(pf, &pidl)) && pidl) {
            ok = SHGetPathFromIDListEx(pidl, out, PATH_CHARS, GPFIDL_DEFAULT) && out[0];
            CoTaskMemFree(pidl);
        }
    }
    if (pf) IPersistFolder2_Release(pf);
    if (fv) IFolderView_Release(fv);
    if (sv) IShellView_Release(sv);
    if (sb) IShellBrowser_Release(sb);
    if (sp) IServiceProvider_Release(sp);
    return ok;
}

static BOOL CollectOpenFolders(PathList *out)
{
    IShellWindows *sw = NULL;
    long i, n = 0;
    if (FAILED(CoCreateInstance(&kCLSID_ShellWindows, NULL, CLSCTX_ALL,
                                &kIID_IShellWindows, (void **)&sw)))
        return FALSE;
    IShellWindows_get_Count(sw, &n);
    for (i = 0; i < n; i++) {
        VARIANT v;
        IDispatch *disp = NULL;
        WCHAR path[PATH_CHARS];
        VariantInit(&v);
        V_VT(&v) = VT_I4;
        V_I4(&v) = i;
        if (SUCCEEDED(IShellWindows_Item(sw, v, &disp)) && disp) {
            if (FolderOfBrowser(disp, path)) ListAdd(out, path);
            IDispatch_Release(disp);
        }
    }
    IShellWindows_Release(sw);
    return TRUE;
}

/* Compare what Explorer shows now with the last scan. Every window or tab
 * that now shows a folder it didn't show before is one visit. */
static void Scan(BOOL count)
{
    PathList now;
    if (g_scanning) return;     /* cross-process COM can re-enter via timers */
    g_scanning = TRUE;
    ZeroMemory(&now, sizeof(now));
    if (CollectOpenFolders(&now)) {
        if (count && g_haveBaseline && !g_cfg.paused) {
            BOOL *used = (BOOL *)calloc(g_seen.count + 1, sizeof(BOOL));
            int i, j;
            if (used) {
                for (i = 0; i < now.count; i++) {
                    BOOL known = FALSE;
                    for (j = 0; j < g_seen.count && !known; j++)
                        if (!used[j] && SamePath(now.items[i], g_seen.items[j]))
                            used[j] = known = TRUE;
                    if (!known) Visit(now.items[i]);
                }
                free(used);
            }
        }
        ListFree(&g_seen);
        g_seen = now;
        g_haveBaseline = TRUE;
    } else {
        ListFree(&now);
    }
    g_scanning = FALSE;
}

/* Any title or caption change inside an Explorer window (navigating changes
 * them) schedules a scan; the timer restarts on each event, so a burst of
 * events costs one scan. */
static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                  LONG idChild, DWORD thread, DWORD time)
{
    WCHAR cls[32];
    HWND root;
    (void)hook; (void)event; (void)idObject; (void)idChild; (void)thread; (void)time;
    if (!hwnd) return;
    root = GetAncestor(hwnd, GA_ROOT);
    if (root && GetClassNameW(root, cls, ARRAYSIZE(cls)) && wcscmp(cls, EXPLORER_CLASS) == 0)
        SetTimer(g_hMain, TIMER_SCAN, SCAN_DELAY_MS, NULL);
}

/* ========================================================================
 * Tray
 * ====================================================================== */

static BOOL AddTray(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMain;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_trayIcon;
    lstrcpynW(g_nid.szTip, APP_NAME, ARRAYSIZE(g_nid.szTip));
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (g_trayAdded) UpdateTip();
    return g_trayAdded;
}

static void RemoveTray(void)
{
    if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = FALSE;
}

static void UpdateTip(void)
{
    if (!g_trayAdded) return;
    swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"%ls\n%d folders colored%ls", APP_NAME,
             ColoredCount(), g_cfg.paused ? L"\nCounting paused" : L"");
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void Balloon(const WCHAR *title, const WCHAR *text)
{
    if (!g_trayAdded) return;
    g_nid.uFlags = NIF_INFO;
    lstrcpynW(g_nid.szInfoTitle, title, ARRAYSIZE(g_nid.szInfoTitle));
    lstrcpynW(g_nid.szInfo, text, ARRAYSIZE(g_nid.szInfo));
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* "parent\name" with '&' doubled so the menu doesn't eat it */
static void MenuLabel(const FolderEntry *f, WCHAR *out, size_t n)
{
    const WCHAR *p = f->path, *last = wcsrchr(p, L'\\'), *from = p;
    WCHAR shown[PATH_CHARS], rate[32];
    size_t o = 0;
    if (last && last > p) {
        const WCHAR *q = last - 1;
        while (q > p && *q != L'\\') q--;
        from = (*q == L'\\') ? q + 1 : p;
    }
    for (; *from && o + 2 < ARRAYSIZE(shown); from++) {
        if (*from == L'&') shown[o++] = L'&';
        shown[o++] = *from;
    }
    shown[o] = 0;
    swprintf(rate, ARRAYSIZE(rate), L"%.1f/day", PerDay(f));
    swprintf(out, n, L"%ls\t%ls", shown, rate);
}

static void ShowMenu(void)
{
    HMENU menu = CreatePopupMenu(), hot = CreatePopupMenu(), speed = CreatePopupMenu();
    WCHAR text[PATH_CHARS + 64];
    int order[HOT_ROWS], rows = 0, i, j;
    INT64 now = NowUnix();
    POINT pt;

    for (i = 0; i < HOT_ROWS; i++) { free(g_hotPath[i]); g_hotPath[i] = NULL; }

    /* top folders by heat */
    for (i = 0; i < g_folderCount; i++) {
        Cool(&g_folders[i], now);
        if (PerDay(&g_folders[i]) < 0.05) continue;
        for (j = rows; j > 0 && PerDay(&g_folders[order[j - 1]]) < PerDay(&g_folders[i]); j--)
            if (j < HOT_ROWS) order[j] = order[j - 1];
        if (j < HOT_ROWS) {
            order[j] = i;
            if (rows < HOT_ROWS) rows++;
        }
    }
    for (i = 0; i < rows; i++) {
        const FolderEntry *f = &g_folders[order[i]];
        int tier = (f->flags & F_APPLIED) ? f->tier : 0;
        MENUITEMINFOW mii;
        MenuLabel(f, text, ARRAYSIZE(text));
        AppendMenuW(hot, MF_STRING, IDM_HOT_BASE + i, text);
        g_hotPath[i] = DupStr(f->path);
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = g_menuBmp[tier];
        if (mii.hbmpItem) SetMenuItemInfoW(hot, IDM_HOT_BASE + i, FALSE, &mii);
    }
    if (!rows) AppendMenuW(hot, MF_STRING | MF_GRAYED, IDM_EMPTY, L"Nothing yet - open some folders");

    for (i = 0; i < (int)ARRAYSIZE(kSpeeds); i++)
        AppendMenuW(speed, MF_STRING, IDM_SPEED_BASE + kSpeeds[i], kSpeedNames[i]);
    CheckMenuRadioItem(speed, IDM_SPEED_BASE + kSpeeds[0],
                       IDM_SPEED_BASE + kSpeeds[ARRAYSIZE(kSpeeds) - 1],
                       IDM_SPEED_BASE + g_cfg.halfLife, MF_BYCOMMAND);

    swprintf(text, ARRAYSIZE(text), L"%ls - %d folders colored", APP_NAME, ColoredCount());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_HEADER, text);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)hot, L"Hottest folders");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_cfg.colors ? MF_CHECKED : 0), IDM_COLORS, L"Color folder icons");
    AppendMenuW(menu, MF_STRING | (g_cfg.paused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause counting");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)speed, L"Cooling speed");
    AppendMenuW(menu, MF_STRING | (g_cfg.autostart ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXCLUDE, L"Excluded folders...");
    AppendMenuW(menu, MF_STRING, IDM_CLEAR, L"Clear history...");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(g_hMain);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hMain, NULL);
    PostMessageW(g_hMain, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void OnCommand(UINT id)
{
    if (id >= IDM_HOT_BASE && id < IDM_HOT_BASE + HOT_ROWS) {
        const WCHAR *path = g_hotPath[id - IDM_HOT_BASE];
        if (path) ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    if (id > IDM_SPEED_BASE && id <= IDM_SPEED_BASE + 365) {
        g_cfg.halfLife = id - IDM_SPEED_BASE;
        SaveConfig();
        SettleAll();
        MarkDirty();
        return;
    }
    switch (id) {
    case IDM_COLORS:
        g_cfg.colors = !g_cfg.colors;
        SaveConfig();
        if (g_cfg.colors) {
            int i;
            for (i = 0; i < g_folderCount; i++) g_folders[i].skip = FALSE;
            SettleAll();
        } else {
            RemoveAllIcons();
        }
        UpdateTip();
        break;
    case IDM_PAUSE:
        g_cfg.paused = !g_cfg.paused;
        SaveConfig();
        UpdateTip();
        break;
    case IDM_AUTOSTART:
        g_cfg.autostart = !g_cfg.autostart;
        SaveConfig();
        ApplyAutostart();
        break;
    case IDM_EXCLUDE: {
        WCHAR args[MAX_PATH + 4];
        EnsureExcludeFile();
        swprintf(args, ARRAYSIZE(args), L"\"%ls\"", g_excludePath);
        ShellExecuteW(NULL, L"open", L"notepad.exe", args, NULL, SW_SHOWNORMAL);
        break;
    }
    case IDM_CLEAR:
        if (MessageBoxW(g_hMain,
                L"Put every folder icon back to normal and forget all visits?",
                APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES) {
            int i;
            RemoveAllIcons();
            for (i = 0; i < g_folderCount; i++) free(g_folders[i].path);
            g_folderCount = 0;
            SaveDb();
            UpdateTip();
        }
        break;
    case IDM_ABOUT:
        MessageBoxW(g_hMain,
            APP_NAME L" " APP_VERSION L"\n\n"
            L"Folder icons that show how often you open them:\n"
            L"blue, green, orange, red. Folders you stop opening cool down\n"
            L"and go back to the normal icon.\n\n"
            APP_URL L"\n"
            L"Copyright (c) 2026 iALTURKi. MIT License.",
            L"About " APP_NAME, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        break;
    case IDM_EXIT:
        DestroyWindow(g_hMain);
        break;
    }
}

/* ========================================================================
 * Main window
 * ====================================================================== */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        switch (wp) {
        case TIMER_SCAN:
            KillTimer(hwnd, TIMER_SCAN);
            Scan(TRUE);
            /* a new window can raise its first events before it is listed
             * in ShellWindows, so look once more shortly after */
            SetTimer(hwnd, TIMER_RECHECK, RECHECK_MS, NULL);
            break;
        case TIMER_RECHECK:
            KillTimer(hwnd, TIMER_RECHECK);
            Scan(TRUE);
            break;
        case TIMER_POLL:
            if (ExclusionsChanged()) SettleAll();
            if (FindWindowW(EXPLORER_CLASS, NULL)) Scan(TRUE);
            break;
        case TIMER_COOL:
            SettleAll();
            if (g_dirty) SaveDb();
            UpdateTip();
            break;
        case TIMER_SAVE:
            KillTimer(hwnd, TIMER_SAVE);
            SaveDb();
            break;
        case TIMER_TRAY:
            if (AddTray()) KillTimer(hwnd, TIMER_TRAY);
            break;
        }
        return 0;

    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) ShowMenu();
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;

    case WM_APP_PING:
        Balloon(APP_NAME L" is already running", L"Right-click the tray icon for options.");
        return 0;

    case WM_QUERYENDSESSION:
        SaveDb();
        return TRUE;

    case WM_ENDSESSION:
        if (wp) SaveDb();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_SCAN);
        KillTimer(hwnd, TIMER_POLL);
        KillTimer(hwnd, TIMER_COOL);
        KillTimer(hwnd, TIMER_SAVE);
        KillTimer(hwnd, TIMER_TRAY);
        KillTimer(hwnd, TIMER_RECHECK);
        if (g_hook) UnhookWinEvent(g_hook);
        g_hook = NULL;
        SaveDb();
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        /* Explorer restarted: tray icon is gone and so are its windows */
        AddTray();
        g_haveBaseline = FALSE;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int show)
{
    WNDCLASSEXW wc;
    HANDLE mutex;
    MSG msg;
    int i;
    (void)hPrev; (void)cmdLine; (void)show;
    g_hInst = hInst;

    mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(MAIN_CLASS, NULL);
        if (other) PostMessageW(other, WM_APP_PING, 0, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    if (!InitPaths()) {
        MessageBoxW(NULL, L"Could not create the data folder in %LOCALAPPDATA%.", APP_NAME,
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    LoadConfig();
    EnsureIcons();
    EnsureExcludeFile();
    LoadExclusions();
    MakeMenuBitmaps();
    LoadDb();
    RepointIcons();

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassExW(&wc);
    /* A real (never shown) top-level window, not HWND_MESSAGE: message-only
     * windows don't get the TaskbarCreated broadcast. */
    g_hMain = CreateWindowExW(WS_EX_TOOLWINDOW, MAIN_CLASS, APP_NAME, WS_POPUP,
                              0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hMain) return 1;
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_trayIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!AddTray()) SetTimer(g_hMain, TIMER_TRAY, TRAY_RETRY_MS, NULL);
    if (g_cfg.autostart) ApplyAutostart();

    g_hook = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL,
                             WinEventProc, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SettleAll();
    Scan(FALSE);
    SetTimer(g_hMain, TIMER_POLL, POLL_MS, NULL);
    SetTimer(g_hMain, TIMER_COOL, COOL_MS, NULL);

    if (!g_cfg.welcomed) {
        Balloon(APP_NAME L" is running",
                L"Folders you open often turn blue, then green, orange and red. "
                L"Right-click the tray icon for options.");
        g_cfg.welcomed = TRUE;
        SaveConfig();
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_trayIcon) DestroyIcon(g_trayIcon);
    for (i = 0; i <= TIER_COUNT; i++) if (g_menuBmp[i]) DeleteObject(g_menuBmp[i]);
    for (i = 0; i < HOT_ROWS; i++) free(g_hotPath[i]);
    for (i = 0; i < g_folderCount; i++) free(g_folders[i].path);
    free(g_folders);
    ListFree(&g_seen);
    ListFree(&g_excl);
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
