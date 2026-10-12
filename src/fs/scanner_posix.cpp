#include "fs/scanner.h"

#include <dirent.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace scan::detail {

NativePath to_native(const std::string& utf8) { return utf8; }

NativePath join(const NativePath& dir, const std::string& name_utf8) {
    if (!dir.empty() && dir.back() == '/') return dir + name_utf8;
    return dir + "/" + name_utf8;
}

bool list_dir(const NativePath& dir, std::vector<DirEntry>& out) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    DIR* d = ::fdopendir(fd);
    if (!d) {
        ::close(fd);
        return false;
    }
    while (dirent* ent = ::readdir(d)) {
        const char* n = ent->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
        DirEntry e;
        e.name = n;
        e.hidden = n[0] == '.';
        struct stat st;
        if (::fstatat(fd, n, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            e.link = S_ISLNK(st.st_mode);
            e.is_dir = S_ISDIR(st.st_mode);
            e.size = e.is_dir ? 0 : static_cast<uint64_t>(st.st_size);
            e.mtime = static_cast<int64_t>(st.st_mtime) * 1000;
        } else {
            e.is_dir = ent->d_type == DT_DIR;
            e.link = ent->d_type == DT_LNK;
        }
        out.push_back(std::move(e));
    }
    ::closedir(d);
    return true;
}

void lower_thread_priority() {
#ifdef __linux__
    setpriority(PRIO_PROCESS, 0, 10);  // on Linux this applies to the calling thread
#endif
}

bool stat_dir(const NativePath& path, int64_t& mtime) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    mtime = static_cast<int64_t>(st.st_mtime) * 1000;
    return true;
}

}  // namespace scan::detail
