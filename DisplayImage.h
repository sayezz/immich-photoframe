#pragma once

#include <string>
#include <vector>
#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_set>
#include <random>
#include <chrono>
#include <atomic>

#include <opencv2/opencv.hpp>

#include "ImmichClient.h"
#include "json.hpp"

class DisplayImage {
public:
    // ImmichClient must outlive DisplayImage.
    explicit DisplayImage(ImmichClient& client);
    ~DisplayImage();

    // --- Configuration setters (call before startPreloading) ---
    void setTagNames(const std::vector<std::string>& tags);
    void setAlbumNames(const std::vector<std::string>& albums);
    // "tag" or "album" — determines which Immich source to use
    void setSourceMode(const std::string& mode);
    void setScreenSize(int width, int height);
    void setShowDate(bool value);
    void setShowImgCount(bool value);
    void setShowFolderName(bool value);
    void setShowQrCode(bool value);
    void setRefreshIntervalSeconds(int seconds);
    void setDbPath(const std::string& path);

    // --- Initial asset load + preload start ---
    // Returns number of assets found.  Will block until at least one asset is available.
    int start();

    // --- Navigation ---
    cv::Mat getNextImage();
    cv::Mat getPrevImage();
    // Non-blocking variant: returns empty Mat if nothing ready within timeout_ms.
    // Use this in the main loop so cv::waitKey stays reachable.
    cv::Mat tryGetNextImage(int timeout_ms = 100);

    // Force an immediate asset-list refresh (e.g. after config hot-reload).
    void triggerRefresh();

    // --- Info ---
    int totalAssets()   const;
    int visitedAssets() const;

private:
    // ---- Preload thread: downloads images from Immich queue ----
    void preloadLoop();

    // ---- Refresh thread: re-queries source assets every N seconds ----
    void refreshLoop();

    // ---- Fetch assets from configured source (tag or album) ----
    std::vector<ImageInfo> fetchAssets();

    // ---- Asset list management (called from refreshLoop) ----
    void applyAssetList(std::vector<ImageInfo> fresh);

    // ---- Rendering ----
    struct QueueEntry {
        ImageInfo  info;
        cv::Mat    img;
        int        displayIndex = 0;  // captured at enqueue time (1-based)
        int        totalCount   = 0;  // captured at enqueue time
    };
    cv::Mat renderImage(const QueueEntry& entry);
    void drawDateAndFolder(cv::Mat& out, const ImageInfo& info);
    void drawImageCount(cv::Mat& out, int displayIndex, int totalCount);
    void drawQrCode(cv::Mat& out, const std::string& url);
    void drawRoundedRect(cv::Mat& out, cv::Rect rect,
                         cv::Scalar bgColor, int radius, double alpha);

    // ---- DB persistence ----
    void loadDb();
    void saveVisited(const std::string& assetId);
    void saveShareCache();
    void resetVisitedIfNeeded();

    // ---- Umlaut helper ----
    std::string replaceUmlauts(const std::string& input);

    // ---- Dependencies ----
    ImmichClient& m_client;

    // ---- Config ----
    std::vector<std::string> m_tagNames             = { "Fotorahmen" };
    std::vector<std::string> m_albumNames            = { "Fotorahmen" };
    std::string m_sourceMode          = "tag";   // "tag" | "album"
    std::string m_dbPath              = "db.json";
    int         m_screenWidth         = 1920;
    int         m_screenHeight        = 1200;
    int         m_refreshIntervalSec  = 300;
    bool        m_showDate            = true;
    bool        m_showImgCount        = true;
    bool        m_showFolderName      = true;
    bool        m_showQrCode          = true;

    // ---- Asset list (refreshed periodically) ----
    std::vector<ImageInfo>    m_assets;       // current known assets
    std::unordered_set<std::string> m_visitedIds; // immichIds already shown
    mutable std::mutex        m_assetsMutex;

    // ---- Download queue (preload thread → main thread) ----
    std::queue<QueueEntry>  m_queue;
    std::mutex              m_queueMutex;
    std::condition_variable m_queueCv;

    // ---- History for back-navigation ----
    std::deque<QueueEntry> m_history;
    int                    m_historyIdx = -1;
    static constexpr int HISTORY_SIZE = 15;
    static constexpr int QUEUE_BUFFER = 5;

    // ---- Thread management ----
    std::thread       m_preloadThread;
    std::thread       m_refreshThread;
    std::atomic<bool> m_stop { false };
    std::atomic<bool> m_forceRefresh { false };  // set by triggerRefresh()
};

