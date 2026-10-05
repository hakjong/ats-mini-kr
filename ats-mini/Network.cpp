#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Splash.h"
#include "TcpMode.h"
#include "Ota.h"
#include "Stations.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <NTPClient.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <time.h>
#include <ctype.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define CONNECT_TIME  3000  // Time of inactivity to start connecting WiFi
#define WIFI_MULTI_TOTAL_TIMEOUT  30000
#define NTP_SYNC_TIMEOUT  10000
#define SPLASH_MAX_FILE_SIZE (512U * 1024U)
#define BACKUP_MAX_FILE_SIZE (16U * 1024U)
#define MEMORY_NAME_SIZE sizeof(((Memory *)nullptr)->name)

#ifndef WIFI_POWER_LEVEL
#define WIFI_POWER_LEVEL WIFI_POWER_17dBm
#endif

WiFiMulti wifiMulti;

//
// Access Point (AP) mode settings
//
static const char *apSSID    = RECEIVER_NAME;
static const char *apPWD     = 0;       // No password
static const int   apChannel = 10;      // WiFi channel number (1..13)
static const bool  apHideMe  = false;   // TRUE: disable SSID broadcast
static const int   apClients = 3;       // Maximum simultaneous connected clients

static bool itIsTimeToWiFi = false; // TRUE: Need to connect to WiFi
static uint32_t connectTime = 0;

enum NetAction : uint8_t { NET_STOP, NET_INIT, NET_SYNC_ONCE, NET_REFRESH };

struct NetRequest
{
  NetAction action;
  uint8_t mode;
  uint32_t generation;
};

struct NetEvent
{
  uint32_t generation;
  uint32_t epoch;
  uint32_t duration;
  char line1[96];
  char line2[96];
};

static QueueHandle_t netRequests = nullptr;
static QueueHandle_t netEvents = nullptr;
static std::atomic<uint32_t> netGeneration{0};
static std::atomic<uint32_t> netCompleted{0};
static std::atomic<bool> ntpHasTime{false};
static std::atomic<NetAction> netAction{NET_STOP};
static std::atomic<uint8_t> netRequestedMode{NET_OFF};

// Settings
String loginUsername = "";
String loginPassword = "";
static bool wifiScanHidden = false;

// AsyncWebServer object on port 80
AsyncWebServer server(80);

// NTP Client to get time
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org");

static bool wifiInitAP();
static bool wifiConnect(uint32_t generation);
static void wifiStopHardware();
static void netWorker(void *parameter);
static bool netQueue(NetAction action, uint8_t mode = NET_OFF);
static void netPost(uint32_t generation, const char *line1 = nullptr,
                    const char *line2 = nullptr, uint32_t duration = 2000, uint32_t epoch = 0);
static void webInit();
static void wifiRegisterPowerLevelCallback();
static void wifiPowerLevelOnEvent(WiFiEvent_t event);

static void webSetConfig(AsyncWebServerRequest *request);
static void webUploadSplash(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final);
static bool webIsAuthenticated(AsyncWebServerRequest *request);
static void webUploadFirmwareComplete(AsyncWebServerRequest *request);
static void webUpdatePage(AsyncWebServerRequest *request, const OtaStatus &status = otaStatus(), int code = 0);
static void webUploadFirmware(AsyncWebServerRequest *request, const String &filename,
                              size_t index, uint8_t *data, size_t len, bool final);
static void webRestoreComplete(AsyncWebServerRequest *request);
static void webUploadBackup(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final);
static bool webParseUTCDateTime(const String &text, uint32_t *epoch);

static const String webInputField(const String &name, const String &value, bool pass = false);
static const String webStyleSheet();
static const String webPage(const String &body);
static String webNavigation(const char *activePage);
static const String webUtcOffsetSelector();
static const String webThemeSelector();
static const String webRadioPage();
static const String webFavoritePage();
static const String webMemoryPage();
static const String webBackupPage(const String &message = "");
static const String webBackupYaml();
static const String webConfigPage();

struct BackupUploadState
{
  String contents;
  bool tooLarge;
  bool invalidFile;
};

struct SplashUploadState
{
  bool incomplete;
  bool tooLarge;
};

static bool webIsAuthenticated(AsyncWebServerRequest *request)
{
  return(loginUsername == "" || loginPassword == "" ||
         request->authenticate(loginUsername.c_str(), loginPassword.c_str()));
}

//
// Delayed WiFi connection
//
static bool netQueue(NetAction action, uint8_t mode)
{
  if(!netRequests)
  {
    netRequests = xQueueCreate(1, sizeof(NetRequest));
    netEvents = xQueueCreate(6, sizeof(NetEvent));
    if(!netRequests || !netEvents ||
       xTaskCreatePinnedToCore(netWorker, "network", 12288, nullptr, 1, nullptr, 0) != pdPASS)
    {
      if(netRequests) vQueueDelete(netRequests);
      if(netEvents) vQueueDelete(netEvents);
      netRequests = netEvents = nullptr;
      statusShow("WiFi task failed");
      return false;
    }
  }

  NetRequest request = { action, mode, netGeneration.fetch_add(1) + 1 };
  netRequestedMode.store(mode);
  netAction.store(action);
  xQueueOverwrite(netRequests, &request);
  return true;
}

static void netPost(uint32_t generation, const char *line1, const char *line2,
                    uint32_t duration, uint32_t epoch)
{
  NetEvent event = {};
  event.generation = generation;
  event.epoch = epoch;
  event.duration = duration;
  strlcpy(event.line1, line1 ? line1 : "", sizeof(event.line1));
  strlcpy(event.line2, line2 ? line2 : "", sizeof(event.line2));
  if(xQueueSend(netEvents, &event, 0) != pdTRUE)
  {
    NetEvent discarded;
    xQueueReceive(netEvents, &discarded, 0);
    xQueueSend(netEvents, &event, 0);
  }
}

void netRequestConnect()
{
  connectTime = millis();
  itIsTimeToWiFi = true;
}

bool netTickTime()
{
  otaTick();

  NetEvent event;
  while(netEvents && xQueueReceive(netEvents, &event, 0) == pdTRUE)
  {
    if(event.generation != netGeneration.load()) continue;
    if(event.epoch)
    {
      clockSetEpoch(event.epoch);
      ntpHasTime.store(clockAvailable());
    }
    if(event.line1[0] || event.line2[0] || event.duration == 0)
      statusShow(event.line1, event.line2, event.duration);
  }

  // Connect to WiFi if requested
  if(itIsTimeToWiFi && ((millis() - connectTime) > CONNECT_TIME))
  {
    netInit(wifiModeIdx);
    itIsTimeToWiFi = false;
  }

  static bool wasConnecting = false;
  static bool previousBlink = false;
  bool connecting = netIsConnecting();
  bool blink = connecting && (millis() & 0x200);
  bool redraw = connecting != wasConnecting || (connecting && blink != previousBlink);
  wasConnecting = connecting;
  previousBlink = blink;
  return redraw;
}

bool netIsConnecting()
{
  if(netCompleted.load() == netGeneration.load() || WiFi.status() == WL_CONNECTED) return false;

  NetAction action = netAction.load();
  return action == NET_SYNC_ONCE || (action == NET_INIT && netRequestedMode.load() > NET_AP_ONLY);
}

//
// Get current connection status
// (-1 - not connected, 0 - disabled, 1 - connected, 2 - connected to network)
//
int8_t getWiFiStatus()
{
  wifi_mode_t mode = WiFi.getMode();

  switch(mode)
  {
    case WIFI_MODE_NULL:
      return(0);
    case WIFI_AP:
      return(WiFi.softAPgetStationNum()? 1 : -1);
    case WIFI_STA:
      return(WiFi.status()==WL_CONNECTED? 2 : -1);
    case WIFI_AP_STA:
      return((WiFi.status()==WL_CONNECTED)? 2 : WiFi.softAPgetStationNum()? 1 : -1);
    default:
      return(-1);
  }
}

char *getWiFiIPAddress()
{
  static char ip[16];
  return strcpy(ip, WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
}

//
// Stop WiFi hardware
//
void netStop()
{
  tcpStop();
  if(!netRequests)
  {
    wifiStopHardware();
    return;
  }
  if(!netQueue(NET_STOP)) return;
  uint32_t generation = netGeneration.load();
  // Called before CPU sleep: wait until the radio has actually stopped.
  uint32_t start = millis();
  while(netCompleted.load() < generation && millis() - start < 10000) delay(10);
}

static void wifiStopHardware()
{
  wifi_mode_t mode = WiFi.getMode();

  ntpClient.end();
  MDNS.end();

  // If network connection up, shut it down
  if((mode==WIFI_STA) || (mode==WIFI_AP_STA))
    WiFi.disconnect(true);

  // If access point up, shut it down
  if((mode==WIFI_AP) || (mode==WIFI_AP_STA))
    WiFi.softAPdisconnect(true);

  WiFi.mode(WIFI_MODE_NULL);
}

//
// Start WiFi initialization without delaying the receiver UI.
//
void netInit(uint8_t netMode)
{
  tcpStop();
  if(netMode == NET_OFF && !netRequests) return;
  if(netMode == NET_AP_ONLY)
    statusShow("Starting access point...", nullptr, 0);
  else
    statusShow(nullptr);
  netQueue(NET_INIT, netMode);
}

// Synchronize once without changing the saved WiFi mode.
void netSyncTimeOnce()
{
  statusShow(nullptr);
  netQueue(NET_SYNC_ONCE, wifiModeIdx);
}

void netCancelSyncOnce()
{
  if(netCompleted.load() == netGeneration.load() || netAction.load() != NET_SYNC_ONCE) return;
  statusShow(nullptr);
  netQueue(wifiModeIdx == NET_OFF || wifiModeIdx == NET_SYNC ? NET_STOP : NET_INIT, wifiModeIdx);
}

//
// Returns TRUE if NTP time is available
//
bool ntpIsAvailable()
{
  return(ntpHasTime.load());
}

//
// Update NTP time and synchronize clock with NTP time
//
bool ntpSyncTime()
{
  if(WiFi.status() == WL_CONNECTED && netCompleted.load() == netGeneration.load()) netQueue(NET_REFRESH);
  return(false);
}

static uint32_t netGetNtp(uint32_t generation, bool fresh)
{
  ntpClient.begin();
  uint32_t start = millis();
  for(uint8_t attempt = 0; attempt < 10 && millis() - start < NTP_SYNC_TIMEOUT; ++attempt)
  {
    if(generation != netGeneration.load()) break;
    bool updated = fresh ? ntpClient.forceUpdate() : ntpClient.update();
    if(updated || (!fresh && ntpClient.isTimeSet()))
      return ntpClient.getEpochTime();
    delay(500);
  }
  return 0;
}

static void netWorker(void *parameter)
{
  (void)parameter;
  NetRequest request;
  while(xQueueReceive(netRequests, &request, portMAX_DELAY) == pdTRUE)
  {
    if(request.generation != netGeneration.load()) continue;

    if(request.action == NET_REFRESH)
    {
      if(WiFi.status() == WL_CONNECTED)
      {
        ntpClient.update();
        if(ntpClient.isTimeSet())
          netPost(request.generation, nullptr, nullptr, 2000, ntpClient.getEpochTime());
      }
    }
    else
    {
      wifiStopHardware();
      if(request.action != NET_STOP && !(request.action == NET_INIT && request.mode == NET_OFF))
      {
        wifiRegisterPowerLevelCallback();
        uint8_t mode = request.mode;
        if(request.action == NET_SYNC_ONCE)
          mode = request.mode == NET_OFF || request.mode == NET_SYNC ? NET_SYNC :
                 request.mode == NET_AP_ONLY ? NET_AP_CONNECT : request.mode;
        if(mode == NET_AP_ONLY || mode == NET_AP_CONNECT)
        {
          WiFi.mode(mode == NET_AP_ONLY ? WIFI_AP : WIFI_AP_STA);
          wifiInitAP();
        }
        else WiFi.mode(WIFI_STA);

        bool connected = mode > NET_AP_ONLY && wifiConnect(request.generation);
        if(request.generation != netGeneration.load())
        {
          wifiStopHardware();
          continue;
        }

        if(connected)
        {
          ntpClient.setUpdateInterval(5 * 60 * 1000);
          netPost(request.generation, "Syncing time...", nullptr, 0);
          uint32_t epoch = netGetNtp(request.generation, request.action == NET_SYNC_ONCE);
          if(request.generation != netGeneration.load())
          {
            wifiStopHardware();
            continue;
          }
          if(epoch) netPost(request.generation, nullptr, nullptr, 2000, epoch);
          if(request.action == NET_SYNC_ONCE && request.mode == NET_AP_ONLY)
          {
            wifiStopHardware();
            wifiRegisterPowerLevelCallback();
            WiFi.mode(WIFI_AP);
            wifiInitAP();
          }
          else if(mode == NET_SYNC)
          {
            wifiStopHardware();
            netPost(request.generation,
                    request.action == NET_SYNC_ONCE ? (epoch ? "Time synchronized" : "NTP sync failed") : nullptr,
                    nullptr, request.action == NET_SYNC_ONCE ? 2000 : 0);
          }
          else if(request.action != NET_SYNC_ONCE)
            netPost(request.generation,
                    ("Connected to WiFi network (" + WiFi.SSID() + ")").c_str(),
                    ("IP : " + WiFi.localIP().toString() + " or atsmini.local").c_str());
          if(request.action == NET_SYNC_ONCE && mode != NET_SYNC)
            netPost(request.generation, epoch ? "Time synchronized" : "NTP sync failed");
        }
        else if(mode == NET_SYNC || request.action == NET_SYNC_ONCE)
        {
          wifiStopHardware();
          if(request.action == NET_SYNC_ONCE && request.mode == NET_AP_ONLY)
          {
            wifiRegisterPowerLevelCallback();
            WiFi.mode(WIFI_AP);
            wifiInitAP();
          }
          netPost(request.generation, request.action == NET_SYNC_ONCE ? "WiFi connection failed" : "No WiFi connection");
        }
        else if(mode == NET_AP_ONLY || mode == NET_AP_CONNECT)
          netPost(request.generation, ("Use Access Point " + String(apSSID)).c_str(),
                  ("IP : " + WiFi.softAPIP().toString() + " or atsmini.local").c_str());
        else
          netPost(request.generation, "No WiFi connection");

        if((mode != NET_SYNC || request.mode == NET_AP_ONLY) && request.generation == netGeneration.load())
        {
          webInit();
          MDNS.begin("atsmini");
          MDNS.addService("http", "tcp", 80);
        }
      }
    }

    if(request.generation == netGeneration.load())
    {
      netCompleted.store(request.generation);
    }
  }
  vTaskDelete(nullptr);
}

static void wifiRegisterPowerLevelCallback()
{
  static bool registered = false;

  if(registered) return;

  WiFi.onEvent(wifiPowerLevelOnEvent, ARDUINO_EVENT_WIFI_AP_START);
  WiFi.onEvent(wifiPowerLevelOnEvent, ARDUINO_EVENT_WIFI_STA_START);
  registered = true;
}

static void wifiPowerLevelOnEvent(WiFiEvent_t event)
{
  (void)event;
  WiFi.setTxPower(WIFI_POWER_LEVEL);
}

//
// Initialize WiFi access point (AP)
//
static bool wifiInitAP()
{
  // These are our own access point (AP) addresses
  IPAddress ip(10, 1, 1, 1);
  IPAddress gateway(10, 1, 1, 1);
  IPAddress subnet(255, 255, 255, 0);

  // Start as access point (AP)
  WiFi.softAP(apSSID, apPWD, apChannel, apHideMe, apClients);
  WiFi.softAPConfig(ip, gateway, subnet);

  return(true);
}

//
// Connect to a WiFi network
//
static bool wifiConnect(uint32_t generation)
{
  // Clean credentials
  wifiMulti.APlistClean();

  // Get the preferences
  Preferences wifiPrefs;
  wifiPrefs.begin("network", true, STORAGE_PARTITION);
  loginUsername = wifiPrefs.getString("loginusername", "");
  loginPassword = wifiPrefs.getString("loginpassword", "");
  wifiScanHidden = wifiPrefs.getBool("wifiscanhidden", false);

  // Try connecting to known WiFi networks
  for(int j=0 ; (j<3) ; j++)
  {
    char nameSSID[16], namePASS[16];
    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    String ssid = wifiPrefs.getString(nameSSID, "");
    String password = wifiPrefs.getString(namePASS, "");

    if(ssid != "")
      wifiMulti.addAP(ssid.c_str(), password.c_str());
  }

  // Done with preferences
  wifiPrefs.end();

  wl_status_t wifiStatus = WL_NO_SSID_AVAIL;
  uint32_t start = millis();
  while((millis() - start < WIFI_MULTI_TOTAL_TIMEOUT) && wifiStatus != WL_CONNECTED &&
        generation == netGeneration.load())
  {
    wifiStatus = (wl_status_t)wifiMulti.run(5000, wifiScanHidden);
    if(wifiStatus != WL_CONNECTED && millis() - start < WIFI_MULTI_TOTAL_TIMEOUT)
      delay(1000);
  }

  return(wifiStatus == WL_CONNECTED && generation == netGeneration.load());
}

//
// Initialize internal web server
//
static void webInit()
{
  server.on("/", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webRadioPage());
  });

  server.on("/favorite", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webFavoritePage());
  });

  server.on("/memory", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webMemoryPage());
  });

  server.on("/backup/download", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    AsyncWebServerResponse *response = request->beginResponse(200, "application/yaml", webBackupYaml());
    response->addHeader("Content-Disposition", "attachment; filename=ats-mini-memory.yaml");
    request->send(response);
  });

  server.on("/backup", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webBackupPage());
  });

  server.on("/config", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webConfigPage());
  });

  server.on("/splash.png", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if(!LittleFS.exists(SPLASH_PATH))
      return request->send(404, "text/plain", "Not found");
    request->send(LittleFS, SPLASH_PATH, "image/png");
  });

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // This method saves configuration form contents
  server.on("/setconfig", HTTP_POST, webSetConfig, webUploadSplash);
  server.on("/backup/restore", HTTP_POST, webRestoreComplete, webUploadBackup);

  // Register subpaths first: the server also matches /update to /update/... .
  server.on("/update/upload", HTTP_POST, webUploadFirmwareComplete, webUploadFirmware);
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    webUpdatePage(request, otaStatus(), 200);
  });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    if(!otaRequestLatest(request->hasParam("action", true) && request->getParam("action", true)->value() == "install"))
      return webUpdatePage(request, {OTA_FAILED, "An update is already in progress."}, 409);
    request->redirect("/update");
  });

  // Start web server
  server.begin();
}

static void webUploadFirmware(AsyncWebServerRequest *request, const String &filename,
                              size_t index, uint8_t *data, size_t len, bool)
{
  if(!webIsAuthenticated(request) || request->getResponse()) return;

  if(!index)
  {
    if(!filename.endsWith(".bin"))
      return webUpdatePage(request, {OTA_FAILED, "Select a firmware .bin file."});
    const auto *param = request->getParam("size", true);
    if(!param) return webUpdatePage(request, {OTA_FAILED, "Invalid firmware size."});
    size_t imageSize = strtoul(param->value().c_str(), nullptr, 10);
    // Round-trip the number to reject signs, whitespace, suffixes, and overflow.
    if(!imageSize || imageSize > request->contentLength() || String(imageSize) != param->value())
      return webUpdatePage(request, {OTA_FAILED, "Invalid firmware size."});
    if(!otaBegin(imageSize))
      return webUpdatePage(request, {OTA_FAILED, "An update is already in progress."}, 409);
    request->client()->setRxTimeout(15);
    request->onDisconnect([]() { otaEndUpload(); });
  }

  if(!otaWrite(data, len)) return webUpdatePage(request);
}

static void webUploadFirmwareComplete(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();
  if(request->getResponse()) return; // Preserve an upload error queued above.
  if(!request->hasParam("firmware", true, true))
    return webUpdatePage(request, {OTA_FAILED, "No complete firmware uploaded."});
  otaFinish();
  webUpdatePage(request);
}

static void webUploadSplash(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final)
{
  if(!webIsAuthenticated(request)) return;

  if(index == 0)
  {
    LittleFS.remove(SPLASH_TEMP_PATH);

    SplashUploadState *state = static_cast<SplashUploadState *>(calloc(1, sizeof(SplashUploadState)));
    if(!state) return;

    request->_tempObject = state;
    request->onDisconnect([request, state]() {
      if(state->incomplete)
        request->_tempFile.close();

      // Discard an upload that was not installed by webSetConfig().
      LittleFS.remove(SPLASH_TEMP_PATH);
    });

    // The browser filter is only advisory, so enforce the extension here too.
    if(filename.endsWith(".png"))
    {
      request->_tempFile = LittleFS.open(SPLASH_TEMP_PATH, "w");
      state->incomplete = request->_tempFile;
    }
  }

  SplashUploadState *state = static_cast<SplashUploadState *>(request->_tempObject);

  if(request->_tempFile && len)
  {
    if((index + len) > SPLASH_MAX_FILE_SIZE)
    {
      state->tooLarge = true;
      state->incomplete = false;
      request->_tempFile.close();
      LittleFS.remove(SPLASH_TEMP_PATH);
    }
    else if(request->_tempFile.write(data, len) != len)
    {
      state->incomplete = false;
      request->_tempFile.close();
      LittleFS.remove(SPLASH_TEMP_PATH);
    }
  }

  if(final && request->_tempFile)
    request->_tempFile.close();

  if(final && state)
    state->incomplete = false;
}

void webSetConfig(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();

  uint32_t prefsSave = 0;
  uint32_t epoch;
  bool setClock = false;

  if(request->hasParam("datetime", true))
  {
    String dateTime = request->getParam("datetime", true)->value();
    if(dateTime != "")
    {
      if(!webParseUTCDateTime(dateTime, &epoch))
        return request->send(400, "text/plain", "Date/time must use the YYYY-mm-dd HH:MM:SS format and contain a valid UTC date and time.");
      setClock = true;
    }
  }

  if(request->hasParam("deletesplash", true))
  {
    LittleFS.remove(SPLASH_TEMP_PATH);
    LittleFS.remove(SPLASH_PATH);
  }
  else if(request->hasParam("splash", true, true))
  {
    String filename = request->getParam("splash", true, true)->value();

    if(filename != "")
    {
      SplashUploadState *state = static_cast<SplashUploadState *>(request->_tempObject);
      if(state && state->tooLarge)
        return request->send(413, "text/plain", "The splash image must not exceed 512 KB.");

      if(!filename.endsWith(".png"))
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(400, "text/plain", "The splash image filename must end in .png.");
      }

      if(!LittleFS.exists(SPLASH_TEMP_PATH))
        return request->send(500, "text/plain", "The splash image could not be stored.");

      String error = splashValidate();
      if(error != "")
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(400, "text/plain", error);
      }

      if(!LittleFS.rename(SPLASH_TEMP_PATH, SPLASH_PATH))
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(500, "text/plain", "The splash image could not be installed.");
      }
    }
  }

  // Start modifying preferences
  prefs.begin("network", false, STORAGE_PARTITION);

  // Save user name and password
  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    loginUsername = request->getParam("username", true)->value();
    loginPassword = request->getParam("password", true)->value();

    prefs.putString("loginusername", loginUsername);
    prefs.putString("loginpassword", loginPassword);
  }

  // Save SSIDs and their passwords
  bool haveSSID = false;
  for(int j=0 ; j<3 ; j++)
  {
    char nameSSID[16], namePASS[16];

    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(request->hasParam(nameSSID, true) && request->hasParam(namePASS, true))
    {
      String ssid = request->getParam(nameSSID, true)->value();
      String pass = request->getParam(namePASS, true)->value();
      prefs.putString(nameSSID, ssid);
      prefs.putString(namePASS, pass);
      haveSSID |= ssid != "" && pass != "";
    }
  }

  // Save hidden SSID scanning preference
  wifiScanHidden = request->hasParam("wifiscanhidden", true);
  prefs.putBool("wifiscanhidden", wifiScanHidden);

  // Save time zone
  if(request->hasParam("utcoffset", true))
  {
    int idx = request->getParam("utcoffset", true)->value().toInt();
    if(idx >= 0 && idx < getTotalUTCOffsets())
    {
      utcOffsetIdx = idx;
      prefsSave |= SAVE_SETTINGS;
    }
  }

  // Save theme
  if(request->hasParam("theme", true))
  {
    String theme = request->getParam("theme", true)->value();
    themeIdx = theme.toInt();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save scroll direction and menu zoom
  scrollDirection = request->hasParam("scroll", true)? -1 : 1;
  zoomMenu        = request->hasParam("zoom", true);
  setEncoderHalfStep(request->hasParam("encoderhalfstep", true));
  prefsSave |= SAVE_SETTINGS;

  // Done with the preferences
  prefs.end();

  // Save preferences immediately
  prefsRequestSave(prefsSave, true);

  if(setClock) clockSetEpoch(epoch);

  // Show config page again
  request->redirect("/config");

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (wifiModeIdx>NET_AP_ONLY) && (WiFi.status()!=WL_CONNECTED))
    netRequestConnect();
}

static const String webInputField(const String &name, const String &value, bool pass)
{
  String newValue(value);

  newValue.replace("\"", "&quot;");
  newValue.replace("'", "&apos;");

  return(
    "<INPUT TYPE='" + String(pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + newValue + "'>"
  );
}

static bool webParseUTCDateTime(const String &text, uint32_t *epoch)
{
  int year, month, day, hour, minute, second;
  return(epoch && text.length() == 19 &&
         sscanf(text.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
                &year, &month, &day, &hour, &minute, &second) == 6 &&
         clockUTCDateTimeToEpoch(year, month, day, hour, minute, second, epoch));
}

static const String webStyleSheet()
{
  return
"BODY"
"{"
  "margin: 0;"
  "padding: 0;"
"}"
"H1"
"{"
  "text-align: center;"
"}"
"TABLE"
"{"
  "width: 100%;"
  "max-width: 768px;"
  "border: 0px;"
  "margin-left: auto;"
  "margin-right: auto;"
"}"
"TH, TD"
"{"
  "padding: 0.5em;"
"}"
".HEADING"
"{"
  "background-color: #80A0FF;"
  "column-span: all;"
  "text-align: center;"
"}"
"TD.LABEL"
"{"
  "text-align: right;"
"}"
"INPUT[type=text], INPUT[type=password], SELECT"
"{"
  "width: 95%;"
  "padding: 0.5em;"
"}"
"INPUT[type=submit]"
"{"
  "width: 50%;"
  "padding: 0.5em 0;"
"}"
".CENTER"
"{"
  "text-align: center;"
"}"
;
}

static String webNavigation(const char *activePage)
{
  static const struct { const char *name; const char *path; } pages[] =
  {
    {"Status", "/"},
    {"Favorite", "/favorite"},
    {"Memory", "/memory"},
    {"Backup", "/backup"},
    {"Config", "/config"},
    {"Update", "/update"},
  };
  String result = "<P ALIGN='CENTER'>";
  for(size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++)
  {
    if(i) result += "&nbsp;|&nbsp;";
    if(!strcmp(activePage, pages[i].path)) result += pages[i].name;
    else result += String("<A HREF='") + pages[i].path + "'>" + pages[i].name + "</A>";
  }
  return result + "</P>";
}

static const String webPage(const String &body)
{
  return
"<!DOCTYPE HTML>"
"<HTML>"
"<HEAD>"
  "<META CHARSET='UTF-8'>"
  "<META NAME='viewport' CONTENT='width=device-width, initial-scale=1.0'>"
  "<TITLE>ATS-Mini Config</TITLE>"
  "<STYLE>" + webStyleSheet() + "</STYLE>"
"</HEAD>"
"<BODY STYLE='font-family: sans-serif;'>" + body + "</BODY>"
"</HTML>"
;
}

static const String webUtcOffsetSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalUTCOffsets(); i++)
  {
    char text[96];

    sprintf(text,
      "<OPTION VALUE='%d' DATA-MINUTES='%d'%s>%s</OPTION>",
      i, utcOffsets[i].offset * 15, utcOffsetIdx==i? " SELECTED":"",
      utcOffsets[i].desc
    );

    result += text;
  }

  return(result);
}

static const String webThemeSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalThemes(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
       i, themeIdx==i? " SELECTED":"", theme[i].name
    );

    result += text;
  }

  return(result);
}

static const String webRadioPage()
{
  String ip = "";
  String ssid = "";
  String receiverTime = "Not synchronized";
  int offsetMinutes = getCurrentUTCOffset() * 15;
  int offsetMagnitude = abs(offsetMinutes);
  char utcOffset[10];
  snprintf(utcOffset, sizeof(utcOffset), "UTC%c%02d:%02d",
           offsetMinutes < 0? '-' : '+', offsetMagnitude / 60, offsetMagnitude % 60);
  String freq = currentMode == FM?
    String(currentFrequency / 100.0) + "MHz "
  : String(currentFrequency + currentBFO / 1000.0) + "kHz ";

  if(clockAvailable())
  {
    time_t localTime = time(NULL) + offsetMinutes * 60;
    struct tm fields;
    gmtime_r(&localTime, &fields);
    char text[20];

    strftime(text, sizeof(text),
             clockGetDate(NULL, NULL, NULL, NULL)? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S",
             &fields);

    receiverTime = text;
  }

  receiverTime += " (" + String(utcOffset) + ")";

  if(WiFi.status()==WL_CONNECTED)
  {
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
  }
  else
  {
    ip = WiFi.softAPIP().toString();
    ssid = String(apSSID);
  }

  return webPage(
"<H1>ATS-Mini Pocket Receiver</H1>" + webNavigation("/") +
"<TABLE COLUMNS=2>"
"<TR>"
  "<TD CLASS='LABEL'>IP Address</TD>"
  "<TD><A HREF='http://" + ip + "'>" + ip + "</A> (" + ssid + ")</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>MAC Address</TD>"
  "<TD>" + String(getMACAddress()) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Firmware</TD>"
  "<TD>" + String(getVersion(true)) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Date/Time</TD>"
  "<TD>" + receiverTime + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Band</TD>"
  "<TD>" + String(getCurrentBand()->bandName) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Frequency</TD>"
  "<TD>" + freq + String(bandModeDesc[currentMode]) + "</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Signal Strength</TD>"
  "<TD>" + String(rssi) + "dBuV</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Signal to Noise</TD>"
  "<TD>" + String(snr) + "dB</TD>"
"</TR>"
"<TR>"
  "<TD CLASS='LABEL'>Battery Voltage</TD>"
  "<TD>" + String(batteryMonitor()) + "V</TD>"
"</TR>"
"</TABLE>"
);
}

static const String webFavoritePage()
{
  String items = "";

  for(int j=0 ; j<MEMORY_COUNT ; j++)
  {
    char text[64];
    sprintf(text, "<TR><TD CLASS='LABEL' WIDTH='10%%'>%02d</TD><TD>", j+1);
    items += text;

    if(!memories[j].freq)
      items += "&nbsp;---&nbsp;</TD></TR>";
    else
    {
      String freq = memories[j].mode == FM?
        String(memories[j].freq / 1000000.0) + "MHz "
      : String(memories[j].freq / 1000.0) + "kHz ";
      items += freq + bandModeDesc[memories[j].mode] + "</TD></TR>";
    }
  }

  return webPage(
"<H1>ATS-Mini Pocket Receiver Favorite</H1>" + webNavigation("/favorite") +
"<TABLE COLUMNS=2>" + items + "</TABLE>"
);
}

static const String webMemoryPage()
{
  static const char *const groupNames[] = { "FM", "MW", "SW" };
  String data = "const groups={";
  uint16_t frequencies[256];

  for(uint8_t group = 0; group < STATION_GROUP_COUNT; ++group)
  {
    uint16_t count = 0;
    stationsReadGroup(group, frequencies, &count);
    if(group) data += ',';
    data += String(groupNames[group]) + ":[";
    for(uint16_t i = 0; i < count; ++i)
    {
      if(i) data += ',';
      data += frequencies[i];
    }
    data += ']';
  }
  data += "};";

  return webPage(
"<H1>ATS-Mini Pocket Receiver Memory</H1>" + webNavigation("/memory") +
"<TABLE COLUMNS=2 ID='memory'></TABLE>"
"<SCRIPT>" + data +
"const table=document.getElementById('memory');"
"for(const [name,list] of Object.entries(groups)){"
  "table.insertAdjacentHTML('beforeend',`<tr><th colspan='2' class='HEADING'>${name} Memory (${list.length})</th></tr>`);"
  "if(!list.length)table.insertAdjacentHTML('beforeend',\"<tr><td colspan='2' class='CENTER'>--- Empty ---</td></tr>\");"
  "list.forEach((frequency,index)=>{"
    "const text=name==='FM'?(frequency/100).toFixed(2)+' MHz':frequency+' kHz';"
    "table.insertAdjacentHTML('beforeend',`<tr><td class='LABEL' width='10%'>${index+1}</td><td>${text}</td></tr>`);"
  "});"
"}"
"</SCRIPT>"
);
}

static const String webBackupPage(const String &message)
{
  String status = message.length()? "<P CLASS='CENTER'>" + message + "</P>" : "";
  return webPage(
"<H1>ATS-Mini Memory Backup</H1>" + webNavigation("/backup") + status +
"<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Download</TH></TR>"
  "<TR><TD COLSPAN=2 CLASS='CENTER'>Downloads all Favorite slots and FM/MW/SW Memory frequencies.</TD></TR>"
  "<TR><TD COLSPAN=2 CLASS='CENTER'><A HREF='/backup/download' DOWNLOAD>Download YAML</A></TD></TR>"
"</TABLE>"
"<FORM ACTION='/backup/restore' METHOD='POST' ENCTYPE='multipart/form-data'>"
  "<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Restore</TH></TR>"
  "<TR><TD CLASS='LABEL'>YAML File</TD><TD><INPUT TYPE='FILE' NAME='backup' ACCEPT='.yaml,.yml' REQUIRED></TD></TR>"
  "<TR><TD COLSPAN=2 CLASS='CENTER'><SMALL>Restoring replaces all Favorite and Memory entries.</SMALL></TD></TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'><INPUT TYPE='SUBMIT' VALUE='Restore'></TH></TR>"
  "</TABLE>"
"</FORM>"
);
}

static void appendHex(String &result, const char *data, size_t length)
{
  static const char hex[] = "0123456789ABCDEF";
  for(size_t i = 0; i < length; ++i)
  {
    uint8_t value = static_cast<uint8_t>(data[i]);
    result += hex[value >> 4];
    result += hex[value & 0x0F];
  }
}

static const String webBackupYaml()
{
  static const char *const groupNames[] = { "fm", "mw", "sw" };
  String result;
  result.reserve(16384);
  result = "version: 1\nfavorites:\n";
  for(uint8_t i = 0; i < MEMORY_COUNT; ++i)
  {
    result += "  - [" + String(i + 1) + ", " + memories[i].band + ", " + memories[i].freq +
              ", " + memories[i].mode + ", \"";
    appendHex(result, memories[i].name, sizeof(memories[i].name));
    result += "\"]\n";
  }
  result += "memory:\n";
  uint16_t frequencies[256];
  for(uint8_t group = 0; group < STATION_GROUP_COUNT; ++group)
  {
    uint16_t count = 0;
    stationsReadGroup(group, frequencies, &count);
    result += "  " + String(groupNames[group]) + ": [";
    for(uint16_t i = 0; i < count; ++i)
    {
      if(i) result += ", ";
      result += frequencies[i];
    }
    result += "]\n";
  }
  return result;
}

struct MemoryBackup
{
  Memory favorites[MEMORY_COUNT];
  uint16_t counts[STATION_GROUP_COUNT];
  uint16_t frequencies[STATION_GROUP_COUNT][256];
};

static bool decodeHexName(const char *hex, char *name)
{
  if(strlen(hex) != MEMORY_NAME_SIZE * 2) return false;
  for(size_t i = 0; i < MEMORY_NAME_SIZE; ++i)
  {
    if(!isxdigit(hex[i * 2]) || !isxdigit(hex[i * 2 + 1])) return false;
    char pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
    name[i] = strtoul(pair, nullptr, 16);
  }
  return true;
}

static bool parseFrequencyList(const String &line, uint16_t *frequencies, uint16_t *count)
{
  int start = line.indexOf('[');
  int end = line.lastIndexOf(']');
  if(start < 0 || end < start || line.substring(end + 1).length()) return false;
  String values = line.substring(start + 1, end);
  values.trim();
  *count = 0;
  while(values.length())
  {
    int comma = values.indexOf(',');
    String value = comma < 0? values : values.substring(0, comma);
    value.trim();
    if(!value.length() || *count >= 256) return false;
    char *tail;
    unsigned long frequency = strtoul(value.c_str(), &tail, 10);
    if(!frequency || frequency > UINT16_MAX || *tail) return false;
    frequencies[(*count)++] = frequency;
    if(comma < 0) break;
    values = values.substring(comma + 1);
    values.trim();
  }
  return true;
}

static bool parseBackup(const String &contents, MemoryBackup &backup)
{
  bool favoriteSeen[MEMORY_COUNT] = {};
  bool groupSeen[STATION_GROUP_COUNT] = {};
  bool versionSeen = false;
  int offset = 0;

  while(offset <= contents.length())
  {
    int next = contents.indexOf('\n', offset);
    if(next < 0) next = contents.length();
    String line = contents.substring(offset, next);
    line.trim();
    offset = next + 1;
    if(!line.length() || line.startsWith("#") || line == "favorites:" || line == "memory:") continue;
    if(line == "version: 1")
    {
      versionSeen = true;
      continue;
    }
    if(line.startsWith("- ["))
    {
      unsigned int slot, band, mode;
      unsigned long long frequency;
      char nameHex[MEMORY_NAME_SIZE * 2 + 1] = {};
      int consumed = 0;
      if(sscanf(line.c_str(), "- [%u, %u, %llu, %u, \"%20[0-9A-Fa-f]\"]%n",
                &slot, &band, &frequency, &mode, nameHex, &consumed) != 5 ||
         consumed != line.length() || !slot || slot > MEMORY_COUNT || favoriteSeen[slot - 1] ||
         band >= static_cast<unsigned int>(getTotalBands()) ||
         mode >= static_cast<unsigned int>(getTotalModes()) || frequency > UINT32_MAX)
        return false;
      Memory &favorite = backup.favorites[slot - 1];
      favorite.freq = frequency;
      favorite.band = band;
      favorite.mode = mode;
      if(!decodeHexName(nameHex, favorite.name) ||
         (favorite.freq && !isMemoryInBand(&bands[favorite.band], &favorite))) return false;
      favoriteSeen[slot - 1] = true;
      continue;
    }

    static const char *const groupNames[] = { "fm:", "mw:", "sw:" };
    bool matched = false;
    for(uint8_t group = 0; group < STATION_GROUP_COUNT; ++group)
      if(line.startsWith(groupNames[group]))
      {
        if(groupSeen[group] || !parseFrequencyList(line, backup.frequencies[group], &backup.counts[group]))
          return false;
        groupSeen[group] = matched = true;
        break;
      }
    if(!matched) return false;
  }

  if(!versionSeen) return false;
  for(uint8_t i = 0; i < MEMORY_COUNT; ++i)
    if(!favoriteSeen[i]) return false;
  for(uint8_t group = 0; group < STATION_GROUP_COUNT; ++group)
  {
    if(!groupSeen[group]) return false;
    if(!stationsValidateGroup(group, backup.frequencies[group], backup.counts[group])) return false;
  }
  return true;
}

static void webUploadBackup(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool)
{
  if(!webIsAuthenticated(request)) return;
  if(!index)
  {
    BackupUploadState *state = new BackupUploadState;
    if(!state) return;
    state->tooLarge = false;
    state->invalidFile = !filename.endsWith(".yaml") && !filename.endsWith(".yml");
    state->contents.reserve(BACKUP_MAX_FILE_SIZE);
    request->_tempObject = state;
  }
  BackupUploadState *state = static_cast<BackupUploadState *>(request->_tempObject);
  if(!state || state->tooLarge || state->invalidFile) return;
  if(index + len > BACKUP_MAX_FILE_SIZE)
  {
    state->tooLarge = true;
    state->contents = "";
    return;
  }
  for(size_t i = 0; i < len; ++i) state->contents += static_cast<char>(data[i]);
}

static void webRestoreComplete(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();
  BackupUploadState *state = static_cast<BackupUploadState *>(request->_tempObject);
  request->_tempObject = nullptr;
  if(!state) return request->send(400, "text/html", webBackupPage("No backup file uploaded."));
  if(state->invalidFile)
  {
    delete state;
    return request->send(400, "text/html", webBackupPage("Select a .yaml or .yml backup file."));
  }
  if(state->tooLarge)
  {
    delete state;
    return request->send(413, "text/html", webBackupPage("The backup file must not exceed 16 KB."));
  }

  MemoryBackup *backup = static_cast<MemoryBackup *>(ps_calloc(1, sizeof(MemoryBackup)));
  bool valid = backup && parseBackup(state->contents, *backup);
  delete state;
  if(!valid)
  {
    free(backup);
    return request->send(400, "text/html", webBackupPage("Invalid ATS Mini memory backup."));
  }

  for(uint8_t group = 0; group < STATION_GROUP_COUNT; ++group)
    if(!stationsWriteGroup(group, backup->frequencies[group], backup->counts[group]))
    {
      free(backup);
      return request->send(500, "text/html", webBackupPage("Failed to restore Memory frequencies."));
    }
  memcpy(memories, backup->favorites, sizeof(backup->favorites));
  prefs.begin("memories", false, STORAGE_PARTITION);
  for(uint8_t i = 0; i < MEMORY_COUNT; ++i) prefsSaveMemory(i, false);
  prefs.end();
  free(backup);
  request->send(200, "text/html", webBackupPage("Favorite and Memory restored successfully."));
}

const String webConfigPage()
{
  prefs.begin("network", true, STORAGE_PARTITION);
  String ssid1 = prefs.getString("wifissid1", "");
  String pass1 = prefs.getString("wifipass1", "");
  String ssid2 = prefs.getString("wifissid2", "");
  String pass2 = prefs.getString("wifipass2", "");
  String ssid3 = prefs.getString("wifissid3", "");
  String pass3 = prefs.getString("wifipass3", "");
  bool scanHidden = prefs.getBool("wifiscanhidden", false);
  prefs.end();

  String splashImage = LittleFS.exists(SPLASH_PATH)?
    "<IMG SRC='/splash.png?" + String(millis()) + "' ALT='Current splash screen' STYLE='max-width:100%;height:auto;'>"
  : "Not installed";
  String splashResolution = String(spr.width()) + "x" + String(spr.height());

  return webPage(
"<H1>ATS-Mini Config</H1>" + webNavigation("/config") +
"<FORM ACTION='/setconfig' METHOD='POST' ENCTYPE='multipart/form-data' ONSUBMIT='browserDateTime(true)'>"
  "<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 1</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid1", ssid1) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass1", pass1, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 2</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid2", ssid2) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass2", pass2, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 3</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid3", ssid3) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass3", pass3, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>This Web UI Login Credentials</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Username</TD>"
    "<TD>" + webInputField("username", loginUsername) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("password", loginPassword, true) + "</TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Settings</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Scan Hidden SSIDs</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='wifiscanhidden' VALUE='on'" +
    (scanHidden? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Use Browser Date/Time</TD>"
    "<TD><INPUT TYPE='CHECKBOX' ID='browserdatetime' ONCHANGE='browserDateTime()'></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>UTC Date/Time</TD>"
    "<TD><INPUT TYPE='TEXT' ID='datetime' NAME='datetime' PLACEHOLDER='YYYY-mm-dd HH:MM:SS'></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Time Zone</TD>"
    "<TD>"
      "<SELECT ID='utcoffset' NAME='utcoffset'>" + webUtcOffsetSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Theme</TD>"
    "<TD>"
      "<SELECT NAME='theme'>" + webThemeSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Reverse Scrolling</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='scroll' VALUE='on'" +
    (scrollDirection<0? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Half-step Encoder</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='encoderhalfstep' VALUE='on'" +
    (encoderHalfStep? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Zoomed Menu</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='zoom' VALUE='on'" +
    (zoomMenu? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Splash Screen</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Current Image</TD>"
    "<TD>" + splashImage + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Upload PNG</TD>"
    "<TD><INPUT TYPE='FILE' NAME='splash' ACCEPT='.png'>"
    "<BR><SMALL>Required resolution: " + splashResolution + " pixels; maximum size: 512 KB</SMALL></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Delete Image</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='deletesplash' VALUE='on'></TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>"
    "<INPUT TYPE='SUBMIT' VALUE='Save'>"
  "</TH></TR>"
  "</TABLE>"
"</FORM>"
"<SCRIPT>"
"function browserDateTime(submit)"
"{"
  "const enabled=document.getElementById('browserdatetime').checked;"
  "const dateTime=document.getElementById('datetime');"
  "const utcOffset=document.getElementById('utcoffset');"
  "if(enabled)"
  "{"
    "const now=new Date();"
    "dateTime.value=now.toISOString().slice(0,19).replace('T',' ');"
    "const minutes=-now.getTimezoneOffset();"
    "for(const option of utcOffset.options)"
      "if(Number(option.dataset.minutes)===minutes) utcOffset.value=option.value;"
  "}"
  "dateTime.disabled=utcOffset.disabled=enabled&&!submit;"
"}"
"</SCRIPT>"
);
}

// Explicit request errors leave the active operation's status unchanged.
static void webUpdatePage(AsyncWebServerRequest *request, const OtaStatus &status, int code)
{
  const bool busy = status.phase == OTA_CHECK_QUEUED || status.phase == OTA_QUEUED ||
                    status.phase == OTA_CONNECTING || status.phase == OTA_WRITING;
  const bool complete = status.phase == OTA_COMPLETE || status.phase == OTA_REBOOT_PENDING;
  const bool available = status.phase == OTA_AVAILABLE;
  const String refresh = complete? "<SCRIPT>setTimeout(()=>location.replace('/'),20000);</SCRIPT>" :
                         busy? "<SCRIPT>setTimeout(()=>location.replace('/update'),1000);</SCRIPT>" : "";
  const String page = webPage(
"<H1>Firmware Update</H1>" + webNavigation("/update") +
"<TABLE COLUMNS=1>"
"<TR><TD CLASS='CENTER'>" + status.message + "</TD></TR>"
"<!--"
"<TR><TH CLASS='HEADING'>"
  "<FORM METHOD='POST' ACTION='/update'>"
  "<BUTTON TYPE='SUBMIT' NAME='action' VALUE='" + String(available? "install" : "check") + "' STYLE='padding: 0.5em 2em;'" +
    String(busy || complete? " DISABLED" : "") + ">" + (available? "Update" : "Check for updates") + "</BUTTON>"
  "</FORM>"
"</TH></TR>"
"-->"
"<TR><TD CLASS='CENTER'>"
  "<H3>Manual upload</H3>"
  "<FORM METHOD='POST' ACTION='/update/upload' ENCTYPE='multipart/form-data' ONSUBMIT='this.elements.size.value=this.elements.firmware.files[0].size;this.querySelector(\"button\").disabled=true;'>"
  // Send the size before the file so the first upload callback can read it.
  "<INPUT TYPE='HIDDEN' NAME='size'>"
  "<P><INPUT TYPE='FILE' NAME='firmware' ARIA-LABEL='Firmware file' ACCEPT='.bin' REQUIRED" + String(busy || complete? " DISABLED" : "") + "></P>"
  "<SMALL>Use the <CODE>-ota.bin</CODE> or <CODE>ats-mini.ino.bin</CODE> for your receiver variant.</SMALL>"
  "<DIV CLASS='HEADING' STYLE='padding: 0.5em; margin-top: 1em;'>"
  "<BUTTON TYPE='SUBMIT' STYLE='padding: 0.5em 2em;'" + String(busy || complete? " DISABLED" : "") + ">Upload</BUTTON>"
  "</DIV>"
  "</FORM>"
"</TD></TR>"
"</TABLE>" + refresh
);
  if(!code) code = status.phase == OTA_FAILED? 400 : 200;
  AsyncWebServerResponse *response = request->beginResponse(code, "text/html", page);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Connection", "close");
  request->send(response);
}
