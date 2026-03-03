#include "ImmichClient.h"
#include "json.hpp"

#include <curl/curl.h>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// libcurl write callbacks
// ---------------------------------------------------------------------------

static size_t writeString(void* buf, size_t size, size_t nmemb, std::string* out)
{
    out->append(static_cast<char*>(buf), size * nmemb);
    return size * nmemb;
}

static size_t writeBinary(void* buf, size_t size, size_t nmemb, std::vector<uint8_t>* out)
{
    const size_t total = size * nmemb;
    auto* p = static_cast<uint8_t*>(buf);
    out->insert(out->end(), p, p + total);
    return total;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ImmichClient::ImmichClient(const std::string& serverUrl, const std::string& apiKey)
    : m_serverUrl(serverUrl), m_apiKeys({ apiKey })
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ImmichClient::ImmichClient(const std::string& serverUrl, const std::vector<std::string>& apiKeys)
    : m_serverUrl(serverUrl), m_apiKeys(apiKeys)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

    // httpGet and httpPost use m_apiKeys[0] — protect the read with the cache mutex
    // (same mutex guards both cache and keys, keeping it simple)
std::string ImmichClient::httpGet(const std::string& path)
{
    std::string primaryKey;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        primaryKey = m_apiKeys.empty() ? "" : m_apiKeys[0];
    }

    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string url     = m_serverUrl + path;
    std::string authHdr = "x-api-key: " + primaryKey;
    std::string body;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, authHdr.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        180L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        std::cerr << "[Immich] GET " << path << " failed: " << curl_easy_strerror(res) << "\n";

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return body;
}

std::string ImmichClient::httpPost(const std::string& path, const std::string& body)
{
    std::string primaryKey;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        primaryKey = m_apiKeys.empty() ? "" : m_apiKeys[0];
    }
    return httpPostWithKey(path, body, primaryKey);
}

std::string ImmichClient::httpPostWithKey(const std::string& path, const std::string& body, const std::string& key)
{
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string url     = m_serverUrl + path;
    std::string authHdr = "x-api-key: " + key;
    std::string resp;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, authHdr.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        std::cerr << "[Immich] POST " << path << " failed: " << curl_easy_strerror(res) << "\n";

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

std::vector<uint8_t> ImmichClient::httpGetBinary(const std::string& url)
{
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string authHdr;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        authHdr = "x-api-key: " + (m_apiKeys.empty() ? std::string{} : m_apiKeys[0]);
    }
    std::vector<uint8_t> data;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, authHdr.c_str());

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeBinary);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[Immich] Binary GET failed: " << curl_easy_strerror(res) << "\n";
        data.clear();
    } else {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        if (httpCode < 200 || httpCode >= 300) {
            std::cerr << "[Immich] Binary GET HTTP " << httpCode << " for " << url << "\n";
            data.clear();
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return data;
}

// ---------------------------------------------------------------------------
// Shared helper — parse a single Immich asset JSON object → ImageInfo
// ---------------------------------------------------------------------------

static ImageInfo parseAssetJson(const nlohmann::json& asset)
{
    ImageInfo info;
    info.immichId = asset.value("id", "");
    info.fileName = asset.value("originalFileName", "");

    // Folder name: second-to-last path component
    std::string path = asset.value("originalPath", "");
    if (!path.empty()) {
        auto pos1 = path.rfind('/');
        if (pos1 != std::string::npos && pos1 > 0) {
            auto pos2 = path.rfind('/', pos1 - 1);
            info.albumName = (pos2 == std::string::npos)
                           ? path.substr(0, pos1)
                           : path.substr(pos2 + 1, pos1 - pos2 - 1);
        }
    }

    std::string dt = asset.value("localDateTime",
                      asset.value("fileCreatedAt", ""));
    if (!dt.empty()) {
        std::istringstream iss(dt);
        std::tm tm{};
        iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        if (!iss.fail()) {
            std::ostringstream fmt;
            fmt << std::setw(2) << std::setfill('0') << tm.tm_mday    << "."
                << std::setw(2) << std::setfill('0') << (tm.tm_mon+1) << "."
                << (tm.tm_year + 1900);
            info.date = fmt.str();
        }
    }
    if (info.date.empty()) info.date = "Unknown date";

    return info;
}

// ---------------------------------------------------------------------------
// Tag resolution
// ---------------------------------------------------------------------------

std::string ImmichClient::findTagId(const std::string& tagName)
{
    std::string resp = httpGet("/api/tags");
    if (resp.empty()) return {};

    try {
        auto j = json::parse(resp);
        for (const auto& tag : j) {
            if (tag.value("name", "") == tagName)
                return tag.value("id", "");
        }
    } catch (const std::exception& e) {
        std::cerr << "[Immich] findTagId parse error: " << e.what() << "\n";
    }
    return {};
}

// ---------------------------------------------------------------------------
// Asset list by tag  (paginated)
// ---------------------------------------------------------------------------

std::vector<ImageInfo> ImmichClient::getTaggedAssets(const std::string& tagName)
{
    std::string tagId = findTagId(tagName);
    if (tagId.empty()) {
        std::cerr << "[Immich] Tag '" << tagName << "' not found.\n";
        return {};
    }

    std::vector<ImageInfo> result;
    int page = 1;
    const int pageSize = 100;

    while (true) {
        json req;
        req["tagIds"]  = json::array({ tagId });
        req["type"]    = "IMAGE";
        req["page"]    = page;
        req["size"]    = pageSize;

        std::string resp = httpPost("/api/search/metadata", req.dump());
        if (resp.empty()) break;

        try {
            auto j      = json::parse(resp);
            auto& items = j["assets"]["items"];

            for (const auto& asset : items) {
                ImageInfo info = parseAssetJson(asset);
                if (!info.immichId.empty())
                    result.push_back(std::move(info));
            }

            int count = j["assets"].value("count", 0);
            if (count < pageSize) break;   // last page
            ++page;

        } catch (const std::exception& e) {
            std::cerr << "[Immich] getTaggedAssets parse error: " << e.what() << "\n";
            break;
        }
    }

    std::cout << "[Immich] " << result.size()
              << " assets found with tag '" << tagName << "'.\n";
    return result;
}

// ---------------------------------------------------------------------------
// Album assets  (GET /api/albums/{id})
// ---------------------------------------------------------------------------

std::vector<ImageInfo> ImmichClient::getAlbumAssets(const std::string& albumName)
{
    // Find album ID
    std::string albumId;
    {
        std::string resp = httpGet("/api/albums");
        if (resp.empty()) {
            std::cerr << "[Immich] Failed to retrieve album list.\n";
            return {};
        }
        try {
            auto j = json::parse(resp);
            for (const auto& album : j)
                if (album.value("albumName", "") == albumName) {
                    albumId = album.value("id", "");
                    break;
                }
        } catch (const std::exception& e) {
            std::cerr << "[Immich] getAlbumAssets list parse error: " << e.what() << "\n";
            return {};
        }
    }

    if (albumId.empty()) {
        std::cerr << "[Immich] Album '" << albumName << "' not found.\n";
        return {};
    }

    // Fetch full album (includes assets array)
    std::string resp = httpGet("/api/albums/" + albumId + "?withoutAssets=false");
    if (resp.empty()) return {};

    std::vector<ImageInfo> result;
    try {
        auto j = json::parse(resp);
        for (const auto& asset : j["assets"]) {
            if (asset.value("type", "") != "IMAGE") continue;
            ImageInfo info = parseAssetJson(asset);
            if (!info.immichId.empty())
                result.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        std::cerr << "[Immich] getAlbumAssets parse error: " << e.what() << "\n";
    }

    std::cout << "[Immich] " << result.size()
              << " assets found in album '" << albumName << "'.\n";
    return result;
}

// ---------------------------------------------------------------------------
// Image download
// ---------------------------------------------------------------------------

cv::Mat ImmichClient::downloadImage(const std::string& assetId)
{
    std::string url = m_serverUrl + "/api/assets/" + assetId + "/thumbnail?size=preview";
    auto data = httpGetBinary(url);

    if (data.empty()) return cv::Mat();

    cv::Mat buf(1, static_cast<int>(data.size()), CV_8UC1, data.data());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);

    if (img.empty())
        std::cerr << "[Immich] Failed to decode image for asset " << assetId << "\n";

    return img;
}

// ---------------------------------------------------------------------------
// Share link (create once, cache in memory + db.json)
// ---------------------------------------------------------------------------

std::string ImmichClient::getShareUrl(const std::string& assetId)
{
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_shareCache.find(assetId);
        if (it != m_shareCache.end())
            return m_serverUrl + "/share/" + it->second;
    }

    // Try each API key — the asset owner's key will succeed, others will get 400
    json req;
    req["type"]         = "INDIVIDUAL";
    req["assetIds"]     = json::array({ assetId });
    req["allowDownload"]= true;
    req["showMetadata"] = true;
    std::string body = req.dump();

    for (const auto& key : m_apiKeys) {
        std::string resp = httpPostWithKey("/api/shared-links", body, key);
        if (resp.empty()) continue;
        try {
            auto j = json::parse(resp);
            std::string shareKey = j.value("key", "");
            if (shareKey.empty()) continue;

            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_shareCache[assetId] = shareKey;
            return m_serverUrl + "/share/" + shareKey;
        } catch (...) {}
    }

    std::cerr << "[Immich] getShareUrl: no key could create share link for " << assetId << "\n";
    return {};
}

// ---------------------------------------------------------------------------
// Share cache persistence helpers
// ---------------------------------------------------------------------------

void ImmichClient::loadShareCache(const std::map<std::string, std::string>& cache)
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_shareCache = cache;
}

void ImmichClient::setApiKeys(const std::vector<std::string>& keys)
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (!keys.empty()) {
        m_apiKeys = keys;
        std::cout << "[Immich] API keys updated (" << keys.size() << " key(s)).\n";
    }
}

std::map<std::string, std::string> ImmichClient::getShareCache() const
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_shareCache;
}
