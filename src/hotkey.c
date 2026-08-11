#include <stdlib.h>
#include <stdio.h>
#include <Windows.h>
#include "common.h"

// simple file logger (GUI app has no console, so log to clumsy-hotkey.log
// next to the executable; after UAC elevation the cwd may be System32)
static void hkLog(const char *fmt, ...) {
    FILE *f;
    char buf[512];
    char path[MAX_PATH];
    char *slash;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "clumsy-hotkey.log");
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

static HANDLE hotkeyThread;
static volatile short hotkeyRunning = 0;
static volatile short hotkeyStopFlag = 0;
static volatile LONG hotkeyVk = VK_F8;
static volatile short hotkeyHoldMode = 0;

// cross-thread flags consumed by the main UI timer
// the hook thread only touches these flags - no window messaging
// is involved, so nothing can break the IUP dialog
static volatile short hotkeyPressFlag = 0;
static volatile short hotkeyReleaseFlag = 0;

static HHOOK g_hook = NULL;

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT*)lParam;
        // log every keydown (not just the hotkey)
        if (wParam == WM_KEYDOWN) {
            hkLog("KEYDOWN vk=%d (hotkey=%d)", (int)p->vkCode, (int)(DWORD)hotkeyVk);
        }
        if (p->vkCode == (DWORD)hotkeyVk) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                hkLog("KEYDOWN vk=%d -> press flag", (int)p->vkCode);
                InterlockedExchange16(&hotkeyPressFlag, 1);
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                hkLog("KEYUP vk=%d -> release flag", (int)p->vkCode);
                InterlockedExchange16(&hotkeyReleaseFlag, 1);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI HotkeyThreadProc(LPVOID arg) {
    MSG msg;
    short lastState = 0;
    UNREFERENCED_PARAMETER(arg);

    hkLog("Installing WH_KEYBOARD_LL hook, VK=%d", (int)hotkeyVk);
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (!g_hook) {
        hkLog("FAILED to install low-level keyboard hook, error=%lu", GetLastError());
        LOG("Failed to install low-level keyboard hook (%lu)", GetLastError());
    } else {
        hkLog("Hook installed OK");
        LOG("Global hotkey hook installed: VK=%d, hold=%d", (int)hotkeyVk, (short)hotkeyHoldMode);
    }

    // fallback: poll GetAsyncKeyState in parallel. some games use DirectInput
    // or raw input and swallow keys before the low-level hook sees them
    hkLog("Starting GetAsyncKeyState poll fallback, VK=%d", (int)hotkeyVk);
    lastState = 0;

    while (!hotkeyStopFlag) {
        // pump hook messages (required for WH_KEYBOARD_LL)
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // poll fallback
        {
            short state = (short)(GetAsyncKeyState((int)hotkeyVk) & 0x8000);
            if (state && !lastState) {
                hkLog("POLL KEYDOWN vk=%d -> press flag", (int)hotkeyVk);
                InterlockedExchange16(&hotkeyPressFlag, 1);
            }
            if (!state && lastState) {
                hkLog("POLL KEYUP vk=%d -> release flag", (int)hotkeyVk);
                InterlockedExchange16(&hotkeyReleaseFlag, 1);
            }
            lastState = state;
        }

        Sleep(10);
    }

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    LOG("Hotkey thread exiting");
    return 0;
}

void hotkeyStart(void) {
    if (hotkeyRunning) return;
    hotkeyStopFlag = 0;
    hotkeyRunning = 1;
    hotkeyThread = CreateThread(NULL, 0, HotkeyThreadProc, NULL, 0, NULL);
}

void hotkeyRestart(void) {
    hotkeyStop();
    hotkeyStart();
}

void hotkeySetHoldMode(short hold) {
    hotkeyHoldMode = hold;
}

void hotkeyStop(void) {
    if (!hotkeyRunning) return;
    hotkeyStopFlag = 1;
    WaitForSingleObject(hotkeyThread, INFINITE);
    CloseHandle(hotkeyThread);
    hotkeyThread = NULL;
    hotkeyRunning = 0;
}

short hotkeyConsumePress(void) {
    return InterlockedExchange16(&hotkeyPressFlag, 0);
}

short hotkeyConsumeRelease(void) {
    return InterlockedExchange16(&hotkeyReleaseFlag, 0);
}

int hotkeyGetVk(void) {
    return (int)hotkeyVk;
}

void hotkeySetVk(int vk) {
    hotkeyVk = vk;
}

UINT hotkeyGetId(void) {
    return 0;
}

// convert a VK code to a human-readable key name (e.g. "F8", "A", "Enter")
// uses GetKeyNameText with the proper scan-code lParam so the OS layout
// decides the label, just like a normal keyboard
void hotkeyGetName(int vk, char *buf, int bufsize) {
    UINT scan;
    LONG lParam;
    if (!buf || bufsize <= 0) return;
    buf[0] = '\0';

    scan = MapVirtualKey((UINT)vk, MAPVK_VK_TO_VSC);
    lParam = (LONG)(scan << 16);

    // keys that live in the extended keyboard block need the 24th bit set
    // for GetKeyNameText to return the correct label
    switch (vk) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_DIVIDE:
        case VK_RETURN:
            lParam |= (0x1 << 24);
            break;
    }

    if (GetKeyNameTextA(lParam, buf, bufsize) == 0) {
        snprintf(buf, bufsize, "VK_%d", vk);
    }
}
