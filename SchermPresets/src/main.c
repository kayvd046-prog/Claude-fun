/*
 * SchermPresets - beeldschermconfiguraties opslaan en terugzetten als presets.
 *
 * Bovenaan het venster staat een interactieve monitorkaart: klik op een scherm
 * om het aan of uit te zetten in de preset die je gaat opslaan. De kaart laadt
 * automatisch de huidige Windows-indeling.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0602
#define WINVER 0x0602

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdlib.h>

/* ======================================================== */
/* Bestandsformaat                                          */
/* ======================================================== */

#define PRESET_MAGIC   0x53505253u
#define PRESET_VERSION 1u
#define PRESET_EXT     L".spreset"

typedef struct {
    DWORD magic, version, numPaths, numModes, numAdapters;
} PresetHeader;

typedef struct {
    LUID  id;
    WCHAR devicePath[128];
} AdapterRecord;

#define MAX_ADAPTERS 16

/* ======================================================== */
/* Monitor-kaart                                            */
/* ======================================================== */

#define MAX_MON 8

typedef struct {
    WCHAR  name[64];    /* vriendelijke naam, bijv. "Dell U2722D" */
    RECT   vRect;       /* positie in het virtuele bureaublad      */
    BOOL   enabled;     /* TRUE = opnemen in preset                */
    LUID   adapterId;
    UINT32 targetId;
    UINT32 sourceId;
} Monitor;

static Monitor g_mon[MAX_MON];
static int     g_nMon;
static HWND    g_hCanvas;

/* Kleuren per monitor (BGR voor GDI) */
static const COLORREF MON_CLR[] = {
    0xB84010, 0x1050B8, 0x10883C, 0x8830A0,
    0xB07010, 0x1090B0, 0xB03070, 0x6090A0
};

/* ======================================================== */
/* GUI-handles en helpers                                   */
/* ======================================================== */

#define IDC_CANVAS      99
#define IDC_LIST        100
#define IDC_EDIT_NAME   101
#define IDC_BTN_SAVE    102
#define IDC_BTN_APPLY   103
#define IDC_BTN_DELETE  104
#define IDC_BTN_REFRESH 105
#define IDC_BTN_FOLDER  106
#define IDC_BTN_EXTEND  107
#define IDC_BTN_CLONE   108
#define IDC_BTN_FIRST   109
#define IDC_BTN_SECOND  110
#define IDC_STATUS      111
#define IDC_BTN_RELOAD  112

static HWND  g_hList, g_hEdit, g_hStatus;
static HFONT g_hFont, g_hFontSm;
static int   g_dpi = 96;

#define S(x) MulDiv((x), g_dpi, 96)

static void SetStatus(const WCHAR *fmt, ...)
{
    WCHAR buf[512]; va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(buf, 512, fmt, ap);
    va_end(ap);
    SetWindowTextW(g_hStatus, buf);
}

/* ======================================================== */
/* CCD-hulpfuncties                                         */
/* ======================================================== */

static BOOL LuidEq(LUID a, LUID b)
{ return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

static BOOL GetAdapterPath(LUID id, WCHAR *out, size_t cch)
{
    DISPLAYCONFIG_ADAPTER_NAME r = {0};
    r.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    r.header.size = sizeof(r);
    r.header.adapterId = id;
    if (DisplayConfigGetDeviceInfo(&r.header) != ERROR_SUCCESS) return FALSE;
    return SUCCEEDED(StringCchCopyW(out, cch, r.adapterDevicePath));
}

static DWORD CollectAdapters(const DISPLAYCONFIG_PATH_INFO *paths, DWORD n,
                             AdapterRecord *rec, DWORD maxRec)
{
    DWORD cnt = 0;
    for (DWORD i = 0; i < n; i++) {
        LUID luids[2] = { paths[i].sourceInfo.adapterId, paths[i].targetInfo.adapterId };
        for (int j = 0; j < 2; j++) {
            BOOL known = FALSE;
            for (DWORD k = 0; k < cnt; k++)
                if (LuidEq(rec[k].id, luids[j])) { known = TRUE; break; }
            if (!known && cnt < maxRec &&
                GetAdapterPath(luids[j], rec[cnt].devicePath, 128)) {
                rec[cnt].id = luids[j];
                cnt++;
            }
        }
    }
    return cnt;
}

static LONG QueryCCD(DISPLAYCONFIG_PATH_INFO **outP, UINT32 *outNP,
                     DISPLAYCONFIG_MODE_INFO **outM, UINT32 *outNM, UINT32 flags)
{
    UINT32 np = 0, nm = 0;
    LONG rc = GetDisplayConfigBufferSizes(flags, &np, &nm);
    if (rc != ERROR_SUCCESS) return rc;
    DISPLAYCONFIG_PATH_INFO *p = calloc(np, sizeof(*p));
    DISPLAYCONFIG_MODE_INFO *m = calloc(nm ? nm : 1, sizeof(*m));
    if (!p || !m) { free(p); free(m); return ERROR_OUTOFMEMORY; }
    rc = QueryDisplayConfig(flags, &np, p, &nm, m, NULL);
    if (rc != ERROR_SUCCESS) { free(p); free(m); return rc; }
    *outP = p; *outNP = np; *outM = m; *outNM = nm;
    return ERROR_SUCCESS;
}

/* ======================================================== */
/* Monitor-kaart laden                                      */
/* ======================================================== */

static void LoadMonitors(void)
{
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    UINT32 np = 0, nm = 0;

    g_nMon = 0;
    /* QDC_ALL_PATHS geeft ook inactieve (uitgeschakelde) monitoren terug. */
    if (QueryCCD(&paths, &np, &modes, &nm, QDC_ALL_PATHS) != ERROR_SUCCESS)
        return;

    /* We tellen bij elkaar op hoever de actieve monitoren reiken zodat we
     * inactieve monitoren rechts ernaast kunnen plaatsen. */
    LONG placeholderX = 0;

    /* Bijhouden welke targets al verwerkt zijn (QDC_ALL_PATHS levert per
     * target meerdere paden op voor elke mogelijke bron). */
    typedef struct { LUID adp; UINT32 tid; } Seen;
    Seen seen[64]; int nSeen = 0;

    for (UINT32 i = 0; i < np && g_nMon < MAX_MON; i++) {
        /* Sla duplicaten over */
        BOOL dup = FALSE;
        for (int k = 0; k < nSeen; k++)
            if (LuidEq(seen[k].adp, paths[i].targetInfo.adapterId) &&
                seen[k].tid == paths[i].targetInfo.id) { dup = TRUE; break; }
        if (dup) continue;

        BOOL active = (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;

        /* Naam ophalen; sla het pad over als er geen monitor aangesloten is */
        DISPLAYCONFIG_TARGET_DEVICE_NAME tdn = {0};
        tdn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tdn.header.size = sizeof(tdn);
        tdn.header.adapterId = paths[i].targetInfo.adapterId;
        tdn.header.id = paths[i].targetInfo.id;
        BOOL hasName = DisplayConfigGetDeviceInfo(&tdn.header) == ERROR_SUCCESS &&
                       tdn.monitorFriendlyDeviceName[0];

        /* Sla over als niet actief én geen naam (fysiek niet aangesloten) */
        if (!active && !hasName) continue;

        Monitor *mon = &g_mon[g_nMon];
        if (hasName)
            StringCchCopyW(mon->name, 64, tdn.monitorFriendlyDeviceName);
        else
            StringCchPrintfW(mon->name, 64, L"Scherm %d", g_nMon + 1);

        if (active) {
            /* Positie uit de bronmodus */
            UINT32 srcIdx = paths[i].sourceInfo.modeInfoIdx;
            if (srcIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && srcIdx < nm &&
                modes[srcIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                DISPLAYCONFIG_SOURCE_MODE *sm = &modes[srcIdx].sourceMode;
                mon->vRect.left   = sm->position.x;
                mon->vRect.top    = sm->position.y;
                mon->vRect.right  = sm->position.x + (LONG)sm->width;
                mon->vRect.bottom = sm->position.y + (LONG)sm->height;
                if (mon->vRect.right > placeholderX) placeholderX = mon->vRect.right;
            } else {
                mon->vRect.left = placeholderX; mon->vRect.top = 0;
                mon->vRect.right = placeholderX + 1920; mon->vRect.bottom = 1080;
                placeholderX += 1920 + 40;
            }
        } else {
            /* Inactieve monitor: placeholder rechts van de actieve schermen */
            mon->vRect.left   = placeholderX + 40;
            mon->vRect.top    = 0;
            mon->vRect.right  = placeholderX + 40 + 1920;
            mon->vRect.bottom = 1080;
            placeholderX = mon->vRect.right;
        }

        mon->enabled   = active;
        mon->adapterId = paths[i].targetInfo.adapterId;
        mon->targetId  = paths[i].targetInfo.id;
        mon->sourceId  = paths[i].sourceInfo.id;

        if (nSeen < 64) { seen[nSeen].adp = paths[i].targetInfo.adapterId;
                          seen[nSeen].tid = paths[i].targetInfo.id; nSeen++; }
        g_nMon++;
    }

    free(paths);
    free(modes);
}

/* ======================================================== */
/* Canvas: interactieve monitorkaart                        */
/* ======================================================== */

/* Schaal de virtuele monitorpositie naar canvas-coördinaten. */
static RECT ScaleMonRect(int cw, int ch, int i)
{
    /* Bounding box van alle monitoren */
    LONG vl = g_mon[0].vRect.left, vt = g_mon[0].vRect.top;
    LONG vr = g_mon[0].vRect.right, vb = g_mon[0].vRect.bottom;
    for (int k = 1; k < g_nMon; k++) {
        if (g_mon[k].vRect.left   < vl) vl = g_mon[k].vRect.left;
        if (g_mon[k].vRect.top    < vt) vt = g_mon[k].vRect.top;
        if (g_mon[k].vRect.right  > vr) vr = g_mon[k].vRect.right;
        if (g_mon[k].vRect.bottom > vb) vb = g_mon[k].vRect.bottom;
    }
    LONG vw = vr - vl, vh = vb - vt;
    if (vw <= 0) vw = 1;
    if (vh <= 0) vh = 1;

    const int PAD = 8;
    int aw = cw - 2*PAD, ah = ch - 2*PAD;

    /* Behoud aspect ratio */
    if (aw * vh > ah * vw)
        aw = ah * vw / vh;
    else
        ah = aw * vh / vw;

    int ox = PAD + (cw - 2*PAD - aw) / 2;
    int oy = PAD + (ch - 2*PAD - ah) / 2;

    const RECT *r = &g_mon[i].vRect;
    RECT out;
    out.left   = ox + (r->left   - vl) * aw / vw;
    out.top    = oy + (r->top    - vt) * ah / vh;
    out.right  = ox + (r->right  - vl) * aw / vw;
    out.bottom = oy + (r->bottom - vt) * ah / vh;
    /* minimale grootte zodat de tekst leesbaar is */
    if (out.right  - out.left < 32) out.right  = out.left + 32;
    if (out.bottom - out.top  < 20) out.bottom = out.top  + 20;
    return out;
}

static int HitTest(int cw, int ch, int mx, int my)
{
    for (int i = 0; i < g_nMon; i++) {
        RECT r = ScaleMonRect(cw, ch, i);
        if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom)
            return i;
    }
    return -1;
}

static void PaintCanvas(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    int cw = cr.right, ch = cr.bottom;

    /* Achtergrond */
    HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 40));
    FillRect(hdc, &cr, hBg);
    DeleteObject(hBg);

    if (g_nMon == 0) {
        SetBkColor(hdc, RGB(30, 30, 40));
        SetTextColor(hdc, RGB(180, 180, 180));
        SelectObject(hdc, g_hFontSm);
        DrawTextW(hdc, L"Geen actieve schermen gevonden.", -1, &cr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return;
    }

    SelectObject(hdc, g_hFontSm);
    SetBkMode(hdc, TRANSPARENT);

    for (int i = 0; i < g_nMon; i++) {
        RECT r = ScaleMonRect(cw, ch, i);
        COLORREF baseClr = MON_CLR[i % 8];
        BOOL on = g_mon[i].enabled;

        /* Vul rechthoek */
        COLORREF fill = on ? baseClr : RGB(60, 60, 65);
        HBRUSH hFill = CreateSolidBrush(fill);
        FillRect(hdc, &r, hFill);
        DeleteObject(hFill);

        /* Rand */
        COLORREF border = on ? RGB(
            min(255, GetRValue(baseClr) + 60),
            min(255, GetGValue(baseClr) + 60),
            min(255, GetBValue(baseClr) + 60))
            : RGB(100, 100, 110);
        HPEN hPen = CreatePen(PS_SOLID, 2, border);
        HPEN hOld = SelectObject(hdc, hPen);
        HBRUSH hNull = GetStockObject(NULL_BRUSH);
        HBRUSH hOldBr = SelectObject(hdc, hNull);
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, hOld);
        SelectObject(hdc, hOldBr);
        DeleteObject(hPen);

        /* Naam + status */
        SetTextColor(hdc, on ? RGB(255,255,255) : RGB(140,140,145));
        RECT tr = r;
        InflateRect(&tr, -4, -3);
        DrawTextW(hdc, g_mon[i].name, -1, &tr,
                  DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

        const WCHAR *badge = on ? L"AAN" : L"UIT";
        RECT br = r;
        br.top = br.bottom - S(16);
        InflateRect(&br, -4, -2);
        SetTextColor(hdc, on ? RGB(200,255,200) : RGB(255,160,160));
        DrawTextW(hdc, badge, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    EndPaint(hwnd, &ps);
}

/* ======================================================== */
/* Monitor direct aan/uitzetten via CCD                     */
/* ======================================================== */

static void ToggleMonitorLive(int idx)
{
    Monitor *mon = &g_mon[idx];

    if (mon->enabled) {
        /* --- Uitzetten: verwijder dit scherm uit de actieve paden --- */
        DISPLAYCONFIG_PATH_INFO *paths; DISPLAYCONFIG_MODE_INFO *modes;
        UINT32 np, nm;
        if (QueryCCD(&paths, &np, &modes, &nm, QDC_ONLY_ACTIVE_PATHS) != ERROR_SUCCESS) {
            SetStatus(L"Kon schermconfiguratie niet opvragen."); return;
        }

        DISPLAYCONFIG_PATH_INFO filtered[64]; UINT32 nf = 0;
        for (UINT32 i = 0; i < np; i++) {
            if (!(LuidEq(paths[i].targetInfo.adapterId, mon->adapterId) &&
                  paths[i].targetInfo.id == mon->targetId))
                filtered[nf++] = paths[i];
        }
        free(modes);

        LONG rc = SetDisplayConfig(nf, filtered, 0, NULL,
                                   SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                   SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        free(paths);

        if (rc != ERROR_SUCCESS) {
            SetStatus(L"Uitzetten van '%s' mislukt (fout %ld).", mon->name, rc); return;
        }
        SetStatus(L"Scherm '%s' uitgeschakeld.", mon->name);

    } else {
        /* --- Aanzetten: voeg dit scherm toe aan de actieve paden --- */
        DISPLAYCONFIG_PATH_INFO *allPaths; DISPLAYCONFIG_MODE_INFO *allModes;
        UINT32 nap, nam;
        if (QueryCCD(&allPaths, &nap, &allModes, &nam, QDC_ALL_PATHS) != ERROR_SUCCESS) {
            SetStatus(L"Kon schermconfiguratie niet opvragen."); return;
        }

        /* Zoek het pad voor dit target in QDC_ALL_PATHS */
        DISPLAYCONFIG_PATH_INFO newPath; BOOL found = FALSE;
        for (UINT32 i = 0; i < nap; i++) {
            if (LuidEq(allPaths[i].targetInfo.adapterId, mon->adapterId) &&
                allPaths[i].targetInfo.id == mon->targetId) {
                newPath = allPaths[i];
                newPath.flags |= DISPLAYCONFIG_PATH_ACTIVE;
                newPath.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                newPath.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                found = TRUE; break;
            }
        }
        free(allPaths); free(allModes);

        if (!found) { SetStatus(L"Pad voor '%s' niet gevonden.", mon->name); return; }

        /* Huidige actieve paden + nieuw pad */
        DISPLAYCONFIG_PATH_INFO *actPaths; DISPLAYCONFIG_MODE_INFO *actModes;
        UINT32 nact, nactm;
        if (QueryCCD(&actPaths, &nact, &actModes, &nactm, QDC_ONLY_ACTIVE_PATHS) != ERROR_SUCCESS) {
            SetStatus(L"Kon actieve schermen niet opvragen."); return;
        }

        DISPLAYCONFIG_PATH_INFO combined[65];
        memcpy(combined, actPaths, nact * sizeof(*actPaths));
        combined[nact] = newPath;
        free(actPaths); free(actModes);

        LONG rc = SetDisplayConfig(nact + 1, combined, 0, NULL,
                                   SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                   SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        if (rc != ERROR_SUCCESS) {
            SetStatus(L"Aanzetten van '%s' mislukt (fout %ld).", mon->name, rc); return;
        }
        SetStatus(L"Scherm '%s' ingeschakeld.", mon->name);
    }

    /* Herlaad de kaart om de nieuwe live-staat te tonen */
    LoadMonitors();
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, TRUE);
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
        PaintCanvas(hwnd);
        return 0;

    case WM_LBUTTONDOWN: {
        RECT cr; GetClientRect(hwnd, &cr);
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        int idx = HitTest(cr.right, cr.bottom, mx, my);
        if (idx < 0) return 0;

        /* Blokkeer uitzetten van het laatste actieve scherm */
        if (g_mon[idx].enabled) {
            int aantalAan = 0;
            for (int k = 0; k < g_nMon; k++) aantalAan += g_mon[k].enabled;
            if (aantalAan <= 1) {
                SetStatus(L"'%s' kan niet uit: minimaal één scherm moet aan blijven.",
                          g_mon[idx].name);
                return 0;
            }
        }

        /* Pas direct toe in Windows */
        ToggleMonitorLive(idx);
        return 0;
    }

    case WM_SETCURSOR: {
        RECT cr; GetClientRect(hwnd, &cr);
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        if (HitTest(cr.right, cr.bottom, pt.x, pt.y) >= 0) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ======================================================== */
/* Presetmap                                                */
/* ======================================================== */

static BOOL GetPresetDir(WCHAR *out, size_t cch)
{
    WCHAR appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata)))
        return FALSE;
    StringCchPrintfW(out, cch, L"%s\\SchermPresets", appdata);
    CreateDirectoryW(out, NULL);
    return TRUE;
}

static BOOL GetPresetPath(const WCHAR *name, WCHAR *out, size_t cch)
{
    WCHAR dir[MAX_PATH];
    if (!GetPresetDir(dir, MAX_PATH)) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s%s", dir, name, PRESET_EXT));
}

static BOOL IsValidName(const WCHAR *name)
{
    if (!name[0] || wcslen(name) > 100) return FALSE;
    return wcspbrk(name, L"\\/:*?\"<>|") == NULL;
}

/* ======================================================== */
/* Preset opslaan (met toggles uit de kaart)                */
/* ======================================================== */

static BOOL SaveCurrentAsPreset(const WCHAR *name)
{
    /* Sla de huidige live schermindeling op — klikken in de kaart past die
     * al direct aan in Windows, dus QDC_ONLY_ACTIVE_PATHS is altijd correct. */
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    UINT32 np = 0, nm = 0;

    LONG rc = QueryCCD(&paths, &np, &modes, &nm, QDC_ONLY_ACTIVE_PATHS);
    if (rc != ERROR_SUCCESS) {
        SetStatus(L"Kon schermconfiguratie niet opvragen (fout %ld).", rc);
        return FALSE;
    }

    AdapterRecord adapters[MAX_ADAPTERS];
    DWORD na = CollectAdapters(paths, np, adapters, MAX_ADAPTERS);

    WCHAR fpath[MAX_PATH];
    if (!GetPresetPath(name, fpath, MAX_PATH)) { free(paths); free(modes); return FALSE; }

    HANDLE h = CreateFileW(fpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        SetStatus(L"Kon presetbestand niet aanmaken.");
        free(paths); free(modes); return FALSE;
    }

    PresetHeader hdr = { PRESET_MAGIC, PRESET_VERSION, np, nm, na };
    DWORD w;
    BOOL ok = WriteFile(h, &hdr,     sizeof(hdr),         &w, NULL) &&
              WriteFile(h, paths,    np * sizeof(*paths),  &w, NULL) &&
              WriteFile(h, modes,    nm * sizeof(*modes),  &w, NULL) &&
              WriteFile(h, adapters, na * sizeof(*adapters), &w, NULL);
    CloseHandle(h);
    free(paths); free(modes);

    if (!ok) { DeleteFileW(fpath); SetStatus(L"Schrijven van preset is mislukt."); return FALSE; }
    SetStatus(L"Preset '%s' opgeslagen (%u actieve schermen).", name, np);
    return TRUE;
}

/* ======================================================== */
/* Preset toepassen                                         */
/* ======================================================== */

static void RemapAdapters(DISPLAYCONFIG_PATH_INFO *paths, DWORD np,
                          DISPLAYCONFIG_MODE_INFO *modes, DWORD nm,
                          const AdapterRecord *saved, DWORD ns)
{
    DISPLAYCONFIG_PATH_INFO *cp = NULL; DISPLAYCONFIG_MODE_INFO *cm = NULL;
    UINT32 cnp = 0, cnm = 0;
    if (QueryCCD(&cp, &cnp, &cm, &cnm, QDC_ALL_PATHS) != ERROR_SUCCESS) return;
    AdapterRecord cur[MAX_ADAPTERS];
    DWORD nc = CollectAdapters(cp, cnp, cur, MAX_ADAPTERS);
    free(cp); free(cm);

    for (DWORD i = 0; i < ns; i++) {
        LUID nid; BOOL found = FALSE;
        for (DWORD j = 0; j < nc; j++)
            if (_wcsicmp(saved[i].devicePath, cur[j].devicePath) == 0) {
                nid = cur[j].id; found = TRUE; break;
            }
        if (!found || LuidEq(saved[i].id, nid)) continue;
        for (DWORD p = 0; p < np; p++) {
            if (LuidEq(paths[p].sourceInfo.adapterId, saved[i].id))
                paths[p].sourceInfo.adapterId = nid;
            if (LuidEq(paths[p].targetInfo.adapterId, saved[i].id))
                paths[p].targetInfo.adapterId = nid;
        }
        for (DWORD m = 0; m < nm; m++)
            if (LuidEq(modes[m].adapterId, saved[i].id))
                modes[m].adapterId = nid;
    }
}

static BOOL ApplyPreset(const WCHAR *name)
{
    WCHAR fpath[MAX_PATH];
    if (!GetPresetPath(name, fpath, MAX_PATH)) return FALSE;

    HANDLE h = CreateFileW(fpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        SetStatus(L"Kon preset '%s' niet openen.", name); return FALSE;
    }

    PresetHeader hdr; DWORD r;
    BOOL ok = ReadFile(h, &hdr, sizeof(hdr), &r, NULL) && r == sizeof(hdr) &&
              hdr.magic == PRESET_MAGIC && hdr.version == PRESET_VERSION &&
              hdr.numPaths > 0 && hdr.numPaths <= 64 &&
              hdr.numModes <= 128 && hdr.numAdapters <= MAX_ADAPTERS;
    if (!ok) {
        CloseHandle(h);
        SetStatus(L"Preset '%s' is beschadigd.", name); return FALSE;
    }

    DISPLAYCONFIG_PATH_INFO *paths = calloc(hdr.numPaths, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(hdr.numModes ? hdr.numModes : 1, sizeof(*modes));
    AdapterRecord adapters[MAX_ADAPTERS];

    ok = paths && modes &&
         ReadFile(h, paths,    hdr.numPaths    * sizeof(*paths),    &r, NULL) && r == hdr.numPaths    * sizeof(*paths) &&
         ReadFile(h, modes,    hdr.numModes    * sizeof(*modes),    &r, NULL) && r == hdr.numModes    * sizeof(*modes) &&
         ReadFile(h, adapters, hdr.numAdapters * sizeof(*adapters), &r, NULL) && r == hdr.numAdapters * sizeof(*adapters);
    CloseHandle(h);

    if (!ok) { free(paths); free(modes); SetStatus(L"Lezen van preset mislukt."); return FALSE; }

    RemapAdapters(paths, hdr.numPaths, modes, hdr.numModes, adapters, hdr.numAdapters);

    LONG rc = SetDisplayConfig(hdr.numPaths, paths, hdr.numModes, modes,
                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                               SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    free(paths); free(modes);

    if (rc != ERROR_SUCCESS) {
        SetStatus(L"Toepassen van '%s' mislukt (fout %ld). Zijn alle schermen aangesloten?",
                  name, rc);
        return FALSE;
    }
    SetStatus(L"Preset '%s' toegepast.", name);

    /* Ververs de kaart zodat de nieuwe indeling zichtbaar is */
    LoadMonitors();
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, TRUE);
    return TRUE;
}

/* ======================================================== */
/* Snel schakelen                                           */
/* ======================================================== */

static void ApplyTopology(UINT32 topology, const WCHAR *label)
{
    LONG rc = SetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | topology);
    if (rc == ERROR_SUCCESS) {
        SetStatus(L"Modus '%s' toegepast.", label);
        LoadMonitors();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, TRUE);
    } else
        SetStatus(L"Modus '%s' mislukt (fout %ld).", label, rc);
}

/* ======================================================== */
/* Presetlijst                                              */
/* ======================================================== */

static void RefreshList(void)
{
    SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);
    WCHAR dir[MAX_PATH], pat[MAX_PATH];
    if (!GetPresetDir(dir, MAX_PATH)) return;
    StringCchPrintfW(pat, MAX_PATH, L"%s\\*%s", dir, PRESET_EXT);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR *dot = wcsrchr(fd.cFileName, L'.');
        if (dot) *dot = 0;
        SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static BOOL GetSelPreset(WCHAR *out, size_t cch)
{
    LRESULT sel = SendMessageW(g_hList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return FALSE;
    if (SendMessageW(g_hList, LB_GETTEXTLEN, sel, 0) >= (LRESULT)cch) return FALSE;
    SendMessageW(g_hList, LB_GETTEXT, sel, (LPARAM)out);
    return TRUE;
}

static void SelectByName(const WCHAR *name)
{
    LRESULT idx = SendMessageW(g_hList, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)name);
    if (idx != LB_ERR) SendMessageW(g_hList, LB_SETCURSEL, idx, 0);
}

/* ======================================================== */
/* Knopacties                                               */
/* ======================================================== */

static void OnSave(HWND hwnd)
{
    WCHAR name[128] = L"";
    GetWindowTextW(g_hEdit, name, 128);
    if (!name[0] && !GetSelPreset(name, 128)) {
        SetStatus(L"Geef een naam op voor de nieuwe preset."); return;
    }
    if (!IsValidName(name)) {
        SetStatus(L"Ongeldige naam: gebruik geen \\ / : * ? \" < > |"); return;
    }
    WCHAR fpath[MAX_PATH];
    if (GetPresetPath(name, fpath, MAX_PATH) &&
        GetFileAttributesW(fpath) != INVALID_FILE_ATTRIBUTES) {
        WCHAR msg[256];
        StringCchPrintfW(msg, 256, L"Preset '%s' bestaat al. Overschrijven?", name);
        if (MessageBoxW(hwnd, msg, L"SchermPresets", MB_YESNO|MB_ICONQUESTION) != IDYES) return;
    }
    if (SaveCurrentAsPreset(name)) {
        SetWindowTextW(g_hEdit, L"");
        RefreshList();
        SelectByName(name);
    }
}

static void OnApply(void)
{
    WCHAR name[128];
    if (!GetSelPreset(name, 128)) {
        SetStatus(L"Selecteer eerst een preset in de lijst."); return;
    }
    ApplyPreset(name);
}

static void OnDelete(HWND hwnd)
{
    WCHAR name[128];
    if (!GetSelPreset(name, 128)) {
        SetStatus(L"Selecteer eerst een preset in de lijst."); return;
    }
    WCHAR msg[256];
    StringCchPrintfW(msg, 256, L"Preset '%s' verwijderen?", name);
    if (MessageBoxW(hwnd, msg, L"SchermPresets", MB_YESNO|MB_ICONQUESTION) != IDYES) return;
    WCHAR fpath[MAX_PATH];
    if (GetPresetPath(name, fpath, MAX_PATH) && DeleteFileW(fpath)) {
        SetStatus(L"Preset '%s' verwijderd.", name);
        RefreshList();
    } else
        SetStatus(L"Verwijderen van '%s' mislukt.", name);
}

static void OnReload(void)
{
    LoadMonitors();
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, TRUE);
    SetStatus(L"Schermindeling herladen (%d scherm(en)).", g_nMon);
}

/* ======================================================== */
/* Vensterconstructie                                       */
/* ======================================================== */

static HWND MakeCtrl(HWND parent, const WCHAR *cls, const WCHAR *text,
                     DWORD style, int x, int y, int w, int h, int id)
{
    HWND hw = CreateWindowExW(0, cls, text, WS_CHILD|WS_VISIBLE|style,
                              S(x), S(y), S(w), S(h), parent,
                              (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(hw, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return hw;
}

static void CreateControls(HWND hwnd)
{
    /* --- Monitorkaart --- */
    MakeCtrl(hwnd, L"STATIC",
             L"Schermen  —  klik op een scherm om het aan/uit te zetten in de preset:",
             0, 12, 8, 516, 16, 0);

    g_hCanvas = CreateWindowExW(WS_EX_CLIENTEDGE, L"SchermPresetsCanvas", NULL,
                                WS_CHILD|WS_VISIBLE,
                                S(12), S(28), S(516), S(148), hwnd,
                                (HMENU)(INT_PTR)IDC_CANVAS, GetModuleHandleW(NULL), NULL);

    MakeCtrl(hwnd, L"BUTTON", L"Indeling herladen", 0, 12, 182, 148, 24, IDC_BTN_RELOAD);

    /* --- Presetlijst --- */
    MakeCtrl(hwnd, L"STATIC", L"Opgeslagen presets:", 0, 12, 214, 280, 16, 0);
    g_hList = MakeCtrl(hwnd, L"LISTBOX", NULL,
                       WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_SORT,
                       12, 232, 280, 160, IDC_LIST);

    MakeCtrl(hwnd, L"BUTTON", L"Toepassen", BS_DEFPUSHBUTTON, 304, 232, 148, 30, IDC_BTN_APPLY);
    MakeCtrl(hwnd, L"BUTTON", L"Verwijderen",      0, 304, 268, 148, 26, IDC_BTN_DELETE);
    MakeCtrl(hwnd, L"BUTTON", L"Lijst vernieuwen", 0, 304, 300, 148, 26, IDC_BTN_REFRESH);
    MakeCtrl(hwnd, L"BUTTON", L"Presetmap openen", 0, 304, 332, 148, 26, IDC_BTN_FOLDER);

    /* --- Opslaan --- */
    MakeCtrl(hwnd, L"STATIC", L"Naam nieuwe preset:", 0, 12, 400, 280, 16, 0);
    g_hEdit = MakeCtrl(hwnd, L"EDIT", NULL, WS_BORDER|ES_AUTOHSCROLL,
                       12, 418, 280, 24, IDC_EDIT_NAME);
    MakeCtrl(hwnd, L"BUTTON", L"Opslaan als preset", 0, 304, 416, 148, 28, IDC_BTN_SAVE);

    /* --- Snel schakelen --- */
    MakeCtrl(hwnd, L"BUTTON", L"Snel schakelen (zoals Win+P)", BS_GROUPBOX,
             12, 452, 516, 58, 0);
    MakeCtrl(hwnd, L"BUTTON", L"Uitbreiden",    0,  24, 476, 114, 26, IDC_BTN_EXTEND);
    MakeCtrl(hwnd, L"BUTTON", L"Dupliceren",    0, 146, 476, 114, 26, IDC_BTN_CLONE);
    MakeCtrl(hwnd, L"BUTTON", L"Alleen scherm 1", 0, 268, 476, 114, 26, IDC_BTN_FIRST);
    MakeCtrl(hwnd, L"BUTTON", L"Alleen scherm 2", 0, 390, 476, 114, 26, IDC_BTN_SECOND);

    /* --- Status --- */
    g_hStatus = MakeCtrl(hwnd, L"STATIC", L"Klaar.", SS_LEFTNOWORDWRAP,
                         12, 520, 516, 16, IDC_STATUS);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        LoadMonitors();
        RefreshList();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_LIST:
            if (HIWORD(wParam) == LBN_DBLCLK) OnApply();
            return 0;
        case IDC_BTN_SAVE:    OnSave(hwnd);   return 0;
        case IDC_BTN_APPLY:   OnApply();      return 0;
        case IDC_BTN_DELETE:  OnDelete(hwnd); return 0;
        case IDC_BTN_REFRESH: RefreshList();  SetStatus(L"Lijst vernieuwd."); return 0;
        case IDC_BTN_FOLDER: {
            WCHAR dir[MAX_PATH];
            if (GetPresetDir(dir, MAX_PATH))
                ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        case IDC_BTN_RELOAD:  OnReload();     return 0;
        case IDC_BTN_EXTEND:  ApplyTopology(SDC_TOPOLOGY_EXTEND,   L"Uitbreiden");       return 0;
        case IDC_BTN_CLONE:   ApplyTopology(SDC_TOPOLOGY_CLONE,    L"Dupliceren");       return 0;
        case IDC_BTN_FIRST:   ApplyTopology(SDC_TOPOLOGY_INTERNAL, L"Alleen scherm 1");  return 0;
        case IDC_BTN_SECOND:  ApplyTopology(SDC_TOPOLOGY_EXTERNAL, L"Alleen scherm 2");  return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ======================================================== */
/* Ingang                                                   */
/* ======================================================== */

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR cmdLine, int nCmdShow)
{
    (void)hPrev; (void)cmdLine;

    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);

    g_hFont = CreateFontW(-MulDiv(9, g_dpi, 72), 0,0,0, FW_NORMAL,0,0,0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI");
    g_hFontSm = CreateFontW(-MulDiv(8, g_dpi, 72), 0,0,0, FW_NORMAL,0,0,0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI");

    /* Registreer canvasklasse */
    WNDCLASSW cc = {0};
    cc.lpfnWndProc   = CanvasProc;
    cc.hInstance     = hInstance;
    cc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    cc.hbrBackground = NULL;
    cc.lpszClassName = L"SchermPresetsCanvas";
    RegisterClassW(&cc);

    /* Registreer hoofdvensterklasse */
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SchermPresetsWnd";
    wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    RECT rc = { 0, 0, S(540), S(544) };
    AdjustWindowRect(&rc, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX, FALSE);

    HWND hwnd = CreateWindowExW(0, L"SchermPresetsWnd", L"SchermPresets",
                                WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g_hFont);
    DeleteObject(g_hFontSm);
    return 0;
}
