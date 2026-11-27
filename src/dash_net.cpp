#ifdef BLEND_CYD
#include "dash_net.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <time.h>

#include "cert_bundle.h"
#include "dash_store.h"

namespace {

// Sigly is devnet-only for now, so balances come from devnet. Prices are always the real
// (mainnet) ones: devnet SOL is valued at what SOL costs, to show how the screen will look.
const char* kPublicRpc = "https://api.devnet.solana.com";
const bool kMainnet = false;
const char* kCoinGecko = "https://api.coingecko.com";
const char* kTokenProgram = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
const char* kToken2022Program = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb";

constexpr uint32_t kRefreshMs = 120000;      // every 2 minutes
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

uint32_t unixNow() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;  // before 2023 = the clock isn't set yet
}

// Arduino's Stream waits for slow data by spinning in a tight loop ("is it here yet? is it
// here yet?") without ever pausing. On core 0 that starves the idle task, and after 5 s the
// task watchdog decides the chip froze and restarts it: the restart loop after joining
// Wi-Fi. This wrapper waits politely instead: when no byte is there yet it sleeps 5 ms
// (delay() lets other tasks run), and gives up on a timeout or a closed connection.
class PatientStream : public Stream {
 public:
  PatientStream(WiFiClient& c, uint32_t timeoutMs) : c_(c), timeout_(timeoutMs) {}
  int available() override { return c_.available(); }
  int read() override { return waitForData() ? c_.read() : -1; }
  int peek() override { return waitForData() ? c_.peek() : -1; }
  size_t readBytes(char* buf, size_t len) {  // ArduinoJson reads through this one
    size_t got = 0;
    while (got < len && waitForData()) {
      int n = c_.read((uint8_t*)buf + got, len - got);
      if (n > 0) got += n;
    }
    return got;
  }
  size_t write(uint8_t) override { return 0; }  // read-only

 private:
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
};

// One HTTPS request; the answer is read straight from the network into `doc` through
// `filter` (nullptr = keep everything). body == nullptr means GET, else POST JSON.
bool fetchJson(const char* url, const char* body, JsonDocument& doc, JsonDocument* filter) {
  WiFiClientSecure tls;
  tls.setCACertBundle(kCertBundle);  // check the server's certificate against Mozilla's roots
  HTTPClient http;
  http.useHTTP10(true);  // no "chunked" answers, so the stream is plain JSON for ArduinoJson
  http.setTimeout(10000);
  uint32_t t0 = millis();
  Serial.printf("dash: asking %.40s\n", url);
  if (!http.begin(tls, url)) return false;
  http.addHeader("Accept", "application/json");
  int code;
  if (body != nullptr) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t*)body, strlen(body));
  } else {
    code = http.GET();
  }
  bool ok = false;
  if (code == 200) {
    PatientStream in(http.getStream(), 10000);
    DeserializationError err =
        filter != nullptr ? deserializeJson(doc, in, DeserializationOption::Filter(*filter))
                          : deserializeJson(doc, in);
    ok = !err;
    if (err) Serial.printf("dash: bad JSON from %s: %s\n", url, err.c_str());
  } else {
    Serial.printf("dash: %s -> HTTP %d\n", url, code);
  }
  http.end();
  Serial.printf("dash:   answer %s after %lu ms\n", ok ? "read" : "failed", millis() - t0);
  return ok;
}

const char* rpcUrl() { return gSetup.rpcUrl[0] != '\0' ? gSetup.rpcUrl : kPublicRpc; }

bool rpcError(JsonDocument& doc, const char* what) {
  if (doc["error"].isNull()) return false;
  Serial.printf("dash: RPC %s error: %s\n", what, doc["error"]["message"] | "?");
  return true;
}

// One full refresh. Returns the state to show.
NetState refresh(Portfolio* p) {
  static char body[256];
  static char path[640];
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

  NetState state = NetState::kOk;
  int n = snprintf(path, sizeof path, "%s", kCoinGecko);
  coinGeckoPath(*p, false, path + n, sizeof path - n);
  if (fetchJson(path, nullptr, doc, nullptr)) {
    portfolioApplyPrices(doc.as<JsonVariantConst>(), p);
  } else {
    state = NetState::kPriceError;
  }
  // Devnet tokens have no market price (their mints aren't the real ones), so only ask
  // CoinGecko about tokens on mainnet.
  if (kMainnet && state == NetState::kOk && coinGeckoPath(*p, true, path + n, sizeof path - n)) {
    if (fetchJson(path, nullptr, doc, nullptr)) {
      portfolioApplyPrices(doc.as<JsonVariantConst>(), p);
    } else {
      state = NetState::kPriceError;
    }
  }
  portfolioFinish(p, kMainnet);
  return state;
}

void publish(const Portfolio& p, uint32_t at) {
  xSemaphoreTake(gLock, portMAX_DELAY);
  gShared = p;
  gSharedAt = at;
  gSharedVersion++;
  xSemaphoreGive(gLock);
}

// The background task: join Wi-Fi, refresh, wait, repeat.
void netTask(void*) {
  static Portfolio p;  // static: too big for this task's stack
  uint32_t lastSaved = 0;
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
    NetState result = refresh(&p);
    Serial.printf("dash: refresh %s in %lu ms, %u tokens, $%.2f, free heap %u\n",
                  result == NetState::kOk ? "ok" : "failed", millis() - t0, p.count,
                  p.totalUsd, (unsigned)ESP.getFreeHeap());
    if (result != NetState::kRpcError) {  // balances are right even if prices are missing
      uint32_t at = unixNow();
      publish(p, at);
      if (result == NetState::kOk && (lastSaved == 0 || at - lastSaved >= kSaveEveryS)) {
        dashSavePortfolio(p, at);
        lastSaved = at;
      }
    }
    gState = result;
    uint32_t wait = result == NetState::kOk ? kRefreshMs : kRetryMs;
    for (uint32_t t = 0; t < wait && !gRefreshNow; t += 250) delay(250);
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
  page.reserve(4096);
  page += F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Sigly setup</title><style>"
            "body{background:#0E1116;color:#EEF1F5;font:16px system-ui,sans-serif;margin:0;padding:20px}"
            "h1{font-size:22px;margin:0 0 4px}p{color:#A3ADBA;margin:0 0 18px;line-height:1.4}"
            "label{display:block;font-weight:600;margin:16px 0 6px}"
            "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;"
            "border:0;background:#181D25;color:#EEF1F5;font-size:16px}"
            "button{margin-top:24px;width:100%;padding:14px;border:0;border-radius:24px;"
            "background:#C7F284;color:#0E1116;font-size:16px;font-weight:700}"
            "small{color:#A3ADBA}</style>"
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
