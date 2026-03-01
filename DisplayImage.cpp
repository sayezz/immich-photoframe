#include "DisplayImage.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <qrencode.h>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

DisplayImage::DisplayImage(ImmichClient& client)
    : m_client(client)
{
    std::cout << "[DisplayImage] Created.\n";
}

DisplayImage::~DisplayImage()
{
    m_stop = true;
    m_queueCv.notify_all();
    if (m_preloadThread.joinable()) m_preloadThread.join();
    if (m_refreshThread.joinable()) m_refreshThread.join();
    std::cout << "[DisplayImage] Destroyed.\n";
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void DisplayImage::setTagNames(const std::vector<std::string>& tags)    { m_tagNames   = tags; }
void DisplayImage::setAlbumNames(const std::vector<std::string>& albums) { m_albumNames = albums; }
void DisplayImage::setSourceMode(const std::string& mode)         { m_sourceMode = mode; }
void DisplayImage::setScreenSize(int w, int h)                    { m_screenWidth = w; m_screenHeight = h; }
void DisplayImage::setShowDate(bool v)                            { m_showDate = v; }
void DisplayImage::setShowImgCount(bool v)                        { m_showImgCount = v; }
void DisplayImage::setShowFolderName(bool v)                      { m_showFolderName = v; }
void DisplayImage::setShowQrCode(bool v)                          { m_showQrCode = v; }
void DisplayImage::setRefreshIntervalSeconds(int s)               { m_refreshIntervalSec = s; }
void DisplayImage::setDbPath(const std::string& p)                { m_dbPath = p; }
int  DisplayImage::totalAssets()   const                          { std::lock_guard<std::mutex> l(m_assetsMutex); return static_cast<int>(m_assets.size()); }
int  DisplayImage::visitedAssets() const                          { std::lock_guard<std::mutex> l(m_assetsMutex); return static_cast<int>(m_visitedIds.size()); }

// ---------------------------------------------------------------------------
// start() — load DB, fetch assets, launch threads
// ---------------------------------------------------------------------------

std::vector<ImageInfo> DisplayImage::fetchAssets()
{
    // Collect from all configured sources, deduplicate by immichId
    std::vector<ImageInfo> result;
    std::unordered_set<std::string> seen;

    auto merge = [&](std::vector<ImageInfo> batch) {
        for (auto& a : batch)
            if (seen.insert(a.immichId).second)
                result.push_back(std::move(a));
    };

    if (m_sourceMode == "album") {
        for (const auto& name : m_albumNames)
            merge(m_client.getAlbumAssets(name));
    } else {
        for (const auto& name : m_tagNames)
            merge(m_client.getTaggedAssets(name));
    }
    return result;
}

int DisplayImage::start()
{
    loadDb();

    // Initial asset fetch
    auto assets = fetchAssets();
    applyAssetList(std::move(assets));

    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        if (m_assets.empty()) {
            std::cerr << "[DisplayImage] No assets found (source: " << m_sourceMode << ").\n";
        } else {
            std::cout << "[DisplayImage] Starting with " << m_assets.size() << " assets."
                      << " (source: " << m_sourceMode << ")\n";
        }
    }

    m_preloadThread = std::thread(&DisplayImage::preloadLoop, this);
    m_refreshThread = std::thread(&DisplayImage::refreshLoop, this);

    return totalAssets();
}

// ---------------------------------------------------------------------------
// applyAssetList — diff new list vs current, add/remove accordingly
// ---------------------------------------------------------------------------

void DisplayImage::applyAssetList(std::vector<ImageInfo> fresh)
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);

    // Build set of incoming IDs
    std::unordered_set<std::string> freshIds;
    for (const auto& a : fresh) freshIds.insert(a.immichId);

    // Detect removed: remove from m_assets and from visitedIds
    std::vector<ImageInfo> kept;
    for (auto& a : m_assets) {
        if (freshIds.count(a.immichId))
            kept.push_back(std::move(a));
        else {
            m_visitedIds.erase(a.immichId);
            std::cout << "[Refresh] Removed asset: " << a.immichId << "\n";
        }
    }
    m_assets = std::move(kept);

    // Detect added
    std::unordered_set<std::string> currentIds;
    for (const auto& a : m_assets) currentIds.insert(a.immichId);

    for (auto& a : fresh) {
        if (!currentIds.count(a.immichId)) {
            std::cout << "[Refresh] New asset: " << a.immichId << "\n";
            m_assets.push_back(std::move(a));
        }
    }

    // Reset visited if everything has been seen — mutex already held by caller
    if (!m_assets.empty() && m_visitedIds.size() >= m_assets.size()) {
        std::cout << "[DisplayImage] All assets visited — resetting.\n";
        m_visitedIds.clear();
    }
}

// ---------------------------------------------------------------------------
// Refresh thread — periodically sync tagged asset list
// ---------------------------------------------------------------------------

void DisplayImage::triggerRefresh()
{
    m_forceRefresh = true;
    std::cout << "[Refresh] Immediate refresh triggered by config change.\n";
}

void DisplayImage::refreshLoop()
{
    auto nextRun = std::chrono::steady_clock::now()
                 + std::chrono::seconds(m_refreshIntervalSec);

    while (!m_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (m_stop) break;
        if (!m_forceRefresh && std::chrono::steady_clock::now() < nextRun) continue;
        m_forceRefresh = false;

        std::cout << "[Refresh] Checking for updates (" << m_sourceMode << ")...\n";
        auto fresh = fetchAssets();
        applyAssetList(std::move(fresh));
        saveShareCache();  // persist any new share keys
        nextRun = std::chrono::steady_clock::now()
                + std::chrono::seconds(m_refreshIntervalSec);
    }
}

// ---------------------------------------------------------------------------
// Preload thread — downloads images into the queue
// ---------------------------------------------------------------------------

void DisplayImage::preloadLoop()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    while (!m_stop) {
        resetVisitedIfNeeded();

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if (static_cast<int>(m_queue.size()) >= QUEUE_BUFFER) {
                m_queueCv.wait_for(lock, std::chrono::milliseconds(200));
                continue;
            }
        }

        // Pick a random unvisited asset
        ImageInfo chosen;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_assetsMutex);
            if (m_assets.empty()) {
                // no assets yet — wait
            } else {
                std::vector<const ImageInfo*> available;
                for (const auto& a : m_assets)
                    if (!m_visitedIds.count(a.immichId))
                        available.push_back(&a);

                if (!available.empty()) {
                    std::uniform_int_distribution<> dist(0, static_cast<int>(available.size()) - 1);
                    chosen = *available[dist(rng)];
                    found  = true;
                }
            }
        }

        if (!found) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Download image
        cv::Mat img = m_client.downloadImage(chosen.immichId);
        if (img.empty()) {
            std::cerr << "[Preload] Download failed for: " << chosen.fileName << " (" << chosen.immichId << ")\n";
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        std::cout << "[Preload] Downloaded: " << chosen.fileName << "\n";

        // Fetch (or create) share URL
        if (m_showQrCode && chosen.shareUrl.empty()) {
            chosen.shareUrl = m_client.getShareUrl(chosen.immichId);
            saveShareCache();
        }

        // Mark visited and enqueue
        int displayIndex, totalCount;
        {
            std::lock_guard<std::mutex> al(m_assetsMutex);
            m_visitedIds.insert(chosen.immichId);
            // update shareUrl in main asset list too
            for (auto& a : m_assets)
                if (a.immichId == chosen.immichId)
                    a.shareUrl = chosen.shareUrl;
            // capture position now, before any reset can fire
            displayIndex = static_cast<int>(m_visitedIds.size());
            totalCount   = static_cast<int>(m_assets.size());
        }
        saveVisited(chosen.immichId);

        {
            std::lock_guard<std::mutex> ql(m_queueMutex);
            m_queue.push({ chosen, img, displayIndex, totalCount });
        }
        m_queueCv.notify_one();
    }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

cv::Mat DisplayImage::getNextImage()
{
    // Forward in history
    if (m_historyIdx < static_cast<int>(m_history.size()) - 1) {
        ++m_historyIdx;
        return renderImage(m_history[m_historyIdx]);
    }

    // Draw from queue
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_queueCv.wait(lock, [this]{ return !m_queue.empty() || m_stop.load(); });
    if (m_queue.empty()) return cv::Mat();

    QueueEntry entry = m_queue.front();
    m_queue.pop();
    lock.unlock();
    m_queueCv.notify_one();

    if (static_cast<int>(m_history.size()) >= HISTORY_SIZE)
        m_history.pop_front();

    m_history.push_back(entry);
    m_historyIdx = static_cast<int>(m_history.size()) - 1;
    return renderImage(entry);
}

cv::Mat DisplayImage::getPrevImage()
{
    if (m_history.empty() || m_historyIdx <= 0) return cv::Mat();
    --m_historyIdx;
    return renderImage(m_history[m_historyIdx]);
}

cv::Mat DisplayImage::tryGetNextImage(int timeout_ms)
{
    // Serve from history first (no blocking needed)
    if (m_historyIdx < static_cast<int>(m_history.size()) - 1) {
        ++m_historyIdx;
        return renderImage(m_history[m_historyIdx]);
    }

    std::unique_lock<std::mutex> lock(m_queueMutex);
    bool ready = m_queueCv.wait_for(lock,
                                    std::chrono::milliseconds(timeout_ms),
                                    [this]{ return !m_queue.empty() || m_stop.load(); });
    if (!ready || m_queue.empty()) return cv::Mat();

    QueueEntry entry = m_queue.front();
    m_queue.pop();
    lock.unlock();
    m_queueCv.notify_one();

    if (static_cast<int>(m_history.size()) >= HISTORY_SIZE)
        m_history.pop_front();

    m_history.push_back(entry);
    m_historyIdx = static_cast<int>(m_history.size()) - 1;
    return renderImage(entry);
}


// ---------------------------------------------------------------------------

cv::Mat DisplayImage::renderImage(const QueueEntry& entry)
{
    const cv::Mat&    src  = entry.img;
    const ImageInfo&  info = entry.info;
    if (src.empty()) return cv::Mat();

    // Letterbox scale
    double imgAspect    = static_cast<double>(src.cols) / src.rows;
    double scrAspect    = static_cast<double>(m_screenWidth) / m_screenHeight;
    int nw, nh;
    if (imgAspect > scrAspect) { nw = m_screenWidth;  nh = static_cast<int>(m_screenWidth  / imgAspect); }
    else                       { nh = m_screenHeight; nw = static_cast<int>(m_screenHeight * imgAspect); }

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_AREA);

    cv::Mat canvas = cv::Mat::zeros(m_screenHeight, m_screenWidth, CV_8UC3);
    int ox = (m_screenWidth  - nw) / 2;
    int oy = (m_screenHeight - nh) / 2;
    resized.copyTo(canvas(cv::Rect(ox, oy, nw, nh)));

    if (m_showDate || m_showFolderName) drawDateAndFolder(canvas, info);
    if (m_showImgCount)                 drawImageCount(canvas, entry.displayIndex, entry.totalCount);
    if (m_showQrCode && !info.shareUrl.empty()) drawQrCode(canvas, info.shareUrl);

    return canvas;
}

// --- Top-left: date + folder ---

void DisplayImage::drawDateAndFolder(cv::Mat& out, const ImageInfo& info)
{
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> jitter(-12, 12);

    const int    fontFace = cv::FONT_HERSHEY_SIMPLEX;
    const double scale    = 0.8;
    const int    thick    = 1;

    std::string dateStr   = m_showDate       ? info.date                        : "";
    std::string folderStr = m_showFolderName ? replaceUmlauts(info.albumName)   : "";
    if (dateStr.empty() && folderStr.empty()) return;

    int bl = 0;
    cv::Size dateSz   = dateStr.empty()   ? cv::Size(0,0) : cv::getTextSize(dateStr,   fontFace, scale, thick, &bl);
    cv::Size folderSz = folderStr.empty() ? cv::Size(0,0) : cv::getTextSize(folderStr, fontFace, scale, thick, &bl);

    int lineH = std::max(dateSz.height, folderSz.height);
    bool both = !dateStr.empty() && !folderStr.empty();
    int bgW   = std::max(dateSz.width, folderSz.width) + 20;
    int bgH   = (both ? 2 * lineH + 8 : lineH) + 16;

    int bx = 20 + jitter(rng); bx = std::max(0, std::min(bx, out.cols - bgW - 5));
    int by = 20 + jitter(rng); by = std::max(0, by);

    drawRoundedRect(out, cv::Rect(bx, by, bgW, bgH), cv::Scalar(0,0,0), 8, 0.55);

    cv::Scalar white(255,255,255);
    int tx = bx + 8;
    if (!dateStr.empty()) {
        cv::putText(out, dateStr, cv::Point(tx, by + 8 + dateSz.height),
                    fontFace, scale, white, thick, cv::LINE_AA);
    }
    if (!folderStr.empty()) {
        int ty = dateStr.empty() ? by + 8 + folderSz.height
                                 : by + 8 + dateSz.height + 6 + folderSz.height;
        cv::putText(out, folderStr, cv::Point(tx, ty),
                    fontFace, scale, white, thick, cv::LINE_AA);
    }
}

// --- Bottom-right: count ---

void DisplayImage::drawImageCount(cv::Mat& out, int displayIndex, int totalCount)
{
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> jitter(-12, 12);

    std::string text = std::to_string(displayIndex) + "/" + std::to_string(totalCount);

    const int fontFace = cv::FONT_HERSHEY_SIMPLEX;
    const double scale = 0.7;
    const int thick    = 1;
    int bl = 0;
    cv::Size sz = cv::getTextSize(text, fontFace, scale, thick, &bl);

    const int pad = 10;
    int bgW = sz.width + 2*pad, bgH = sz.height + 2*pad;
    int bx = out.cols - bgW - 15 + jitter(rng);
    int by = out.rows - bgH - 15 + jitter(rng);
    bx = std::max(0, std::min(bx, out.cols - bgW - 5));
    by = std::max(0, std::min(by, out.rows - bgH - 5));

    drawRoundedRect(out, cv::Rect(bx, by, bgW, bgH), cv::Scalar(0,0,0), 6, 0.55);
    cv::putText(out, text, cv::Point(bx + pad, by + pad + sz.height),
                fontFace, scale, cv::Scalar(255,255,255), thick, cv::LINE_AA);
}

// --- Bottom-left: QR code ---

void DisplayImage::drawQrCode(cv::Mat& out, const std::string& url)
{
    if (url.empty()) return;

    QRcode* qr = QRcode_encodeString(url.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!qr) return;

    const int mod    = 4;    // pixels per QR module
    const int margin = 8;
    int qrPx = qr->width * mod;
    int sz   = qrPx + 2 * margin;

    cv::Mat qrImg(sz, sz, CV_8UC3, cv::Scalar(255,255,255));
    for (int y = 0; y < qr->width; ++y)
        for (int x = 0; x < qr->width; ++x)
            if (qr->data[y * qr->width + x] & 1)
                cv::rectangle(qrImg,
                              cv::Point(margin + x*mod, margin + y*mod),
                              cv::Point(margin + (x+1)*mod - 1, margin + (y+1)*mod - 1),
                              cv::Scalar(0,0,0), cv::FILLED);
    QRcode_free(qr);

    const int pad = 12;
    int ox = pad, oy = out.rows - sz - pad;
    if (ox + sz > out.cols || oy < 0) return;
    qrImg.copyTo(out(cv::Rect(ox, oy, sz, sz)));
}

// --- Rounded rect background ---

void DisplayImage::drawRoundedRect(cv::Mat& out, cv::Rect rect,
                                    cv::Scalar bgColor, int r, double alpha)
{
    if (rect.x < 0) rect.x = 0;
    if (rect.y < 0) rect.y = 0;
    if (rect.x + rect.width  > out.cols) rect.width  = out.cols - rect.x;
    if (rect.y + rect.height > out.rows) rect.height = out.rows - rect.y;
    if (rect.width <= 0 || rect.height <= 0) return;

    cv::Mat roi  = out(rect);
    cv::Mat over = roi.clone();
    over.setTo(bgColor);

    cv::Mat mask = cv::Mat::zeros(rect.height, rect.width, CV_8UC1);
    cv::rectangle(mask, cv::Rect(r, 0, rect.width - 2*r, rect.height), 255, cv::FILLED);
    cv::rectangle(mask, cv::Rect(0, r, rect.width, rect.height - 2*r), 255, cv::FILLED);
    cv::circle(mask, cv::Point(r,                r),                r, 255, cv::FILLED);
    cv::circle(mask, cv::Point(rect.width - r,   r),                r, 255, cv::FILLED);
    cv::circle(mask, cv::Point(r,                rect.height - r),  r, 255, cv::FILLED);
    cv::circle(mask, cv::Point(rect.width - r,   rect.height - r),  r, 255, cv::FILLED);

    cv::Mat blended;
    cv::addWeighted(over, alpha, roi, 1.0 - alpha, 0, blended);
    blended.copyTo(roi, mask);
}

// ---------------------------------------------------------------------------
// Umlaut helper
// ---------------------------------------------------------------------------

std::string DisplayImage::replaceUmlauts(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0xC3 && i + 1 < in.size()) {
            switch (static_cast<unsigned char>(in[i+1])) {
                case 0xA4: out += "ae"; ++i; continue;
                case 0xB6: out += "oe"; ++i; continue;
                case 0xBC: out += "ue"; ++i; continue;
                case 0x84: out += "Ae"; ++i; continue;
                case 0x96: out += "Oe"; ++i; continue;
                case 0x9C: out += "Ue"; ++i; continue;
                case 0x9F: out += "ss"; ++i; continue;
            }
        }
        out += static_cast<char>(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// DB persistence
// ---------------------------------------------------------------------------

void DisplayImage::loadDb()
{
    std::ifstream f(m_dbPath);
    if (!f.is_open()) {
        std::cerr << "[DB] db.json not found — starting fresh.\n";
        return;
    }
    try {
        json j; f >> j;

        // Visited IDs
        auto ids = j.value("visitedAssetIds", std::vector<std::string>{});
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_visitedIds.insert(ids.begin(), ids.end());
        std::cout << "[DB] Loaded " << m_visitedIds.size() << " visited IDs.\n";

        // Share link cache
        if (j.contains("shareLinks") && j["shareLinks"].is_object()) {
            std::map<std::string, std::string> cache;
            for (auto& [k, v] : j["shareLinks"].items())
                cache[k] = v.get<std::string>();
            m_client.loadShareCache(cache);
            std::cout << "[DB] Loaded " << cache.size() << " cached share links.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[DB] Parse error: " << e.what() << "\n";
    }
}

void DisplayImage::saveVisited(const std::string& assetId)
{
    json j;
    {
        std::ifstream f(m_dbPath);
        if (f.is_open()) { try { f >> j; } catch (...) {} }
    }
    if (!j.contains("visitedAssetIds")) j["visitedAssetIds"] = json::array();

    auto& arr = j["visitedAssetIds"];
    if (std::find(arr.begin(), arr.end(), assetId) == arr.end())
        arr.push_back(assetId);

    // Always refresh shareLinks from client cache while we're saving
    auto cache = m_client.getShareCache();
    j["shareLinks"] = cache;

    std::ofstream out(m_dbPath);
    if (out.is_open()) out << j.dump(2);
}

void DisplayImage::saveShareCache()
{
    json j;
    {
        std::ifstream f(m_dbPath);
        if (f.is_open()) { try { f >> j; } catch (...) {} }
    }
    j["shareLinks"] = m_client.getShareCache();
    std::ofstream out(m_dbPath);
    if (out.is_open()) out << j.dump(2);
}

void DisplayImage::resetVisitedIfNeeded()
{
    // Called while m_assetsMutex may NOT be held (called from preloadLoop without lock)
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    if (m_assets.empty()) return;
    if (m_visitedIds.size() < m_assets.size()) return;

    std::cout << "[DisplayImage] All assets visited — resetting.\n";
    m_visitedIds.clear();

    json j;
    {
        std::ifstream f(m_dbPath);
        if (f.is_open()) { try { f >> j; } catch (...) {} }
    }
    j["visitedAssetIds"] = json::array();
    j["shareLinks"]      = m_client.getShareCache();
    std::ofstream out(m_dbPath);
    if (out.is_open()) out << j.dump(2);
}
