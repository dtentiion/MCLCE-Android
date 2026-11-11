#include "STO_Android_Paths.h"
#include "STO_iOS_Paths.h"

#include <string>
#include <sys/stat.h>

namespace {

std::string g_documents;
std::string g_support;
std::string g_caches;
std::string g_gamehdd;

void make_dir(const std::string& path) {
    if (!path.empty()) mkdir(path.c_str(), 0770);
}

} // namespace

void mcle_android_set_dirs(const char* documents, const char* support, const char* caches) {
    g_documents = documents ? documents : "";
    g_support = support ? support : "";
    g_caches = caches ? caches : "";
    g_gamehdd = g_support + "/GameHDD";
    make_dir(g_documents);
    make_dir(g_support);
    make_dir(g_caches);
    make_dir(g_gamehdd);
}

const char* ios_documents_dir(void) { return g_documents.c_str(); }
const char* ios_app_support_dir(void) { return g_support.c_str(); }
const char* ios_caches_dir(void) { return g_caches.c_str(); }

// Nothing ships inside the apk, what the bundle had on iOS is looked for
// next to the game files
const char* ios_bundle_resources_dir(void) { return g_documents.c_str(); }

const char* ios_gamehdd_dir(void) { return g_gamehdd.c_str(); }

extern "C" const char* mcle_save_root(void) { return ios_gamehdd_dir(); }
