#pragma once

// What the menu host uses in place of Foundation. The host is the iOS
// port's MinecraftViewController brought over function for function, and
// these are the few things it needed from NSString, NSData, NSFileManager
// and the main queue.
//
// A string is UTF-8 in a std::string, and an empty one stands for nil.

#include <cstdarg>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MCLE_Android_Main.h"

namespace host {

// One argument of a call into a movie: a number, a bool or a string
struct Arg {
    enum Kind { Number = 0, Bool = 1, String = 2 };
    Kind kind;
    double number = 0.0;
    std::string string;

    Arg(double v) : kind(Number), number(v) {}
    Arg(float v) : kind(Number), number(v) {}
    Arg(int v) : kind(Number), number(v) {}
    Arg(unsigned v) : kind(Number), number(v) {}
    Arg(long v) : kind(Number), number((double)v) {}
    Arg(unsigned long v) : kind(Number), number((double)v) {}
    Arg(long long v) : kind(Number), number((double)v) {}
    Arg(unsigned long long v) : kind(Number), number((double)v) {}
    Arg(bool v) : kind(Bool), number(v ? 1.0 : 0.0) {}
    Arg(const char* v) : kind(String), string(v ? v : "") {}
    Arg(const std::string& v) : kind(String), string(v) {}
};
typedef std::vector<Arg> Args;

struct Size {
    float width = 0;
    float height = 0;
};
struct Point {
    float x = 0;
    float y = 0;
};
struct Rect {
    Point origin;
    Size size;
};

// printf into a string (stringWithFormat, with %s and c_str() for %@)
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// NSLog
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

bool has_prefix(const std::string& s, const std::string& prefix);
bool has_suffix(const std::string& s, const std::string& suffix);
bool contains(const std::string& s, const std::string& what);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
std::string lowercase(std::string s);
std::string trim(const std::string& s);
// componentsSeparatedByString
std::vector<std::string> split(const std::string& s, const std::string& separator);

// stringByAppendingPathComponent, lastPathComponent, pathExtension,
// stringByDeletingPathExtension, stringByDeletingLastPathComponent
std::string path_join(const std::string& dir, const std::string& name);
std::string last_path_component(const std::string& path);
std::string path_extension(const std::string& path);
std::string delete_path_extension(const std::string& path);
std::string delete_last_path_component(const std::string& path);

// The folder the game files are in (NSDocumentDirectory)
std::string documents_dir();
// file:// and the path, its spaces as %20
std::string file_url(const std::string& path);

bool file_exists(const std::string& path);
bool is_directory(const std::string& path);
// Empty when the file can't be read
std::vector<uint8_t> read_file(const std::string& path);
std::string read_text_file(const std::string& path);
// Written to a temporary file that's then renamed over the path
bool write_file(const std::string& path, const void* data, size_t length);
bool write_text_file(const std::string& path, const std::string& text);
bool remove_file(const std::string& path);
bool copy_file(const std::string& from, const std::string& to);
bool make_directories(const std::string& path);
// The names in a folder, not sorted
std::vector<std::string> list_directory(const std::string& path);
// Seconds since 1970 the file was last changed, 0 without the file
double file_modified(const std::string& path);

// wchar_t is 4 bytes here as it is on iOS. The game's wide strings hold
// UTF-32, and the ones that go into a save or over the network hold
// UTF-16 units one per wchar_t.
std::wstring wide_from_utf8(const std::string& s);
std::string utf8_from_wide(const std::wstring& w);
std::wstring utf16_wide_from_utf8(const std::string& s);
std::string utf8_from_utf16_wide(const std::wstring& w);
// How many UTF-16 units the string is (NSString's length)
size_t utf16_length(const std::string& s);

// CACurrentMediaTime
inline double now() { return mcle_media_time(); }
// dispatch_async(dispatch_get_main_queue(), ...)
inline void main_async(std::function<void()> work) { mcle_main_async(std::move(work)); }
// dispatch_after on the main queue
inline void main_after(double seconds, std::function<void()> work) {
    mcle_main_after((int64_t)(seconds * 1000.0), std::move(work));
}

} // namespace host
