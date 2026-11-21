// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_report.h"
#if !defined(MELEE_PORT_OFFLINE) || !MELEE_PORT_OFFLINE
#include "slippi_net.h"
#include "host.h"
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <sys/stat.h>
#include <CommonCrypto/CommonDigest.h>
#include "slippi_http_apple.h"
#else
#include <curl/curl.h>
#include <sys/stat.h>
#include <openssl/md5.h>
#endif
#include <nlohmann/json.hpp>
#include "json_safe.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

namespace slippi::report {
namespace {
using json = nlohmann::json;
#if defined(_WIN32)
const wchar_t* USER_AGENT = L"SlippiDolphin (b: ishiiruka) (v: 3.7.0) (o: windows)";
#elif defined(__APPLE__)
const char* USER_AGENT = "SlippiDolphin (b: ishiiruka) (v: 3.7.0) (o: macos)";
#else
const char* USER_AGENT = "SlippiDolphin (b: ishiiruka) (v: 3.7.0) (o: linux)";
#endif
const char* ENDPOINT = "https://internal.slippi.gg/graphql";
constexpr int MAX_ATTEMPTS = 5;

struct StatusJob { std::string uid, play_key, match_id, status; };
struct Job { bool is_status = false; GameReport game; StatusJob status; int attempts = 0; };

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Job> g_queue;
std::thread g_thread;
bool g_quit = false;
std::string g_iso_path, g_cache_dir;
std::atomic<bool> g_hash_done{false};
std::string g_iso_hash;

#if defined(_WIN32)
// ---- HTTP (WinHTTP)
std::wstring widen(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n ? n - 1 : 0, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); return w; }

bool http(const char* method, const std::string& url, const std::string& headers_utf8, const std::string& body, int* status, std::string* response) {
  const std::wstring headers = widen(headers_utf8);
  std::wstring wurl = widen(url);
  URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
  wchar_t host[256]{}, path[2048]{};
  uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
  HINTERNET session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, 5000, 5000, 15000, 15000);
  bool ok = false;
  HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
  if (conn) {
    std::wstring wmethod = widen(method);
    HINTERNET req = WinHttpOpenRequest(conn, wmethod.c_str(), path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req) {
      if (WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1,
                             body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
          WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (status) *status = (int)code;
        std::string out;
        for (;;) {
          DWORD avail = 0;
          if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
          std::string chunk(avail, 0); DWORD got = 0;
          if (!WinHttpReadData(req, chunk.data(), avail, &got)) break;
          out.append(chunk.data(), got);
        }
        if (response) *response = out;
        ok = true;
      }
      WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
  }
  WinHttpCloseHandle(session);
  return ok;
}
#elif defined(__APPLE__)
// ---- HTTP (NSURLSession, see slippi_http_apple.mm): same contract as the WinHTTP path.
bool http(const char* method, const std::string& url, const std::string& headers, const std::string& body, int* status, std::string* response) {
  std::string error;
  if (apple_http(method, url, headers, body, USER_AGENT, status, response, &error)) return true;
  host::log("slippi http: %s %s failed: %s", method, url.substr(0, 80).c_str(), error.c_str());
  return false;
}
#else
// ---- HTTP (libcurl). Same contract as the WinHTTP path: `headers` are CRLF-separated
// header lines; `status` receives the HTTP code; `response` the body.
size_t curl_sink(char* data, size_t size, size_t count, void* user) {
  static_cast<std::string*>(user)->append(data, size * count);
  return size * count;
}
struct CurlGlobal { CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); } ~CurlGlobal() { curl_global_cleanup(); } };

bool http(const char* method, const std::string& url, const std::string& headers, const std::string& body, int* status, std::string* response) {
  static CurlGlobal global;
  CURL* curl = curl_easy_init();
  if (!curl) return false;
  struct curl_slist* list = nullptr;
  size_t pos = 0;
  while (pos < headers.size()) {
    size_t end = headers.find("\r\n", pos);
    if (end == std::string::npos) end = headers.size();
    if (end > pos) list = curl_slist_append(list, headers.substr(pos, end - pos).c_str());
    pos = end + 2;
  }
  std::string out;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sink);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  if (!body.empty() || std::strcmp(method, "POST") == 0 || std::strcmp(method, "PUT") == 0) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  const CURLcode result = curl_easy_perform(curl);
  long code = 0;
  if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (list) curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) { host::log("slippi http: %s %s failed: %s", method, url.substr(0, 80).c_str(), curl_easy_strerror(result)); return false; }
  if (status) *status = static_cast<int>(code);
  if (response) *response = std::move(out);
  return true;
}
#endif

// GraphQL POST; returns the `data` object or null (and logs) on failure.
json graphql(const std::string& query, const json& variables) {
  json body = {{"query", query}, {"variables", variables}};
  int status = 0; std::string response;
  if (!http("POST", ENDPOINT, "Content-Type: application/json\r\n", body.dump(), &status, &response)) { host::log("slippi report: request failed (network)"); return nullptr; }
  json r = json::parse(response, nullptr, false);
  if (r.is_discarded()) { host::log("slippi report: bad response (HTTP %d): %s", status, response.substr(0, 200).c_str()); return nullptr; }
  if (r.count("errors") && r["errors"].is_array() && !r["errors"].empty()) { host::log("slippi report: server error: %s", r["errors"].dump().substr(0, 300).c_str()); return nullptr; }
  if (!r.count("data")) return nullptr;
  return r["data"];
}

// ---- gzip container with stored (uncompressed) deflate blocks: valid gzip, no zlib needed.
uint32_t crc32(const uint8_t* p, size_t n) {
  static uint32_t table[256]; static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
std::string gzip_stored(const std::string& in) {
  std::string out;
  const uint8_t header[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0x0B};
  out.append((const char*)header, 10);
  size_t pos = 0;
  do {
    size_t n = std::min<size_t>(65535, in.size() - pos);
    bool last = pos + n >= in.size();
    out.push_back(last ? 1 : 0);
    uint16_t len = (uint16_t)n, nlen = (uint16_t)~len;
    out.push_back((char)(len & 0xFF)); out.push_back((char)(len >> 8)); out.push_back((char)(nlen & 0xFF)); out.push_back((char)(nlen >> 8));
    out.append(in.data() + pos, n);
    pos += n;
  } while (pos < in.size());
  uint32_t crc = crc32((const uint8_t*)in.data(), in.size()), isize = (uint32_t)in.size();
  for (int i = 0; i < 4; ++i) out.push_back((char)((crc >> (8 * i)) & 0xFF));
  for (int i = 0; i < 4; ++i) out.push_back((char)((isize >> (8 * i)) & 0xFF));
  return out;
}

// ---- ISO MD5, cached by path, size and modification time.
#if defined(_WIN32)
std::string md5_file(const std::string& path) {
  BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) return "";
  std::string result;
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f) {
      std::vector<uint8_t> buf(4 << 20);
      size_t n;
      while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), (ULONG)n, 0);
      std::fclose(f);
      uint8_t digest[16];
      if (BCryptFinishHash(h, digest, 16, 0) >= 0) { char hex[33]; for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]); result = hex; }
    }
    BCryptDestroyHash(h);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return result;
}
std::string iso_identity_key() {
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (!GetFileAttributesExA(g_iso_path.c_str(), GetFileExInfoStandard, &fa)) return "";
  char buf[128]; std::snprintf(buf, sizeof buf, "|%llu|%llu", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
  return g_iso_path + buf;
}
#else
std::string md5_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  std::vector<uint8_t> buf(4 << 20);
  uint8_t digest[16];
  size_t n;
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CC_MD5_CTX ctx; CC_MD5_Init(&ctx);
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) CC_MD5_Update(&ctx, buf.data(), (CC_LONG)n);
  CC_MD5_Final(digest, &ctx);
#pragma clang diagnostic pop
#else
  MD5_CTX ctx; MD5_Init(&ctx);
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) MD5_Update(&ctx, buf.data(), n);
  MD5_Final(digest, &ctx);
#endif
  std::fclose(f);
  char hex[33]; for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}
std::string iso_identity_key() {
  struct stat st{};
  if (stat(g_iso_path.c_str(), &st) != 0) return "";
  char buf[128]; std::snprintf(buf, sizeof buf, "|%llu|%llu", (unsigned long long)st.st_size, (unsigned long long)st.st_mtime);
  return g_iso_path + buf;
}
#endif
void hash_iso() {
  const std::string key = iso_identity_key();
  std::string cache = g_cache_dir + "/iso_md5_cache.txt";
  { std::ifstream in(cache); std::string line; while (std::getline(in, line)) { size_t eq = line.rfind('='); if (eq != std::string::npos && line.substr(0, eq) == key) { g_iso_hash = line.substr(eq + 1); g_hash_done = true; return; } } }
  auto t0 = std::chrono::steady_clock::now();
  g_iso_hash = md5_file(g_iso_path);
  host::log("slippi report: ISO md5 %s (%.1f s)", g_iso_hash.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  if (!g_iso_hash.empty() && !key.empty()) { std::ofstream out(cache, std::ios::app); out << key << "=" << g_iso_hash << "\n"; }
  g_hash_done = true;
}

bool send_status(const StatusJob& s) {
  json vars = {{"report", {{"matchId", s.match_id}, {"fbUid", s.uid}, {"playKey", s.play_key}, {"status", s.status}}}};
  json data = graphql("mutation ($report: OnlineMatchStatusReportInput!) { reportOnlineMatchStatus (report: $report) }", vars);
  bool ok = !data.is_null() && jget(data, "reportOnlineMatchStatus", false);
  host::log("slippi report: match status '%s' for %s: %s", s.status.c_str(), s.match_id.c_str(), ok ? "accepted" : "failed");
  return ok;
}

void upload_replay(const std::string& path, const std::string& url) {
  std::ifstream in(path, std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (contents.empty()) { host::log("slippi report: no replay to upload (%s)", path.c_str()); return; }
  std::string gz = gzip_stored(contents);
  int status = 0;
  bool ok = http("PUT", url, "Content-Type: application/octet-stream\r\nContent-Encoding: gzip\r\nX-Goog-Content-Length-Range: 0,10000000\r\n", gz, &status, nullptr);
  host::log("slippi report: replay upload %s (HTTP %d, %zu bytes)", ok && status / 100 == 2 ? "done" : "failed", status, gz.size());
}

// Returns true when the report is finished (accepted or given up).
bool send_game(Job& job) {
  const GameReport& g = job.game;
  ++job.attempts;
  json players = json::array();
  for (auto& p : g.players)
    players.push_back({{"fbUid", p.uid}, {"slotType", p.slot_type}, {"damageDone", p.damage_done}, {"stocksRemaining", p.stocks_remaining},
                       {"characterId", p.character_id}, {"colorId", p.color_id}, {"startingStocks", p.starting_stocks}, {"startingPercent", p.starting_percent}});
  json payload = {{"fbUid", g.uid}, {"mode", g.online_mode}, {"players", players}, {"isoHash", g_iso_hash}, {"matchId", g.match_id}, {"playKey", g.play_key},
                  {"gameDurationFrames", g.duration_frames}, {"gameIndex", g.game_index}, {"tiebreakIndex", g.tiebreak_index}, {"winnerIdx", g.winner_index},
                  {"gameEndMethod", g.game_end_method}, {"lrasInitiator", g.lras_initiator}, {"stageId", g.stage_id}};
  json data = graphql("mutation ($report: OnlineGameReportInput!) { reportOnlineGame (report: $report) { success uploadUrl } }", {{"report", payload}});
  bool success = data.is_object() && data.count("reportOnlineGame") && data["reportOnlineGame"].is_object() && jget(data["reportOnlineGame"], "success", false);   // a null result must not crash after a game
  if (success) {
    host::log("slippi report: game %u of %s reported", g.game_index, g.match_id.c_str());
    auto& r = data["reportOnlineGame"];
    if (r.count("uploadUrl") && r["uploadUrl"].is_string() && !g.replay_path.empty()) upload_replay(g.replay_path, r["uploadUrl"].get<std::string>());
    return true;
  }
  if (job.attempts >= MAX_ATTEMPTS) { host::log("slippi report: giving up on game report for %s after %d attempts", g.match_id.c_str(), job.attempts); return true; }
  std::this_thread::sleep_for(std::chrono::milliseconds(100 * job.attempts));
  return false;
}

void worker() {
  hash_iso();
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(g_mutex);
      g_cv.wait(lk, [] { return g_quit || !g_queue.empty(); });
      if (g_queue.empty()) return;
      job = g_queue.front();
      if (g_quit) { g_queue.pop_front(); job.attempts = MAX_ATTEMPTS - 1; }   // one last attempt each on shutdown
    }
    bool done = job.is_status ? (send_status(job.status), true) : send_game(job);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_queue.empty() && !g_quit) { if (done) g_queue.pop_front(); else g_queue.front().attempts = job.attempts; }
  }
}
}  // namespace

void init(const std::string& iso_path, const std::string& cache_dir) {
  g_iso_path = iso_path; g_cache_dir = cache_dir;
  g_thread = std::thread(worker);
}

void join_rank_thread_for_shutdown();
void shutdown() {
  { std::lock_guard<std::mutex> lk(g_mutex); g_quit = true; }
  g_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
  join_rank_thread_for_shutdown();   // a joinable std::thread at static destruction would terminate the process
}

void log_game(const GameReport& report) {
  { std::lock_guard<std::mutex> lk(g_mutex); Job j; j.game = report; g_queue.push_back(std::move(j)); }
  g_cv.notify_all();
}

void match_status(const std::string& uid, const std::string& play_key, const std::string& match_id, const std::string& status, bool background) {
  StatusJob s{uid, play_key, match_id, status};
  if (!background) { send_status(s); return; }
  { std::lock_guard<std::mutex> lk(g_mutex); Job j; j.is_status = true; j.status = s; g_queue.push_back(std::move(j)); }
  g_cv.notify_all();
}

// ---------------------------------------------------------------- rank
namespace {
std::mutex g_rank_mutex;
RankInfo g_rank;
std::atomic<RankFetchStatus> g_rank_status{RankFetchStatus::Error};
std::thread g_rank_thread;

// SlippiRank::decide
int8_t decide_rank(float o, uint16_t global, uint16_t regional, uint32_t updates) {
  if (updates < 5) return 0;
  if (o <= 765.42f) return 1;
  if (o > 765.43f && o <= 913.71f) return 2;
  if (o > 913.72f && o <= 1054.86f) return 3;
  if (o > 1054.87f && o <= 1188.87f) return 4;
  if (o > 1188.88f && o <= 1315.74f) return 5;
  if (o > 1315.75f && o <= 1435.47f) return 6;
  if (o > 1435.48f && o <= 1548.06f) return 7;
  if (o > 1548.07f && o <= 1653.51f) return 8;
  if (o > 1653.52f && o <= 1751.82f) return 9;
  if (o > 1751.83f && o <= 1842.99f) return 10;
  if (o > 1843.0f && o <= 1927.02f) return 11;
  if (o > 1927.03f && o <= 2003.91f) return 12;
  if (o > 2003.92f && o <= 2073.66f) return 13;
  if (o > 2073.67f && o <= 2136.27f) return 14;
  if (o > 2136.28f && o <= 2191.74f) return 15;
  if (o >= 2191.75f && (global > 0 || regional > 0)) return 19;
  if (o > 2191.75f && o <= 2274.99f) return 16;
  if (o > 2275.0f && o <= 2350.0f) return 17;
  if (o > 2350.0f) return 18;
  return 0;
}

void join_rank_thread() { if (g_rank_thread.joinable()) g_rank_thread.join(); }
}  // namespace
void join_rank_thread_for_shutdown() { join_rank_thread(); }

void fetch_user_rank(const std::string& uid) {
  if (uid.empty()) return;
  join_rank_thread();
  g_rank_status = RankFetchStatus::Fetching;
  g_rank_thread = std::thread([uid] {
    std::string url = "https://users-rest-dot-slippi.uc.r.appspot.com/user/" + uid + "?additionalFields=chatMessages,rank";
    int status = 0; std::string response;
    if (!http("GET", url, "", "", &status, &response)) { g_rank_status = RankFetchStatus::Error; host::log("slippi rank: user fetch failed (network)"); return; }
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.count("rank") || !j["rank"].is_object()) { g_rank_status = RankFetchStatus::Error; host::log("slippi rank: user fetch HTTP %d, no rank in response", status); return; }
    auto& r = j["rank"];
    RankInfo info;
    info.rating_ordinal = jget(r, "ratingOrdinal", 0.0f);
    info.global_placing = (uint16_t)(r.count("dailyGlobalPlacement") && r["dailyGlobalPlacement"].is_number() ? r["dailyGlobalPlacement"].get<int>() : 0);
    info.regional_placing = (uint16_t)(r.count("dailyRegionalPlacement") && r["dailyRegionalPlacement"].is_number() ? r["dailyRegionalPlacement"].get<int>() : 0);
    info.rating_update_count = jget(r, "ratingUpdateCount", 0u);
    info.rank = decide_rank(info.rating_ordinal, info.global_placing, info.regional_placing, info.rating_update_count);
    { std::lock_guard<std::mutex> lk(g_rank_mutex); g_rank = info; }
    g_rank_status = RankFetchStatus::Fetched;
    host::log("slippi rank: rating %.1f, %u rated games, rank index %d", info.rating_ordinal, info.rating_update_count, info.rank);
  });
}

void fetch_match_result(const std::string& match_id, const std::string& uid, const std::string& play_key) {
  join_rank_thread();
  g_rank_status = RankFetchStatus::Fetching;
  g_rank_thread = std::thread([match_id, uid, play_key] {
    for (int attempt = 0; attempt < 3; ++attempt) {
      json vars = {{"request", {{"matchId", match_id}, {"fbUid", uid}, {"playKey", play_key}}}};
      json data = graphql("query ($request: OnlineMatchRequestInput!) { getRankedMatchPersonalResult(request: $request) { participant { ordinal dailyGlobalPlacement dailyRegionalPlacement ratingUpdateCount ratingChange } } }", vars);
      if (data.is_null() || !data.count("getRankedMatchPersonalResult") || !data["getRankedMatchPersonalResult"].is_object()) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
      auto& p = data["getRankedMatchPersonalResult"]["participant"];
      bool has_change = p.count("ratingChange") && p["ratingChange"].is_number();
      if (!has_change && attempt < 2) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
      RankInfo info;
      info.rating_ordinal = p.count("ordinal") && p["ordinal"].is_number() ? p["ordinal"].get<float>() : 0.0f;
      info.global_placing = (uint16_t)(p.count("dailyGlobalPlacement") && p["dailyGlobalPlacement"].is_number() ? p["dailyGlobalPlacement"].get<int>() : 0);
      info.regional_placing = (uint16_t)(p.count("dailyRegionalPlacement") && p["dailyRegionalPlacement"].is_number() ? p["dailyRegionalPlacement"].get<int>() : 0);
      info.rating_update_count = p.count("ratingUpdateCount") && p["ratingUpdateCount"].is_number() ? p["ratingUpdateCount"].get<uint32_t>() : 0;
      info.rating_change = has_change ? p["ratingChange"].get<float>() : 0.0f;
      int8_t prev = decide_rank(info.rating_ordinal, info.global_placing, info.regional_placing, info.rating_update_count);
      info.rating_ordinal += info.rating_change;
      info.rating_update_count += has_change ? 1 : 0;
      info.rank = decide_rank(info.rating_ordinal, info.global_placing, info.regional_placing, info.rating_update_count);
      info.rank_change = (int8_t)(info.rank - prev);
      { std::lock_guard<std::mutex> lk(g_rank_mutex); g_rank = info; }
      g_rank_status = RankFetchStatus::Fetched;
      host::log("slippi rank: match result: rating %.1f (%+.1f), rank index %d (%+d)", info.rating_ordinal, info.rating_change, info.rank, info.rank_change);
      return;
    }
    g_rank_status = RankFetchStatus::Error;
    host::log("slippi rank: match result fetch failed");
  });
}

RankFetchStatus rank_info(RankInfo* out) {
  if (out) { std::lock_guard<std::mutex> lk(g_rank_mutex); *out = g_rank; }
  return g_rank_status.load();
}
}  // namespace slippi::report
#endif  // reporting is deliberately absent from the diagnostic target
