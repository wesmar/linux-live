// RAII owners for Win32 resources. Every handle the program opens is owned by one of these.
#pragma once
#include <Windows.h>
#include <memory>
#include <type_traits>
#include <utility>

namespace DL {

// Generic owner. Traits supplies the handle type, its "no handle" value and how to close it.
template <class Traits>
class Unique {
public:
    using Type = typename Traits::Type;

    Unique() noexcept = default;
    explicit Unique(Type value) noexcept : m_value(value) {}
    Unique(const Unique&) = delete;
    Unique& operator=(const Unique&) = delete;
    Unique(Unique&& other) noexcept : m_value(std::exchange(other.m_value, Traits::Invalid())) {}
    Unique& operator=(Unique&& other) noexcept {
        if (this != &other) reset(std::exchange(other.m_value, Traits::Invalid()));
        return *this;
    }
    ~Unique() { reset(); }

    [[nodiscard]] Type get() const noexcept { return m_value; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_value != Traits::Invalid(); }
    [[nodiscard]] Type* put() noexcept { reset(); return &m_value; }
    Type release() noexcept { return std::exchange(m_value, Traits::Invalid()); }
    void reset(Type value = Traits::Invalid()) noexcept {
        if (m_value != Traits::Invalid()) Traits::Close(m_value);
        m_value = value;
    }

private:
    Type m_value = Traits::Invalid();
};

namespace traits {
template <class T, auto CloseFn>
struct NullIsInvalid {
    using Type = T;
    static Type Invalid() noexcept { return nullptr; }
    static void Close(Type h) noexcept { CloseFn(h); }
};
struct File {  // CreateFile / FindFirstVolume report failure as INVALID_HANDLE_VALUE
    using Type = HANDLE;
    static Type Invalid() noexcept { return INVALID_HANDLE_VALUE; }
    static void Close(Type h) noexcept { CloseHandle(h); }
};
struct VolumeFind {
    using Type = HANDLE;
    static Type Invalid() noexcept { return INVALID_HANDLE_VALUE; }
    static void Close(Type h) noexcept { FindVolumeClose(h); }
};
inline void CloseKernel(HANDLE h) noexcept { CloseHandle(h); }
inline void CloseKey(HKEY h) noexcept { RegCloseKey(h); }
inline void DeleteGdi(HGDIOBJ h) noexcept { DeleteObject(h); }
inline void DestroyMenuHandle(HMENU h) noexcept { DestroyMenu(h); }
inline void DestroyAccel(HACCEL h) noexcept { DestroyAcceleratorTable(h); }
inline void Unhook(HHOOK h) noexcept { UnhookWindowsHookEx(h); }
inline void DeleteMemoryDc(HDC h) noexcept { DeleteDC(h); }
inline void DestroyIconHandle(HICON h) noexcept { DestroyIcon(h); }
}  // namespace traits

using FileHandle = Unique<traits::File>;
using VolumeFind = Unique<traits::VolumeFind>;
using KernelHandle = Unique<traits::NullIsInvalid<HANDLE, traits::CloseKernel>>;
using RegKey = Unique<traits::NullIsInvalid<HKEY, traits::CloseKey>>;
using MenuHandle = Unique<traits::NullIsInvalid<HMENU, traits::DestroyMenuHandle>>;
using AccelHandle = Unique<traits::NullIsInvalid<HACCEL, traits::DestroyAccel>>;
using HookHandle = Unique<traits::NullIsInvalid<HHOOK, traits::Unhook>>;
using MemoryDc = Unique<traits::NullIsInvalid<HDC, traits::DeleteMemoryDc>>;  // CreateCompatibleDC
using IconHandle = Unique<traits::NullIsInvalid<HICON, traits::DestroyIconHandle>>;
template <class T>
using Gdi = Unique<traits::NullIsInvalid<T, traits::DeleteGdi>>;  // HBRUSH, HPEN, HFONT, HBITMAP ...

// Window DC from GetDC, released on scope exit.
class WindowDc {
public:
    explicit WindowDc(HWND window) noexcept : m_window(window), m_dc(GetDC(window)) {}
    WindowDc(const WindowDc&) = delete;
    WindowDc& operator=(const WindowDc&) = delete;
    ~WindowDc() { if (m_dc) ReleaseDC(m_window, m_dc); }
    [[nodiscard]] HDC get() const noexcept { return m_dc; }

private:
    HWND m_window;
    HDC m_dc;
};

// Selects an object into a DC and restores the previous one.
class SelectGuard {
public:
    SelectGuard(HDC dc, HGDIOBJ object) noexcept : m_dc(dc), m_old(SelectObject(dc, object)) {}
    SelectGuard(const SelectGuard&) = delete;
    SelectGuard& operator=(const SelectGuard&) = delete;
    ~SelectGuard() { if (m_old) SelectObject(m_dc, m_old); }

private:
    HDC m_dc;
    HGDIOBJ m_old;
};

// BeginPaint / EndPaint.
class PaintScope {
public:
    explicit PaintScope(HWND window) noexcept : m_window(window) { BeginPaint(window, &m_ps); }
    PaintScope(const PaintScope&) = delete;
    PaintScope& operator=(const PaintScope&) = delete;
    ~PaintScope() { EndPaint(m_window, &m_ps); }
    [[nodiscard]] HDC dc() const noexcept { return m_ps.hdc; }
    [[nodiscard]] const RECT& area() const noexcept { return m_ps.rcPaint; }

private:
    HWND m_window;
    PAINTSTRUCT m_ps{};
};

// Runs a callable on scope exit (for the rare one-off cleanups that have no handle type).
template <class F>
class [[nodiscard]] ScopeExit {
public:
    explicit ScopeExit(F f) noexcept(std::is_nothrow_move_constructible_v<F>) : m_f(std::move(f)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { if (m_active) m_f(); }
    void dismiss() noexcept { m_active = false; }

private:
    F m_f;
    bool m_active = true;
};

}  // namespace DL
