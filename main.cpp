#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

#include <sys/inotify.h>
#include <unistd.h>
#include <libgen.h>    // dirname
#include <cstring>     // strdup
#include <climits>     // PATH_MAX

#include <opencv2/opencv.hpp>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xfixes.h>

#include "json.hpp"
#include "ImmichClient.h"
#include "DisplayImage.h"

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Input state
// ---------------------------------------------------------------------------
static bool gIsPressed    = false;
static bool gPendingClick = false;
static int  gClickX       = 0;
static int  gClickY       = 0;
static int  gScreenWidth  = 1920;
static int  gScreenHeight = 1200;

static void onMouse(int event, int x, int y, int, void*)
{
    if (event == cv::EVENT_LBUTTONDOWN || event == cv::EVENT_RBUTTONDOWN) {
        gIsPressed = true; gClickX = x; gClickY = y;
    }
    if ((event == cv::EVENT_LBUTTONUP || event == cv::EVENT_RBUTTONUP) && gIsPressed) {
        gIsPressed = false; gPendingClick = true;
    }
}

// ---------------------------------------------------------------------------
// X11: hide cursor, read screen size
// ---------------------------------------------------------------------------
static void hideCursorInWindow(Display* dpy, Window win)
{
    Pixmap pm = XCreatePixmap(dpy, win, 1, 1, 1);
    XColor black{}; black.flags = DoRed | DoGreen | DoBlue;
    Cursor inv = XCreatePixmapCursor(dpy, pm, pm, &black, &black, 0, 0);
    XDefineCursor(dpy, win, inv);
    XFlush(dpy);
    XFreePixmap(dpy, pm);
}

static void initWindow(const std::string& name)
{
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::cerr << "Cannot open X display\n"; return; }

    Screen* screen = DefaultScreenOfDisplay(dpy);
    gScreenWidth  = screen->width;
    gScreenHeight = screen->height;

    // Hide cursor globally using XFixes — persists at the X server level,
    // cannot be overridden by the window manager or any other client.
    XFixesHideCursor(dpy, DefaultRootWindow(dpy));
    XFlush(dpy);
    // Also set an invisible pixmap cursor as belt-and-suspenders.
    hideCursorInWindow(dpy, DefaultRootWindow(dpy));

    // Also hide on the OpenCV window itself once it appears.
    // Retry a few times in case the window hasn't been mapped yet.
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Window root = DefaultRootWindow(dpy), parent;
        Window* children = nullptr; unsigned int n = 0;
        if (XQueryTree(dpy, root, &root, &parent, &children, &n)) {
            for (unsigned int i = 0; i < n; ++i) {
                char* xname = nullptr;
                if (XFetchName(dpy, children[i], &xname) && xname) {
                    if (name == xname) {
                        hideCursorInWindow(dpy, children[i]);
                        XFree(xname);
                        if (children) XFree(children);
                        goto done;
                    }
                    XFree(xname);
                }
            }
            if (children) XFree(children);
        }
    }
done:
    XCloseDisplay(dpy);
}

// ---------------------------------------------------------------------------
// Config hot-reload — inotify watcher
// ---------------------------------------------------------------------------
static std::atomic<bool> gConfigChanged{false};

static void watchConfig(const std::string& configPath, std::atomic<bool>& stop)
{
    // Watch the directory so we catch writes, saves-as-rename, and editor tricks
    char* pathCopy = strdup(configPath.c_str());
    std::string dir  = dirname(pathCopy);  // e.g. "/home/pi/git/immich-photoframe"
    free(pathCopy);
    std::string file = configPath.substr(configPath.rfind('/') + 1);  // "config.json"

    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) { std::cerr << "[Config] inotify_init failed — hot-reload disabled.\n"; return; }

    int wd = inotify_add_watch(fd, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) { std::cerr << "[Config] inotify_add_watch failed — hot-reload disabled.\n"; close(fd); return; }

    std::cout << "[Config] Watching " << configPath << " for changes.\n";

    char buf[sizeof(inotify_event) + NAME_MAX + 1];
    while (!stop) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }

        for (char* p = buf; p < buf + n; ) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            if (ev->len > 0 && file == ev->name)
                gConfigChanged = true;
            p += sizeof(inotify_event) + ev->len;
        }
    }

    inotify_rm_watch(fd, wd);
    close(fd);
}

// ---------------------------------------------------------------------------
// Status screen
// ---------------------------------------------------------------------------
static cv::Mat makeStatusScreen(int w, int h, const std::string& msg)
{
    cv::Mat img = cv::Mat::zeros(h, w, CV_8UC3);
    int bl = 0;
    cv::Size ts = cv::getTextSize(msg, cv::FONT_HERSHEY_SIMPLEX, 1.0, 2, &bl);
    cv::putText(img, msg, cv::Point((w - ts.width)/2, (h + ts.height)/2),
                cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(255,255,255), 2, cv::LINE_AA);
    return img;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct AppConfig {
    int         timer             = 30;
    bool        enableTouch       = true;
    bool        showDate          = true;
    bool        showImgCount      = true;
    bool        showFolderName    = true;
    bool        showQrCode        = true;
    int         refreshInterval   = 300;   // seconds
    std::string sourceMode            = "tag";
    std::vector<std::string> tagNames  = { "Fotorahmen" };
    std::vector<std::string> albumNames = { "Fotorahmen" };
    std::string immichServer           = "http://192.168.16.7:2283";
    std::vector<std::string> immichApiKeys;
};

static AppConfig loadConfig(const std::string& path)
{
    AppConfig cfg;
    std::ifstream f(path);
    if (!f.is_open()) { std::cerr << "[Config] Cannot open " << path << "\n"; return cfg; }
    try {
        json j; f >> j;
        if (j.contains("timer"))            cfg.timer           = j["timer"];
        if (j.contains("enableTouch"))      cfg.enableTouch     = j["enableTouch"];
        if (j.contains("showDate"))         cfg.showDate        = j["showDate"];
        if (j.contains("showImgCount"))     cfg.showImgCount    = j["showImgCount"];
        if (j.contains("showFolderName"))   cfg.showFolderName  = j["showFolderName"];
        if (j.contains("showQrCode"))       cfg.showQrCode      = j["showQrCode"];
        if (j.contains("refreshInterval"))  cfg.refreshInterval = j["refreshInterval"];
        if (j.contains("sourceMode"))       cfg.sourceMode       = j["sourceMode"];
        // Support both single string and array for tagNames / albumNames
        auto readStringList = [&](const char* key, std::vector<std::string>& out) {
            if (!j.contains(key)) return;
            if (j[key].is_array()) {
                for (const auto& v : j[key])
                    if (v.is_string()) out.push_back(v.get<std::string>());
            } else if (j[key].is_string()) {
                out = { j[key].get<std::string>() };
            }
        };
        readStringList("tagNames",   cfg.tagNames);
        readStringList("albumNames", cfg.albumNames);
        // legacy single-value keys
        if (cfg.tagNames.empty())   readStringList("tagName",   cfg.tagNames);
        if (cfg.albumNames.empty()) readStringList("albumName", cfg.albumNames);
        if (j.contains("immich") && j["immich"].is_object()) {
            auto& im = j["immich"];
            if (im.contains("serverUrl"))   cfg.immichServer = im["serverUrl"];
            // Support both "apiKeys" (array) and legacy "apiKey" (single string)
            if (im.contains("apiKeys") && im["apiKeys"].is_array()) {
                for (const auto& k : im["apiKeys"])
                    if (k.is_string()) cfg.immichApiKeys.push_back(k.get<std::string>());
            } else if (im.contains("apiKey")) {
                cfg.immichApiKeys.push_back(im["apiKey"].get<std::string>());
            }
        }
        auto joinNames = [](const std::vector<std::string>& v) {
            std::string s;
            for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += v[i]; }
            return s;
        };
        std::string src = (cfg.sourceMode == "album")
            ? ("albums=" + joinNames(cfg.albumNames))
            : ("tags=" + joinNames(cfg.tagNames));
        std::cout << "[Config] server=" << cfg.immichServer
                  << "  " << src
                  << "  keys=" << cfg.immichApiKeys.size()
                  << "  timer=" << cfg.timer << "s\n";
    } catch (const std::exception& e) {
        std::cerr << "[Config] Error: " << e.what() << "\n";
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    // Unbuffer stdout/stderr so logs appear immediately when piped to a file
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    const std::string WIN = "PiPhotoFrame";

    AppConfig cfg = loadConfig("config.json");

    // Create window
    cv::namedWindow(WIN, cv::WINDOW_NORMAL);
    cv::setWindowProperty(WIN, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
    if (cfg.enableTouch)
        cv::setMouseCallback(WIN, onMouse, nullptr);

    cv::imshow(WIN, makeStatusScreen(1920, 1200, "Verbinde mit Immich..."));
    cv::waitKey(2);

    // Init X11 (cursor hide + screen size)
    initWindow(WIN);

    cv::imshow(WIN, makeStatusScreen(gScreenWidth, gScreenHeight, "Verbinde mit Immich..."));
    cv::waitKey(2);

    // Create Immich client and display engine
    ImmichClient client(cfg.immichServer, cfg.immichApiKeys);
    DisplayImage display(client);

    display.setSourceMode(cfg.sourceMode);
    display.setTagNames(cfg.tagNames);
    display.setAlbumNames(cfg.albumNames);
    display.setScreenSize(gScreenWidth, gScreenHeight);
    display.setShowDate(cfg.showDate);
    display.setShowImgCount(cfg.showImgCount);
    display.setShowFolderName(cfg.showFolderName);
    display.setShowQrCode(cfg.showQrCode);
    display.setRefreshIntervalSeconds(cfg.refreshInterval);

    // Start: fetch assets + launch preload/refresh threads
    int count = display.start();

    if (count == 0) {
        std::string src = (cfg.sourceMode == "album")
            ? "album(s): " + cfg.albumNames[0]
            : "tag(s): "   + cfg.tagNames[0];
        cv::imshow(WIN, makeStatusScreen(gScreenWidth, gScreenHeight,
                                         "Keine Bilder gefunden (" + src + ")"));
        // Keep trying via the refresh thread — wait until queue has something
        std::cout << "[Main] Waiting for assets...\n";
    } else {
        cv::imshow(WIN, makeStatusScreen(gScreenWidth, gScreenHeight, "Lade Bild..."));
        cv::waitKey(2);
    }

    // Get first image — poll so cv::waitKey keeps running and window stays responsive
    cv::Mat currentImg;
    while (currentImg.empty()) {
        int key = cv::waitKey(50);
        if (key == 27) { cv::destroyAllWindows(); return 0; }
        currentImg = display.tryGetNextImage(50);
    }
    cv::imshow(WIN, currentImg);

    // Main loop
    auto lastSwitch = std::chrono::steady_clock::now();
    std::chrono::seconds interval(cfg.timer);  // mutable — updated on config reload
    bool pendingForward = false;
    bool pendingAdvance = false;

    // Config watcher thread
    std::atomic<bool> watcherStop{false};
    std::thread watcherThread(watchConfig, std::string("config.json"), std::ref(watcherStop));

    while (true) {
        int key = cv::waitKey(10);
        if (key == 27) break;  // ESC

        // ---- Config hot-reload -------------------------------------------
        if (gConfigChanged.exchange(false)) {
            std::cout << "[Config] config.json changed — reloading...\n";
            AppConfig newCfg = loadConfig("config.json");

            // Overlay toggles — take effect on next rendered frame
            display.setShowDate(newCfg.showDate);
            display.setShowImgCount(newCfg.showImgCount);
            display.setShowFolderName(newCfg.showFolderName);
            display.setShowQrCode(newCfg.showQrCode);

            // Timer
            interval = std::chrono::seconds(newCfg.timer);

            // Touch
            if (newCfg.enableTouch != cfg.enableTouch) {
                if (newCfg.enableTouch)
                    cv::setMouseCallback(WIN, onMouse, nullptr);
                else
                    cv::setMouseCallback(WIN, nullptr, nullptr);
            }

            // API keys
            if (newCfg.immichApiKeys != cfg.immichApiKeys)
                client.setApiKeys(newCfg.immichApiKeys);

            // Source config — trigger immediate asset-list refresh if changed
            bool sourceChanged =
                newCfg.sourceMode  != cfg.sourceMode  ||
                newCfg.tagNames    != cfg.tagNames     ||
                newCfg.albumNames  != cfg.albumNames   ||
                newCfg.refreshInterval != cfg.refreshInterval;

            if (sourceChanged) {
                display.setSourceMode(newCfg.sourceMode);
                display.setTagNames(newCfg.tagNames);
                display.setAlbumNames(newCfg.albumNames);
                display.setRefreshIntervalSeconds(newCfg.refreshInterval);
                display.triggerRefresh();
            }

            // serverUrl change requires restart (client is already bound to it)
            if (newCfg.immichServer != cfg.immichServer)
                std::cerr << "[Config] serverUrl changed — restart required for this to take effect.\n";

            cfg = newCfg;
        }
        // ------------------------------------------------------------------

        auto now     = std::chrono::steady_clock::now();
        bool elapsed = (now - lastSwitch) >= interval;
        bool newRequest = gPendingClick || (elapsed && !gIsPressed);

        if (newRequest && !pendingAdvance) {
            pendingForward  = true;
            pendingAdvance  = true;

            if (gPendingClick) {
                pendingForward = (gClickX >= gScreenWidth / 2);
                gPendingClick  = false;

                // Visual tap ring
                cv::Mat fb = currentImg.clone();
                cv::Scalar ring = pendingForward ? cv::Scalar(255,255,255) : cv::Scalar(100,100,255);
                cv::circle(fb, cv::Point(gClickX, gClickY), 14, ring, 2, cv::LINE_AA);
                cv::imshow(WIN, fb);
                cv::waitKey(80);
            }

            lastSwitch = std::chrono::steady_clock::now();
        }

        if (pendingAdvance) {
            cv::Mat next = pendingForward ? display.tryGetNextImage(0)
                                          : display.getPrevImage();
            if (!next.empty()) {
                currentImg     = next;
                pendingAdvance = false;
                cv::imshow(WIN, currentImg);
            }
            // else: queue not ready yet — retry next iteration
        }
    }

    cv::destroyAllWindows();
    watcherStop = true;
    watcherThread.join();
    return 0;
}

