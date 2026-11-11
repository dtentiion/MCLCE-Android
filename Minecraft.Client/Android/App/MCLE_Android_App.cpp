// The app shell. The activity's surfaces, controller events and lifecycle
// come in over JNI, and one thread runs the menu host on them: what the
// main thread and its CADisplayLink do for the iOS port.

#include <jni.h>

#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MCLEHost.h"
#include "MCLEPlatform.h"
#include "MCLE_Android_Audio.h"
#include "MCLE_Android_Files.h"
#include "MCLE_Android_Main.h"
#include "INP_Android_Controller.h"
#include "STO_Android_Paths.h"
#include "MetalContext.h"
#include "ruffle_ios.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mcle", __VA_ARGS__)

extern "C" void mcle_save_now_sync(void); // Game/MCLEGameLoop.cpp
extern "C" void mcle_metal_attach_ui_layer(void* layer, int width, int height); // Render
extern "C" unsigned long mcle_gl_world_frames(void); // Render
extern PlayerHandle* g_ruffle_player;

// ruffle_android: the player's surfaces over new windows
extern "C" int ruffle_android_player_set_window(PlayerHandle* h, void* window, int width, int height);
extern "C" int ruffle_android_overlay_set_window(PlayerHandle* h, void* window, int width, int height);
extern "C" void ruffle_android_player_resize(PlayerHandle* h, int width, int height);

int g_ruffle_swf_version = -99;
int g_ruffle_player_ok = -99;
int g_ruffle_framerate_mHz = -99;
int g_ruffle_surface_probe = -99;
PlayerHandle* g_ruffle_player = NULL;

namespace {

enum { kLayerWorld = 0, kLayerSwf, kLayerCustomDraw, kLayerSwfTop, kLayerCount };

struct Timed {
    double when;
    std::function<void()> work;
};

std::mutex g_mu;
std::condition_variable g_cv;
// What the activity last handed over, and what the frame thread is drawing on
ANativeWindow* g_pending[kLayerCount] = {};
bool g_pending_changed[kLayerCount] = {};
ANativeWindow* g_windows[kLayerCount] = {};
// The view's size in pixels, which every layer but the world's has
int g_width = 0;
int g_height = 0;
// The world's surface can have fewer lines than the view
int g_world_width = 0;
int g_world_height = 0;
float g_density = 1.0f;
bool g_resumed = false;
bool g_thread_started = false;
std::atomic<bool> g_host_started{false};
pthread_t g_frame_thread;
// Nanoseconds from one display refresh to the next
std::atomic<int64_t> g_refresh_ns{16666667};
// Seconds the pass that's running took to come round
std::atomic<float> g_frame_seconds{1.0f / 60.0f};

std::mutex g_queue_mu;
std::deque<std::function<void()>> g_queue;
std::vector<Timed> g_timed;

JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;
jmethodID g_set_splash = nullptr;
jmethodID g_set_intro_backdrop = nullptr;
jmethodID g_set_world_lines = nullptr;

// The splash line as the host last set it, sent to the activity when it changes
struct Splash {
    std::string text;
    bool hidden = true;
    float alpha = 1.0f;
    float x = 0, y = 0, rotation = 0, scale = 1.0f;
    bool dirty = false;
} g_splash;

bool all_windows_locked() {
    for (ANativeWindow* window : g_pending) {
        if (!window) return false;
    }
    return true;
}

JNIEnv* frame_env() {
    static thread_local JNIEnv* env = nullptr;
    if (!env && g_vm) {
        // Attaching renames the thread unless it's told the name
        JavaVMAttachArgs args = { JNI_VERSION_1_6, "mcle-frame", nullptr };
        g_vm->AttachCurrentThread(&env, &args);
    }
    return env;
}

void run_queue() {
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lk(g_queue_mu);
        ready.swap(g_queue);
        const double now = mcle_media_time();
        for (size_t i = 0; i < g_timed.size();) {
            if (g_timed[i].when <= now) {
                ready.push_back(std::move(g_timed[i].work));
                g_timed.erase(g_timed.begin() + (long)i);
            } else {
                ++i;
            }
        }
    }
    for (std::function<void()>& work : ready) work();
}

void send_splash() {
    if (!g_splash.dirty) return;
    g_splash.dirty = false;
    JNIEnv* env = frame_env();
    if (!env || !g_activity || !g_set_splash) return;
    jstring text = env->NewStringUTF(g_splash.text.c_str());
    env->CallVoidMethod(g_activity, g_set_splash, text, (jboolean)g_splash.hidden, g_splash.alpha,
                        g_splash.x, g_splash.y, g_splash.rotation, g_splash.scale);
    env->DeleteLocalRef(text);
}

// The surfaces that changed while the app was away go back under the
// renderer and the movie player. A window that only changed size keeps
// its surface.
void adopt_windows() {
    ANativeWindow* old[kLayerCount] = {};
    bool replaced[kLayerCount] = {};
    bool resized[kLayerCount] = {};
    int width, height, world_width, world_height;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (int i = 0; i < kLayerCount; ++i) {
            if (!g_pending_changed[i]) continue;
            g_pending_changed[i] = false;
            if (g_pending[i] == g_windows[i]) {
                resized[i] = true;
                continue;
            }
            replaced[i] = true;
            old[i] = g_windows[i];
            g_windows[i] = g_pending[i];
            ANativeWindow_acquire(g_windows[i]);
        }
        width = g_width;
        height = g_height;
        world_width = g_world_width;
        world_height = g_world_height;
    }
    const bool ui_resized = resized[kLayerSwf] || resized[kLayerCustomDraw] || resized[kLayerSwfTop];
    if (g_host_started.load()) {
        if (replaced[kLayerWorld] || resized[kLayerWorld]) {
            mcle_metal_attach_layer(g_windows[kLayerWorld], world_width, world_height);
        }
        if (replaced[kLayerCustomDraw] || ui_resized) mcle_metal_attach_ui_layer(g_windows[kLayerCustomDraw], width, height);
        if (g_ruffle_player) {
            if (replaced[kLayerSwf]) {
                ruffle_android_player_set_window(g_ruffle_player, g_windows[kLayerSwf], width, height);
            }
            if (replaced[kLayerSwfTop]) {
                ruffle_android_overlay_set_window(g_ruffle_player, g_windows[kLayerSwfTop], width, height);
            }
            if (ui_resized) ruffle_android_player_resize(g_ruffle_player, width, height);
        }
    }
    for (ANativeWindow* window : old) {
        if (window) ANativeWindow_release(window);
    }
}

void on_vsync(long frame_time_ns, void* data) {
    *(int64_t*)data = frame_time_ns;
}

// A phone that can't draw the world at its own resolution at the consoles'
// 60 a second draws it at 720 lines instead, the 360's and the PS3's, and
// the display scales that up. The menus and the HUD are surfaces of their
// own and keep the screen's.
void watch_world_rate(int64_t took) {
    static bool lowered = false;
    static unsigned long seen = 0;
    static int settled = 0, frames = 0, slow = 0;
    static int64_t sum = 0;
    if (lowered) return;
    const unsigned long drawn = mcle_gl_world_frames();
    if (drawn == seen) {
        // No world on screen, the next one builds its chunks first
        settled = frames = slow = 0;
        sum = 0;
        return;
    }
    seen = drawn;
    if (settled < 300) {
        settled++;
        return;
    }
    // Not a hitch's
    if (took > 100000000) return;
    sum += took;
    if (++frames < 180) return;
    const int64_t average = sum / frames;
    const int64_t wanted = std::max<int64_t>(g_refresh_ns.load(), 16666667);
    frames = 0;
    sum = 0;
    slow = average > wanted + wanted / 7 ? slow + 1 : 0;
    if (slow < 3) return;
    lowered = true;
    JNIEnv* env = frame_env();
    if (env && g_activity && g_set_world_lines) env->CallVoidMethod(g_activity, g_set_world_lines, 720);
}

// How long a pass took to come round after the one before it
void note_pass(int64_t before, int64_t start) {
    const int64_t took = before ? std::max<int64_t>(0, start - before) : g_refresh_ns.load();
    g_frame_seconds.store((float)(took / 1e9));
    watch_world_rate(took);
}

// One pass a display refresh, like the CADisplayLink. A pass that ran past
// its refresh goes straight into the next one.
void wait_for_vsync() {
    static int64_t pass_start = 0;
    const int64_t before = pass_start;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    if (pass_start && now - pass_start >= g_refresh_ns.load()) {
        pass_start = now;
        note_pass(before, pass_start);
        return;
    }
    int64_t fired = 0;
    AChoreographer_postFrameCallback(AChoreographer_getInstance(), on_vsync, &fired);
    while (!fired) ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
    pass_start = fired;
    note_pass(before, pass_start);
}

void* frame_thread(void*) {
    pthread_setname_np(pthread_self(), "mcle-frame");
    ALooper_prepare(0);
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_resumed && all_windows_locked(); });
        }
        wait_for_vsync();
        adopt_windows();
        if (!g_host_started.load()) {
            LOG("[app] starting the host, %dx%d at %.2f", g_width, g_height, g_density);
            // AppDelegate's didFinishLaunching
            mcle_android_prime_documents_folder();
            ruffle_ios_init();
            g_host.viewDidLoad();
            g_host.viewWillAppear();
            g_host_started.store(true);
        }
        mcle_android_pad_frame();
        run_queue();
        g_host.tick();
        send_splash();
    }
    return nullptr;
}

// stdout and stderr go nowhere on Android. The movie player and the game
// both print to them, so they're read back off a pipe into the log.
void* output_thread(void* arg) {
    const int fd = (int)(intptr_t)arg;
    std::string line;
    char buf[1024];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, "mcle", line.c_str());
                line.clear();
            } else {
                line += buf[i];
            }
        }
    }
    return nullptr;
}

void redirect_output() {
    int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, output_thread, (void*)(intptr_t)fds[0]) == 0) pthread_detach(thread);
}

void surface_changed(int layer, ANativeWindow* window, int width, int height) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pending[layer]) ANativeWindow_release(g_pending[layer]);
    g_pending[layer] = window;
    g_pending_changed[layer] = window != nullptr;
    if (window && width > 0 && height > 0) {
        if (layer == kLayerWorld) {
            g_world_width = width;
            g_world_height = height;
        } else {
            g_width = width;
            g_height = height;
        }
    }
    if (all_windows_locked() && !g_thread_started) {
        g_thread_started = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        // The main thread's stack on iOS, the menus and the world load run deep
        pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
        pthread_create(&g_frame_thread, &attr, frame_thread, nullptr);
        pthread_attr_destroy(&attr);
    }
    g_cv.notify_all();
}

} // namespace

void mcle_main_async(std::function<void()> work) {
    std::lock_guard<std::mutex> lk(g_queue_mu);
    g_queue.push_back(std::move(work));
}

void mcle_main_after(int64_t milliseconds, std::function<void()> work) {
    std::lock_guard<std::mutex> lk(g_queue_mu);
    g_timed.push_back({ mcle_media_time() + (double)milliseconds / 1000.0, std::move(work) });
}

bool mcle_main_is_current(void) {
    return g_thread_started && pthread_equal(pthread_self(), g_frame_thread);
}

double mcle_media_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// dispatch/dispatch.h, the main queue only

dispatch_queue_t dispatch_get_main_queue(void) {
    return nullptr;
}

void dispatch_async_f(dispatch_queue_t, void* context, dispatch_function_t work) {
    mcle_main_async([context, work] { work(context); });
}

void dispatch_sync_f(dispatch_queue_t, void* context, dispatch_function_t work) {
    if (mcle_main_is_current()) {
        work(context);
        return;
    }
    std::promise<void> done;
    mcle_main_async([&] {
        work(context);
        done.set_value();
    });
    done.get_future().wait();
}

int pthread_main_np(void) {
    return mcle_main_is_current() ? 1 : 0;
}

// Game/MCLE_iOS_MachHandler.cpp's, there are no Mach exceptions here
extern "C" void mcle_install_mach_exception_handler(void) {}

// What the system had on the crash signals before the game's handler. Its
// handler writes the tombstone, so the game's puts it back when it's done.
static const int kCrashSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
static struct sigaction g_system_signal[NSIG];

static void keep_system_signals() {
    for (int sig : kCrashSignals) sigaction(sig, nullptr, &g_system_signal[sig]);
}

extern "C" void mcle_android_restore_signal(int sig) {
    if (sig > 0 && sig < NSIG) sigaction(sig, &g_system_signal[sig], nullptr);
}

namespace platform {

host::Size view_size() {
    host::Size size;
    size.width = (float)g_width / g_density;
    size.height = (float)g_height / g_density;
    return size;
}

float view_scale() {
    return g_density;
}

void* world_layer() { return g_windows[kLayerWorld]; }
void* swf_layer() { return g_windows[kLayerSwf]; }
void* custom_draw_layer() { return g_windows[kLayerCustomDraw]; }
void* swf_top_layer() { return g_windows[kLayerSwfTop]; }

int pad_is_playstation() {
    return mcle_android_pad_is_playstation();
}

void splash_set_text(const std::string& text) {
    if (g_splash.text == text) return;
    g_splash.text = text;
    g_splash.dirty = true;
}

void splash_set_hidden(bool hidden) {
    if (g_splash.hidden == hidden) return;
    g_splash.hidden = hidden;
    g_splash.dirty = true;
}

void splash_set_alpha(float alpha) {
    if (g_splash.alpha == alpha) return;
    g_splash.alpha = alpha;
    g_splash.dirty = true;
}

void splash_set_transform(float centre_x, float centre_y, float rotation, float scale) {
    if (g_splash.x == centre_x && g_splash.y == centre_y && g_splash.rotation == rotation &&
        g_splash.scale == scale) return;
    g_splash.x = centre_x;
    g_splash.y = centre_y;
    g_splash.rotation = rotation;
    g_splash.scale = scale;
    g_splash.dirty = true;
}

void intro_backdrop_set_shown(bool shown) {
    JNIEnv* env = frame_env();
    if (!env || !g_activity || !g_set_intro_backdrop) return;
    env->CallVoidMethod(g_activity, g_set_intro_backdrop, (jboolean)shown);
}

float frame_seconds() {
    return g_frame_seconds.load();
}

} // namespace platform

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    keep_system_signals();
    redirect_output();
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_com_legacy_mclce_MainActivity_nativeCreate(
    JNIEnv* env, jobject activity, jstring documents, jstring support, jstring caches, jfloat density) {
    if (g_activity) env->DeleteGlobalRef(g_activity);
    g_activity = env->NewGlobalRef(activity);
    jclass cls = env->GetObjectClass(activity);
    g_set_splash = env->GetMethodID(cls, "setSplash", "(Ljava/lang/String;ZFFFFF)V");
    g_set_intro_backdrop = env->GetMethodID(cls, "setIntroBackdrop", "(Z)V");
    g_set_world_lines = env->GetMethodID(cls, "setWorldLines", "(I)V");
    g_density = density > 0 ? density : 1.0f;
    // An activity made again starts without the splash text
    g_splash.dirty = true;

    const char* d = env->GetStringUTFChars(documents, nullptr);
    const char* s = env->GetStringUTFChars(support, nullptr);
    const char* c = env->GetStringUTFChars(caches, nullptr);
    mcle_android_set_dirs(d, s, c);
    LOG("[app] game files in %s", d);
    env->ReleaseStringUTFChars(documents, d);
    env->ReleaseStringUTFChars(support, s);
    env->ReleaseStringUTFChars(caches, c);
}

JNIEXPORT void JNICALL Java_com_legacy_mclce_MainActivity_nativeSurface(
    JNIEnv* env, jobject, jint layer, jobject surface, jint width, jint height) {
    if (layer < 0 || layer >= kLayerCount) return;
    surface_changed(layer, surface ? ANativeWindow_fromSurface(env, surface) : nullptr, width, height);
}

JNIEXPORT void JNICALL Java_com_legacy_mclce_MainActivity_nativeRefreshRate(JNIEnv*, jobject, jfloat refresh_rate) {
    if (refresh_rate >= 1.0f) g_refresh_ns.store((int64_t)(1e9 / refresh_rate));
}

JNIEXPORT void JNICALL Java_com_legacy_mclce_MainActivity_nativeResume(JNIEnv*, jobject, jfloat refresh_rate) {
    if (refresh_rate >= 1.0f) g_refresh_ns.store((int64_t)(1e9 / refresh_rate));
    mcle_android_audio_set_paused(false);
    std::lock_guard<std::mutex> lk(g_mu);
    g_resumed = true;
    g_cv.notify_all();
}

JNIEXPORT void JNICALL Java_com_legacy_mclce_MainActivity_nativePause(JNIEnv*, jobject) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_resumed = false;
    }
    mcle_android_audio_set_paused(true);
    // applicationDidEnterBackground: the world is saved off this thread
    if (g_host_started.load()) std::thread([] { mcle_save_now_sync(); }).detach();
}

JNIEXPORT void JNICALL Java_com_legacy_mclce_MainActivity_nativeKey(
    JNIEnv*, jobject, jint device, jint key_code, jboolean down) {
    mcle_android_pad_key(device, key_code, down ? 1 : 0);
}

JNIEXPORT void JNICALL Java_com_legacy_mclce_MainActivity_nativeAxes(
    JNIEnv* env, jobject, jint device, jfloatArray axes) {
    jfloat values[16];
    const jsize count = env->GetArrayLength(axes);
    if (count > 16) return;
    env->GetFloatArrayRegion(axes, 0, count, values);
    mcle_android_pad_axes(device, values, count);
}

JNIEXPORT void JNICALL Java_com_legacy_mclce_MainActivity_nativePadChanged(
    JNIEnv*, jobject, jint device, jboolean connected, jint vendor) {
    mcle_android_pad_changed(device, connected ? 1 : 0, vendor);
    // The pad's own skin, like the GameController notifications on iOS
    if (g_host_started.load()) mcle_main_async([] { g_host.platformPadChanged(); });
}

} // extern "C"
