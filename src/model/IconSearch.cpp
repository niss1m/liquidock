#include "model/IconSearch.h"

#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <map>

#include "core/ConfigPaths.h"
#include "core/Log.h"

#pragma comment(lib, "winhttp.lib")

namespace liquidock {
namespace {

constexpr wchar_t kKeyFile[] = L"macosicons.key";
constexpr wchar_t kCacheFolder[] = L"icons";
constexpr wchar_t kHost[] = L"api.macosicons.com";
constexpr wchar_t kPath[] = L"/api/v1/search";
// A few rows of the grid. The API will hand back a hundred, and a hundred
// downloads for a panel that shows fourteen at a time is somebody else's
// bandwidth spent on nothing.
constexpr int kHitsPerPage = 36;
// Long enough for a cold CDN, short enough that a dead network does not leave
// the panel saying "searching" for a minute.
constexpr DWORD kTimeoutMs = 12000;

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        length, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        length);
    return out;
}

// --- the smallest JSON reader that answers this one question ----------------
//
// The shape is fixed and known: { "hits": [ { scalars... }, ... ], ... }. A
// scanner that can find one named array, walk the objects in it and pull the
// scalar fields out of each is a hundred lines; a JSON library is a dependency,
// and this project has spent its whole life not acquiring one.
class Scanner {
public:
    explicit Scanner(const std::string& text) : text_(text) {}

    void Space() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' ||
                text_[at_] == '\r')) {
            ++at_;
        }
    }

    bool Take(char expected) {
        Space();
        if (at_ < text_.size() && text_[at_] == expected) {
            ++at_;
            return true;
        }
        return false;
    }

    char Peek() {
        Space();
        return at_ < text_.size() ? text_[at_] : '\0';
    }

    bool Done() const { return at_ >= text_.size(); }

    // A JSON string, escapes and all, decoded into UTF-8.
    bool String(std::string& out) {
        Space();
        if (at_ >= text_.size() || text_[at_] != '"') {
            return false;
        }
        ++at_;
        out.clear();
        while (at_ < text_.size()) {
            const char ch = text_[at_++];
            if (ch == '"') {
                return true;
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (at_ >= text_.size()) {
                return false;
            }
            const char escape = text_[at_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned code = 0;
                    if (!Hex4(code)) {
                        return false;
                    }
                    // A surrogate pair is two escapes describing one character,
                    // and an icon called "🎨" arrives as exactly that.
                    if (code >= 0xD800 && code <= 0xDBFF && at_ + 1 < text_.size() &&
                        text_[at_] == '\\' && text_[at_ + 1] == 'u') {
                        const size_t mark = at_;
                        at_ += 2;
                        unsigned low = 0;
                        if (Hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            at_ = mark;
                        }
                    }
                    AppendUtf8(code, out);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    // Any value at all, stepped over without being kept.
    void Skip() {
        Space();
        if (at_ >= text_.size()) {
            return;
        }
        const char ch = text_[at_];
        if (ch == '"') {
            std::string ignored;
            String(ignored);
            return;
        }
        if (ch == '{' || ch == '[') {
            const char open = ch;
            const char close = (ch == '{') ? '}' : ']';
            int depth = 0;
            while (at_ < text_.size()) {
                const char here = text_[at_];
                if (here == '"') {
                    std::string ignored;
                    String(ignored);
                    continue;
                }
                ++at_;
                if (here == open) {
                    ++depth;
                } else if (here == close) {
                    if (--depth == 0) {
                        return;
                    }
                }
            }
            return;
        }
        // A number, true, false or null: everything up to whatever ends it.
        while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}' &&
               text_[at_] != ']') {
            ++at_;
        }
    }

    // A scalar, as text. Empty for a value that was an object or an array,
    // which is stepped over instead.
    std::string Scalar() {
        Space();
        if (at_ < text_.size() && text_[at_] == '"') {
            std::string out;
            return String(out) ? out : std::string();
        }
        if (at_ < text_.size() && (text_[at_] == '{' || text_[at_] == '[')) {
            Skip();
            return {};
        }
        const size_t start = at_;
        while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}' &&
               text_[at_] != ']' && text_[at_] != ' ' && text_[at_] != '\n' &&
               text_[at_] != '\r' && text_[at_] != '\t') {
            ++at_;
        }
        return text_.substr(start, at_ - start);
    }

private:
    bool Hex4(unsigned& out) {
        if (at_ + 4 > text_.size()) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[at_++];
            out <<= 4;
            if (ch >= '0' && ch <= '9') {
                out |= static_cast<unsigned>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                out |= static_cast<unsigned>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                out |= static_cast<unsigned>(ch - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    static void AppendUtf8(unsigned code, std::string& out) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    const std::string& text_;
    size_t at_ = 0;
};

// The hits, or false if the document was not the shape it should be.
bool ReadHits(const std::string& text, std::vector<IconHit>& out) {
    Scanner scan(text);
    if (!scan.Take('{')) {
        return false;
    }
    while (!scan.Done()) {
        if (scan.Take('}')) {
            break;
        }
        std::string key;
        if (!scan.String(key) || !scan.Take(':')) {
            return false;
        }
        if (key != "hits") {
            scan.Skip();
        } else {
            if (!scan.Take('[')) {
                return false;
            }
            while (!scan.Done() && !scan.Take(']')) {
                if (!scan.Take('{')) {
                    scan.Skip();
                    scan.Take(',');
                    continue;
                }
                IconHit hit;
                while (!scan.Done() && !scan.Take('}')) {
                    std::string field;
                    if (!scan.String(field) || !scan.Take(':')) {
                        break;
                    }
                    const std::string value = scan.Scalar();
                    if (field == "appName") {
                        hit.appName = FromUtf8(value);
                    } else if (field == "lowResPngUrl") {
                        hit.pngUrl = FromUtf8(value);
                    } else if (field == "usersName" || field == "credit") {
                        // The documented field is `credit`; what the API
                        // actually sends is `usersName`. Both are read, and
                        // whichever arrives is the author's name.
                        if (hit.credit.empty()) {
                            hit.credit = FromUtf8(value);
                        }
                    } else if (field == "creditUrl") {
                        hit.creditUrl = FromUtf8(value);
                    } else if (field == "downloads") {
                        hit.downloads = atoi(value.c_str());
                    }
                    scan.Take(',');
                }
                // A hit with no image is not a hit. The list is what the panel
                // draws, and a blank tile that cannot be picked is worse than
                // one fewer result.
                if (!hit.pngUrl.empty()) {
                    out.push_back(std::move(hit));
                }
                scan.Take(',');
            }
        }
        scan.Take(',');
    }
    return true;
}

// A named string at the top level, for reading the error out of a refusal.
std::string TopLevelString(const std::string& text, const char* wanted) {
    Scanner scan(text);
    if (!scan.Take('{')) {
        return {};
    }
    while (!scan.Done() && !scan.Take('}')) {
        std::string key;
        if (!scan.String(key) || !scan.Take(':')) {
            return {};
        }
        if (key == wanted) {
            return scan.Scalar();
        }
        scan.Skip();
        scan.Take(',');
    }
    return {};
}

// --- the network -------------------------------------------------------------

// One session for a whole search, so the thirty PNG downloads that follow it
// reuse the connection the search already negotiated rather than paying for a
// TLS handshake apiece.
class Http {
public:
    Http() {
        session_ = WinHttpOpen(L"LiquiDock/" LIQUIDOCK_VERSION_W,
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_) {
            WinHttpSetTimeouts(session_, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        }
    }
    ~Http() {
        for (auto& [host, handle] : connections_) {
            WinHttpCloseHandle(handle);
        }
        if (session_) {
            WinHttpCloseHandle(session_);
        }
    }
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    bool ok() const { return session_ != nullptr; }

    // `body` empty means GET. Returns false only when the exchange itself
    // failed; an HTTP error is a successful exchange with a status to read.
    bool Send(const std::wstring& host, const std::wstring& path, const std::string& body,
              const std::wstring& headers, DWORD& status, std::vector<uint8_t>& out) {
        HINTERNET connection = Connect(host);
        if (!connection) {
            return false;
        }
        HINTERNET request = WinHttpOpenRequest(connection, body.empty() ? L"GET" : L"POST",
                                               path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) {
            return false;
        }

        bool sent = true;
        if (!headers.empty()) {
            sent = WinHttpAddRequestHeaders(request, headers.c_str(),
                                            static_cast<DWORD>(-1),
                                            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        if (sent) {
            sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      body.empty() ? nullptr : const_cast<char*>(body.data()),
                                      static_cast<DWORD>(body.size()),
                                      static_cast<DWORD>(body.size()), 0) != FALSE;
        }
        if (sent) {
            sent = WinHttpReceiveResponse(request, nullptr) != FALSE;
        }
        if (!sent) {
            WinHttpCloseHandle(request);
            return false;
        }

        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

        out.clear();
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
                break;
            }
            const size_t was = out.size();
            out.resize(was + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, out.data() + was, available, &read)) {
                out.resize(was);
                break;
            }
            out.resize(was + read);
        }
        WinHttpCloseHandle(request);
        return true;
    }

private:
    HINTERNET Connect(const std::wstring& host) {
        auto found = connections_.find(host);
        if (found != connections_.end()) {
            return found->second;
        }
        HINTERNET handle =
            WinHttpConnect(session_, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (handle) {
            connections_.emplace(host, handle);
        }
        return handle;
    }

    HINTERNET session_ = nullptr;
    std::map<std::wstring, HINTERNET> connections_;
};

// Splits an absolute https URL into the two pieces WinHttp wants.
bool SplitUrl(const std::wstring& url, std::wstring& host, std::wstring& path) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return false; // http would be a downgrade nobody asked for
    }
    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    return !host.empty();
}

uint64_t Fingerprint(const std::wstring& text) {
    uint64_t hash = 1469598103934665603ull;
    for (const wchar_t ch : text) {
        hash = (hash ^ static_cast<uint64_t>(ch)) * 1099511628211ull;
    }
    return hash;
}

// A file name somebody could recognise in items.txt, and that cannot collide:
// the app's name for the human, the URL's fingerprint for the machine.
std::wstring CacheName(const IconHit& hit) {
    std::wstring slug;
    for (const wchar_t ch : hit.appName) {
        if (iswalnum(ch)) {
            slug.push_back(static_cast<wchar_t>(towlower(ch)));
        } else if (!slug.empty() && slug.back() != L'-') {
            slug.push_back(L'-');
        }
        if (slug.size() >= 40) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == L'-') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = L"icon";
    }
    wchar_t tail[24]{};
    swprintf_s(tail, L"-%08x", static_cast<unsigned>(Fingerprint(hit.pngUrl) & 0xFFFFFFFFull));
    return slug + tail + L".png";
}

bool WriteFile(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        return false;
    }
    const size_t written = fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    if (written == bytes.size()) {
        return true;
    }
    // A half-written PNG in the cache would be found by the next search and
    // trusted, so it goes rather than staying to be believed.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return false;
}

} // namespace

IconSearch::~IconSearch() {
    Stop();
}

std::wstring IconSearch::KeyPath() {
    return ConfigFilePath(kKeyFile);
}

std::wstring IconSearch::CacheDirectory() {
    const std::wstring base = ConfigDirectory();
    if (base.empty()) {
        return {};
    }
    const std::filesystem::path directory = std::filesystem::path(base) / kCacheFolder;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return directory.wstring();
}

std::wstring IconSearch::LoadKey() {
    const std::wstring path = KeyPath();
    if (path.empty()) {
        return {};
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rt, ccs=UTF-8") != 0 || !file) {
        return {};
    }
    wchar_t line[512]{};
    std::wstring key;
    if (fgetws(line, static_cast<int>(std::size(line)), file)) {
        key = line;
    }
    fclose(file);
    while (!key.empty() && (key.back() == L'\n' || key.back() == L'\r' || key.back() == L' ' ||
                            key.back() == L'\t')) {
        key.pop_back();
    }
    return key;
}

bool IconSearch::SaveKey(const std::wstring& key) {
    const std::wstring path = KeyPath();
    if (path.empty()) {
        return false;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wt, ccs=UTF-8") != 0 || !file) {
        return false;
    }
    fwprintf(file, L"%s\n", key.c_str());
    fclose(file);
    return true;
}

void IconSearch::ForgetKey() {
    const std::wstring path = KeyPath();
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool IconSearch::HasKey() {
    return !LoadKey().empty();
}

void IconSearch::Start(std::wstring query, HWND notify, UINT message) {
    Stop();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        hits_.clear();
        error_.clear();
    }
    const unsigned generation = generation_.load(std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread([this, query = std::move(query), notify, message, generation]() mutable {
        Run(std::move(query), notify, message, generation);
        running_.store(false, std::memory_order_relaxed);
    });
}

void IconSearch::Stop() {
    generation_.fetch_add(1, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_relaxed);
}

std::vector<IconHit> IconSearch::Take() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

std::wstring IconSearch::error() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void IconSearch::Run(std::wstring query, HWND notify, UINT message, unsigned generation) {
    const auto stale = [this, generation] {
        return generation_.load(std::memory_order_relaxed) != generation;
    };
    const auto fail = [this, notify, message, &stale](std::wstring text) {
        if (stale()) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::move(text);
            hits_.clear();
        }
        if (notify) {
            PostMessageW(notify, message, 0, 0);
        }
    };

    const std::wstring key = LoadKey();
    if (key.empty()) {
        fail(L"No macOSicons key yet.");
        return;
    }

    Http http;
    if (!http.ok()) {
        fail(L"Windows would not open a connection.");
        return;
    }

    // The query is escaped rather than pasted: an icon called 6" nail is a
    // perfectly reasonable thing to search for, and it would otherwise end the
    // JSON string early.
    std::string escaped;
    for (const char ch : ToUtf8(query)) {
        if (ch == '"' || ch == '\\') {
            escaped.push_back('\\');
            escaped.push_back(ch);
        } else if (static_cast<unsigned char>(ch) < 0x20) {
            escaped.push_back(' ');
        } else {
            escaped.push_back(ch);
        }
    }
    const std::string body = "{\"query\":\"" + escaped +
                             "\",\"searchOptions\":{\"hitsPerPage\":" +
                             std::to_string(kHitsPerPage) + ",\"page\":1}}";

    std::wstring headers = L"Content-Type: application/json\r\nx-api-key: ";
    headers += key;

    DWORD status = 0;
    std::vector<uint8_t> response;
    if (!http.Send(kHost, kPath, body, headers, status, response)) {
        fail(L"Could not reach macosicons.com.");
        return;
    }
    if (stale()) {
        return;
    }

    const std::string text(reinterpret_cast<const char*>(response.data()), response.size());
    if (status != 200) {
        // The status first, because it is the part that says what to do about
        // it; the server's own message only when it adds something.
        std::wstring reason;
        if (status == 401 || status == 403) {
            reason = L"macOSicons refused the key.";
        } else if (status == 429) {
            reason = L"macOSicons is rate limiting; try again shortly.";
        } else {
            reason = L"macOSicons answered " + std::to_wstring(status) + L".";
        }
        const std::string detail = TopLevelString(text, "message");
        if (!detail.empty()) {
            reason += L" " + FromUtf8(detail);
        }
        LogWarn("Icon search failed with HTTP {}", status);
        fail(std::move(reason));
        return;
    }

    std::vector<IconHit> hits;
    if (!ReadHits(text, hits)) {
        fail(L"macOSicons sent something this build could not read.");
        return;
    }
    if (hits.empty()) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            hits_.clear();
            error_.clear();
        }
        if (notify) {
            PostMessageW(notify, message, 0, 0);
        }
        return;
    }

    // The names first. The grid can draw them while the images are still
    // coming, which is the difference between a panel that fills in and one
    // that sits blank for two seconds and then appears.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        hits_ = hits;
        error_.clear();
    }
    if (notify) {
        PostMessageW(notify, message, 0, 0);
    }

    const std::wstring cache = CacheDirectory();
    if (cache.empty()) {
        return;
    }
    for (IconHit& hit : hits) {
        if (stale()) {
            return;
        }
        const std::filesystem::path file = std::filesystem::path(cache) / CacheName(hit);
        std::error_code ec;
        // Already downloaded, most likely by the search before this one. The
        // cache is also where a chosen icon lives for good, so this is the same
        // check that stops a re-pick re-downloading.
        if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > 0) {
            hit.file = file.wstring();
            continue;
        }
        std::wstring host;
        std::wstring path;
        if (!SplitUrl(hit.pngUrl, host, path)) {
            continue;
        }
        DWORD imageStatus = 0;
        std::vector<uint8_t> bytes;
        if (!http.Send(host, path, {}, {}, imageStatus, bytes) || imageStatus != 200 ||
            bytes.empty()) {
            continue;
        }
        if (WriteFile(file.wstring(), bytes)) {
            hit.file = file.wstring();
        }
    }
    if (stale()) {
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        hits_ = std::move(hits);
    }
    if (notify) {
        PostMessageW(notify, message, 0, 0);
    }
}

} // namespace liquidock
