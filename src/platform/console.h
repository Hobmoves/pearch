#pragma once

// Raw terminal access: keyboard/mouse/resize events, the alternate screen,
// and colour support detection. Win32 console and POSIX tty implementations.

#include <memory>
#include <string>
#include <string_view>

namespace term {

enum class Key : unsigned char {
    None, Char, Enter, Escape, Backspace, Delete, Tab, BackTab,
    Up, Down, Left, Right, Home, End, PageUp, PageDown, Insert,
};

enum class MouseKind : unsigned char { Press, Release, WheelUp, WheelDown, Move };

struct Event {
    enum class Type : unsigned char { Key, Resize, Mouse, Wake } type = Type::Key;
    Key key = Key::None;
    char32_t ch = 0;  // for Key::Char (lowercase letter when ctrl is held)
    bool ctrl = false, alt = false, shift = false;
    MouseKind mouse = MouseKind::Move;
    int x = 0, y = 0;  // 0-based cell
};

enum class ColorMode { None, Ansi256, TrueColor };

bool stdout_is_tty();
bool stdin_is_tty();
bool stderr_is_tty();
ColorMode detect_color(bool force_off);
// Enable ANSI escapes + UTF-8 for plain (non full-screen) output.
void prepare_output();
// Windows: true when pearch got its own console window (double-click, hotkey).
bool owns_console();
// Terminal width for plain output (80 if unknown).
int output_width();

class Terminal {
public:
    Terminal();
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // Raw mode, alternate screen, hidden cursor, optional mouse reporting.
    bool start(bool mouse = true);
    void stop();
    bool active() const noexcept;

    void size(int& cols, int& rows) const;
    // Waits up to timeout_ms (-1 = forever) for an event.
    bool poll(Event& ev, int timeout_ms);
    // Thread-safe: makes a pending poll() return an Event::Type::Wake.
    void wake();
    void write(std::string_view s);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace term
