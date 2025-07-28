#define UNICODE
#define _UNICODE
#include <windows.h>
#include <tchar.h>
#include <iostream>

HHOOK hHook = NULL;
HWND hwndOverlay = NULL;
bool isOverlayVisible = false;

// Forward declaration
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // Register window class
    const wchar_t CLASS_NAME[] = L"AltTabOverlayWindow";

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    // Create overlay window
    hwndOverlay = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        L"",
        WS_POPUP,
        400, 200, 600, 200,
        NULL, NULL, hInstance, NULL
    );

    SetLayeredWindowAttributes(hwndOverlay, 0, 230, LWA_ALPHA); // 0 = color key (unused), alpha 0–255

    // Install low-level keyboard hook
    hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!hHook) {
        MessageBox(NULL, L"Failed to install keyboard hook!", L"Error", MB_ICONERROR);
        return 1;
    }

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hHook);
    return 0;
}

// Hook procedure
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {

        KBDLLHOOKSTRUCT* kbd = (KBDLLHOOKSTRUCT*)lParam;
        DWORD vkCode = kbd->vkCode;

        bool isAltDown  = GetAsyncKeyState(VK_MENU) & 0x8000;
        bool isAlt = (vkCode == VK_LMENU || vkCode == VK_RMENU || vkCode == VK_MENU);
        isAltDown = isAltDown || (isAlt && wParam == WM_KEYDOWN);   
        if (isAlt && wParam == WM_KEYUP) {
            isAltDown = false;
        }

        if (!isAltDown) {
            ShowWindow(hwndOverlay, SW_HIDE);
            isOverlayVisible = false;
        }

        if (wParam == WM_SYSKEYDOWN || wParam == WM_KEYDOWN) {
            if (kbd->vkCode == VK_TAB && isAltDown) {
                if (!isOverlayVisible) {
                    ShowWindow(hwndOverlay, SW_SHOW);
                    isOverlayVisible = true;
                }
                return 1; // Block default Alt+Tab
            }
            if (kbd->vkCode == VK_ESCAPE) {
                PostQuitMessage(0);
            }
        }
    }

    return CallNextHookEx(hHook, nCode, wParam, lParam);
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);
        SetBkMode(hdc, TRANSPARENT);
        DrawText(hdc, L"Custom Alt+Tab Menu", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
