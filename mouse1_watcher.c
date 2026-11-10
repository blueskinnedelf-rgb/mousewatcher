/*
 * Mouse1 Watcher - finds out where left-click goes missing on Windows.
 *
 * What it does (and deliberately does NOT do, to stay anti-cheat friendly):
 *   - Listens to Raw Input from every mouse (the same API games and Discord use).
 *   - Polls GetAsyncKeyState(VK_LBUTTON) to see what Windows itself thinks the
 *     left button is doing.
 *   - Compares the two, and logs when they disagree.
 *   - NO hooks, NO DLL injection, NO opening handles to other processes,
 *     NO reading other processes' memory, NO sending input.
 *
 * Problems it can flag:
 *   SWALLOWED  a mouse sent a press, but Windows never registered it
 *              (something in software ate the click: overlay, macro tool, hook)
 *   STUCK      Windows thinks left is held down, but no mouse is holding it
 *              (classic "click stops working until you click again" bug)
 *   PHANTOM    Windows registered a press that no physical mouse sent
 *   INJECTED   a click arrived from software (SendInput), not from a device
 *   SWAP       the "switch primary and secondary buttons" setting changed
 *   DESKTOP    Windows switched to the secure desktop (UAC prompt, lock, Ctrl+Alt+Del)
 *   DEVICE     a mouse was plugged in / unplugged / reset
 *
 * Press the marker key shown in the window (Ctrl+Alt+M, or a fallback like
 * Scroll Lock if that's taken) when the click dies to drop a MARKER into the log.
 *
 * Build (MinGW): x86_64-w64-mingw32-gcc -O2 -s -o Mouse1Watcher.exe mouse1_watcher.c -luser32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define VERSION "4"
#define MAXDEV 32
#define SWALLOW_MS 200   /* raw press with no OS press within this window */
#define PHANTOM_MS 200   /* OS press with no raw press within this window */
#define STUCK_MS   400   /* OS says down, all mice say up, for this long */

typedef struct {
    HANDLE h;
    char id[160];        /* short device id, e.g. VID_046D&PID_C539 */
    int down;
    unsigned presses;
    /* activity other than left click, so we can tell "whole mouse dead"
       apart from "only the left button dead" */
    unsigned moves, otherPresses;          /* per-minute counters */
    unsigned mLeft;
    ULONGLONG lastMoveT, lastOtherT, lastLeftT, lastAnyT;
} Dev;

static Dev devs[MAXDEV];
static int ndev;
static FILE *logf;
static volatile LONG quitting;

/* OS-level (what Windows thinks) state */
static int osDown;
static ULONGLONG osDownT, osUpT, lastOsPressT;
static int osPressNeedsCheck;
static ULONGLONG osPressCheckT;

/* raw (device-level) state */
static ULONGLONG lastRawDownT, lastRawUpT;
static int rawPending;
static ULONGLONG rawPendingT;
static char rawPendingDev[160];

static int stuckReported;

/* counters */
static unsigned cRaw, cOs, cSwallowed, cStuck, cPhantom, cInjected, cMarkers;
static unsigned mRaw, mOs; /* per-minute */

/* environment state */
static HWND lastFg;
static int lastSwap = -1;
static RECT lastClip;
static int clipKnown;
static char lastDesk[64] = "";
static int statusDirty;

/* ---------- helpers ---------- */

static void w2u(const wchar_t *w, char *out, int outsz) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outsz, NULL, NULL)) out[0] = 0;
}

static void stamp(char *buf, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/* toConsole: 0 = file only, 1 = file + console */
static void logmsg(int toConsole, const char *tag, const char *fmt, ...) {
    char ts[32], msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    stamp(ts, sizeof ts);
    if (logf) {
        fprintf(logf, "%s [%-9s] %s\n", ts, tag, msg);
        fflush(logf);
    }
    if (toConsole) {
        printf("\r%-79s\r%s [%s] %s\n", "", ts + 11, tag, msg);
        statusDirty = 1;
    }
}

static Dev *getdev(HANDLE h) {
    for (int i = 0; i < ndev; i++) if (devs[i].h == h) return &devs[i];
    if (ndev >= MAXDEV) return &devs[MAXDEV - 1];
    Dev *d = &devs[ndev++];
    memset(d, 0, sizeof *d);
    d->h = h;
    if (!h) {
        strcpy(d->id, "NO-DEVICE (software-injected input)");
        return d;
    }
    char name[512] = "";
    UINT sz = sizeof name;
    if (GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, name, &sz) == (UINT)-1 || !name[0]) {
        snprintf(d->id, sizeof d->id, "handle %p", h);
        return d;
    }
    /* try to pull VID/PID out of the long device path */
    const char *v = strstr(name, "VID_");
    if (!v) v = strstr(name, "vid_");
    if (v) {
        snprintf(d->id, sizeof d->id, "%.17s", v);
    } else {
        /* not a USB HID (e.g. RDP, virtual, bluetooth) - keep a trimmed path */
        const char *p = name;
        if (!strncmp(p, "\\\\?\\", 4)) p += 4;
        snprintf(d->id, sizeof d->id, "%.60s", p);
    }
    return d;
}

static int anyRawDown(void) {
    for (int i = 0; i < ndev; i++) if (devs[i].down) return 1;
    return 0;
}

static void describeFg(HWND fg, char *out, size_t n) {
    if (!fg) { snprintf(out, n, "(none)"); return; }
    wchar_t wt[256] = L"", wc[128] = L"";
    char t[512], c[256];
    DWORD pid = 0;
    GetWindowTextW(fg, wt, 256);
    GetClassNameW(fg, wc, 128);
    GetWindowThreadProcessId(fg, &pid);
    w2u(wt, t, sizeof t);
    w2u(wc, c, sizeof c);
    snprintf(out, n, "\"%s\" class=%s pid=%lu", t, c, (unsigned long)pid);
}

static void readDesktop(char *out, size_t n) {
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d) { snprintf(out, n, "SECURE (UAC prompt / lock screen / Ctrl+Alt+Del)"); return; }
    wchar_t wn[64] = L"";
    DWORD need = 0;
    if (GetUserObjectInformationW(d, UOI_NAME, wn, sizeof wn, &need)) w2u(wn, out, (int)n);
    else snprintf(out, n, "unknown");
    CloseDesktop(d);
}

static void snapshot(const char *why) {
    ULONGLONG now = GetTickCount64();
    char fg[700], desk[64];
    RECT clip;
    describeFg(GetForegroundWindow(), fg, sizeof fg);
    readDesktop(desk, sizeof desk);
    GetClipCursor(&clip);
    logmsg(0, "SNAPSHOT", "---- %s (watcher v%s) ----", why, VERSION);
    if (osDown)
        logmsg(0, "SNAPSHOT", "Windows thinks left button is: DOWN (held %llu ms)", now - osDownT);
    else
        logmsg(0, "SNAPSHOT", "Windows thinks left button is: up");
    for (int i = 0; i < ndev; i++) {
        Dev *d = &devs[i];
        char a[32], b[32], c[32];
        if (d->lastLeftT) snprintf(a, sizeof a, "%.1fs ago", (now - d->lastLeftT) / 1000.0); else strcpy(a, "never");
        if (d->lastMoveT) snprintf(b, sizeof b, "%.1fs ago", (now - d->lastMoveT) / 1000.0); else strcpy(b, "never");
        if (d->lastOtherT) snprintf(c, sizeof c, "%.1fs ago", (now - d->lastOtherT) / 1000.0); else strcpy(c, "never");
        logmsg(0, "SNAPSHOT", "Mouse %-20s left=%s presses=%u | last left %s, last MOVE %s, last other button/wheel %s",
               d->id, d->down ? "DOWN" : "up", d->presses, a, b, c);
    }
    if (lastRawDownT) logmsg(0, "SNAPSHOT", "Last mouse press %llu ms ago", now - lastRawDownT);
    else logmsg(0, "SNAPSHOT", "Last mouse press: none yet");
    if (lastRawUpT) logmsg(0, "SNAPSHOT", "Last mouse release %llu ms ago", now - lastRawUpT);
    if (lastOsPressT) logmsg(0, "SNAPSHOT", "Last Windows press %llu ms ago", now - lastOsPressT);
    else logmsg(0, "SNAPSHOT", "Last Windows press: none yet");
    logmsg(0, "SNAPSHOT", "Buttons swapped: %s", GetSystemMetrics(SM_SWAPBUTTON) ? "YES" : "no");
    logmsg(0, "SNAPSHOT", "Cursor clip: %ld,%ld - %ld,%ld", clip.left, clip.top, clip.right, clip.bottom);
    logmsg(0, "SNAPSHOT", "Input desktop: %s", desk);
    logmsg(0, "SNAPSHOT", "Foreground: %s", fg);
    logmsg(0, "SNAPSHOT", "Totals: raw=%u windows=%u swallowed=%u stuck=%u phantom=%u injected=%u",
           cRaw, cOs, cSwallowed, cStuck, cPhantom, cInjected);
    logmsg(0, "SNAPSHOT", "---- end ----");
}

static void listMice(void) {
    UINT n = 0;
    GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    if (!n) return;
    RAWINPUTDEVICELIST *l = (RAWINPUTDEVICELIST *)calloc(n, sizeof *l);
    if (!l) return;
    n = GetRawInputDeviceList(l, &n, sizeof(RAWINPUTDEVICELIST));
    int count = 0;
    for (UINT i = 0; i < n && n != (UINT)-1; i++) {
        if (l[i].dwType != RIM_TYPEMOUSE) continue;
        Dev *d = getdev(l[i].hDevice);
        logmsg(1, "DEVICE", "Mouse present: %s", d->id);
        count++;
    }
    logmsg(1, "DEVICE", "%d mouse device(s) found (virtual mice from drivers/software count too)", count);
    free(l);
}

/* ---------- raw input ---------- */

static void onRawInput(HRAWINPUT hri) {
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT sz = sizeof buf;
    if (GetRawInputData(hri, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    RAWINPUT *ri = (RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    USHORT f = ri->data.mouse.usButtonFlags;
    ULONGLONG now = GetTickCount64();
    Dev *d = getdev(ri->header.hDevice);
    int injected = (ri->header.hDevice == NULL);

    /* a mouse that went completely silent and came back = likely wireless/USB dropout */
    if (d->lastAnyT && now - d->lastAnyT > 3000 && d->down)
        logmsg(1, "SILENCE", "%s was silent for %.1f s while left was held", d->id, (now - d->lastAnyT) / 1000.0);
    d->lastAnyT = now;

    if (ri->data.mouse.lLastX || ri->data.mouse.lLastY) { d->moves++; d->lastMoveT = now; }
    USHORT other = RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN |
                   RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
    if (f & other) {
        d->otherPresses++;
        d->lastOtherT = now;
        logmsg(0, "RAW", "other button (%s) %s",
               (f & RI_MOUSE_RIGHT_BUTTON_DOWN) ? "right" :
               (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) ? "middle" : "side", d->id);
    }
    if (f & RI_MOUSE_WHEEL) d->lastOtherT = now;
    if (!(f & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP))) return;
    d->lastLeftT = now;

    if (f & RI_MOUSE_LEFT_BUTTON_DOWN) {
        if (d->down) logmsg(1, "ODD", "%s sent a 2nd press without a release (missed release event)", d->id);
        int othersHolding = anyRawDown();
        d->down = 1;
        d->presses++;
        d->mLeft++;
        cRaw++; mRaw++;
        lastRawDownT = now;
        if (injected) { cInjected++; logmsg(1, "INJECTED", "Left press came from software, not a mouse"); }
        logmsg(0, "RAW", "press   %s", d->id);
        if (!othersHolding && !osDown && !injected) {
            rawPending = 1;
            rawPendingT = now;
            snprintf(rawPendingDev, sizeof rawPendingDev, "%s", d->id);
        }
    }
    if (f & RI_MOUSE_LEFT_BUTTON_UP) {
        if (!d->down) logmsg(0, "RAW", "release without press from %s (press may predate watcher)", d->id);
        d->down = 0;
        lastRawUpT = now;
        logmsg(0, "RAW", "release %s", d->id);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INPUT:
        onRawInput((HRAWINPUT)l);
        return DefWindowProcW(h, m, w, l);
    case WM_INPUT_DEVICE_CHANGE: {
        Dev *d = getdev((HANDLE)l);
        if (w == GIDC_ARRIVAL) logmsg(1, "DEVICE", "Mouse connected: %s", d->id);
        else {
            logmsg(1, "DEVICE", "Mouse DISCONNECTED: %s%s", d->id, d->down ? " (while left was held!)" : "");
            if (d->down) { d->down = 0; lastRawUpT = GetTickCount64(); }
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- polling: what does Windows think? ---------- */

static void pollOs(ULONGLONG now) {
    SHORT s = GetAsyncKeyState(VK_LBUTTON);
    int down = (s & 0x8000) != 0;
    int pressedSince = (s & 0x0001) != 0; /* catches very fast clicks between polls */

    if ((down && !osDown) || (pressedSince && !osDown && !down)) {
        cOs++; mOs++;
        lastOsPressT = now;
        osPressNeedsCheck = 1;
        osPressCheckT = now;
        logmsg(0, "WINDOWS", "press");
        if (down) { osDown = 1; osDownT = now; }
        else { logmsg(0, "WINDOWS", "release (fast click)"); osUpT = now; }
    } else if (!down && osDown) {
        osDown = 0;
        osUpT = now;
        logmsg(0, "WINDOWS", "release (held %llu ms)", now - osDownT);
        if (stuckReported) {
            logmsg(1, "UNSTUCK", "Windows released left after being stuck %llu ms", now - osDownT);
            stuckReported = 0;
        }
    }

    /* raw press that Windows never saw */
    if (rawPending && now - rawPendingT > SWALLOW_MS) {
        rawPending = 0;
        if (!(lastOsPressT + SWALLOW_MS >= rawPendingT) && !osDown) {
            cSwallowed++;
            logmsg(1, "SWALLOWED", "%s pressed left, but Windows never registered it", rawPendingDev);
            snapshot("auto: swallowed click");
        }
    }

    /* Windows press that no mouse sent */
    if (osPressNeedsCheck && now - osPressCheckT > PHANTOM_MS) {
        osPressNeedsCheck = 0;
        if (!(lastRawDownT + PHANTOM_MS >= osPressCheckT)) {
            cPhantom++;
            logmsg(1, "PHANTOM", "Windows registered a left press that no mouse sent");
            snapshot("auto: phantom press");
        }
    }

    /* Windows holding left down with nobody pressing it */
    if (osDown && !stuckReported && !anyRawDown() &&
        now - osDownT > STUCK_MS && now - lastRawUpT > STUCK_MS) {
        stuckReported = 1;
        cStuck++;
        logmsg(1, "STUCK", "Windows thinks left is HELD, but no mouse is holding it");
        snapshot("auto: stuck button");
    }
}

static void pollEnvironment(void) {
    HWND fg = GetForegroundWindow();
    if (fg != lastFg) {
        char d[700];
        lastFg = fg;
        describeFg(fg, d, sizeof d);
        logmsg(0, "FOCUS", "%s", d);
    }
    int swap = GetSystemMetrics(SM_SWAPBUTTON);
    if (swap != lastSwap) {
        if (lastSwap != -1 || swap) logmsg(1, "SWAP", "Primary/secondary buttons swapped: %s", swap ? "YES" : "no");
        lastSwap = swap;
    }
    RECT c;
    if (GetClipCursor(&c)) {
        if (!clipKnown || memcmp(&c, &lastClip, sizeof c)) {
            logmsg(0, "CLIP", "Cursor confined to %ld,%ld - %ld,%ld", c.left, c.top, c.right, c.bottom);
            lastClip = c;
            clipKnown = 1;
        }
    }
    char desk[64];
    readDesktop(desk, sizeof desk);
    if (strcmp(desk, lastDesk)) {
        if (lastDesk[0]) logmsg(1, "DESKTOP", "Input desktop is now: %s", desk);
        snprintf(lastDesk, sizeof lastDesk, "%s", desk);
    }
}

static BOOL WINAPI onCtrl(DWORD t) {
    (void)t;
    InterlockedExchange(&quitting, 1);
    Sleep(500);
    return TRUE;
}

int main(void) {
    /* log file next to the exe */
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\') len--;
    path[len] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t file[64];
    swprintf(file, 64, L"mouse1_log_%04d-%02d-%02d_%02d-%02d-%02d.txt",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (len + wcslen(file) < MAX_PATH) wcscat(path, file);
    logf = _wfopen(path, L"w");
    if (!logf) logf = _wfopen(file, L"w"); /* fall back to current folder */

    /* marker hotkey: first one that isn't already taken by another app */
    static const struct { UINT mod, vk; const char *name; } keys[] = {
        { MOD_CONTROL | MOD_ALT, 'M',       "Ctrl + Alt + M" },
        { 0,                     VK_SCROLL, "Scroll Lock" },
        { 0,                     VK_PAUSE,  "Pause/Break" },
        { MOD_CONTROL | MOD_ALT, VK_F12,    "Ctrl + Alt + F12" },
        { MOD_CONTROL | MOD_SHIFT, VK_F12,  "Ctrl + Shift + F12" },
        { MOD_CONTROL | MOD_ALT, 'K',       "Ctrl + Alt + K" },
    };
    const char *markerKey = NULL;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (RegisterHotKey(NULL, 1, keys[i].mod | MOD_NOREPEAT, keys[i].vk)) { markerKey = keys[i].name; break; }
    }

    SetConsoleTitleW(L"Mouse1 Watcher v" VERSION);
    printf("==============================================================\n");
    printf(" Mouse1 Watcher  VERSION " VERSION "  - leave this window open while you play.\n");
    if (markerKey) {
        printf(" When left click stops working, press:   %s\n", markerKey);
        printf(" (you'll hear a beep). Close this window when you're done.\n");
    } else {
        printf(" WARNING: every marker key is taken by other apps - markers\n");
        printf(" are off. Just note the time when click breaks instead.\n");
    }
    printf("==============================================================\n");
    char up[MAX_PATH * 3];
    w2u(path, up, sizeof up);
    printf(" Log file: %s\n\n", logf ? up : "(could not create log file!)");

    logmsg(0, "START", "Mouse1 Watcher VERSION %s started", VERSION);
    if (markerKey) logmsg(0, "START", "Marker key: %s", markerKey);
    else logmsg(1, "WARN", "No marker key available (all candidates taken by other apps)");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"Mouse1WatcherMsg";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01; /* generic desktop */
    rid.usUsage = 0x02;     /* mouse */
    rid.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid.hwndTarget = hwnd;
    if (!hwnd || !RegisterRawInputDevices(&rid, 1, sizeof rid))
        logmsg(1, "ERROR", "Could not register for raw mouse input (error %lu)", GetLastError());


    SetConsoleCtrlHandler(onCtrl, TRUE);
    listMice();
    snapshot("startup");
    pollEnvironment();

    ULONGLONG lastEnv = 0, lastStatus = 0, lastMinute = GetTickCount64();
    MSG msg;
    while (!quitting) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY && msg.wParam == 1) {
                cMarkers++;
                MessageBeep(MB_ICONASTERISK);
                logmsg(1, "MARKER", "######## MARKER #%u - user says left click is broken now ########", cMarkers);
                snapshot("user marker");
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ULONGLONG now = GetTickCount64();
        pollOs(now);
        if (now - lastEnv >= 500) { pollEnvironment(); lastEnv = now; }
        if (now - lastMinute >= 60000) {
            logmsg(0, "MINUTE", "last 60s: mouse presses=%u, Windows presses=%u%s", mRaw, mOs,
                   (mRaw != mOs) ? "  <-- MISMATCH" : "");
            for (int i = 0; i < ndev; i++) {
                Dev *d = &devs[i];
                if (d->moves || d->mLeft || d->otherPresses)
                    logmsg(0, "MINUTE", "   %-20s left=%u other buttons=%u movement packets=%u",
                           d->id, d->mLeft, d->otherPresses, d->moves);
                d->moves = d->otherPresses = d->mLeft = 0;
            }
            mRaw = mOs = 0;
            lastMinute = now;
        }
        if (now - lastStatus >= 1000 || statusDirty) {
            printf("\r v" VERSION " | Left: %-4s | clicks mouse=%u windows=%u | swallowed=%u stuck=%u phantom=%u markers=%u   ",
                   osDown ? "DOWN" : "up", cRaw, cOs, cSwallowed, cStuck, cPhantom, cMarkers);
            fflush(stdout);
            lastStatus = now;
            statusDirty = 0;
        }
    }

    logmsg(0, "STOP", "Watcher stopped. Totals: raw=%u windows=%u swallowed=%u stuck=%u phantom=%u injected=%u markers=%u",
           cRaw, cOs, cSwallowed, cStuck, cPhantom, cInjected, cMarkers);
    if (logf) fclose(logf);
    return 0;
}
