#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <stdexcept>

inline static HWND g_hiddenSwapchainWindow = nullptr;
inline static constexpr wchar_t kHiddenSwapchainWindowClass[] = L"OptiDx11SwapchainHost";

// Window property set on a hidden window that was created on its own message-pump thread
inline static constexpr wchar_t kHiddenSwapchainWindowPumpProp[] = L"OptiDx11SwapchainHostPump";

// Posted (or sent, from the owning thread) to the hidden window to make it a hidden child of the window in
// wParam (0 = back to a top-level popup), sized to that window's client area. Done by the window's own thread
// so the presenting thread never blocks on another thread's message queue.
inline static constexpr UINT kHiddenSwapchainWindowMsgAttach = WM_APP + 0x4F70;

inline bool HiddenSwapchainWindowAttach(HWND hwnd, HWND parent)
{
    if (parent != nullptr && !IsWindow(parent))
        return false;

    if (parent != nullptr && GetAncestor(hwnd, GA_PARENT) == parent)
        return true;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    LONG_PTR newStyle = style & ~(static_cast<LONG_PTR>(WS_POPUP) | WS_CHILD | WS_VISIBLE);
    newStyle |= (parent != nullptr) ? (WS_CHILD | WS_CLIPSIBLINGS) : static_cast<LONG_PTR>(WS_POPUP);

    // SetParent leaves WS_CHILD/WS_POPUP alone: the style has to be switched before attaching to a parent
    SetWindowLongPtrW(hwnd, GWL_STYLE, newStyle);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOPARENTNOTIFY);

    SetLastError(0);
    if (SetParent(hwnd, parent) == nullptr && GetLastError() != 0)
    {
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
        return false;
    }

    RECT client = { 0, 0, 1, 1 };
    if (parent != nullptr)
        GetClientRect(parent, &client);

    SetWindowPos(hwnd, nullptr, 0, 0, client.right > 0 ? client.right : 1, client.bottom > 0 ? client.bottom : 1,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

    return true;
}

inline static LRESULT CALLBACK HiddenSwapchainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case kHiddenSwapchainWindowMsgAttach:
        return HiddenSwapchainWindowAttach(hwnd, reinterpret_cast<HWND>(wParam)) ? 1 : 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_DESTROY:
        // Window created on its own pump thread: let that thread exit
        if (RemovePropW(hwnd, kHiddenSwapchainWindowPumpProp) != nullptr)
            PostQuitMessage(0);
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct HiddenSwapchainWindowThreadStart
{
    HANDLE ready = nullptr;
    HWND hwnd = nullptr;
};

inline DWORD WINAPI HiddenSwapchainWindowThread(LPVOID param)
{
    auto start = static_cast<HiddenSwapchainWindowThreadStart*>(param);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHiddenSwapchainWindowClass, L"", WS_POPUP, 0, 0,
                                1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (hwnd != nullptr)
    {
        SetPropW(hwnd, kHiddenSwapchainWindowPumpProp, reinterpret_cast<HANDLE>(1));
        ShowWindow(hwnd, SW_HIDE);
    }

    start->hwnd = hwnd;
    SetEvent(start->ready); // 'start' belongs to the creating thread from here on

    if (hwnd == nullptr)
        return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        DispatchMessageW(&msg);

    return 0;
}

// The hidden window gets a thread of its own that always pumps messages. ReShade maps the mouse through this window
// (it is the output window of the swapchain ReShade wraps), so it has to follow the game window. The thread that
// creates the swapchain (usually the game's main thread) does not necessarily pump messages, and then any
// SetWindowPos or SetParent request for a window it owns never runs (or would block). This used to depend on
// finding ReShade in a module snapshot, which can fail while other threads load modules (ERROR_BAD_LENGTH); the
// window then stayed 1x1 and ReShade's overlay ignored the mouse. The thread costs nothing without ReShade.
inline HWND CreateHiddenSwapchainWindow()
{
    if (g_hiddenSwapchainWindow != nullptr)
    {
        if (IsWindow(g_hiddenSwapchainWindow))
            return g_hiddenSwapchainWindow;

        // Destroyed along with a parent game window, create a new one
        g_hiddenSwapchainWindow = nullptr;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HiddenSwapchainWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHiddenSwapchainWindowClass;

    if (!RegisterClassExW(&wc))
    {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("RegisterClassExW failed");
    }

    // On its own message-pump thread; falls back to a window owned by this thread if that thread cannot start
    {
        // Heap allocated and only freed once the thread has signalled, so a timeout never leaves it dangling
        auto start = new HiddenSwapchainWindowThreadStart();
        start->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        HANDLE thread = nullptr;
        if (start->ready != nullptr)
            thread = CreateThread(nullptr, 0, HiddenSwapchainWindowThread, start, 0, nullptr);

        if (thread != nullptr)
        {
            const auto waitResult = WaitForSingleObject(start->ready, 10000);
            CloseHandle(thread);

            if (waitResult == WAIT_OBJECT_0)
            {
                g_hiddenSwapchainWindow = start->hwnd;
                CloseHandle(start->ready);
                delete start;
            }
            // else: leak 'start' on purpose, the thread may still write to it
        }
        else
        {
            if (start->ready != nullptr)
                CloseHandle(start->ready);

            delete start;
        }

        if (g_hiddenSwapchainWindow != nullptr)
            return g_hiddenSwapchainWindow;
    }

    g_hiddenSwapchainWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kHiddenSwapchainWindowClass, L"", WS_POPUP, 0, 0, 1, 1,
                                              nullptr, nullptr, wc.hInstance, nullptr);

    if (g_hiddenSwapchainWindow == nullptr)
        throw std::runtime_error("CreateWindowExW failed");

    ShowWindow(g_hiddenSwapchainWindow, SW_HIDE);
    return g_hiddenSwapchainWindow;
}

inline void DestroyHiddenSwapchainWindow()
{
    if (g_hiddenSwapchainWindow != nullptr)
    {
        // DestroyWindow only works on the owning thread; a pump-thread window closes itself
        if (GetWindowThreadProcessId(g_hiddenSwapchainWindow, nullptr) == GetCurrentThreadId())
            DestroyWindow(g_hiddenSwapchainWindow);
        else
            PostMessageW(g_hiddenSwapchainWindow, WM_CLOSE, 0, 0);

        g_hiddenSwapchainWindow = nullptr;
    }
}
