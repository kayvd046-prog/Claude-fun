/*
 * SchermPresets - beeldschermconfiguraties opslaan en terugzetten als presets.
 *
 * Gebruikt de Windows CCD API (QueryDisplayConfig / SetDisplayConfig) om de
 * volledige actieve schermconfiguratie (welke schermen aan/uit staan,
 * uitbreiden/dupliceren, resolutie, positie, refresh rate) op te slaan in
 * %APPDATA%\SchermPresets en later weer toe te passen.
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
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdlib.h>

/* ---- Bestandsformaat ---- */

#define PRESET_MAGIC   0x53505253u  /* "SRPS" */
#define PRESET_VERSION 1u
#define PRESET_EXT     L".spreset"

typedef struct {
    DWORD magic;
    DWORD version;
    DWORD numPaths;
    DWORD numModes;
    DWORD numAdapters;
} PresetHeader;

/* Adapter-LUID's veranderen na een herstart; daarom slaan we per LUID het
 * apparaatpad van de adapter op, zodat we bij het toepassen de oude LUID's
 * kunnen vertalen naar de huidige. */
typedef struct {
    LUID  id;
    WCHAR devicePath[128];
} AdapterRecord;

#define MAX_ADAPTERS 16

/* ---- GUI ---- */

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

static HWND g_hList, g_hEdit, g_hStatus;
static HFONT g_hFont;
static int g_dpi = 96;

#define S(x) MulDiv((x), g_dpi, 96)

static void SetStatus(const WCHAR *fmt, ...)
{
    WCHAR buf[512];
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(buf, 512, fmt, ap);
    va_end(ap);
    SetWindowTextW(g_hStatus, buf);
}

/* ---- Presetmap ---- */

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
    if (!GetPresetDir(dir, MAX_PATH))
        return FALSE;
    StringCchPrintfW(out, cch, L"%s\\%s%s", dir, name, PRESET_EXT);
    return TRUE;
}

static BOOL IsValidPresetName(const WCHAR *name)
{
    if (!name[0] || wcslen(name) > 100)
        return FALSE;
    return wcspbrk(name, L"\\/:*?\"<>|") == NULL;
}

/* ---- CCD-hulpfuncties ---- */

static BOOL LuidEqual(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static BOOL GetAdapterDevicePath(LUID adapterId, WCHAR *out, size_t cch)
{
    DISPLAYCONFIG_ADAPTER_NAME req;
    ZeroMemory(&req, sizeof(req));
    req.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    req.header.size = sizeof(req);
    req.header.adapterId = adapterId;
    if (DisplayConfigGetDeviceInfo(&req.header) != ERROR_SUCCESS)
        return FALSE;
    StringCchCopyW(out, cch, req.adapterDevicePath);
    return TRUE;
}

/* Verzamelt de unieke adapter-LUID's uit een lijst paden, met hun apparaatpad. */
static DWORD CollectAdapters(const DISPLAYCONFIG_PATH_INFO *paths, DWORD numPaths,
                             AdapterRecord *records, DWORD maxRecords)
{
    DWORD count = 0;
    for (DWORD i = 0; i < numPaths; i++) {
        LUID luids[2] = { paths[i].sourceInfo.adapterId, paths[i].targetInfo.adapterId };
        for (int j = 0; j < 2; j++) {
            BOOL known = FALSE;
            for (DWORD k = 0; k < count; k++) {
                if (LuidEqual(records[k].id, luids[j])) {
                    known = TRUE;
                    break;
                }
            }
            if (!known && count < maxRecords &&
                GetAdapterDevicePath(luids[j], records[count].devicePath, 128)) {
                records[count].id = luids[j];
                count++;
            }
        }
    }
    return count;
}

static LONG QueryActiveConfig(DISPLAYCONFIG_PATH_INFO **outPaths, UINT32 *outNumPaths,
                              DISPLAYCONFIG_MODE_INFO **outModes, UINT32 *outNumModes,
                              UINT32 flags)
{
    UINT32 numPaths = 0, numModes = 0;
    LONG rc = GetDisplayConfigBufferSizes(flags, &numPaths, &numModes);
    if (rc != ERROR_SUCCESS)
        return rc;

    DISPLAYCONFIG_PATH_INFO *paths = calloc(numPaths, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(numModes, sizeof(*modes));
    if (!paths || !modes) {
        free(paths);
        free(modes);
        return ERROR_OUTOFMEMORY;
    }

    rc = QueryDisplayConfig(flags, &numPaths, paths, &numModes, modes, NULL);
    if (rc != ERROR_SUCCESS) {
        free(paths);
        free(modes);
        return rc;
    }
    *outPaths = paths;
    *outNumPaths = numPaths;
    *outModes = modes;
    *outNumModes = numModes;
    return ERROR_SUCCESS;
}

/* ---- Preset opslaan ---- */

static BOOL SaveCurrentAsPreset(const WCHAR *name)
{
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    UINT32 numPaths = 0, numModes = 0;

    LONG rc = QueryActiveConfig(&paths, &numPaths, &modes, &numModes, QDC_ONLY_ACTIVE_PATHS);
    if (rc != ERROR_SUCCESS) {
        SetStatus(L"Kon schermconfiguratie niet opvragen (fout %ld).", rc);
        return FALSE;
    }

    AdapterRecord adapters[MAX_ADAPTERS];
    DWORD numAdapters = CollectAdapters(paths, numPaths, adapters, MAX_ADAPTERS);

    WCHAR path[MAX_PATH];
    if (!GetPresetPath(name, path, MAX_PATH)) {
        free(paths);
        free(modes);
        return FALSE;
    }

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        SetStatus(L"Kon presetbestand niet aanmaken.");
        free(paths);
        free(modes);
        return FALSE;
    }

    PresetHeader hdr = { PRESET_MAGIC, PRESET_VERSION, numPaths, numModes, numAdapters };
    DWORD written;
    BOOL ok = WriteFile(h, &hdr, sizeof(hdr), &written, NULL) &&
              WriteFile(h, paths, numPaths * sizeof(*paths), &written, NULL) &&
              WriteFile(h, modes, numModes * sizeof(*modes), &written, NULL) &&
              WriteFile(h, adapters, numAdapters * sizeof(*adapters), &written, NULL);
    CloseHandle(h);
    free(paths);
    free(modes);

    if (!ok) {
        DeleteFileW(path);
        SetStatus(L"Schrijven van preset is mislukt.");
        return FALSE;
    }
    SetStatus(L"Preset '%s' opgeslagen (%u actieve schermen).", name, numPaths);
    return TRUE;
}

/* ---- Preset toepassen ---- */

/* Vertaalt opgeslagen adapter-LUID's naar de huidige LUID's door de
 * apparaatpaden van de adapters te vergelijken. */
static void RemapAdapterIds(DISPLAYCONFIG_PATH_INFO *paths, DWORD numPaths,
                            DISPLAYCONFIG_MODE_INFO *modes, DWORD numModes,
                            const AdapterRecord *saved, DWORD numSaved)
{
    DISPLAYCONFIG_PATH_INFO *curPaths = NULL;
    DISPLAYCONFIG_MODE_INFO *curModes = NULL;
    UINT32 curNumPaths = 0, curNumModes = 0;

    if (QueryActiveConfig(&curPaths, &curNumPaths, &curModes, &curNumModes,
                          QDC_ALL_PATHS) != ERROR_SUCCESS)
        return;

    AdapterRecord current[MAX_ADAPTERS];
    DWORD numCurrent = CollectAdapters(curPaths, curNumPaths, current, MAX_ADAPTERS);
    free(curPaths);
    free(curModes);

    for (DWORD i = 0; i < numSaved; i++) {
        LUID newId;
        BOOL found = FALSE;
        for (DWORD j = 0; j < numCurrent; j++) {
            if (_wcsicmp(saved[i].devicePath, current[j].devicePath) == 0) {
                newId = current[j].id;
                found = TRUE;
                break;
            }
        }
        if (!found || LuidEqual(saved[i].id, newId))
            continue;

        for (DWORD p = 0; p < numPaths; p++) {
            if (LuidEqual(paths[p].sourceInfo.adapterId, saved[i].id))
                paths[p].sourceInfo.adapterId = newId;
            if (LuidEqual(paths[p].targetInfo.adapterId, saved[i].id))
                paths[p].targetInfo.adapterId = newId;
        }
        for (DWORD m = 0; m < numModes; m++) {
            if (LuidEqual(modes[m].adapterId, saved[i].id))
                modes[m].adapterId = newId;
        }
    }
}

static BOOL ApplyPreset(const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    if (!GetPresetPath(name, path, MAX_PATH))
        return FALSE;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        SetStatus(L"Kon preset '%s' niet openen.", name);
        return FALSE;
    }

    PresetHeader hdr;
    DWORD read;
    BOOL ok = ReadFile(h, &hdr, sizeof(hdr), &read, NULL) && read == sizeof(hdr) &&
              hdr.magic == PRESET_MAGIC && hdr.version == PRESET_VERSION &&
              hdr.numPaths > 0 && hdr.numPaths <= 64 && hdr.numModes <= 128 &&
              hdr.numAdapters <= MAX_ADAPTERS;
    if (!ok) {
        CloseHandle(h);
        SetStatus(L"Preset '%s' is beschadigd of van een oudere versie.", name);
        return FALSE;
    }

    DISPLAYCONFIG_PATH_INFO *paths = calloc(hdr.numPaths, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(hdr.numModes ? hdr.numModes : 1, sizeof(*modes));
    AdapterRecord adapters[MAX_ADAPTERS];

    ok = paths && modes &&
         ReadFile(h, paths, hdr.numPaths * sizeof(*paths), &read, NULL) &&
         read == hdr.numPaths * sizeof(*paths) &&
         ReadFile(h, modes, hdr.numModes * sizeof(*modes), &read, NULL) &&
         read == hdr.numModes * sizeof(*modes) &&
         ReadFile(h, adapters, hdr.numAdapters * sizeof(*adapters), &read, NULL) &&
         read == hdr.numAdapters * sizeof(*adapters);
    CloseHandle(h);

    if (!ok) {
        free(paths);
        free(modes);
        SetStatus(L"Lezen van preset '%s' is mislukt.", name);
        return FALSE;
    }

    RemapAdapterIds(paths, hdr.numPaths, modes, hdr.numModes, adapters, hdr.numAdapters);

    LONG rc = SetDisplayConfig(hdr.numPaths, paths, hdr.numModes, modes,
                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                               SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    free(paths);
    free(modes);

    if (rc != ERROR_SUCCESS) {
        SetStatus(L"Toepassen van '%s' is mislukt (fout %ld). Zijn alle schermen aangesloten?",
                  name, rc);
        return FALSE;
    }
    SetStatus(L"Preset '%s' toegepast.", name);
    return TRUE;
}

/* ---- Snel schakelen (zoals Win+P) ---- */

static void ApplyTopology(UINT32 topology, const WCHAR *label)
{
    LONG rc = SetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | topology);
    if (rc == ERROR_SUCCESS)
        SetStatus(L"Modus '%s' toegepast.", label);
    else
        SetStatus(L"Modus '%s' is mislukt (fout %ld).", label, rc);
}

/* ---- Lijstbeheer ---- */

static void RefreshPresetList(void)
{
    SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);

    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    if (!GetPresetDir(dir, MAX_PATH))
        return;
    StringCchPrintfW(pattern, MAX_PATH, L"%s\\*%s", dir, PRESET_EXT);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        WCHAR *dot = wcsrchr(fd.cFileName, L'.');
        if (dot)
            *dot = 0;
        SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static BOOL GetSelectedPreset(WCHAR *out, size_t cch)
{
    LRESULT sel = SendMessageW(g_hList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return FALSE;
    if (SendMessageW(g_hList, LB_GETTEXTLEN, sel, 0) >= (LRESULT)cch)
        return FALSE;
    SendMessageW(g_hList, LB_GETTEXT, sel, (LPARAM)out);
    return TRUE;
}

static void SelectPresetByName(const WCHAR *name)
{
    LRESULT idx = SendMessageW(g_hList, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)name);
    if (idx != LB_ERR)
        SendMessageW(g_hList, LB_SETCURSEL, idx, 0);
}

/* ---- Knopacties ---- */

static void OnSave(HWND hwnd)
{
    WCHAR name[128] = L"";
    GetWindowTextW(g_hEdit, name, 128);

    /* Geen naam ingevuld: overschrijf de geselecteerde preset. */
    if (!name[0] && !GetSelectedPreset(name, 128)) {
        SetStatus(L"Geef een naam op voor de nieuwe preset.");
        return;
    }
    if (!IsValidPresetName(name)) {
        SetStatus(L"Ongeldige naam: gebruik geen \\ / : * ? \" < > |");
        return;
    }

    WCHAR path[MAX_PATH];
    if (GetPresetPath(name, path, MAX_PATH) &&
        GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        WCHAR msg[256];
        StringCchPrintfW(msg, 256, L"Preset '%s' bestaat al. Overschrijven?", name);
        if (MessageBoxW(hwnd, msg, L"SchermPresets", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    if (SaveCurrentAsPreset(name)) {
        SetWindowTextW(g_hEdit, L"");
        RefreshPresetList();
        SelectPresetByName(name);
    }
}

static void OnApply(void)
{
    WCHAR name[128];
    if (!GetSelectedPreset(name, 128)) {
        SetStatus(L"Selecteer eerst een preset in de lijst.");
        return;
    }
    ApplyPreset(name);
}

static void OnDelete(HWND hwnd)
{
    WCHAR name[128];
    if (!GetSelectedPreset(name, 128)) {
        SetStatus(L"Selecteer eerst een preset in de lijst.");
        return;
    }
    WCHAR msg[256];
    StringCchPrintfW(msg, 256, L"Preset '%s' verwijderen?", name);
    if (MessageBoxW(hwnd, msg, L"SchermPresets", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    WCHAR path[MAX_PATH];
    if (GetPresetPath(name, path, MAX_PATH) && DeleteFileW(path)) {
        SetStatus(L"Preset '%s' verwijderd.", name);
        RefreshPresetList();
    } else {
        SetStatus(L"Verwijderen van '%s' is mislukt.", name);
    }
}

static void OnOpenFolder(void)
{
    WCHAR dir[MAX_PATH];
    if (GetPresetDir(dir, MAX_PATH))
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- Venster ---- */

static HWND MakeCtrl(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style,
                     int x, int y, int w, int h, int id)
{
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                S(x), S(y), S(w), S(h), parent,
                                (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return hwnd;
}

static void CreateControls(HWND hwnd)
{
    MakeCtrl(hwnd, L"STATIC", L"Opgeslagen presets:", 0, 12, 12, 280, 18, 0);
    g_hList = MakeCtrl(hwnd, L"LISTBOX", NULL,
                       WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_SORT,
                       12, 34, 280, 244, IDC_LIST);

    MakeCtrl(hwnd, L"BUTTON", L"Toepassen", BS_DEFPUSHBUTTON, 304, 34, 148, 30, IDC_BTN_APPLY);
    MakeCtrl(hwnd, L"BUTTON", L"Verwijderen", 0, 304, 70, 148, 26, IDC_BTN_DELETE);
    MakeCtrl(hwnd, L"BUTTON", L"Lijst vernieuwen", 0, 304, 102, 148, 26, IDC_BTN_REFRESH);
    MakeCtrl(hwnd, L"BUTTON", L"Presetmap openen", 0, 304, 134, 148, 26, IDC_BTN_FOLDER);

    MakeCtrl(hwnd, L"STATIC", L"Naam nieuwe preset:", 0, 12, 290, 280, 18, 0);
    g_hEdit = MakeCtrl(hwnd, L"EDIT", NULL, WS_BORDER | ES_AUTOHSCROLL,
                       12, 310, 280, 24, IDC_EDIT_NAME);
    MakeCtrl(hwnd, L"BUTTON", L"Huidige indeling opslaan", 0, 304, 308, 148, 28, IDC_BTN_SAVE);

    MakeCtrl(hwnd, L"BUTTON", L"Snel schakelen (zoals Win+P)", BS_GROUPBOX,
             12, 346, 440, 62, 0);
    MakeCtrl(hwnd, L"BUTTON", L"Uitbreiden", 0, 24, 370, 100, 28, IDC_BTN_EXTEND);
    MakeCtrl(hwnd, L"BUTTON", L"Dupliceren", 0, 132, 370, 100, 28, IDC_BTN_CLONE);
    MakeCtrl(hwnd, L"BUTTON", L"Alleen scherm 1", 0, 240, 370, 100, 28, IDC_BTN_FIRST);
    MakeCtrl(hwnd, L"BUTTON", L"Alleen scherm 2", 0, 348, 370, 100, 28, IDC_BTN_SECOND);

    g_hStatus = MakeCtrl(hwnd, L"STATIC", L"Klaar.", SS_LEFTNOWORDWRAP,
                         12, 418, 440, 18, IDC_STATUS);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        RefreshPresetList();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_LIST:
            if (HIWORD(wParam) == LBN_DBLCLK)
                OnApply();
            return 0;
        case IDC_BTN_SAVE:    OnSave(hwnd);    return 0;
        case IDC_BTN_APPLY:   OnApply();       return 0;
        case IDC_BTN_DELETE:  OnDelete(hwnd);  return 0;
        case IDC_BTN_REFRESH: RefreshPresetList(); SetStatus(L"Lijst vernieuwd."); return 0;
        case IDC_BTN_FOLDER:  OnOpenFolder();  return 0;
        case IDC_BTN_EXTEND:  ApplyTopology(SDC_TOPOLOGY_EXTEND,   L"Uitbreiden");      return 0;
        case IDC_BTN_CLONE:   ApplyTopology(SDC_TOPOLOGY_CLONE,    L"Dupliceren");      return 0;
        case IDC_BTN_FIRST:   ApplyTopology(SDC_TOPOLOGY_INTERNAL, L"Alleen scherm 1"); return 0;
        case IDC_BTN_SECOND:  ApplyTopology(SDC_TOPOLOGY_EXTERNAL, L"Alleen scherm 2"); return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR cmdLine, int nCmdShow)
{
    (void)hPrev; (void)cmdLine;

    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);

    g_hFont = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SchermPresetsWnd";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    RECT rc = { 0, 0, S(464), S(444) };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    HWND hwnd = CreateWindowExW(0, L"SchermPresetsWnd", L"SchermPresets",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
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
    return 0;
}
