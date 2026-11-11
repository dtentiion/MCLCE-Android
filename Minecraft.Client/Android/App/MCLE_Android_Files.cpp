// The game files folder made ready at launch: a README so the folder shows
// up, and the Windows64 install unpacked the way the port reads it.

#include "MCLE_Android_Files.h"
#include "MCLEHostSupport.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

uint32_t read_big_endian_32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Writes an .arc's files (ArchiveFile's layout) that aren't in the folder
// yet, all of them or only the names given. Returns how many it wrote.
int unpack_arc(const std::string& docs, const std::string& arcName, const std::set<std::string>* only) {
    const std::vector<uint8_t> arc = host::read_file(host::path_join(docs, arcName));
    if (arc.size() < 4) return 0;
    const uint8_t* d = arc.data();
    const size_t size = arc.size();
    const uint32_t count = read_big_endian_32(d);
    size_t pos = 4;
    int written = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 2 > size) break;
        const size_t nameLen = ((size_t)d[pos] << 8) | d[pos + 1];
        pos += 2;
        if (pos + nameLen + 8 > size) break;
        std::string name((const char*)d + pos, nameLen);
        pos += nameLen;
        const uint32_t offset = read_big_endian_32(d + pos);
        const uint32_t length = read_big_endian_32(d + pos + 4);
        pos += 8;
        // A leading * is a compressed entry to ArchiveFile, these have none
        if (name.empty() || name[0] == '*' || (uint64_t)offset + length > size) continue;
        name = host::replace_all(name, "\\", "/");
        if (name[0] == '/' || host::contains(name, "..")) continue;
        if (only && !only->count(name)) continue;
        const std::string out = host::path_join(docs, name);
        if (host::file_exists(out)) continue;
        host::make_directories(host::delete_last_path_component(out));
        if (host::write_file(out, d + offset, length)) ++written;
    }
    if (written) host::log("[files] unpacked %d files from %s", written, host::last_path_component(arcName).c_str());
    return written;
}

// The Windows64 install copied in as it is. CMinecraftApp reads its media
// out of Common/Media/MediaWindows64.arc (ArchiveFile), the port reads the
// same files loose from the top of the folder, so they're unpacked there.
// Files already there are left alone.
void unpack_windows64_install(const std::string& docs) {
    if (docs.empty()) return;
    unpack_arc(docs, "Common/Media/MediaWindows64.arc", nullptr);
    // The PS4 skins for a PlayStation pad, as the PS4 build has them in its
    // MediaOrbis.arc. The source's OrbisMedia copies are from before TU19.
    const std::set<std::string> orbisSkins = { "skinHDOrbis.swf", "skinOrbis.swf" };
    unpack_arc(docs, "Common/Media/MediaOrbis.arc", &orbisSkins);

    // The loose files read from the top of the folder
    const char* const loose[][2] = {
        { "Common/Media/font/Mojang Font_7.ttf", "Mojang Font_7.ttf" },
        { "Common/Media/font/Mojang Font_11.ttf", "Mojang Font_11.ttf" },
        { "OrbisMedia/Media/skinHDOrbis.swf", "skinHDOrbis.swf" },
        { "OrbisMedia/Media/skinOrbis.swf", "skinOrbis.swf" },
    };
    for (const auto& file : loose) {
        const std::string from = host::path_join(docs, file[0]);
        const std::string to = host::path_join(docs, file[1]);
        if (host::file_exists(from) && !host::file_exists(to)) host::copy_file(from, to);
    }
}

} // namespace

void mcle_android_prime_documents_folder(void) {
    const std::string docs = host::documents_dir();
    if (docs.empty()) return;
    host::make_directories(docs);

    // Rewritten every launch so updates to the text get through
    const std::string readme =
        "Minecraft LCE for Android\n"
        "=========================\n\n"
        "Copy the Common, Windows64Media and music folders from your own\n"
        "copy of the TU19 Windows64 build into this folder, then close the\n"
        "app fully and open it again.\n\n"
        "PlayStation button icons come from Common/Media/MediaOrbis.arc,\n"
        "or OrbisMedia/Media if you copy that in too.\n";
    host::write_text_file(host::path_join(docs, "README.txt"), readme);

    unpack_windows64_install(docs);
}
