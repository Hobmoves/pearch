#include "platform/console.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include "core/text.h"

namespace term {

namespace {

int g_tty_fd = -1;
termios g_saved{};
bool g_raw = false;
int g_sig_pipe[2] = {-1, -1};

const char kEnter[] = "\x1b[?1049h\x1b[?25l\x1b[H\x1b[2J";
const char kMouseOn[] = "\x1b[?1000h\x1b[?1006h";
const char kLeave[] = "\x1b[?2004l\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?25h\x1b[?1049l";

void restore_tty() {
    if (g_tty_fd >= 0 && g_raw) {
        ssize_t ignored = ::write(g_tty_fd, kLeave, sizeof kLeave - 1);
        (void)ignored;
        ::tcsetattr(g_tty_fd, TCSAFLUSH, &g_saved);
        g_raw = false;
    }
}

void on_fatal_signal(int sig) {
    restore_tty();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

void on_winch(int) {
    if (g_sig_pipe[1] >= 0) {
        char c = 'R';
        ssize_t ignored = ::write(g_sig_pipe[1], &c, 1);
        (void)ignored;
    }
}

}  // namespace

bool stdout_is_tty() { return ::isatty(STDOUT_FILENO); }
bool stdin_is_tty() { return ::isatty(STDIN_FILENO); }
bool stderr_is_tty() { return ::isatty(STDERR_FILENO); }

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
    const char* t = std::getenv("TERM");
    std::string termv = t ? t : "";
    if (termv == "dumb") return ColorMode::None;
    const char* ct = std::getenv("COLORTERM");
    std::string ctv = ct ? ct : "";
    if (ctv == "truecolor" || ctv == "24bit") return ColorMode::TrueColor;
    const char* tp = std::getenv("TERM_PROGRAM");
    std::string tpv = tp ? tp : "";
    if (tpv == "iTerm.app" || tpv == "WezTerm" || tpv == "vscode" || tpv == "ghostty") return ColorMode::TrueColor;
    if (termv.find("kitty") != std::string::npos || termv.find("alacritty") != std::string::npos ||
        termv.find("direct") != std::string::npos)
        return ColorMode::TrueColor;
    return ColorMode::Ansi256;
}

void prepare_output() {}

bool owns_console() { return false; }

int output_width() {
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    if (const char* c = std::getenv("COLUMNS")) return std::max(20, std::atoi(c));
    return 80;
}

struct Terminal::Impl {
    int fd = -1;
    bool active = false;
    bool in_paste = false;
    std::string buf;
    std::deque<Event> queue;

    // Parses one event from the front of buf. With `flush`, a lone or
    // unfinished escape sequence is taken as the Esc key.
    bool parse(Event& ev, bool flush) {
        if (buf.empty()) return false;
        ev = Event{};
        auto consume = [&](size_t n) { buf.erase(0, n); };
        unsigned char c = static_cast<unsigned char>(buf[0]);
        if (c == 0x1b) {
            if (buf.size() == 1) {
                if (!flush) return false;
                consume(1);
                ev.key = Key::Escape;
                return true;
            }
            if (buf[1] == '[') {
                size_t i = 2;
                while (i < buf.size() && !(buf[i] >= 0x40 && buf[i] <= 0x7E)) ++i;
                if (i >= buf.size()) {
                    if (!flush) return false;
                    consume(1);
                    ev.key = Key::Escape;
                    return true;
                }
                std::string params = buf.substr(2, i - 2);
                char fin = buf[i];
                consume(i + 1);
                return csi(params, fin, ev) || parse(ev, flush);
            }
            if (buf[1] == 'O') {
                if (buf.size() < 3) {
                    if (!flush) return false;
                    consume(1);
                    ev.key = Key::Escape;
                    return true;
                }
                char fin = buf[2];
                consume(3);
                return csi("", fin, ev) || parse(ev, flush);
            }
            if (buf[1] == 0x1b) {
                consume(1);
                ev.key = Key::Escape;
                return true;
            }
            // Alt + key
            consume(1);
            if (!parse(ev, true)) return false;
            ev.alt = true;
            return true;
        }
        if (c == '\r' || c == '\n') {
            consume(1);
            if (in_paste) { ev.key = Key::Char; ev.ch = ' '; return true; }
            ev.key = Key::Enter;
            ev.ctrl = c == '\n';
            return true;
        }
        if (c == 0x7f || c == 0x08) {
            consume(1);
            ev.key = Key::Backspace;
            ev.ctrl = c == 0x08;
            return true;
        }
        if (c == '\t') { consume(1); ev.key = Key::Tab; return true; }
        if (c < 0x20) {
            consume(1);
            ev.key = Key::Char;
            ev.ctrl = true;
            ev.ch = c == 0 ? U' ' : static_cast<char32_t>('a' + c - 1);
            return true;
        }
        // UTF-8 character: wait until it is complete.
        size_t need = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (buf.size() < need) {
            if (!flush) return false;
            need = 1;
        }
        size_t pos = 0;
        ev.key = Key::Char;
        ev.ch = text::decode_utf8(std::string_view(buf).substr(0, need), pos);
        consume(pos ? pos : 1);
        return true;
    }

    bool csi(const std::string& params, char fin, Event& ev) {
        if (!params.empty() && params[0] == '<') {  // SGR mouse: <b;x;yM
            int b = 0, x = 0, y = 0;
            if (std::sscanf(params.c_str() + 1, "%d;%d;%d", &b, &x, &y) != 3) return false;
            ev.type = Event::Type::Mouse;
            ev.x = x - 1;
            ev.y = y - 1;
            ev.ctrl = (b & 16) != 0;
            if (b & 64) ev.mouse = (b & 1) ? MouseKind::WheelDown : MouseKind::WheelUp;
            else if (b & 32) ev.mouse = MouseKind::Move;
            else if ((b & 3) != 0) return false;  // only the left button
            else ev.mouse = fin == 'M' ? MouseKind::Press : MouseKind::Release;
            return true;
        }
        int a = 0, mod = 1;
        std::sscanf(params.c_str(), "%d;%d", &a, &mod);
        mod = std::max(mod, 1) - 1;
        ev.shift = mod & 1;
        ev.alt = mod & 2;
        ev.ctrl = mod & 4;
        switch (fin) {
            case 'A': ev.key = Key::Up; return true;
            case 'B': ev.key = Key::Down; return true;
            case 'C': ev.key = Key::Right; return true;
            case 'D': ev.key = Key::Left; return true;
            case 'H': ev.key = Key::Home; return true;
            case 'F': ev.key = Key::End; return true;
            case 'Z': ev.key = Key::BackTab; return true;
            case '~':
                switch (a) {
                    case 1: case 7: ev.key = Key::Home; return true;
                    case 4: case 8: ev.key = Key::End; return true;
                    case 2: ev.key = Key::Insert; return true;
                    case 3: ev.key = Key::Delete; return true;
                    case 5: ev.key = Key::PageUp; return true;
                    case 6: ev.key = Key::PageDown; return true;
                    case 200: in_paste = true; return false;
                    case 201: in_paste = false; return false;
                    default: return false;
                }
            default:
                return false;
        }
    }
};

Terminal::Terminal() : impl_(std::make_unique<Impl>()) {}
Terminal::~Terminal() { stop(); }

bool Terminal::start(bool mouse) {
    Impl& d = *impl_;
    if (d.active) return true;
    d.fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (d.fd < 0) return false;
    if (::tcgetattr(d.fd, &g_saved) != 0) {
        ::close(d.fd);
        d.fd = -1;
        return false;
    }
    termios raw = g_saved;
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(d.fd, TCSAFLUSH, &raw);
    g_tty_fd = d.fd;
    g_raw = true;
    if (g_sig_pipe[0] < 0 && ::pipe(g_sig_pipe) == 0) {
        for (int p : g_sig_pipe) ::fcntl(p, F_SETFL, ::fcntl(p, F_GETFL) | O_NONBLOCK);
    }
    struct sigaction sa{};
    sa.sa_handler = on_winch;
    ::sigaction(SIGWINCH, &sa, nullptr);
    for (int sig : {SIGTERM, SIGHUP, SIGQUIT}) ::signal(sig, on_fatal_signal);
    d.active = true;
    std::string init = kEnter;
    if (mouse) init += kMouseOn;
    init += "\x1b[?2004h";  // bracketed paste
    write(init);
    return true;
}

void Terminal::stop() {
    Impl& d = *impl_;
    if (!d.active) return;
    restore_tty();
    ::signal(SIGWINCH, SIG_DFL);
    ::close(d.fd);
    d.fd = -1;
    g_tty_fd = -1;
    d.active = false;
}

bool Terminal::active() const noexcept { return impl_->active; }

void Terminal::size(int& cols, int& rows) const {
    winsize ws{};
    int fd = impl_->fd >= 0 ? impl_->fd : STDOUT_FILENO;
    if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    } else {
        cols = 80;
        rows = 24;
    }
}

bool Terminal::poll(Event& ev, int timeout_ms) {
    Impl& d = *impl_;
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        if (!d.queue.empty()) {
            ev = d.queue.front();
            d.queue.pop_front();
            return true;
        }
        if (d.parse(ev, false)) return true;
        int wait = -1;
        if (timeout_ms >= 0) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            wait = static_cast<int>(std::max<long long>(0, left));
        }
        if (!d.buf.empty()) wait = wait < 0 ? 30 : std::min(wait, 30);  // escape-sequence timeout
        pollfd fds[2] = {{d.fd, POLLIN, 0}, {g_sig_pipe[0], POLLIN, 0}};
        int r = ::poll(fds, g_sig_pipe[0] >= 0 ? 2 : 1, wait);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (!d.buf.empty() && d.parse(ev, true)) return true;
            if (timeout_ms >= 0 && clock::now() >= deadline) return false;
            continue;
        }
        if (g_sig_pipe[0] >= 0 && (fds[1].revents & POLLIN)) {
            char tmp[64];
            ssize_t n = ::read(g_sig_pipe[0], tmp, sizeof tmp);
            bool resize = false, wake = false;
            for (ssize_t i = 0; i < n; ++i) {
                if (tmp[i] == 'R') resize = true;
                if (tmp[i] == 'W') wake = true;
            }
            if (resize) { Event e; e.type = Event::Type::Resize; d.queue.push_back(e); }
            if (wake) { Event e; e.type = Event::Type::Wake; d.queue.push_back(e); }
        }
        if (fds[0].revents & POLLIN) {
            char tmp[4096];
            ssize_t n = ::read(d.fd, tmp, sizeof tmp);
            if (n > 0) d.buf.append(tmp, static_cast<size_t>(n));
        } else if (fds[0].revents & (POLLHUP | POLLERR)) {
            return false;
        }
    }
}

void Terminal::wake() {
    if (g_sig_pipe[1] >= 0) {
        char c = 'W';
        ssize_t ignored = ::write(g_sig_pipe[1], &c, 1);
        (void)ignored;
    }
}

void Terminal::write(std::string_view s) {
    int fd = impl_->fd >= 0 ? impl_->fd : STDOUT_FILENO;
    while (!s.empty()) {
        ssize_t n = ::write(fd, s.data(), s.size());
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;
        }
        s.remove_prefix(static_cast<size_t>(n));
    }
}

}  // namespace term
