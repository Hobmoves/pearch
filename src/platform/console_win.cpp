#include "platform/console.h"

#include <windows.h>
#include <io.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <string>

namespace term {

namespace {

bool handle_is_console(DWORD which) {
    HANDLE h = GetStdHandle(which);
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleMode(h, &mode);
}

// Restores the console if the window is closed or Ctrl+Break is pressed
// while the full-screen UI is up.
HANDLE g_out_for_restore = INVALID_HANDLE_VALUE;
BOOL WINAPI ctrl_handler(DWORD) {
    if (g_out_for_restore != INVALID_HANDLE_VALUE) {
        const char seq[] = "\x1b[?25h\x1b[?1049l";
        DWORD written = 0;
        WriteFile(g_out_for_restore, seq, sizeof seq - 1, &written, nullptr);
    }
    return FALSE;
}

}  // namespace

bool stdout_is_tty() { return handle_is_console(STD_OUTPUT_HANDLE); }
bool stdin_is_tty() { return handle_is_console(STD_INPUT_HANDLE); }
bool stderr_is_tty() { return handle_is_console(STD_ERROR_HANDLE); }

ColorMode detect_color(bool force_off) {
    if (force_off) return ColorMode::None;
    const char* nc = std::getenv("NO_COLOR");
    if (nc && *nc) return ColorMode::None;
    if (const char* pc = std::getenv("PEARCH_COLOR")) {
        std::string v = pc;
        if (v == "none" || v == "off") return ColorMode::None;
        if (v == "256") return ColorMode::Ansi256;
        return ColorMode::TrueColor;
    }
    // Windows 10+ consoles and Windows Terminal render 24-bit colour.
    return ColorMode::TrueColor;
}

void prepare_output() {
    SetConsoleOutputCP(CP_UTF8);
    for (DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE h = GetStdHandle(which);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    }
}

bool owns_console() {
    DWORD pids[4];
    return GetConsoleProcessList(pids, 4) == 1;
}

int output_width() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
    return 80;
}

struct Terminal::Impl {
    HANDLE in = INVALID_HANDLE_VALUE;
    HANDLE out = INVALID_HANDLE_VALUE;
    HANDLE wake_event = nullptr;
    DWORD old_in_mode = 0, old_out_mode = 0;
    UINT old_cp = 0, old_out_cp = 0;
    bool active = false;
    bool mouse_down = false;
    wchar_t pending_high = 0;
    std::deque<Event> queue;

    void translate(const INPUT_RECORD& rec) {
        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            Event e;
            e.type = Event::Type::Resize;
            queue.push_back(e);
            return;
        }
        if (rec.EventType == MOUSE_EVENT) {
            const MOUSE_EVENT_RECORD& m = rec.Event.MouseEvent;
            Event e;
            e.type = Event::Type::Mouse;
            e.x = m.dwMousePosition.X;
            CONSOLE_SCREEN_BUFFER_INFO info;
            int top = GetConsoleScreenBufferInfo(out, &info) ? info.srWindow.Top : 0;
            e.y = m.dwMousePosition.Y - top;
            e.ctrl = (m.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
            if (m.dwEventFlags & MOUSE_WHEELED) {
                short delta = static_cast<short>(HIWORD(m.dwButtonState));
                e.mouse = delta > 0 ? MouseKind::WheelUp : MouseKind::WheelDown;
                queue.push_back(e);
            } else if (m.dwEventFlags == 0 || (m.dwEventFlags & DOUBLE_CLICK)) {
                bool down = (m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
                if (down != mouse_down || (m.dwEventFlags & DOUBLE_CLICK)) {
                    e.mouse = down ? MouseKind::Press : MouseKind::Release;
                    queue.push_back(e);
                }
                mouse_down = down;
            }
            return;
        }
        if (rec.EventType != KEY_EVENT) return;
        const KEY_EVENT_RECORD& k = rec.Event.KeyEvent;
        if (!k.bKeyDown) return;
        Event e;
        const DWORD st = k.dwControlKeyState;
        e.ctrl = (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        e.alt = (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        e.shift = (st & SHIFT_PRESSED) != 0;
        const WORD vk = k.wVirtualKeyCode;
        const wchar_t wc = k.uChar.UnicodeChar;
        switch (vk) {
            case VK_UP: e.key = Key::Up; break;
            case VK_DOWN: e.key = Key::Down; break;
            case VK_LEFT: e.key = Key::Left; break;
            case VK_RIGHT: e.key = Key::Right; break;
            case VK_HOME: e.key = Key::Home; break;
            case VK_END: e.key = Key::End; break;
            case VK_PRIOR: e.key = Key::PageUp; break;
            case VK_NEXT: e.key = Key::PageDown; break;
            case VK_DELETE: e.key = Key::Delete; break;
            case VK_INSERT: e.key = Key::Insert; break;
            case VK_RETURN: e.key = Key::Enter; break;
            case VK_ESCAPE: e.key = Key::Escape; break;
            case VK_BACK: e.key = Key::Backspace; break;
            case VK_TAB: e.key = e.shift ? Key::BackTab : Key::Tab; break;
            default:
                if (e.ctrl && !e.alt && vk >= 'A' && vk <= 'Z') {
                    e.key = Key::Char;
                    e.ch = static_cast<char32_t>('a' + (vk - 'A'));
                } else if (e.ctrl && vk == VK_SPACE) {
                    e.key = Key::Char;
                    e.ch = ' ';
                } else if (wc >= 0xD800 && wc <= 0xDBFF) {
                    pending_high = wc;
                    return;
                } else if (wc >= 0xDC00 && wc <= 0xDFFF && pending_high) {
                    e.key = Key::Char;
                    e.ch = 0x10000 + ((static_cast<char32_t>(pending_high) - 0xD800) << 10) + (wc - 0xDC00);
                    pending_high = 0;
                    e.ctrl = e.alt = false;  // AltGr combinations are just characters
                } else if (wc >= 32) {
                    e.key = Key::Char;
                    e.ch = wc;
                    if (e.ctrl && e.alt) e.ctrl = e.alt = false;  // AltGr
                } else {
                    return;  // modifier keys etc.
                }
        }
        for (WORD r = 0; r < std::max<WORD>(1, k.wRepeatCount); ++r) queue.push_back(e);
    }
};

Terminal::Terminal() : impl_(std::make_unique<Impl>()) {}
Terminal::~Terminal() { stop(); }

bool Terminal::start(bool mouse) {
    Impl& d = *impl_;
    if (d.active) return true;
    d.in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
    d.out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
    if (d.in == INVALID_HANDLE_VALUE || d.out == INVALID_HANDLE_VALUE) return false;
    if (!GetConsoleMode(d.in, &d.old_in_mode) || !GetConsoleMode(d.out, &d.old_out_mode)) return false;
    d.old_cp = GetConsoleCP();
    d.old_out_cp = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    DWORD in_mode = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;  // no quick-edit, no line input
    if (mouse) in_mode |= ENABLE_MOUSE_INPUT;
    SetConsoleMode(d.in, in_mode);
    if (!SetConsoleMode(d.out, d.old_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                   ENABLE_PROCESSED_OUTPUT | DISABLE_NEWLINE_AUTO_RETURN)) {
        SetConsoleMode(d.in, d.old_in_mode);
        return false;  // pre-Windows 10 console without VT support
    }
    d.wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d.active = true;
    g_out_for_restore = d.out;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    write("\x1b[?1049h\x1b[?25l\x1b[H\x1b[2J");
    return true;
}

void Terminal::stop() {
    Impl& d = *impl_;
    if (!d.active) return;
    write("\x1b[0m\x1b[?25h\x1b[?1049l");
    SetConsoleMode(d.in, d.old_in_mode);
    SetConsoleMode(d.out, d.old_out_mode);
    SetConsoleCP(d.old_cp);
    SetConsoleOutputCP(d.old_out_cp);
    SetConsoleCtrlHandler(ctrl_handler, FALSE);
    g_out_for_restore = INVALID_HANDLE_VALUE;
    CloseHandle(d.wake_event);
    CloseHandle(d.in);
    CloseHandle(d.out);
    d.wake_event = nullptr;
    d.in = d.out = INVALID_HANDLE_VALUE;
    d.active = false;
}

bool Terminal::active() const noexcept { return impl_->active; }

void Terminal::size(int& cols, int& rows) const {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(impl_->out, &info)) {
        cols = info.srWindow.Right - info.srWindow.Left + 1;
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    } else {
        cols = 80;
        rows = 24;
    }
}

bool Terminal::poll(Event& ev, int timeout_ms) {
    Impl& d = *impl_;
    for (;;) {
        if (!d.queue.empty()) {
            ev = d.queue.front();
            d.queue.pop_front();
            return true;
        }
        HANDLE handles[2] = {d.in, d.wake_event};
        DWORD r = WaitForMultipleObjects(2, handles, FALSE,
                                         timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
        if (r == WAIT_OBJECT_0 + 1) {
            ev = Event{};
            ev.type = Event::Type::Wake;
            return true;
        }
        if (r != WAIT_OBJECT_0) return false;
        DWORD count = 0;
        if (!GetNumberOfConsoleInputEvents(d.in, &count) || count == 0) {
            if (timeout_ms >= 0) return false;
            continue;
        }
        INPUT_RECORD recs[128];
        DWORD read = 0;
        if (!ReadConsoleInputW(d.in, recs, 128, &read)) return false;
        for (DWORD i = 0; i < read; ++i) d.translate(recs[i]);
        if (d.queue.empty() && timeout_ms >= 0) {
            // Only key-ups or moves arrived; keep waiting for the rest of the timeout.
            timeout_ms = 0;
        }
    }
}

void Terminal::wake() {
    if (impl_->wake_event) SetEvent(impl_->wake_event);
}

void Terminal::write(std::string_view s) {
    HANDLE h = impl_->active ? impl_->out : GetStdHandle(STD_OUTPUT_HANDLE);
    while (!s.empty()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(s.size(), 1 << 20));
        DWORD written = 0;
        if (!WriteFile(h, s.data(), chunk, &written, nullptr) || written == 0) return;
        s.remove_prefix(written);
    }
}

}  // namespace term
