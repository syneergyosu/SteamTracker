#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <wininet.h>
#include <shellapi.h> 
#include <sys/stat.h>

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <fstream> 
#include <ctime> 
#include <cctype>

#include <thread>
#include <mutex>
#include <atomic>

// Third-party headers
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "wininet.lib")

using json = nlohmann::json;

// --- DirectX 11 Global State ---
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- Steam Profile Data Model ---
enum class PlayerStatus {
    Offline,
    Online,
    InGame,
    Banned
};

struct TrackedPlayer {
    std::string steamId;
    std::string personaName = "Loading...";
    std::string avatarUrl;
    std::string gameName;
    int steamLevel = -1;
    int accountAgeYears = -1;
    int cs2Hours = -1;
    bool isBanned = false;
    int communityBanned = 0;
    int vacBans = 0;
    int gameBans = 0;
    int personaState = 0;
    PlayerStatus status = PlayerStatus::Offline;

    char noteBuffer[256] = "";
    bool showNote = false;

    std::string lastAvatarUrl = "";

    ID3D11ShaderResourceView* avatarTexture = nullptr;
};

// --- Thread Safety & Sync Globals ---
std::vector<TrackedPlayer> g_Players;
std::recursive_mutex g_PlayersMutex;
std::atomic<int> g_ActiveFetches(0);
std::mutex g_D3DMutex;
time_t g_LastFileModTime = 0;

char g_ApiKeyBuffer[128] = "DC13DD1969A48C1F18DC43129B206F67";
char g_SteamIdInput[64] = "";
char g_SearchBuffer[128] = "";

// --- Local File Caching System ---
void SaveAvatarToCache(const std::string& steamId, const std::string& rawBytes) {
    CreateDirectoryA("cache", NULL);
    std::string path = "cache\\" + steamId + ".jpg";
    std::ofstream file(path, std::ios::binary);
    if (file.is_open()) {
        file.write(rawBytes.data(), rawBytes.size());
    }
}

std::string LoadAvatarFromCache(const std::string& steamId) {
    std::string path = "cache\\" + steamId + ".jpg";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::string buffer(size, '\0');
        if (file.read(&buffer[0], size)) {
            return buffer;
        }
    }
    return "";
}

ID3D11ShaderResourceView* LoadTextureFromMemoryData(const std::string& imgData) {
    if (imgData.empty() || !g_pd3dDevice) return nullptr;

    int width = 0, height = 0, channels = 0;
    unsigned char* data = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(imgData.data()),
        static_cast<int>(imgData.size()),
        &width, &height, &channels, 4
    );

    if (!data) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem = data;
    subResource.SysMemPitch = desc.Width * 4;

    ID3D11Texture2D* pTexture = nullptr;
    ID3D11ShaderResourceView* outSRV = nullptr;

    std::lock_guard<std::mutex> lock(g_D3DMutex);
    if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;

        g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &outSRV);
        pTexture->Release();
    }

    stbi_image_free(data);
    return outSRV;
}

// --- Save & Load Config ---
void SaveConfig() {
    std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);
    json j;
    j["players"] = json::array();
    for (const auto& p : g_Players) {
        json pj;
        pj["id"] = p.steamId;
        pj["name"] = p.personaName;
        pj["level"] = p.steamLevel;
        pj["age"] = p.accountAgeYears;
        pj["cs2_hours"] = p.cs2Hours;
        pj["banned"] = p.isBanned;
        pj["vacBans"] = p.vacBans;
        pj["gameBans"] = p.gameBans;
        pj["note"] = p.noteBuffer;
        pj["avatarUrl"] = p.lastAvatarUrl;
        j["players"].push_back(pj);
    }
    std::ofstream file("tracker_config.json");
    if (file.is_open()) {
        file << j.dump(4);
    }
    struct stat fileInfo;
    if (stat("tracker_config.json", &fileInfo) == 0) {
        g_LastFileModTime = fileInfo.st_mtime;
    }
}

void RefreshAllBackground();
void RefreshPlayerDetails(TrackedPlayer& player, const std::string& apiKey);

void LoadConfig() {
    std::ifstream file("tracker_config.json");
    if (file.is_open()) {
        json j;
        try {
            file >> j;
            if (j.contains("players")) {
                std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);
                for (const auto& item : j["players"]) {
                    TrackedPlayer p;
                    if (item.is_string()) {
                        p.steamId = item.get<std::string>();
                    }
                    else if (item.is_object()) {
                        p.steamId = item.value("id", "");
                        p.personaName = item.value("name", "Loading...");
                        p.steamLevel = item.value("level", -1);
                        p.accountAgeYears = item.value("age", -1);
                        p.cs2Hours = item.value("cs2_hours", -1);
                        p.isBanned = item.value("banned", false);
                        p.vacBans = item.value("vacBans", 0);
                        p.gameBans = item.value("gameBans", 0);
                        p.status = p.isBanned ? PlayerStatus::Banned : PlayerStatus::Offline;

                        std::string n = item.value("note", "");
                        snprintf(p.noteBuffer, sizeof(p.noteBuffer), "%s", n.c_str());
                        p.showNote = false;

                        p.lastAvatarUrl = item.value("avatarUrl", "");

                        std::string cachedImage = LoadAvatarFromCache(p.steamId);
                        if (!cachedImage.empty()) {
                            p.avatarTexture = LoadTextureFromMemoryData(cachedImage);
                            if (p.avatarTexture == nullptr) {
                                DeleteFileA(("cache\\" + p.steamId + ".jpg").c_str());
                                p.lastAvatarUrl = "";
                            }
                        }
                        else {
                            p.lastAvatarUrl = "";
                        }
                    }
                    if (!p.steamId.empty()) {
                        g_Players.push_back(p);
                    }
                }
            }
        }
        catch (...) {}
    }

    struct stat fileInfo;
    if (stat("tracker_config.json", &fileInfo) == 0) {
        g_LastFileModTime = fileInfo.st_mtime;
    }

    RefreshAllBackground();
}

// --- Helper Functions ---
std::string HttpGet(const std::string& url) {
    std::string response;
    HINTERNET hInternet = InternetOpenA("Mozilla/5.0 (Windows NT 10.0; Win64; x64)", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return "";

    HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return "";
    }

    char buffer[4096];
    DWORD bytesRead = 0;
    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        response.append(buffer, bytesRead);
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return response;
}

bool StringContains(const std::string& str, const std::string& query) {
    auto it = std::search(
        str.begin(), str.end(),
        query.begin(), query.end(),
        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
    return (it != str.end());
}

// --- Steam API Fetching Logic ---
void RefreshPlayerDetails(TrackedPlayer& player, const std::string& apiKey) {
    if (apiKey.empty() || player.steamId.empty()) return;

    std::string summaryUrl = "https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v0002/?key=" + apiKey + "&steamids=" + player.steamId;
    std::string summaryJsonStr = HttpGet(summaryUrl);
    try {
        auto summaryJson = json::parse(summaryJsonStr);
        if (summaryJson.contains("response") && summaryJson["response"].contains("players") && !summaryJson["response"]["players"].empty()) {
            auto& p = summaryJson["response"]["players"][0];
            player.personaName = p.value("personaname", "Unknown");
            player.avatarUrl = p.value("avatarfull", "");
            player.personaState = p.value("personastate", 0);
            player.gameName = p.value("gameextrainfo", "");

            long long timeCreated = p.value("timecreated", 0LL);
            if (timeCreated > 0) {
                long long now = std::time(nullptr);
                player.accountAgeYears = static_cast<int>((now - timeCreated) / (365.25 * 24 * 3600));
            }
            else {
                player.accountAgeYears = -1;
            }
        }
    }
    catch (...) {}

    std::string levelUrl = "https://api.steampowered.com/IPlayerService/GetSteamLevel/v1/?key=" + apiKey + "&steamid=" + player.steamId;
    std::string levelJsonStr = HttpGet(levelUrl);
    try {
        auto levelJson = json::parse(levelJsonStr);
        if (levelJson.contains("response") && levelJson["response"].contains("player_level")) {
            player.steamLevel = levelJson["response"]["player_level"];
        }
        else {
            player.steamLevel = -1;
        }
    }
    catch (...) {}

    std::string gamesUrl = "https://api.steampowered.com/IPlayerService/GetOwnedGames/v0001/?key=" + apiKey + "&steamid=" + player.steamId + "&appids_filter[0]=730";
    std::string gamesJsonStr = HttpGet(gamesUrl);
    try {
        auto gamesJson = json::parse(gamesJsonStr);
        if (gamesJson.contains("response") && gamesJson["response"].contains("games") && !gamesJson["response"]["games"].empty()) {
            player.cs2Hours = gamesJson["response"]["games"][0].value("playtime_forever", 0) / 60;
        }
        else {
            player.cs2Hours = -1;
        }
    }
    catch (...) {}

    std::string bansUrl = "https://api.steampowered.com/ISteamUser/GetPlayerBans/v1/?key=" + apiKey + "&steamids=" + player.steamId;
    std::string bansJsonStr = HttpGet(bansUrl);
    try {
        auto bansJson = json::parse(bansJsonStr);
        if (bansJson.contains("players") && !bansJson["players"].empty()) {
            auto& b = bansJson["players"][0];
            bool vacBanned = b.value("VACBanned", false);
            player.vacBans = b.value("NumberOfVACBans", 0);
            player.gameBans = b.value("NumberOfGameBans", 0);
            player.communityBanned = b.value("CommunityBanned", false) ? 1 : 0;
            player.isBanned = vacBanned || (player.gameBans > 0) || (player.communityBanned > 0);
        }
    }
    catch (...) {}

    if (player.isBanned) {
        player.status = PlayerStatus::Banned;
    }
    else if (!player.gameName.empty()) {
        player.status = PlayerStatus::InGame;
    }
    else if (player.personaState > 0) {
        player.status = PlayerStatus::Online;
    }
    else {
        player.status = PlayerStatus::Offline;
    }

    bool needsAvatarDownload = (player.avatarUrl != player.lastAvatarUrl) || player.lastAvatarUrl.empty();

    if (needsAvatarDownload && !player.avatarUrl.empty()) {
        std::string rawImageBytes = HttpGet(player.avatarUrl);
        if (!rawImageBytes.empty()) {
            ID3D11ShaderResourceView* newTex = LoadTextureFromMemoryData(rawImageBytes);
            if (newTex != nullptr) {
                SaveAvatarToCache(player.steamId, rawImageBytes);
                player.lastAvatarUrl = player.avatarUrl;
                player.avatarTexture = newTex;
            }
            else {
                player.lastAvatarUrl = "";
            }
        }
    }
}

void ApplyPlayerUpdate(TrackedPlayer& realPlayer, TrackedPlayer& tempPlayer) {
    realPlayer.personaName = tempPlayer.personaName;
    realPlayer.steamLevel = tempPlayer.steamLevel;
    realPlayer.accountAgeYears = tempPlayer.accountAgeYears;
    realPlayer.cs2Hours = tempPlayer.cs2Hours;
    realPlayer.isBanned = tempPlayer.isBanned;
    realPlayer.vacBans = tempPlayer.vacBans;
    realPlayer.gameBans = tempPlayer.gameBans;
    realPlayer.status = tempPlayer.status;
    realPlayer.avatarUrl = tempPlayer.avatarUrl;

    if (tempPlayer.avatarTexture != nullptr) {
        if (realPlayer.avatarTexture) realPlayer.avatarTexture->Release();
        realPlayer.avatarTexture = tempPlayer.avatarTexture;
        realPlayer.lastAvatarUrl = tempPlayer.lastAvatarUrl;
    }
}

// --- External Change Detector (Discord Bot / JSON Watcher) ---
void CheckForExternalChanges() {
    struct stat fileInfo;
    if (stat("tracker_config.json", &fileInfo) == 0) {
        if (fileInfo.st_mtime > g_LastFileModTime) {
            g_LastFileModTime = fileInfo.st_mtime;

            std::ifstream file("tracker_config.json");
            if (file.is_open()) {
                json j;
                try {
                    file >> j;
                    if (j.contains("players")) {
                        std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);

                        for (const auto& item : j["players"]) {
                            if (!item.is_object()) continue;
                            std::string steamId = item.value("id", "");
                            if (steamId.empty()) continue;

                            auto it = std::find_if(g_Players.begin(), g_Players.end(), [&](const TrackedPlayer& p) {
                                return p.steamId == steamId;
                                });

                            if (it != g_Players.end()) {
                                // Sync basic fields & notes from external edits
                                it->personaName = item.value("name", it->personaName);
                                it->avatarUrl = item.value("avatarUrl", it->avatarUrl);
                                it->steamLevel = item.value("level", it->steamLevel);
                                it->accountAgeYears = item.value("age", it->accountAgeYears);
                                it->cs2Hours = item.value("cs2_hours", it->cs2Hours);
                                it->isBanned = item.value("banned", it->isBanned);
                                it->vacBans = item.value("vacBans", it->vacBans);
                                it->gameBans = item.value("gameBans", it->gameBans);

                                std::string n = item.value("note", "");
                                snprintf(it->noteBuffer, sizeof(it->noteBuffer), "%s", n.c_str());
                            }
                            else {
                                // Brand new player added externally (e.g. via Discord bot)
                                TrackedPlayer newPlayer;
                                newPlayer.steamId = steamId;
                                newPlayer.personaName = item.value("name", "Loading...");
                                newPlayer.avatarUrl = item.value("avatarUrl", "");
                                newPlayer.steamLevel = item.value("level", -1);
                                newPlayer.accountAgeYears = item.value("age", -1);
                                newPlayer.cs2Hours = item.value("cs2_hours", -1);
                                newPlayer.isBanned = item.value("banned", false);
                                newPlayer.vacBans = item.value("vacBans", 0);
                                newPlayer.gameBans = item.value("gameBans", 0);

                                std::string n = item.value("note", "");
                                snprintf(newPlayer.noteBuffer, sizeof(newPlayer.noteBuffer), "%s", n.c_str());

                                // Check if avatar is already cached locally
                                std::string cachedImage = LoadAvatarFromCache(steamId);
                                if (!cachedImage.empty()) {
                                    newPlayer.avatarTexture = LoadTextureFromMemoryData(cachedImage);
                                    newPlayer.lastAvatarUrl = newPlayer.avatarUrl;
                                }

                                g_Players.push_back(newPlayer);

                                // If no cached image exists yet, download it immediately
                                if (cachedImage.empty() && !newPlayer.avatarUrl.empty()) {
                                    std::thread([steamId, avatarUrl = newPlayer.avatarUrl]() {
                                        std::string rawImageBytes = HttpGet(avatarUrl);
                                        if (!rawImageBytes.empty()) {
                                            ID3D11ShaderResourceView* newTex = LoadTextureFromMemoryData(rawImageBytes);
                                            if (newTex != nullptr) {
                                                SaveAvatarToCache(steamId, rawImageBytes);
                                                std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);
                                                for (auto& realPlayer : g_Players) {
                                                    if (realPlayer.steamId == steamId) {
                                                        if (realPlayer.avatarTexture) realPlayer.avatarTexture->Release();
                                                        realPlayer.avatarTexture = newTex;
                                                        realPlayer.lastAvatarUrl = avatarUrl;
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                        }).detach();
                                }
                            }
                        }
                    }
                }
                catch (...) {}
            }
        }
    }
}

void RefreshAllBackground() {
    if (g_ActiveFetches > 0) return;

    std::thread([]() {
        g_ActiveFetches = 1;
        std::vector<TrackedPlayer> snapshot;

        {
            std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);
            for (const auto& p : g_Players) {
                snapshot.push_back(p);
            }
        }

        if (snapshot.empty()) {
            g_ActiveFetches = 0;
            return;
        }

        std::vector<std::thread> workers;
        int delayMs = 0;

        for (const auto& cachedPlayer : snapshot) {
            workers.emplace_back([cachedPlayer, delayMs]() {
                Sleep(delayMs);

                TrackedPlayer tempPlayer = cachedPlayer;
                tempPlayer.avatarTexture = nullptr;

                RefreshPlayerDetails(tempPlayer, g_ApiKeyBuffer);

                std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);
                for (auto& realPlayer : g_Players) {
                    if (realPlayer.steamId == tempPlayer.steamId) {
                        ApplyPlayerUpdate(realPlayer, tempPlayer);
                        break;
                    }
                }
                });
            delayMs += 50;
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        SaveConfig();
        g_ActiveFetches = 0;
        }).detach();
}

void AddSteamPlayer(const std::string& steamId) {
    if (steamId.empty()) return;

    TrackedPlayer newPlayer;
    newPlayer.steamId = steamId;
    {
        std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);
        for (const auto& p : g_Players) {
            if (p.steamId == steamId) return;
        }
        g_Players.push_back(newPlayer);
    }

    g_ActiveFetches = 1;
    std::thread([newPlayer]() {
        TrackedPlayer tempPlayer = newPlayer;
        RefreshPlayerDetails(tempPlayer, g_ApiKeyBuffer);

        std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);
        for (auto& realPlayer : g_Players) {
            if (realPlayer.steamId == tempPlayer.steamId) {
                ApplyPlayerUpdate(realPlayer, tempPlayer);
                break;
            }
        }
        SaveConfig();
        g_ActiveFetches = 0;
        }).detach();
}

// --- Main GUI Renderer ---
void RenderSteamTrackerUI() {
    std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::Begin("Steam Player Tracker", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "STEAM PROFILE & BAN MONITOR");
    ImGui::Separator();

    ImGui::PushItemWidth(240.0f);
    ImGui::InputTextWithHint("##SteamID", "Enter SteamID64 (e.g., 76561198...)", g_SteamIdInput, IM_ARRAYSIZE(g_SteamIdInput));
    ImGui::PopItemWidth();
    ImGui::SameLine();

    if (ImGui::Button("Track Profile", ImVec2(120, 0))) {
        if (strlen(g_SteamIdInput) > 0) {
            AddSteamPlayer(g_SteamIdInput);
            g_SteamIdInput[0] = '\0';
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh All", ImVec2(100, 0))) {
        RefreshAllBackground();
    }

    ImGui::SameLine();
    static bool autoRefresh = true;
    ImGui::Checkbox("Live Auto-Refresh (60s)", &autoRefresh);

    static float lastRefreshTime = ImGui::GetTime();
    if (autoRefresh && g_ActiveFetches == 0) {
        float currentTime = ImGui::GetTime();
        if (currentTime - lastRefreshTime > 60.0f) {
            RefreshAllBackground();
            lastRefreshTime = currentTime;
        }
    }

    if (g_ActiveFetches > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), " [ Fetching Data... ]");
    }

    int totalCount = static_cast<int>(g_Players.size());
    int bannedCount = 0;
    int inGameCount = 0;
    int onlineCount = 0;

    for (const auto& p : g_Players) {
        if (p.isBanned) bannedCount++;
        if (p.status == PlayerStatus::InGame) inGameCount++;
        else if (p.status == PlayerStatus::Online) onlineCount++;
    }

    ImGui::Spacing();
    ImGui::Text("Total Tracked: %d  |  ", totalCount);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "Banned: %d", bannedCount);
    ImGui::SameLine();
    ImGui::Text("  |  ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.3f, 1.0f), "In-Game: %d", inGameCount);
    ImGui::SameLine();
    ImGui::Text("  |  ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "Online: %d", onlineCount);

    ImGui::SameLine(ImGui::GetWindowWidth() - 250.0f);
    ImGui::PushItemWidth(235.0f);
    ImGui::InputTextWithHint("##Search", "Search Profiles...", g_SearchBuffer, IM_ARRAYSIZE(g_SearchBuffer));
    ImGui::PopItemWidth();

    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild("PlayerGridScrollArea", ImVec2(0, 0), false);

    float panelWidth = 280.0f;
    float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    std::vector<size_t> visibleIndices;
    std::string searchQuery = g_SearchBuffer;

    for (size_t i = 0; i < g_Players.size(); ++i) {
        if (!searchQuery.empty()) {
            if (!StringContains(g_Players[i].personaName, searchQuery) &&
                !StringContains(g_Players[i].steamId, searchQuery)) {
                continue;
            }
        }
        visibleIndices.push_back(i);
    }

    for (size_t v = 0; v < visibleIndices.size(); ++v) {
        size_t i = visibleIndices[v];
        auto& player = g_Players[i];

        ImGui::PushID(static_cast<int>(i));

        float currentPanelHeight = player.showNote ? 175.0f : 135.0f;

        ImGui::BeginChild("ProfileCard", ImVec2(panelWidth, currentPanelHeight), true);

        if (player.avatarTexture) {
            ImGui::Image((ImTextureID)player.avatarTexture, ImVec2(54, 54));
        }
        else {
            ImGui::Dummy(ImVec2(54, 54));
        }

        ImGui::SameLine();
        ImGui::BeginGroup();

        if (player.steamLevel >= 0) {
            ImGui::Text("%s (Lvl %d)", player.personaName.c_str(), player.steamLevel);
        }
        else {
            ImGui::TextUnformatted(player.personaName.c_str());
        }

        ImVec4 statusColor;
        const char* statusLabel = "Offline";

        switch (player.status) {
        case PlayerStatus::Banned:
            statusColor = ImVec4(0.95f, 0.2f, 0.2f, 1.0f);
            statusLabel = "BANNED";
            break;
        case PlayerStatus::InGame:
            statusColor = ImVec4(0.2f, 0.85f, 0.3f, 1.0f);
            statusLabel = player.gameName.empty() ? "Playing" : player.gameName.c_str();
            break;
        case PlayerStatus::Online:
            statusColor = ImVec4(0.2f, 0.6f, 1.0f, 1.0f);
            statusLabel = "Online";
            break;
        case PlayerStatus::Offline:
        default:
            statusColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            statusLabel = "Offline";
            break;
        }

        ImGui::TextColored(statusColor, "[o] %s", statusLabel);

        std::string ageStr = player.accountAgeYears >= 0 ? std::to_string(player.accountAgeYears) + " Yrs" : "Priv";
        std::string csStr = player.cs2Hours >= 0 ? std::to_string(player.cs2Hours) + "h" : "Priv";
        ImGui::TextDisabled("Age: %s | CS2: %s", ageStr.c_str(), csStr.c_str());

        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", player.steamId.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Open Steam Profile in Browser");
            if (ImGui::IsItemClicked()) {
                std::string url = "https://steamcommunity.com/profiles/" + player.steamId;
                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }
        }
        ImGui::EndGroup();

        if (player.showNote) {
            ImGui::SetCursorPosY(106.0f);
            ImGui::PushItemWidth(panelWidth - 16.0f);
            if (ImGui::InputTextWithHint("##Note", "Add a custom note...", player.noteBuffer, IM_ARRAYSIZE(player.noteBuffer))) {}
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                SaveConfig();
            }
            ImGui::PopItemWidth();
        }

        ImGui::SetCursorPosY(currentPanelHeight - 26.0f);
        if (player.isBanned) {
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "VAC: %d | Game: %d", player.vacBans, player.gameBans);
        }
        else {
            ImGui::TextDisabled("Clean Record");
        }

        ImGui::SameLine(panelWidth - 120.0f);
        const char* noteBtnLabel = player.showNote ? "Hide" : (strlen(player.noteBuffer) > 0 ? "Note (*)" : "Note");
        if (ImGui::SmallButton(noteBtnLabel)) {
            player.showNote = !player.showNote;
        }

        ImGui::SameLine(panelWidth - 55.0f);
        if (ImGui::SmallButton("Remove")) {
            if (player.avatarTexture) player.avatarTexture->Release();
            g_Players.erase(g_Players.begin() + i);
            SaveConfig();
            ImGui::EndChild();
            ImGui::PopID();
            break;
        }

        ImGui::EndChild();

        float lastButtonX2 = ImGui::GetItemRectMax().x;
        float nextButtonX2 = lastButtonX2 + ImGui::GetStyle().ItemSpacing.x + panelWidth;
        if (v + 1 < visibleIndices.size() && nextButtonX2 < windowVisibleX2) {
            ImGui::SameLine();
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, hInstance, NULL, NULL, NULL, NULL, _T("SteamTrackerClass"), NULL };
    ::RegisterClassEx(&wc);
    HWND hwnd = ::CreateWindow(wc.lpszClassName, _T("Steam Player Tracker"), WS_OVERLAPPEDWINDOW, 100, 100, 960, 640, NULL, NULL, wc.hInstance, NULL);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    LoadConfig();

    bool done = false;
    float lastCheckTime = 0.0f;

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        float currentTime = ImGui::GetTime();
        if (currentTime - lastCheckTime > 3.0f) {
            CheckForExternalChanges();
            lastCheckTime = currentTime;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderSteamTrackerUI();

        ImGui::Render();
        const float clear_color[4] = { 0.10f, 0.11f, 0.12f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_PlayersMutex);
        for (auto& p : g_Players) {
            if (p.avatarTexture) p.avatarTexture->Release();
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClass(wc.lpszClassName, wc.hInstance);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProc(hWnd, msg, wParam, lParam);
}