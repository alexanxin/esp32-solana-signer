#ifdef BLEND_CYD
#include "dash_net.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <string.h>
#include <strings.h>  // strncasecmp
#include <time.h>

#include "activity.h"
#include "cert_bundle.h"
#include "dash_store.h"

namespace {

// Sigly is devnet-only for now, so balances come from devnet. Prices are always the real
// (mainnet) ones: devnet SOL is valued at what SOL costs, to show how the screen will look.
const char* kPublicRpc = "https://api.devnet.solana.com";
const bool kMainnet = false;
const char* kPrices = "https://lite-api.jup.ag";  // Jupiter Price API, free, no key
const char* kCharts = "https://api.geckoterminal.com";  // price history, free, no key
const char* kTokenProgram = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
const char* kToken2022Program = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb";

constexpr uint32_t kRetryMs = 30000;         // sooner after a failure
constexpr uint32_t kSaveEveryS = 15 * 60;    // flash copy at most every 15 min (wear)
constexpr uint32_t kJoinTimeoutMs = 20000;

char gAddress[45];
DashSetup gSetup;
volatile NetState gState = NetState::kOff;
volatile bool gRefreshNow = false;

// The background task writes these, the screen reads them. A mutex ("mutual exclusion")
// makes sure the screen never copies a portfolio the task is halfway through writing.
SemaphoreHandle_t gLock = nullptr;
Portfolio gShared;
uint32_t gSharedVersion = 0;
uint32_t gSharedAt = 0;
Activity* gSharedAct = nullptr;  // on the heap (made in dashNetBegin): DRAM is nearly full
uint32_t gSharedActVersion = 0;
// The chart: the screen writes a request, the task answers it (both under gLock).
Chart* gChartAsk = nullptr;     // what the screen wants (mint + range); on the heap
volatile bool gChartWanted = false;
Chart* gSharedChart = nullptr;  // the latest answer
uint32_t gSharedChartVersion = 0;

uint32_t unixNow() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;  // before 2023 = the clock isn't set yet
}

// Arduino's Stream waits for slow data by spinning in a tight loop ("is it here yet? is it
// here yet?") without ever pausing. On core 0 that starves the idle task, and after 5 s the
// task watchdog decides the chip froze and restarts it: the restart loop after joining
// Wi-Fi. This wrapper waits politely instead: when no byte is there yet it sleeps 5 ms
// (delay() lets other tasks run), and gives up on a timeout or a closed connection.
//
// It also knows where the answer ends, so the rest can be skipped (drain) and the connection
// is clean for the next question. A server says where it ends in one of two ways:
//   - "Content-Length: 1234": exactly 1234 bytes follow;
//   - "Transfer-Encoding: chunked": the answer comes in pieces, each one starting with its
//     size in hex on its own line ("4d2\r\n" + 1234 bytes + "\r\n"), and a piece of size 0
//     ends it. The server picks this when it starts sending before it knows the total.
class PatientStream : public Stream {
 public:
  // size: Content-Length, or -1 when not told. chunked: the pieces format above.
  PatientStream(WiFiClient& c, uint32_t timeoutMs, int size, bool chunked = false)
      : c_(c), timeout_(timeoutMs), size_(size), chunked_(chunked) {}
  int available() override { return c_.available(); }
  int read() override {
    if (!more()) return -1;
    took(1);
    return c_.read();
  }
  int peek() override { return more() ? c_.peek() : -1; }
  size_t readBytes(char* buf, size_t len) {  // ArduinoJson reads through this one
    size_t got = 0;
    while (got < len && more()) {
      size_t want = len - got;
      if (chunked_ && want > (size_t)chunkLeft_) want = chunkLeft_;
      if (!chunked_ && size_ >= 0 && want > (size_t)(size_ - used_)) want = size_ - used_;
      int n = c_.read((uint8_t*)buf + got, want);
      if (n > 0) {
        got += n;
        took(n);
      }
    }
    return got;
  }
  size_t write(uint8_t) override { return 0; }  // read-only
  // Reads and drops what's left of the answer. False if its end isn't known (then the
  // server closes the connection after it, and it can't be used again).
  bool drain() {
    if (!chunked_ && size_ < 0) return false;
    char junk[64];
    while (more() && readBytes(junk, sizeof junk) > 0) {
    }
    return chunked_ ? done_ : used_ >= size_;
  }

 private:
  // Is there another byte of the answer (waiting for it if needed)?
  bool more() {
    if (chunked_) {
      if (done_) return false;
      if (chunkLeft_ == 0 && !nextChunk()) return false;
    } else if (size_ >= 0 && used_ >= size_) {
      return false;
    }
    return waitForData();
  }
  void took(int n) {
    used_ += n;
    if (chunked_) chunkLeft_ -= n;
  }
  int rawRead() { return waitForData() ? c_.read() : -1; }
  bool rawLine(char* buf, size_t cap) {
    size_t n = 0;
    for (;;) {
      int ch = rawRead();
      if (ch < 0) return false;
      if (ch == '\n') break;
      if (ch != '\r' && n + 1 < cap) buf[n++] = (char)ch;
    }
    buf[n] = '\0';
    return true;
  }
  // Reads the next piece's size line (after the previous piece's closing "\r\n").
  bool nextChunk() {
    char line[24];
    if (started_ && !rawLine(line, sizeof line)) return fail();  // the "\r\n" after a piece
    started_ = true;
    if (!rawLine(line, sizeof line)) return fail();
    chunkLeft_ = (int)strtol(line, nullptr, 16);
    if (chunkLeft_ > 0) return true;
    while (rawLine(line, sizeof line) && line[0] != '\0') {  // optional trailer lines
    }
    done_ = true;
    return false;
  }
  bool fail() {
    chunked_ = false;  // can't tell where it ends any more: drain() says "not clean"
    size_ = -1;
    return false;
  }
  bool waitForData() {
    uint32_t start = millis();
    while (c_.available() <= 0) {
      if (!c_.connected() || millis() - start > timeout_) return false;
      delay(5);
    }
    return true;
  }
  WiFiClient& c_;
  uint32_t timeout_;
  int size_;
  bool chunked_;
  int used_ = 0;
  int chunkLeft_ = 0;
  bool started_ = false;
  bool done_ = false;
};

// The encrypted connection, kept open between questions to the same server. Opening one
// (the TLS "handshake") is the slow part on this chip: about 15 s to Helius, 2 s to Jupiter,
// against 0.2 s for the question itself. So a refresh opens it once and asks everything
// through it ("keep-alive"), then closes it (servers drop idle connections anyway).
WiFiClientSecure* gTls = nullptr;
char gTlsHost[64] = "";

void closeConnection() {
  if (gTls != nullptr) gTls->stop();
  gTlsHost[0] = '\0';
}

// Opens the connection to `host` unless it's already open. Returns false if it can't.
bool openConnection(const char* host) {
  if (gTls == nullptr) {
    gTls = new WiFiClientSecure();
    gTls->setCACertBundle(kCertBundle);  // check the certificate against Mozilla's roots
  }
  if (strcmp(gTlsHost, host) == 0 && gTls->connected()) return true;
  closeConnection();
  uint32_t t0 = millis();
  // The handshake is heavy maths that never pauses. At our usual lowest priority it shares
  // core 0 with the idle task, so it gets about half the time; here it gets it all, and the
  // idle task's watchdog is paused meanwhile (Wi-Fi itself runs at a higher priority still,
  // so nothing that matters waits). Both go back right after.
  disableCore0WDT();
  vTaskPrioritySet(nullptr, 1);
  bool ok = gTls->connect(host, 443);
  vTaskPrioritySet(nullptr, tskIDLE_PRIORITY);
  enableCore0WDT();
  Serial.printf("dash: %s %s after %lu ms (handshake)\n", host, ok ? "connected" : "FAILED",
                millis() - t0);
  if (ok) snprintf(gTlsHost, sizeof gTlsHost, "%s", host);
  return ok;
}

// Does `s` contain `word`, ignoring upper/lower case? ("Connection: Close" vs "close")
bool hasWord(const char* s, const char* word) {
  size_t n = strlen(word);
  for (; *s; s++) {
    if (strncasecmp(s, word, n) == 0) return true;
  }
  return false;
}

// One header line of an HTTP answer, without its "\r\n". False on a timeout or a closed line.
bool readLine(PatientStream& in, char* buf, size_t cap) {
  size_t n = 0;
  for (;;) {
    int c = in.read();
    if (c < 0) return false;
    if (c == '\n') break;
    if (c != '\r' && n + 1 < cap) buf[n++] = (char)c;
  }
  buf[n] = '\0';
  return true;
}

// One HTTPS request; the answer is read straight from the network into `doc` through
// `filter` (nullptr = keep everything). body == nullptr means GET, else POST JSON.
//
// We write the request and read the answer's headers ourselves instead of using Arduino's
// HTTPClient: HTTPClient closes the connection whenever it is done (its destructor calls
// stop()), and it would open a second one on its own at our lowest priority, which cost
// another ~16 s per request. HTTP itself is only a few lines of text:
//   POST /path HTTP/1.1          <- what and where
//   Host: devnet.helius-rpc.com  <- which website on that server
//   Content-Length: 87           <- how many bytes of question follow
//   (empty line, then the JSON question)
// and the answer starts the same way: "HTTP/1.1 200 OK", headers, empty line, the JSON.
bool fetchJson(const char* url, const char* body, JsonDocument& doc, JsonDocument* filter) {
  // Log the address without its "?..." part: an RPC URL can carry an API key there.
  const char* q = strchr(url, '?');
  int shown = q != nullptr ? (int)(q - url) : 60;
  char host[64];
  const char* h = strstr(url, "://");
  h = h != nullptr ? h + 3 : url;
  size_t hl = strcspn(h, "/?:");
  snprintf(host, sizeof host, "%.*s", (int)(hl < sizeof host ? hl : sizeof host - 1), h);
  const char* path = h + hl;
  if (*path != '/') path = "/";

  // Two tries: a kept connection the server has quietly closed fails at once; then open a
  // fresh one and ask again.
  for (int attempt = 0; attempt < 2; attempt++) {
    bool reused = strcmp(gTlsHost, host) == 0 && gTls != nullptr && gTls->connected();
    if (!openConnection(host)) return false;
    uint32_t t0 = millis();
    WiFiClientSecure& c = *gTls;
    char line[160];
    // HTTP/1.1 keeps the line open by default. (With HTTP/1.0 some answers came without a
    // length, and the server then hangs up after them: three handshakes per refresh.)
    c.print(body != nullptr ? "POST " : "GET ");
    c.print(path);
    snprintf(line, sizeof line,
             " HTTP/1.1\r\nHost: %s\r\nUser-Agent: Sigly\r\nAccept: application/json\r\n"
             "Connection: keep-alive\r\n",
             host);
    c.print(line);
    if (body != nullptr) {
      snprintf(line, sizeof line, "Content-Type: application/json\r\nContent-Length: %u\r\n",
               (unsigned)strlen(body));
      c.print(line);
    }
    size_t sent = c.print("\r\n");
    if (body != nullptr) sent = c.print(body);

    PatientStream head(c, 10000, -1);
    if (sent == 0 || !readLine(head, line, sizeof line)) {
      closeConnection();
      if (reused && attempt == 0) continue;  // the kept line was dead: try a fresh one
      Serial.printf("dash: %.*s: no answer\n", shown, url);
      return false;
    }
    uint32_t tWait = millis();
    const char* sp = strchr(line, ' ');
    int code = sp != nullptr ? atoi(sp + 1) : 0;
    int length = -1;
    bool keep = true, chunked = false;
    while (readLine(head, line, sizeof line) && line[0] != '\0') {  // empty line = body next
      if (strncasecmp(line, "Content-Length:", 15) == 0) length = atoi(line + 15);
      if (strncasecmp(line, "Connection:", 11) == 0 && hasWord(line, "close")) keep = false;
      if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && hasWord(line, "chunked")) {
        chunked = true;
      }
    }

    bool ok = false, clean = false;
    PatientStream in(c, 10000, length, chunked);
    if (code == 200) {
      DeserializationError err =
          filter != nullptr ? deserializeJson(doc, in, DeserializationOption::Filter(*filter))
                            : deserializeJson(doc, in);
      ok = !err;
      if (err) Serial.printf("dash: bad JSON from %.*s: %s\n", shown, url, err.c_str());
    } else {
      Serial.printf("dash: %.*s -> HTTP %d\n", shown, url, code);
    }
    // Skip what's left, so the next answer starts clean; without a length (or with
    // "Connection: close") the server hangs up after this answer.
    clean = keep && code > 0 && in.drain();
    if (!clean) closeConnection();
    Serial.printf("dash: %.*s %s: wait %lu + read %lu ms, %s\n", shown, url,
                  ok ? "ok" : "FAILED", tWait - t0, millis() - tWait,
                  clean ? "kept open"
                        : !keep ? "server closes"
                                : (length < 0 && !chunked) ? "no length, closed" : "closed");
    return ok;
  }
  return false;
}

const char* rpcUrl() { return gSetup.rpcUrl[0] != '\0' ? gSetup.rpcUrl : kPublicRpc; }

bool rpcError(JsonDocument& doc, const char* what) {
  if (doc["error"].isNull()) return false;
  Serial.printf("dash: RPC %s error: %s\n", what, doc["error"]["message"] | "?");
  return true;
}

// The Activity tab's list. First the newest signatures (a small answer, every refresh); only
// when one is new, ONE batch request asks getTransaction for all the new ones. `act` is the
// list so far, `next` scratch space, `body` room for the request (kActBodySize).
// Returns 1 when the list changed, 0 when it didn't, -1 when the RPC didn't answer.
constexpr size_t kActBodySize = 1700;  // 6 getTransaction questions of ~200 characters
int refreshActivity(Activity* act, Activity* next, char* body) {
  JsonDocument doc;
  activitySignaturesBody(gAddress, body, kActBodySize);
  if (!fetchJson(rpcUrl(), body, doc, nullptr) || rpcError(doc, "getSignaturesForAddress")) {
    return -1;
  }
  int pending = activityFromSignatures(doc["result"], *act, next);
  if (pending < 0) return -1;
  if (pending > 0) {
    activityDetailsBody(*next, body, kActBodySize);
    JsonDocument filter;
    activityDetailsFilter(filter);
    // If this fails, the new ones stay "pending" (shown with their time only) and are
    // asked again next refresh.
    if (fetchJson(rpcUrl(), body, doc, &filter)) {
      activityApplyDetails(doc.as<JsonVariantConst>(), gAddress, next);
    }
  }
  bool changed = memcmp(next, act, sizeof *act) != 0;
  *act = *next;
  return changed ? 1 : 0;
}

// One full refresh. Returns the state to show. `last` is the previous result (or nullptr):
// its prices fill in when the price request fails.
// Order: everything from the Solana RPC first (balances, then Activity), through one kept
// connection; then Jupiter. Each server's slow handshake happens once per refresh.
// *actResult: what refreshActivity said (1 changed, 0 same, -1 failed).
NetState refresh(Portfolio* p, const Portfolio* last, Activity* act, Activity* actNext,
                 char* actBody, int* actResult) {
  static char body[256];
  // 1 + 5 watchlist + up to 24 held mints of 44 characters. On the heap, made once: the
  // chip's fixed data area (DRAM) is nearly full.
  constexpr size_t kPathSize = 1500;
  static char* path = new char[kPathSize];
  JsonDocument doc;

  snprintf(body, sizeof body,
           R"({"jsonrpc":"2.0","id":1,"method":"getBalance","params":["%s"]})", gAddress);
  if (!fetchJson(rpcUrl(), body, doc, nullptr) || rpcError(doc, "getBalance") ||
      !portfolioStart(doc["result"], p)) {
    return NetState::kRpcError;
  }

  JsonDocument filter;
  tokenAccountsFilter(filter);
  for (const char* program : {kTokenProgram, kToken2022Program}) {
    snprintf(body, sizeof body,
             R"({"jsonrpc":"2.0","id":2,"method":"getTokenAccountsByOwner","params":["%s",)"
             R"({"programId":"%s"},{"encoding":"jsonParsed"}]})",
             gAddress, program);
    if (!fetchJson(rpcUrl(), body, doc, &filter) || rpcError(doc, "getTokenAccountsByOwner") ||
        !portfolioAddTokens(doc["result"], p)) {
      return NetState::kRpcError;
    }
  }

  uint32_t ta = millis();
  *actResult = refreshActivity(act, actNext, actBody);
  Serial.printf("dash: activity %u transactions in %lu ms\n", act->count, millis() - ta);

  // One request prices SOL and every token. Devnet token mints aren't the real ones, so
  // Jupiter doesn't know them: on devnet only SOL (at the real SOL price) and devnet USDC
  // ($1) get a value.
  NetState state = NetState::kOk;
  int n = snprintf(path, kPathSize, "%s", kPrices);
  pricePath(*p, path + n, kPathSize - n);
  if (fetchJson(path, nullptr, doc, nullptr)) {
    portfolioApplyPrices(doc.as<JsonVariantConst>(), p);
  } else {
    state = NetState::kPriceError;
    if (last != nullptr) portfolioCarryPrices(*last, p);
  }
  portfolioFinish(p, kMainnet, gSetup.hideUnder);
  closeConnection();  // idle until the next refresh: the server would drop it anyway
  return state;
}

void publishActivity(const Activity& a) {
  xSemaphoreTake(gLock, portMAX_DELAY);
  *gSharedAct = a;
  gSharedActVersion++;
  xSemaphoreGive(gLock);
}

void publish(const Portfolio& p, uint32_t at) {
  xSemaphoreTake(gLock, portMAX_DELAY);
  gShared = p;
  gSharedAt = at;
  gSharedVersion++;
  xSemaphoreGive(gLock);
}

// Answers the screen's chart request: finds the token's busiest pool (remembered for the
// next range of the same token), then asks for its prices. Two GeckoTerminal requests
// through one connection, closed afterwards. `pool` is the task's memory of the last pool.
void serveChart(Chart* c, char* pool, char* poolMint) {
  xSemaphoreTake(gLock, portMAX_DELAY);
  *c = *gChartAsk;
  gChartWanted = false;
  xSemaphoreGive(gLock);
  c->ok = false;
  c->count = 0;
  uint32_t t0 = millis();
  char path[200];
  int n = snprintf(path, sizeof path, "%s", kCharts);
  JsonDocument filter, doc;
  bool havePool = strcmp(poolMint, c->mint) == 0 && pool[0] != '\0';
  if (!havePool) {
    chartTokenPath(c->mint, path + n, sizeof path - n);
    chartTokenFilter(filter);
    havePool = fetchJson(path, nullptr, doc, &filter) &&
               chartPoolFromToken(doc.as<JsonVariantConst>(), pool, 48);
    if (havePool) snprintf(poolMint, 45, "%s", c->mint);
  }
  if (havePool) {
    chartOhlcvPath(pool, c->mint, c->range, path + n, sizeof path - n);
    chartOhlcvFilter(filter);
    if (fetchJson(path, nullptr, doc, &filter)) chartFromOhlcv(doc.as<JsonVariantConst>(), c);
  }
  closeConnection();
  Serial.printf("dash: chart %s, %u prices in %lu ms\n", c->ok ? "ok" : "not available",
                c->count, millis() - t0);
  xSemaphoreTake(gLock, portMAX_DELAY);
  *gSharedChart = *c;
  gSharedChartVersion++;
  xSemaphoreGive(gLock);
}

// The background task: join Wi-Fi, refresh, wait, repeat.
void netTask(void*) {
  // Two portfolios of ~2 KB each: too big for this task's stack, and as "static" they no
  // longer fit the chip's fixed-size data area (DRAM). So they live on the heap, made
  // once here and kept for as long as the Dashboard runs.
  Portfolio& p = *new Portfolio();     // the refresh being built
  Portfolio& last = *new Portfolio();  // the previous good result (or the cached copy)
  uint32_t lastAt = 0;
  bool haveLast = dashLoadPortfolio(&last, &lastAt);
  uint32_t lastSaved = 0;
  // The Activity list (~1.2 KB each) the same way: built here, cached in flash.
  Activity& act = *new Activity();
  Activity& actNext = *new Activity();
  char* actBody = new char[kActBodySize];
  uint32_t actAt = 0;
  if (!dashLoadActivity(&act, &actAt)) memset(&act, 0, sizeof act);
  bool actFetched = false, actUnsaved = false;
  uint32_t actSaved = 0;
  Chart& chart = *new Chart();
  char* pool = new char[48];
  char* poolMint = new char[45];
  pool[0] = poolMint[0] = '\0';
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      gState = NetState::kConnecting;
      uint32_t start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < kJoinTimeoutMs) delay(250);
      if (WiFi.status() != WL_CONNECTED) {
        gState = NetState::kNoWifi;
        WiFi.reconnect();
        for (uint32_t t = 0; t < kRetryMs && !gRefreshNow; t += 250) delay(250);
        gRefreshNow = false;
        continue;
      }
      Serial.printf("dash: Wi-Fi joined, IP %s\n", WiFi.localIP().toString().c_str());
    }

    gState = NetState::kFetching;
    uint32_t t0 = millis();
    int actResult = -1;
    NetState result = refresh(&p, haveLast ? &last : nullptr, &act, &actNext, actBody, &actResult);
    if (result == NetState::kRpcError) closeConnection();
    Serial.printf("dash: refresh %s in %lu ms, %u tokens, $%.2f, free heap %u\n",
                  result == NetState::kOk ? "ok" : "failed", millis() - t0, p.count,
                  p.totalUsd, (unsigned)ESP.getFreeHeap());
    if (result != NetState::kRpcError) {  // balances are right even if prices are missing
      uint32_t at = unixNow();
      publish(p, at);
      last = p;
      haveLast = true;
      // The cache in flash: what the screen shows right after the next start, before Wi-Fi
      // is up. Saved on the first refresh, then at most every 15 min (flash wears out with
      // too many writes). With missing prices too: balances are fresh, prices carried over.
      if (lastSaved == 0 || at - lastSaved >= kSaveEveryS) {
        dashSavePortfolio(p, at);
        lastSaved = at;
      }
      if (actResult == 1 || (actResult == 0 && !actFetched)) {
        actFetched = true;
        actUnsaved = true;
        publishActivity(act);
      }
      if (actUnsaved && (actSaved == 0 || at - actSaved >= kSaveEveryS)) {
        dashSaveActivity(act, at);
        actSaved = at;
        actUnsaved = false;
      }
    }
    gState = result;
    // Every 1, 2 or 5 minutes (setup page); sooner after a failure.
    uint32_t wait = result == NetState::kOk ? gSetup.refreshMin * 60000u : kRetryMs;
    for (uint32_t t = 0; t < wait && !gRefreshNow; t += 250) {
      // A chart asked meanwhile is answered right away (it doesn't move the next refresh).
      if (gChartWanted && WiFi.status() == WL_CONNECTED) serveChart(&chart, pool, poolMint);
      delay(250);
    }
    gRefreshNow = false;
  }
}

// ---- Setup network -----------------------------------------------------------------------

DNSServer* gDns = nullptr;
WebServer* gWeb = nullptr;
bool gSaved = false;
String gScan;  // <option> list of nearby networks, made before the setup network opens

// Network names come from strangers' routers: escape them before putting them in HTML,
// or a network called "<script>..." would run code on the setup page.
String html(const char* s) {
  String out;
  for (; *s; s++) {
    switch (*s) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += *s;
    }
  }
  return out;
}

struct Zone {
  const char* label;
  const char* rule;
};
const Zone kZones[] = {
    {"Central Europe (Skopje, Berlin, Paris)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"UK, Ireland, Portugal", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Eastern Europe (Athens, Sofia, Kyiv)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Turkey, Moscow", "MSK-3"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"India", "IST-5:30"},
    {"Singapore, Hong Kong", "SGT-8"},
    {"Japan, Korea", "JST-9"},
    {"UTC", "UTC0"},
};

void sendForm() {
  String page;
  page.reserve(9000);  // the six coin lists make it about 7 KB
  page += F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Sigly setup</title><style>"
            "body{background:#0E1116;color:#EEF1F5;font:16px system-ui,sans-serif;margin:0;padding:20px}"
            "h1{font-size:22px;margin:0 0 4px}p{color:#A3ADBA;margin:0 0 18px;line-height:1.4}"
            "label{display:block;font-weight:600;margin:16px 0 6px}"
            "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;"
            "border:0;background:#181D25;color:#EEF1F5;font-size:16px}"
            "button{margin-top:24px;width:100%;padding:14px;border:0;border-radius:24px;"
            "background:#C7F284;color:#0E1116;font-size:16px;font-weight:700}"
            "small{color:#A3ADBA}h2{font-size:18px;margin:28px 0 0}"
            ".row{display:flex;gap:10px;align-items:center;margin:6px 0}"
            ".row span{color:#A3ADBA;width:16px}</style>"
            "<h1>Set up Sigly's Dashboard</h1>"
            "<p>The Dashboard uses Wi-Fi to show your balances and prices. It never unlocks "
            "your keys: signing only happens in Signer mode, with Wi-Fi off.</p>"
            "<form method=post action=/save>"
            "<label for=ssid>Wi-Fi network</label><input id=ssid name=ssid list=nets "
            "maxlength=32 required autocomplete=off value='");
  page += html(gSetup.ssid);
  page += F("'><datalist id=nets>");
  page += gScan;
  page += F("</datalist><label for=pass>Wi-Fi password</label>"
            "<input id=pass name=pass type=password maxlength=63 placeholder='");
  page += gSetup.pass[0] ? F("(unchanged)") : F("");
  page += F("'><label for=tz>Time zone</label><select id=tz name=tz>");
  for (const Zone& z : kZones) {
    page += "<option value='";
    page += z.rule;
    page += strcmp(z.rule, gSetup.tz) == 0 ? "' selected>" : "'>";
    page += z.label;
    page += "</option>";
  }
  page += F("</select><label for=rpc>Your own Solana RPC <small>(optional)</small></label>"
            "<input id=rpc name=rpc type=url maxlength=127 "
            "placeholder='empty = free public RPC' value='");
  page += html(gSetup.rpcUrl);
  page += F("'><small>For example a Helius or Triton URL, if the public one is slow.</small>"
            "<h2>Dashboard</h2>"
            "<label>Market coins <small>(in this order)</small></label>");
  // Six drop-downs, one per place on the Market grid: picking the coin for each place is
  // also how you order them. The names come from Sigly's own list (no typing, no lookalikes).
  char current[80];
  portfolioWatchlist(current, sizeof current);
  const char* cur = current;
  for (int slot = 0; slot < kWatchCount; slot++) {
    const char* comma = strchr(cur, ',');
    size_t len = comma != nullptr ? (size_t)(comma - cur) : strlen(cur);
    page += "<div class=row><span>";
    page += slot + 1;
    page += "</span><select name=w";
    page += slot;
    page += ">";
    for (int c = 0; c < watchChoiceCount(); c++) {
      const char* sym = watchChoice(c);
      bool on = strlen(sym) == len && strncmp(sym, cur, len) == 0;
      page += on ? "<option selected>" : "<option>";
      page += sym;
      page += "</option>";
    }
    page += "</select></div>";
    cur = comma != nullptr ? comma + 1 : cur + len;
  }
  auto choice = [&](const char* name, const char* label, int value, const int* values,
                    const char* const* labels, int n) {
    page += "<label for=";
    page += name;
    page += ">";
    page += label;
    page += "</label><select id=";
    page += name;
    page += " name=";
    page += name;
    page += ">";
    for (int i = 0; i < n; i++) {
      page += "<option value=";
      page += values[i];
      page += values[i] == value ? " selected>" : ">";
      page += labels[i];
      page += "</option>";
    }
    page += "</select>";
  };
  static const int kCur[] = {0, 1};
  static const char* const kCurL[] = {"US dollar ($)", "Euro (\xE2\x82\xAC)"};
  choice("cur", "Currency", gSetup.euro ? 1 : 0, kCur, kCurL, 2);
  static const int kHide[] = {0, 1, 10};
  static const char* const kHideL[] = {"Show all", "Under $1", "Under $10"};
  choice("hide", "Hide small balances", gSetup.hideUnder, kHide, kHideL, 3);
  static const int kEvery[] = {1, 2, 5};
  static const char* const kEveryL[] = {"Every minute", "Every 2 minutes", "Every 5 minutes"};
  choice("every", "Refresh", gSetup.refreshMin, kEvery, kEveryL, 3);
  static const int kAcct[] = {1, 2, 3};
  static const char* const kAcctL[] = {"Account 1", "Account 2", "Account 3"};
  choice("acct", "Show account", gSetup.account, kAcct, kAcctL, 3);
  page += F("<small>The same accounts Phantom and Jupiter show. Unlock the Signer once after "
            "updating, so Sigly knows the addresses of accounts 2 and 3.</small>"
            "<button>Save and restart</button></form>");
  gWeb->send(200, "text/html", page);
}

void handleSave() {
  String ssid = gWeb->arg("ssid"), pass = gWeb->arg("pass"), tz = gWeb->arg("tz"),
         rpc = gWeb->arg("rpc");
  ssid.trim();
  rpc.trim();
  bool rpcOk = rpc.length() == 0 || rpc.startsWith("https://");
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63 || tz.length() >= 48 ||
      rpc.length() >= 128 || !rpcOk) {
    gWeb->send(400, "text/plain",
               rpcOk ? "Please check the fields and try again."
                     : "The RPC address must start with https://");
    return;
  }
  bool sameNetwork = ssid == gSetup.ssid;
  snprintf(gSetup.ssid, sizeof gSetup.ssid, "%s", ssid.c_str());
  if (pass.length() > 0 || !sameNetwork) {  // empty = keep the old password, same network
    snprintf(gSetup.pass, sizeof gSetup.pass, "%s", pass.c_str());
  }
  snprintf(gSetup.tz, sizeof gSetup.tz, "%s", tz.c_str());
  snprintf(gSetup.rpcUrl, sizeof gSetup.rpcUrl, "%s", rpc.c_str());
  // The Dashboard settings. Each value is checked: a form can be sent with anything in it.
  String watch;
  for (int slot = 0; slot < kWatchCount; slot++) {
    if (slot > 0) watch += ",";
    watch += gWeb->arg(String("w") + slot);
  }
  portfolioSetWatchlist(watch.c_str());  // keeps the known names only, fills up to 6
  portfolioWatchlist(gSetup.watch, sizeof gSetup.watch);
  gSetup.euro = gWeb->arg("cur") == "1";
  int hide = gWeb->arg("hide").toInt();
  gSetup.hideUnder = hide == 1 || hide == 10 ? hide : 0;
  int every = gWeb->arg("every").toInt();
  gSetup.refreshMin = every == 1 || every == 5 ? every : 2;
  int acct = gWeb->arg("acct").toInt();
  if (acct < 1 || acct > kDashAccounts) acct = 1;
  if (acct != gSetup.account) dashForgetCache();  // those numbers were another account's
  gSetup.account = acct;
  dashSaveSetup(gSetup);
  gWeb->send(200, "text/html",
             "<!doctype html><meta name=viewport content='width=device-width'>"
             "<body style='background:#0E1116;color:#EEF1F5;font:18px system-ui;padding:20px'>"
             "Saved. Sigly restarts and joins your Wi-Fi. You can close this page.");
  gSaved = true;
}

}  // namespace

void dashNetBegin(const char* address) {
  snprintf(gAddress, sizeof gAddress, "%s", address);
  dashLoadSetup(&gSetup);
  gLock = xSemaphoreCreateMutex();
  gChartAsk = new Chart();
  gSharedChart = new Chart();
  memset(gChartAsk, 0, sizeof *gChartAsk);
  memset(gSharedChart, 0, sizeof *gSharedChart);
  gSharedAct = new Activity();
  memset(gSharedAct, 0, sizeof *gSharedAct);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(gSetup.ssid, gSetup.pass);
  configTzTime(gSetup.tz, "pool.ntp.org", "time.google.com");  // the clock, once online
  gState = NetState::kConnecting;
  // Core 0 (the screen runs on core 1). 12 KB stack: HTTPS + JSON need room.
  // Priority 0 = the same as core 0's idle task. The watchdog restarts the chip when the
  // idle task gets no turn for 5 s; at equal priority the two take turns every 1 ms tick,
  // so even a library that waits in a busy loop (Arduino's Stream, HTTPClient's header
  // reading, a big JSON answer) can never starve it. Fetching a few balances in the
  // background doesn't need to be fast, only to never block anything.
  xTaskCreatePinnedToCore(netTask, "dashnet", 12288, nullptr, tskIDLE_PRIORITY, nullptr, 0);
}

NetState dashNetState() { return gState; }
bool dashNetConnected() { return WiFi.status() == WL_CONNECTED; }
void dashNetRefreshNow() { gRefreshNow = true; }

uint32_t dashNetTakePortfolio(uint32_t seenVersion, Portfolio* out, uint32_t* fetchedAt) {
  if (gLock == nullptr) return seenVersion;
  xSemaphoreTake(gLock, portMAX_DELAY);
  uint32_t v = gSharedVersion;
  if (v != seenVersion) {
    *out = gShared;
    *fetchedAt = gSharedAt;
  }
  xSemaphoreGive(gLock);
  return v;
}

uint32_t dashNetTakeActivity(uint32_t seenVersion, Activity* out) {
  if (gLock == nullptr) return seenVersion;
  xSemaphoreTake(gLock, portMAX_DELAY);
  uint32_t v = gSharedActVersion;
  if (v != seenVersion) *out = *gSharedAct;
  xSemaphoreGive(gLock);
  return v;
}

void dashNetRequestChart(const char* mint, ChartRange range) {
  if (gLock == nullptr) return;
  xSemaphoreTake(gLock, portMAX_DELAY);
  snprintf(gChartAsk->mint, sizeof gChartAsk->mint, "%s", mint);
  gChartAsk->range = range;
  gChartWanted = true;
  xSemaphoreGive(gLock);
}

uint32_t dashNetTakeChart(uint32_t seenVersion, Chart* out) {
  if (gLock == nullptr) return seenVersion;
  xSemaphoreTake(gLock, portMAX_DELAY);
  uint32_t v = gSharedChartVersion;
  if (v != seenVersion) *out = *gSharedChart;
  xSemaphoreGive(gLock);
  return v;
}

void dashPortalBegin(char ssid[16], char pass[12]) {
  dashLoadSetup(&gSetup);
  // Nearby networks for the form's suggestions. Scanning needs the station side on; the
  // setup network then opens on the same radio.
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 15; i++) {
    gScan += "<option value='";
    gScan += html(WiFi.SSID(i).c_str());
    gScan += "'>";
  }
  WiFi.scanDelete();

  // A fresh password every time, from letters that can't be mixed up (no 0/O, 1/l/I).
  const char* letters = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 10; i++) pass[i] = letters[esp_random() % 31];
  pass[10] = '\0';
  snprintf(ssid, 16, "Sigly-setup");
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid, pass);

  // "Captive portal": answer every name lookup with our own address, so the phone's
  // "sign in to network" page opens the form by itself.
  gDns = new DNSServer();
  gDns->start(53, "*", WiFi.softAPIP());
  gWeb = new WebServer(80);
  gWeb->on("/", HTTP_GET, sendForm);
  gWeb->on("/save", HTTP_POST, handleSave);
  gWeb->onNotFound([]() {  // Android/iPhone "is there internet?" checks land here
    gWeb->sendHeader("Location", "http://192.168.4.1/", true);
    gWeb->send(302, "text/plain", "");
  });
  gWeb->begin();
  Serial.printf("dash: setup network %s open\n", ssid);  // password only on the screen
}

bool dashPortalService() {
  if (gWeb == nullptr) return false;
  gDns->processNextRequest();
  gWeb->handleClient();
  return gSaved;
}
#endif  // BLEND_CYD
