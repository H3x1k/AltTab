#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <tchar.h>
#include <psapi.h>
#include <shellapi.h>
#include <string>
#include <iostream>
#include <vector>

// DWM cloak attribute constant
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

// DWM cloak states
#ifndef DWM_CLOAKED_APP
#define DWM_CLOAKED_APP 0x0000001
#endif
#ifndef DWM_CLOAKED_SHELL
#define DWM_CLOAKED_SHELL 0x0000002
#endif
#ifndef DWM_CLOAKED_INHERITED
#define DWM_CLOAKED_INHERITED 0x0000004
#endif



#if !defined(__MINGW32__) || defined(__MINGW64_VERSION_MAJOR)
// Already declared in MinGW-w64
#else
extern "C" BOOL WINAPI QueryFullProcessImageNameW(
    HANDLE hProcess,
    DWORD dwFlags,
    LPWSTR lpExeName,
    PDWORD lpdwSize
);
#endif


HHOOK hHook = NULL;
HWND hwndOverlay = NULL;
bool isOverlayVisible = false;

int screenWidth = GetSystemMetrics(SM_CXSCREEN);
int screenHeight = GetSystemMetrics(SM_CYSCREEN);

int windowWidth = 600;
int windowHeight = 300;

int windowX = (screenWidth - windowWidth) / 2;
int windowY = (screenHeight - windowHeight) / 2;

struct AltTabWindow {
    HWND hwnd;                 // Window handle — to interact with the window
    std::wstring title;        // Window title text (shown in the Alt+Tab menu)
    std::wstring exeName;      // Executable name (e.g., "notepad.exe"), useful for icons or grouping
    HICON icon;                // Window or program icon (for display)
    DWORD processId;           // Process ID, can be useful for filtering or grouping
    char key;
};

std::vector<AltTabWindow> altTabWindows;


// Forward declaration
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef HRESULT(WINAPI* PFNDwmGetWindowAttribute)(HWND hwnd, DWORD dwAttribute, PVOID pvAttribute, DWORD cbAttribute);


HICON GetProgramIcon(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return NULL;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return NULL;

    wchar_t exePath[MAX_PATH];
    DWORD size = MAX_PATH;

    if (!QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
        CloseHandle(hProcess);
        return NULL;
    }
    CloseHandle(hProcess);

    // Extract the first icon from the exe file
    HICON hIcon = NULL;
    UINT iconsExtracted = ExtractIconExW(exePath, 0, NULL, &hIcon, 1);
    if (iconsExtracted == 0) return NULL;

    return hIcon; // Caller must destroy with DestroyIcon(hIcon)
}

std::wstring GetProgramName(HWND hwnd) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) return L"";

    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
        CloseHandle(hProcess);
        return std::wstring(path);
    }

    CloseHandle(hProcess);
    return L"";
}

std::wstring GetWindowTitle(HWND hwnd) {
    wchar_t title[256];
    GetWindowTextW(hwnd, title, sizeof(title) / sizeof(wchar_t));
    return std::wstring(title);
}


bool IsWindowCloaked(HWND hwnd) {
    static PFNDwmGetWindowAttribute pDwmGetWindowAttribute = nullptr;

    if (!pDwmGetWindowAttribute) {
        HMODULE hDwmApi = LoadLibraryW(L"dwmapi.dll");
        if (!hDwmApi) return false;
        pDwmGetWindowAttribute = (PFNDwmGetWindowAttribute)GetProcAddress(hDwmApi, "DwmGetWindowAttribute");
        if (!pDwmGetWindowAttribute) return false;
    }

    DWORD cloaked = 0;
    HRESULT hr = pDwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (SUCCEEDED(hr) && cloaked != 0) {
        return true;
    }
    return false;
}

bool IsAltTabWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd))
        return false;

    if (IsWindowCloaked(hwnd))  // Skip cloaked windows
        return false;

    if (GetAncestor(hwnd, GA_ROOT) != hwnd)
        return false;

    if (GetWindow(hwnd, GW_OWNER) != NULL)
        return false;

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW)
        return false;

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if (!(style & WS_CAPTION))
        return false;

    if (GetWindowTextLengthW(hwnd) == 0)
        return false;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc))
        return false;
    if ((rc.right - rc.left) == 0 || (rc.bottom - rc.top) == 0)
        return false;

    return true;
}

bool FocusWindow(HWND hwnd) {

    // Restore if minimized
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    else
        ShowWindow(hwnd, SW_SHOW);

    // Try to set foreground directly
    if (SetForegroundWindow(hwnd))
        return true;

    // Fallback: force it by attaching threads
    DWORD currentThreadId = GetCurrentThreadId();
    DWORD targetThreadId = GetWindowThreadProcessId(hwnd, nullptr);

    // Attach input queues
    AttachThreadInput(currentThreadId, targetThreadId, TRUE);

    // Set all focus-related states
    BringWindowToTop(hwnd);           // Bring to top of Z-order
    SetForegroundWindow(hwnd);        // Set as foreground window
    SetFocus(hwnd);                   // Set keyboard focus
    SetActiveWindow(hwnd);            // Set active window

    // Detach input queues
    AttachThreadInput(currentThreadId, targetThreadId, FALSE);

    return GetForegroundWindow() == hwnd;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    
    if (IsAltTabWindow(hwnd)) {

        AltTabWindow window;

        window.hwnd = hwnd;

        window.title = GetWindowTitle(hwnd);

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        window.processId = pid;

        std::wstring exePath = GetProgramName(hwnd);
        std::wstring exeName = exePath.substr(exePath.find_last_of(L"\\") + 1);
        if (exeName.size() > 4 && exeName.substr(exeName.size() - 4) == L".exe") {
            exeName = exeName.substr(0, exeName.size() - 4);
        }
        window.exeName = exeName;

        window.icon = GetProgramIcon(hwnd);

        window.key = std::tolower(exeName[0]);

        altTabWindows.push_back(window);
    } 
    
    return TRUE;
}


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
        windowX, windowY, 
        windowWidth, windowHeight,
        NULL, NULL, hInstance, NULL
    );

    SetLayeredWindowAttributes(hwndOverlay, 0, 255, LWA_ALPHA); // 0 = color key (unused), alpha 0–255

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

        if (isAltDown && isOverlayVisible) {
            for (const auto& window: altTabWindows) {
                if (GetAsyncKeyState(std::toupper(window.key)) & 0x8000) {
                    FocusWindow(window.hwnd);

                    ShowWindow(hwndOverlay, SW_HIDE);
                    isOverlayVisible = false;
                }
            }
        }

        if (wParam == WM_SYSKEYDOWN || wParam == WM_KEYDOWN) {
            if (kbd->vkCode == VK_TAB && isAltDown) {
                if (!isOverlayVisible) {
                    altTabWindows.clear();
                    EnumWindows(EnumWindowsProc, 0);

                    ShowWindow(hwndOverlay, SW_SHOW);
                    InvalidateRect(hwndOverlay, NULL, TRUE);
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

        int lineHeight = 20;
        int y = 10; // header height

        for (const auto& window: altTabWindows) {
            RECT lineRect = { 10, y, rect.right - 10, y + lineHeight };
            std::wstring text = std::wstring(1, std::toupper(window.key)) + L" | " + window.exeName + L" | " + window.title;
            DrawText(hdc, text.c_str(), -1, &lineRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
            y += lineHeight;
        }

        windowHeight = y;
        windowY = (screenHeight - windowHeight) / 2;
        SetWindowPos(hwnd, HWND_TOP, windowX, windowY, windowWidth, windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);

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