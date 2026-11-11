#include "MCLEHostSupport.h"

#include "STO_iOS_Paths.h"

#include <android/log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace host {

std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string out;
    if (n > 0) {
        out.resize((size_t)n);
        vsnprintf(&out[0], (size_t)n + 1, fmt, args);
    }
    va_end(args);
    return out;
}

void log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "mcle", fmt, args);
    va_end(args);
}

bool has_prefix(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool has_suffix(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& s, const std::string& what) {
    return s.find(what) != std::string::npos;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    const char* space = " \t\r\n";
    const size_t first = s.find_first_not_of(space);
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(space) - first + 1);
}

std::vector<std::string> split(const std::string& s, const std::string& separator) {
    std::vector<std::string> out;
    if (separator.empty()) {
        out.push_back(s);
        return out;
    }
    size_t start = 0;
    for (;;) {
        const size_t pos = s.find(separator, start);
        if (pos == std::string::npos) break;
        out.push_back(s.substr(start, pos - start));
        start = pos + separator.size();
    }
    out.push_back(s.substr(start));
    return out;
}

std::string path_join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string last_path_component(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string path_extension(const std::string& path) {
    const std::string name = last_path_component(path);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? "" : name.substr(dot + 1);
}

std::string delete_path_extension(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash + 2)) return path;
    return path.substr(0, dot);
}

std::string delete_last_path_component(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "";
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::string documents_dir() {
    return ios_documents_dir();
}

std::string file_url(const std::string& path) {
    return "file://" + replace_all(path, " ", "%20");
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        out.resize((size_t)size);
        if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    fclose(f);
    return out;
}

std::string read_text_file(const std::string& path) {
    const std::vector<uint8_t> data = read_file(path);
    return std::string(data.begin(), data.end());
}

bool write_file(const std::string& path, const void* data, size_t length) {
    const std::string temp = path + ".tmp";
    FILE* f = fopen(temp.c_str(), "wb");
    if (!f) return false;
    const bool ok = length == 0 || fwrite(data, 1, length, f) == length;
    fclose(f);
    if (!ok || rename(temp.c_str(), path.c_str()) != 0) {
        unlink(temp.c_str());
        return false;
    }
    return true;
}

bool write_text_file(const std::string& path, const std::string& text) {
    return write_file(path, text.data(), text.size());
}

bool remove_file(const std::string& path) {
    return unlink(path.c_str()) == 0;
}

bool copy_file(const std::string& from, const std::string& to) {
    const std::vector<uint8_t> data = read_file(from);
    if (data.empty() && !file_exists(from)) return false;
    return write_file(to, data.data(), data.size());
}

bool make_directories(const std::string& path) {
    if (path.empty() || is_directory(path)) return !path.empty();
    make_directories(delete_last_path_component(path));
    return mkdir(path.c_str(), 0770) == 0 || is_directory(path);
}

std::vector<std::string> list_directory(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        out.push_back(e->d_name);
    }
    closedir(d);
    return out;
}

double file_modified(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0.0;
    return (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec / 1e9;
}

namespace {

// The next code point of a UTF-8 string, U+FFFD for a bad sequence
uint32_t next_code_point(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return 0xFFFD;
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

} // namespace

std::wstring wide_from_utf8(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) out += (wchar_t)next_code_point(s, i);
    return out;
}

std::string utf8_from_wide(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) append_utf8(out, (uint32_t)c);
    return out;
}

std::wstring utf16_wide_from_utf8(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next_code_point(s, i);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 | (cp >> 10));
            out += (wchar_t)(0xDC00 | (cp & 0x3FF));
        } else {
            out += (wchar_t)cp;
        }
    }
    return out;
}

std::string utf8_from_utf16_wide(const std::wstring& w) {
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = (uint32_t)w[i] & 0xFFFF;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size()) {
            const uint32_t low = (uint32_t)w[i + 1] & 0xFFFF;
            if (low >= 0xDC00 && low < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        append_utf8(out, cp);
    }
    return out;
}

size_t utf16_length(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) n += next_code_point(s, i) >= 0x10000 ? 2 : 1;
    return n;
}

} // namespace host
