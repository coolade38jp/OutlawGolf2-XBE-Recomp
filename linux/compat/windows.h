#pragma once
// the SDL host: the Win32 names the game's hooks (GameInfo::message) and the runtime's portable files touch.
// Timers and posted messages work against the window thread's queue in linux/main.cpp
#include <cstdint>

typedef long HRESULT;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HWND;
typedef void* HCURSOR;
typedef const wchar_t* PCWSTR;
typedef unsigned int UINT;
typedef uintptr_t UINT_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef int BOOL;
#define WINAPI

inline HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char*) { return nullptr; }
inline HANDLE GetCurrentThread() { return nullptr; }

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

// window messages the game hooks name; the values are Win32's own, the game's tables are built from them
enum : uint32_t {
    WM_CLOSE = 0x0010,
    WM_KILLFOCUS = 0x0008,
    WM_TIMER = 0x0113,
    WM_KEYDOWN = 0x0100,
    WM_KEYUP = 0x0101,
    WM_SYSKEYDOWN = 0x0104,
    WM_SYSKEYUP = 0x0105,
    WM_ACTIVATE = 0x0006,
    WM_SETCURSOR = 0x0020,
    WM_INPUT = 0x00FF,
    WM_LBUTTONUP = 0x0202,
};
enum { WA_INACTIVE = 0 };
enum { VK_BACK = 0x08, VK_RETURN = 0x0D, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_LEFT = 0x25, VK_UP = 0x26,
       VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_F1 = 0x70, VK_F3 = 0x72, VK_F10 = 0x79, VK_OEM_3 = 0xC0 };
#define LOWORD(v) ((uint16_t)((uintptr_t)(v) & 0xFFFF))
#define MAKEINTRESOURCEW(i) ((const wchar_t*)(uintptr_t)(uint16_t)(i))

// linux/main.cpp's window thread
uint64_t GetTickCount64();
UINT_PTR SetTimer(HWND, UINT_PTR, UINT, void*);
BOOL KillTimer(HWND, UINT_PTR);
BOOL PostMessageW(HWND, uint32_t, WPARAM, LPARAM);
short GetAsyncKeyState(int);
HCURSOR SetCursor(HCURSOR);
inline HCURSOR LoadCursorW(void*, const wchar_t*) { return nullptr; }

int MessageBoxW(HWND, const wchar_t*, const wchar_t*, unsigned);
enum { MB_OK = 0, MB_YESNO = 4, MB_ICONERROR = 0x10, MB_ICONWARNING = 0x30, MB_ICONQUESTION = 0x20, MB_DEFBUTTON2 = 0x100 };
enum { IDYES = 6, IDNO = 7 };
#define MB_TOPMOST 0x00040000
