#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <opencv2/opencv.hpp>

// ---------------------------------------------------------------------------
// ImageInfo — metadata for one photo asset
// ---------------------------------------------------------------------------
struct ImageInfo {
    std::string immichId;    // Immich asset UUID
    std::string fileName;    // original file name
    std::string albumName;   // parent folder name on disk, e.g. "2025 - Hochzeit"
    std::string date;        // formatted date string, e.g. "17.10.2025"
    std::string shareUrl;    // full share URL, e.g. "http://host/share/<key>"
};

// ---------------------------------------------------------------------------
// ImmichClient — all communication with the Immich REST API
// ---------------------------------------------------------------------------
class ImmichClient {
public:
    // Single API key (primary use-case)
    ImmichClient(const std::string& serverUrl, const std::string& apiKey);
    // Multiple API keys — used to create share links for assets owned by different users
    ImmichClient(const std::string& serverUrl, const std::vector<std::string>& apiKeys);

    // Resolve tag name → tag UUID.  Returns empty string on failure.
    std::string findTagId(const std::string& tagName);

    // Fetch all IMAGE assets that carry the given tag name.
    // Handles pagination automatically.  Returns empty vector on error.
    std::vector<ImageInfo> getTaggedAssets(const std::string& tagName);

    // Fetch all IMAGE assets in the named shared album.
    std::vector<ImageInfo> getAlbumAssets(const std::string& albumName);

    // Download the preview thumbnail for an asset as a cv::Mat.
    cv::Mat downloadImage(const std::string& assetId);

    // Return a public share URL for the asset.
    // Creates a new shared-link via the API the first time; subsequent calls
    // return the cached key from memory.
    std::string getShareUrl(const std::string& assetId);

    // Pre-seed the share-link cache (loaded from db.json on startup).
    void loadShareCache(const std::map<std::string, std::string>& cache);

    // Read the current share-link cache (to persist in db.json).
    std::map<std::string, std::string> getShareCache() const;

    // Replace API keys at runtime (config hot-reload).
    void setApiKeys(const std::vector<std::string>& keys);

    const std::string& serverUrl() const { return m_serverUrl; }

private:
    std::string httpGet(const std::string& path);
    std::string httpPost(const std::string& path, const std::string& body);
    std::string httpPostWithKey(const std::string& path, const std::string& body, const std::string& key);
    std::vector<uint8_t> httpGetBinary(const std::string& url);

    std::string              m_serverUrl;
    std::vector<std::string> m_apiKeys;   // [0] = primary; all tried for share-link creation
    std::map<std::string, std::string> m_shareCache;  // assetId → shareKey
    mutable std::mutex                 m_cacheMutex;
};
