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
#include <commctrl.h>
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
    int    width, height; /* resolutie voor het label              */
    LUID   adapterId;
    UINT32 targetId;
    UINT32 sourceId;
} Monitor;

static Monitor g_mon[MAX_MON];
static int     g_nMon;
static int     g_hover = -1;   /* monitor onder de muis, -1 = geen */
static HWND    g_hCanvas;

/* Sleepstatus: klik zonder beweging = aan/uit, klik + beweging = verplaatsen */
static int   g_dragIdx  = -1;
static BOOL  g_dragging = FALSE;
static POINT g_dragStart;
static int   g_dragDx, g_dragDy;   /* huidige sleepafstand in canvas-pixels */

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
#define IDC_HINT        113

static HWND  g_hList, g_hEdit, g_hStatus;
static HFONT g_hFont, g_hFontSm, g_hFontBold, g_hFontBig;
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
    g_hover = -1;
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
                mon->width  = (int)sm->width;
                mon->height = (int)sm->height;
                if (mon->vRect.right > placeholderX) placeholderX = mon->vRect.right;
            } else {
                mon->vRect.left = placeholderX; mon->vRect.top = 0;
                mon->vRect.right = placeholderX + 1920; mon->vRect.bottom = 1080;
                mon->width = 1920; mon->height = 1080;
                placeholderX += 1920 + 40;
            }
        } else {
            /* Inactieve monitor: placeholder rechts van de actieve schermen */
            mon->vRect.left   = placeholderX + 40;
            mon->vRect.top    = 0;
            mon->vRect.right  = placeholderX + 40 + 1920;
            mon->vRect.bottom = 1080;
            mon->width = 0; mon->height = 0;
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

/* Transformatie tussen virtueel bureaublad en canvas-pixels */
typedef struct {
    LONG vl, vt, vw, vh;   /* bounding box virtueel bureaublad */
    int  aw, ah, ox, oy;   /* schaalgebied + offset op het canvas */
} Xform;

static void GetXform(int cw, int ch, Xform *x)
{
    LONG vl = g_mon[0].vRect.left, vt = g_mon[0].vRect.top;
    LONG vr = g_mon[0].vRect.right, vb = g_mon[0].vRect.bottom;
    for (int k = 1; k < g_nMon; k++) {
        if (g_mon[k].vRect.left   < vl) vl = g_mon[k].vRect.left;
        if (g_mon[k].vRect.top    < vt) vt = g_mon[k].vRect.top;
        if (g_mon[k].vRect.right  > vr) vr = g_mon[k].vRect.right;
        if (g_mon[k].vRect.bottom > vb) vb = g_mon[k].vRect.bottom;
    }
    x->vl = vl; x->vt = vt;
    x->vw = vr - vl; x->vh = vb - vt;
    if (x->vw <= 0) x->vw = 1;
    if (x->vh <= 0) x->vh = 1;

    const int PAD = 8;
    int aw = cw - 2*PAD, ah = ch - 2*PAD;
    if (aw * x->vh > ah * x->vw)
        aw = (int)((LONGLONG)ah * x->vw / x->vh);
    else
        ah = (int)((LONGLONG)aw * x->vh / x->vw);
    if (aw < 1) aw = 1;
    if (ah < 1) ah = 1;
    x->aw = aw; x->ah = ah;
    x->ox = PAD + (cw - 2*PAD - aw) / 2;
    x->oy = PAD + (ch - 2*PAD - ah) / 2;
}

/* Schaal de virtuele monitorpositie naar canvas-coördinaten. */
static RECT ScaleMonRect(int cw, int ch, int i)
{
    Xform x;
    GetXform(cw, ch, &x);

    const RECT *r = &g_mon[i].vRect;
    RECT out;
    out.left   = x.ox + (int)((LONGLONG)(r->left   - x.vl) * x.aw / x.vw);
    out.top    = x.oy + (int)((LONGLONG)(r->top    - x.vt) * x.ah / x.vh);
    out.right  = x.ox + (int)((LONGLONG)(r->right  - x.vl) * x.aw / x.vw);
    out.bottom = x.oy + (int)((LONGLONG)(r->bottom - x.vt) * x.ah / x.vh);
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

static COLORREF Lighten(COLORREF c, int d)
{
    return RGB(min(255, GetRValue(c) + d),
               min(255, GetGValue(c) + d),
               min(255, GetBValue(c) + d));
}

static void PaintCanvas(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    int cw = cr.right, ch = cr.bottom;

    /* Double buffering: teken alles in een geheugen-DC tegen flikkeren */
    HDC hdc = CreateCompatibleDC(wdc);
    HBITMAP hBmp = CreateCompatibleBitmap(wdc, cw, ch);
    HBITMAP hOldBmp = SelectObject(hdc, hBmp);

    /* Donkere achtergrond */
    HBRUSH hBg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(hdc, &cr, hBg);
    DeleteObject(hBg);

    SetBkMode(hdc, TRANSPARENT);

    if (g_nMon == 0) {
        SetTextColor(hdc, RGB(150, 155, 165));
        SelectObject(hdc, g_hFontSm);
        DrawTextW(hdc, L"Geen schermen gevonden. Klik op 'Indeling herladen'.",
                  -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* Teken het gesleepte scherm als laatste zodat het bovenop ligt */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < g_nMon; i++) {
        BOOL isDrag = (g_dragging && i == g_dragIdx);
        if ((pass == 0) == isDrag) continue;

        RECT r = ScaleMonRect(cw, ch, i);
        if (isDrag) OffsetRect(&r, g_dragDx, g_dragDy);
        COLORREF baseClr = MON_CLR[i % 8];
        BOOL on = g_mon[i].enabled;
        BOOL hov = (i == g_hover) || isDrag;
        int rad = S(8);

        /* Vulling: kleur voor aan, donkergrijs voor uit; hover licht op */
        COLORREF fill = on ? baseClr : RGB(48, 50, 56);
        if (hov) fill = Lighten(fill, 22);

        COLORREF border = on ? Lighten(baseClr, 70) : RGB(95, 98, 108);
        if (hov) border = Lighten(border, 40);

        HBRUSH hFill = CreateSolidBrush(fill);
        HPEN hPen = CreatePen(on ? PS_SOLID : PS_DASH, on ? 2 : 1, border);
        HBRUSH hOldBr = SelectObject(hdc, hFill);
        HPEN hOldPen = SelectObject(hdc, hPen);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
        SelectObject(hdc, hOldBr);
        SelectObject(hdc, hOldPen);
        DeleteObject(hFill);
        DeleteObject(hPen);

        /* Groot schermnummer in het midden (zoals Windows-instellingen) */
        WCHAR num[8];
        StringCchPrintfW(num, 8, L"%d", i + 1);
        SelectObject(hdc, g_hFontBig);
        SetTextColor(hdc, on ? Lighten(baseClr, 110) : RGB(120, 123, 132));
        DrawTextW(hdc, num, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        /* Monitornaam bovenin */
        SelectObject(hdc, g_hFontSm);
        SetTextColor(hdc, on ? RGB(255,255,255) : RGB(150,153,162));
        RECT tr = r;
        InflateRect(&tr, -S(6), -S(4));
        DrawTextW(hdc, g_mon[i].name, -1, &tr,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Onderregel: resolutie links, AAN/UIT-pil rechts */
        if (on && g_mon[i].width > 0) {
            WCHAR res[32];
            StringCchPrintfW(res, 32, L"%d×%d", g_mon[i].width, g_mon[i].height);
            RECT rr = r;
            rr.top = rr.bottom - S(18);
            InflateRect(&rr, -S(6), 0);
            rr.bottom -= S(4);
            SetTextColor(hdc, Lighten(baseClr, 100));
            DrawTextW(hdc, res, -1, &rr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        }

        /* Statuspil rechtsonder */
        {
            const WCHAR *badge = on ? L"AAN" : L"UIT";
            int pw = S(34), ph = S(15);
            RECT pr = { r.right - pw - S(5), r.bottom - ph - S(4),
                        r.right - S(5),      r.bottom - S(4) };
            HBRUSH hPill = CreateSolidBrush(on ? RGB(28, 92, 48) : RGB(96, 40, 44));
            HPEN hPPen = CreatePen(PS_SOLID, 1, on ? RGB(70, 170, 100) : RGB(170, 90, 95));
            HBRUSH ob = SelectObject(hdc, hPill);
            HPEN op = SelectObject(hdc, hPPen);
            RoundRect(hdc, pr.left, pr.top, pr.right, pr.bottom, ph, ph);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(hPill);
            DeleteObject(hPPen);
            SetTextColor(hdc, on ? RGB(190, 255, 205) : RGB(255, 185, 190));
            DrawTextW(hdc, badge, -1, &pr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(wdc, 0, 0, cw, ch, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdc);
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
        free(modes);

        DISPLAYCONFIG_PATH_INFO filtered[64]; UINT32 nf = 0;
        for (UINT32 i = 0; i < np; i++) {
            if (LuidEq(paths[i].targetInfo.adapterId, mon->adapterId) &&
                paths[i].targetInfo.id == mon->targetId) continue;
            filtered[nf] = paths[i];
            /* Wis modeInfoIdx: we geven geen modes mee, SDC_ALLOW_CHANGES
             * laat Windows zelf modi kiezen voor de resterende schermen. */
            filtered[nf].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            filtered[nf].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            nf++;
        }
        free(paths);

        LONG rc = SetDisplayConfig(nf, filtered, 0, NULL,
                                   SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                   SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        if (rc != ERROR_SUCCESS) {
            SetStatus(L"Uitzetten van '%s' mislukt (fout %ld).", mon->name, rc); return;
        }
        SetStatus(L"Scherm '%s' uitgeschakeld.", mon->name);

    } else {
        /* --- Aanzetten: voeg dit scherm toe aan de actieve paden ---
         *
         * Strategie: pak de huidige actieve paden, voeg het gewenste
         * inactive pad toe, wis alle modeInfoIdx en laat Windows de
         * modi opnieuw berekenen via SDC_ALLOW_CHANGES.              */
        DISPLAYCONFIG_PATH_INFO *allPaths; DISPLAYCONFIG_MODE_INFO *allModes;
        UINT32 nap, nam;
        if (QueryCCD(&allPaths, &nap, &allModes, &nam, QDC_ALL_PATHS) != ERROR_SUCCESS) {
            SetStatus(L"Kon schermconfiguratie niet opvragen."); return;
        }
        free(allModes);

        /* Zoek het pad voor dit target */
        DISPLAYCONFIG_PATH_INFO newPath; BOOL found = FALSE;
        for (UINT32 i = 0; i < nap && !found; i++) {
            if (LuidEq(allPaths[i].targetInfo.adapterId, mon->adapterId) &&
                allPaths[i].targetInfo.id == mon->targetId) {
                newPath = allPaths[i];
                found = TRUE;
            }
        }
        free(allPaths);
        if (!found) { SetStatus(L"Pad voor '%s' niet gevonden.", mon->name); return; }

        /* Huidige actieve paden ophalen */
        DISPLAYCONFIG_PATH_INFO *actPaths; DISPLAYCONFIG_MODE_INFO *actModes;
        UINT32 nact, nactm;
        if (QueryCCD(&actPaths, &nact, &actModes, &nactm, QDC_ONLY_ACTIVE_PATHS) != ERROR_SUCCESS) {
            SetStatus(L"Kon actieve schermen niet opvragen."); return;
        }
        free(actModes);

        /* Combineer: bestaande actieve paden + nieuw pad */
        DISPLAYCONFIG_PATH_INFO combined[65];
        for (UINT32 i = 0; i < nact; i++) {
            combined[i] = actPaths[i];
            combined[i].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            combined[i].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        }
        free(actPaths);

        newPath.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        newPath.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        newPath.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        combined[nact] = newPath;

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

/* ======================================================== */
/* Monitor verslepen: snappen en toepassen                  */
/* ======================================================== */

static BOOL RectsOverlap(const RECT *a, const RECT *b)
{
    return a->left < b->right && b->left < a->right &&
           a->top < b->bottom && b->top < a->bottom;
}

static BOOL OverlapsOtherEnabled(int idx, const RECT *r)
{
    for (int k = 0; k < g_nMon; k++) {
        if (k == idx || !g_mon[k].enabled) continue;
        if (RectsOverlap(r, &g_mon[k].vRect)) return TRUE;
    }
    return FALSE;
}

/* Windows eist dat schermen elkaar raken zonder overlap. Zoek de geldige
 * positie (tegen een zijde van een ander scherm) die het dichtst bij de
 * losgelaten positie ligt. */
static void SnapToLayout(int idx, LONG *px, LONG *py)
{
    LONG w = g_mon[idx].vRect.right  - g_mon[idx].vRect.left;
    LONG h = g_mon[idx].vRect.bottom - g_mon[idx].vRect.top;
    LONGLONG bestD = -1;
    LONG bx = *px, by = *py;
    BOOL anyOther = FALSE;

    for (int k = 0; k < g_nMon; k++) {
        if (k == idx || !g_mon[k].enabled) continue;
        anyOther = TRUE;
        const RECT *o = &g_mon[k].vRect;

        /* Klem de vrije as zodat de schermen elkaar blijven raken */
        LONG cy = *py;
        if (cy < o->top - h + 1)  cy = o->top - h + 1;
        if (cy > o->bottom - 1)   cy = o->bottom - 1;
        LONG cx = *px;
        if (cx < o->left - w + 1) cx = o->left - w + 1;
        if (cx > o->right - 1)    cx = o->right - 1;

        POINT cand[4] = {
            { o->right,    cy },   /* rechts van dit scherm */
            { o->left - w, cy },   /* links                 */
            { cx, o->bottom },     /* eronder               */
            { cx, o->top - h },    /* erboven               */
        };
        for (int c = 0; c < 4; c++) {
            RECT t = { cand[c].x, cand[c].y, cand[c].x + w, cand[c].y + h };
            if (OverlapsOtherEnabled(idx, &t)) continue;
            LONGLONG dx = (LONGLONG)cand[c].x - *px;
            LONGLONG dy = (LONGLONG)cand[c].y - *py;
            LONGLONG d = dx*dx + dy*dy;
            if (bestD < 0 || d < bestD) { bestD = d; bx = cand[c].x; by = cand[c].y; }
        }
    }

    if (!anyOther) { *px = 0; *py = 0; return; }   /* enige actieve scherm */
    if (bestD >= 0) { *px = bx; *py = by; }
    else { *px = g_mon[idx].vRect.left; *py = g_mon[idx].vRect.top; }  /* geen plek: terug */
}

static void ApplyMonitorPosition(int idx, LONG nx, LONG ny)
{
    Monitor *mon = &g_mon[idx];

    if (nx == mon->vRect.left && ny == mon->vRect.top) {
        SetStatus(L"Positie van '%s' ongewijzigd.", mon->name);
        return;
    }

    DISPLAYCONFIG_PATH_INFO *paths; DISPLAYCONFIG_MODE_INFO *modes;
    UINT32 np, nm;
    if (QueryCCD(&paths, &np, &modes, &nm, QDC_ONLY_ACTIVE_PATHS) != ERROR_SUCCESS) {
        SetStatus(L"Kon schermconfiguratie niet opvragen.");
        return;
    }

    /* Zoek de bronmodus van het gesleepte scherm */
    UINT32 srcIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    for (UINT32 i = 0; i < np; i++) {
        if (LuidEq(paths[i].targetInfo.adapterId, mon->adapterId) &&
            paths[i].targetInfo.id == mon->targetId) {
            srcIdx = paths[i].sourceInfo.modeInfoIdx;
            break;
        }
    }
    if (srcIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || srcIdx >= nm ||
        modes[srcIdx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        free(paths); free(modes);
        SetStatus(L"Kon bronmodus van '%s' niet vinden.", mon->name);
        return;
    }

    /* Onthoud welke bron primair is (positie 0,0) vóór de wijziging */
    int primIdx = -1;
    for (UINT32 m = 0; m < nm; m++) {
        if (modes[m].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
            modes[m].sourceMode.position.x == 0 &&
            modes[m].sourceMode.position.y == 0) { primIdx = (int)m; break; }
    }

    modes[srcIdx].sourceMode.position.x = nx;
    modes[srcIdx].sourceMode.position.y = ny;

    /* Normaliseer: de primaire bron moet op (0,0) blijven */
    if (primIdx >= 0) {
        LONG ox = modes[primIdx].sourceMode.position.x;
        LONG oy = modes[primIdx].sourceMode.position.y;
        if (ox != 0 || oy != 0) {
            for (UINT32 m = 0; m < nm; m++) {
                if (modes[m].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    modes[m].sourceMode.position.x -= ox;
                    modes[m].sourceMode.position.y -= oy;
                }
            }
        }
    }

    LONG rc = SetDisplayConfig(np, paths, nm, modes,
                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                               SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    free(paths); free(modes);

    if (rc != ERROR_SUCCESS)
        SetStatus(L"Verplaatsen van '%s' mislukt (fout %ld).", mon->name, rc);
    else
        SetStatus(L"Scherm '%s' verplaatst.", mon->name);

    LoadMonitors();
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, TRUE);
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
        PaintCanvas(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* alles wordt in PaintCanvas getekend (double buffered) */

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);

        /* Bezig met (mogelijk) slepen? */
        if (g_dragIdx >= 0 && (wParam & MK_LBUTTON)) {
            int dx = mx - g_dragStart.x, dy = my - g_dragStart.y;
            if (!g_dragging && g_mon[g_dragIdx].enabled &&
                (abs(dx) > S(4) || abs(dy) > S(4)))
                g_dragging = TRUE;
            if (g_dragging) {
                g_dragDx = dx; g_dragDy = dy;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        RECT cr; GetClientRect(hwnd, &cr);
        int idx = HitTest(cr.right, cr.bottom, mx, my);
        if (idx != g_hover) {
            g_hover = idx;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hover != -1 && !g_dragging) {
            g_hover = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        RECT cr; GetClientRect(hwnd, &cr);
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        int idx = HitTest(cr.right, cr.bottom, mx, my);
        if (idx < 0) return 0;

        /* Start mogelijke sleepactie; de beslissing klik-of-sleep valt
         * pas bij het loslaten van de muisknop. */
        g_dragIdx = idx;
        g_dragging = FALSE;
        g_dragStart.x = mx; g_dragStart.y = my;
        g_dragDx = g_dragDy = 0;
        SetCapture(hwnd);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_dragIdx < 0) return 0;
        int idx = g_dragIdx;
        BOOL wasDrag = g_dragging;
        g_dragIdx = -1;
        g_dragging = FALSE;
        ReleaseCapture();

        if (wasDrag) {
            /* Sleepafstand terugvertalen naar het virtuele bureaublad */
            RECT cr; GetClientRect(hwnd, &cr);
            Xform x;
            GetXform(cr.right, cr.bottom, &x);
            LONG nx = g_mon[idx].vRect.left + (LONG)((LONGLONG)g_dragDx * x.vw / x.aw);
            LONG ny = g_mon[idx].vRect.top  + (LONG)((LONGLONG)g_dragDy * x.vh / x.ah);
            g_dragDx = g_dragDy = 0;

            SnapToLayout(idx, &nx, &ny);
            ApplyMonitorPosition(idx, nx, ny);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Gewone klik: scherm aan/uit. Blokkeer het laatste actieve scherm. */
        if (g_mon[idx].enabled) {
            int aantalAan = 0;
            for (int k = 0; k < g_nMon; k++) aantalAan += g_mon[k].enabled;
            if (aantalAan <= 1) {
                SetStatus(L"'%s' kan niet uit: minimaal één scherm moet aan blijven.",
                          g_mon[idx].name);
                return 0;
            }
        }
        ToggleMonitorLive(idx);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (g_dragIdx >= 0) {
            g_dragIdx = -1;
            g_dragging = FALSE;
            g_dragDx = g_dragDy = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

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

static HWND MakeHeader(HWND parent, const WCHAR *text, int x, int y, int w)
{
    HWND h = MakeCtrl(parent, L"STATIC", text, 0, x, y, w, 17, 0);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    return h;
}

static void CreateControls(HWND hwnd)
{
    /* --- Monitorkaart --- */
    MakeHeader(hwnd, L"Schermen", 14, 10, 200);

    g_hCanvas = CreateWindowExW(0, L"SchermPresetsCanvas", NULL,
                                WS_CHILD|WS_VISIBLE|WS_BORDER,
                                S(14), S(30), S(532), S(170), hwnd,
                                (HMENU)(INT_PTR)IDC_CANVAS, GetModuleHandleW(NULL), NULL);

    MakeCtrl(hwnd, L"BUTTON", L"Indeling herladen", 0, 14, 208, 140, 26, IDC_BTN_RELOAD);
    MakeCtrl(hwnd, L"STATIC",
             L"Klik = scherm aan/uit  ·  Slepen = scherm verplaatsen",
             SS_LEFTNOWORDWRAP | SS_CENTERIMAGE, 164, 208, 382, 26, IDC_HINT);

    /* --- Presetlijst --- */
    MakeHeader(hwnd, L"Opgeslagen presets", 14, 248, 280);
    g_hList = MakeCtrl(hwnd, L"LISTBOX", NULL,
                       WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_SORT,
                       14, 268, 300, 148, IDC_LIST);

    MakeCtrl(hwnd, L"BUTTON", L"Toepassen", BS_DEFPUSHBUTTON, 326, 268, 220, 32, IDC_BTN_APPLY);
    MakeCtrl(hwnd, L"BUTTON", L"Verwijderen",      0, 326, 306, 220, 26, IDC_BTN_DELETE);
    MakeCtrl(hwnd, L"BUTTON", L"Lijst vernieuwen", 0, 326, 338, 220, 26, IDC_BTN_REFRESH);
    MakeCtrl(hwnd, L"BUTTON", L"Presetmap openen", 0, 326, 370, 220, 26, IDC_BTN_FOLDER);

    /* --- Opslaan --- */
    MakeHeader(hwnd, L"Nieuwe preset", 14, 428, 280);
    g_hEdit = MakeCtrl(hwnd, L"EDIT", NULL, WS_BORDER|ES_AUTOHSCROLL,
                       14, 448, 300, 26, IDC_EDIT_NAME);
    SendMessageW(g_hEdit, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"Naam, bijv. \"Gamen\" of \"Werk\"");
    MakeCtrl(hwnd, L"BUTTON", L"Huidige indeling opslaan", 0, 326, 447, 220, 28, IDC_BTN_SAVE);

    /* --- Snel schakelen --- */
    MakeCtrl(hwnd, L"BUTTON", L"Snel schakelen (zoals Win+P)", BS_GROUPBOX,
             14, 484, 532, 62, 0);
    MakeCtrl(hwnd, L"BUTTON", L"Uitbreiden",      0,  26, 508, 122, 28, IDC_BTN_EXTEND);
    MakeCtrl(hwnd, L"BUTTON", L"Dupliceren",      0, 156, 508, 122, 28, IDC_BTN_CLONE);
    MakeCtrl(hwnd, L"BUTTON", L"Alleen scherm 1", 0, 286, 508, 122, 28, IDC_BTN_FIRST);
    MakeCtrl(hwnd, L"BUTTON", L"Alleen scherm 2", 0, 416, 508, 122, 28, IDC_BTN_SECOND);

    /* --- Status --- */
    g_hStatus = MakeCtrl(hwnd, L"STATIC", L"Klaar.", SS_LEFTNOWORDWRAP,
                         14, 556, 532, 17, IDC_STATUS);
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

    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID((HWND)lParam) == IDC_HINT) {
            SetTextColor((HDC)wParam, RGB(105, 108, 115));
            SetBkColor((HDC)wParam, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
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
    g_hFontBold = CreateFontW(-MulDiv(10, g_dpi, 72), 0,0,0, FW_SEMIBOLD,0,0,0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI");
    g_hFontBig = CreateFontW(-MulDiv(20, g_dpi, 72), 0,0,0, FW_BOLD,0,0,0,
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

    RECT rc = { 0, 0, S(560), S(582) };
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
    DeleteObject(g_hFontBold);
    DeleteObject(g_hFontBig);
    return 0;
}
