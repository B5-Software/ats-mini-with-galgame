#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "AiSettings.h"

// Always include the on-device configuration server in published firmware.
#define ENABLE_ASYNC_WEB 1

#include <WiFi.h>
#include <WiFiUdp.h>
// 可选异步 Web 服务器 (存在版本兼容问题时可不定义 ENABLE_ASYNC_WEB 以完全禁用)
#ifdef ENABLE_ASYNC_WEB
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#endif
#include <NTPClient.h>
#include <ESPmDNS.h>

// ---------------- Hardcoded WiFi Credentials ----------------
// 如果不需要通过网页配置, 直接在此处写死 WiFi 信息
// 修改为你的路由器 SSID / 密码; 留空则回退到原有偏好/多 SSID 逻辑
#ifndef WIFI_SSID
#define WIFI_SSID ""      // 例: "MyHomeAP" (为空则仍尝试存储的多个网络)
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""  // 例: "MyPassword"
#endif

#define CONNECT_TIME  3000  // Time of inactivity to start connecting WiFi

//
// Access Point (AP) mode settings
//
static const char *apSSID    = RECEIVER_NAME;
// AP password is the generated per-device WebUI password (see loadWebCredentials).
static const int   apChannel = 10;      // WiFi channel number (1..13)
static const bool  apHideMe  = false;   // TRUE: disable SSID broadcast
static const int   apClients = 3;       // Maximum simultaneous connected clients

static uint16_t ajaxInterval = 2500;

static bool itIsTimeToWiFi = false; // TRUE: Need to connect to WiFi
static uint32_t connectTime = millis();

// Settings
String loginUsername = "";
String loginPassword = "";
static String webCsrfToken;
static bool webServerStarted = false;
static void loadWebCredentials();
static bool webAuthenticate(AsyncWebServerRequest *request);

// AsyncWebServer object (only if enabled)
#ifdef ENABLE_ASYNC_WEB
AsyncWebServer server(80);
#endif

// NTP Client to get time
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org");

static bool wifiInitAP();
static bool wifiConnect();
// Web 配置相关 (仅在启用异步 web 时需要)
#ifdef ENABLE_ASYNC_WEB
static void webInit();
static void webSetConfig(AsyncWebServerRequest *request);
#endif

// 以下网页相关函数只在启用异步 web 时编译
#ifdef ENABLE_ASYNC_WEB
static const String webInputField(const String &name, const String &value, bool pass = false);
static const String webStyleSheet();
static const String webPage(const String &body);
static const String webUtcOffsetSelector();
static const String webThemeSelector();
static const String webRadioPage();
static const String webMemoryPage();
static const String webConfigPage();
static const String webAiPage();
static void webSetAi(AsyncWebServerRequest *request);
static String htmlEscape(String value);
#endif

//
// Delayed WiFi connection
//
void netRequestConnect()
{
  connectTime = millis();
  itIsTimeToWiFi = true;
}

void netTickTime()
{
  // Connect to WiFi if requested
  if(itIsTimeToWiFi && ((millis() - connectTime) > CONNECT_TIME))
  {
    netInit(wifiModeIdx);
    connectTime = millis();
    itIsTimeToWiFi = false;
  }
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
  wifi_mode_t mode = WiFi.getMode();

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
// Initialize WiFi network and services
//
void netInit(uint8_t netMode, bool showStatus)
{
  loadWebCredentials();
  // 确保网络命名空间存在
  if(!prefs.begin("network", false, STORAGE_PARTITION)) {
    Serial.println("[NETWORK] Failed to create/open network preferences namespace");
  } else {
    prefs.end();
    Serial.println("[NETWORK] Network preferences namespace initialized");
  }

  // Always disable WiFi first
  netStop();

  switch(netMode)
  {
    case NET_OFF:
      // Do not initialize WiFi if disabled
      return;
    case NET_AP_ONLY:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP);
      // Let user see connection status if successful
      if(wifiInitAP() && showStatus) delay(2000);
      break;
    case NET_AP_CONNECT:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP_STA);
      // Let user see connection status if successful
      if(wifiInitAP() && showStatus) delay(2000);
      break;
    default:
      // No access point
      WiFi.mode(WIFI_STA);
      break;
  }

  // Initialize WiFi and try connecting to a network
  if(netMode>NET_AP_ONLY && wifiConnect())
  {
    // Let user see connection status if successful
    if(netMode!=NET_SYNC && showStatus) delay(2000);

    // NTP time updates will happen every 5 minutes
    ntpClient.setUpdateInterval(5*60*1000);

    // Get NTP time from the network
    clockReset();
    for(int j=0 ; j<10 ; j++)
      if(ntpSyncTime()) break; else delay(500);
  }

  // If only connected to sync...
  if(netMode==NET_SYNC)
  {
    // Drop network connection
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
  }
  else
  {
  // Initialize web server for remote configuration (可选)
#ifdef ENABLE_ASYNC_WEB
  webInit();
#endif

    // Initialize mDNS
    MDNS.begin("atsmini"); // Set the hostname to "atsmini.local"
    MDNS.addService("http", "tcp", 80);
  }
}

//
// Returns TRUE if NTP time is available
//
bool ntpIsAvailable()
{
  return(ntpClient.isTimeSet());
}

//
// Update NTP time and synchronize clock with NTP time
//
bool ntpSyncTime()
{
  if(WiFi.status()==WL_CONNECTED)
  {
    ntpClient.update();

    if(ntpClient.isTimeSet())
      return(clockSet(
        ntpClient.getHours(),
        ntpClient.getMinutes(),
        ntpClient.getSeconds()
      ));
  }
  return(false);
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
  WiFi.softAP(apSSID, loginPassword.c_str(), apChannel, apHideMe, apClients);
  WiFi.softAPConfig(ip, gateway, subnet);

  drawScreen(
    ("AP " + String(apSSID) + "  10.1.1.1").c_str(),
    ("WiFi password: " + loginPassword).c_str()
  );

  ajaxInterval = 2500;
  return(true);
}

//
// Connect to a WiFi network
//
static bool wifiConnect()
{
  String status = "Connecting to WiFi network..";
  Serial.println("[WIFI] === WiFi Connection Debug Start ===");

  bool triedHardcoded = false;

  // 1) 优先尝试写死的 WiFi (如果用户在上面宏里填写了)
  if(strlen(WIFI_SSID) > 0) {
    Serial.printf("[WIFI] Attempting hardcoded WiFi: SSID='%s', Password length=%d\n", WIFI_SSID, strlen(WIFI_PASSWORD));
    triedHardcoded = true;
    
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WIFI] WiFi.begin() called, initial status: %d\n", WiFi.status());
    
    for(int j=0 ; (WiFi.status()!=WL_CONNECTED) && (j<24) ; j++) {
      if(!(j&7)) {
        status += ".";
        drawScreen(status.c_str());
        Serial.printf("[WIFI] Connection attempt %d, status: %d (%s)\n", j+1, WiFi.status(),
          WiFi.status() == WL_IDLE_STATUS ? "IDLE" :
          WiFi.status() == WL_NO_SSID_AVAIL ? "NO_SSID_AVAIL" :
          WiFi.status() == WL_SCAN_COMPLETED ? "SCAN_COMPLETED" :
          WiFi.status() == WL_CONNECTED ? "CONNECTED" :
          WiFi.status() == WL_CONNECT_FAILED ? "CONNECT_FAILED" :
          WiFi.status() == WL_CONNECTION_LOST ? "CONNECTION_LOST" :
          WiFi.status() == WL_DISCONNECTED ? "DISCONNECTED" : "UNKNOWN");
      }
      delay(500);
      if(digitalRead(ENCODER_PUSH_BUTTON)==LOW) {
        Serial.println("[WIFI] User aborted connection");
        WiFi.disconnect();
        break;
      }
    }
    
    if(WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] Hardcoded WiFi connected successfully!\n");
      Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("[WIFI] Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
      Serial.printf("[WIFI] DNS: %s\n", WiFi.dnsIP().toString().c_str());
      Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());
    } else {
      Serial.printf("[WIFI] Hardcoded WiFi failed, final status: %d\n", WiFi.status());
    }
  } else {
    Serial.println("[WIFI] No hardcoded WiFi credentials provided");
  }

  // 2) 如果未连接且未提供硬编码或硬编码失败, 回退原有多 SSID 偏好逻辑
  if(WiFi.status()!=WL_CONNECTED && !triedHardcoded) {
    Serial.println("[WIFI] Trying stored network preferences...");
    if(!prefs.begin("network", true, STORAGE_PARTITION)) {
      Serial.println("[WIFI] Failed to open network preferences (likely doesn't exist)");
    } else {
      loginUsername = prefs.getString("loginusername", "");
      loginPassword = prefs.getString("loginpassword", "");
      Serial.printf("[WIFI] Found stored login credentials: username='%s', password length=%d\n", 
                   loginUsername.c_str(), loginPassword.length());
      
      for(int j=0 ; (j<3) && (WiFi.status()!=WL_CONNECTED) ; j++) {
        char nameSSID[16], namePASS[16];
        sprintf(nameSSID, "wifissid%d", j+1);
        sprintf(namePASS, "wifipass%d", j+1);
        String ssid = prefs.getString(nameSSID, "");
        String password = prefs.getString(namePASS, "");
        
        if(ssid != "") {
          Serial.printf("[WIFI] Trying stored network %d: SSID='%s', password length=%d\n", 
                       j+1, ssid.c_str(), password.length());
          WiFi.begin(ssid, password);
          
          for(int k=0 ; (WiFi.status()!=WL_CONNECTED) && (k<24) ; k++) {
            if(!(k&7)) {
              status += ".";
              drawScreen(status.c_str());
              Serial.printf("[WIFI] Stored network %d attempt %d, status: %d\n", j+1, k+1, WiFi.status());
            }
              delay(500);
              if(digitalRead(ENCODER_PUSH_BUTTON)==LOW) {
                Serial.println("[WIFI] User aborted stored network connection");
                WiFi.disconnect();
                break;
              }
          }
          
          if(WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WIFI] Connected to stored network %d successfully!\n", j+1);
            break;
          } else {
            Serial.printf("[WIFI] Stored network %d failed, status: %d\n", j+1, WiFi.status());
          }
        } else {
          Serial.printf("[WIFI] Stored network %d slot is empty\n", j+1);
        }
      }
      prefs.end();
    }
  }

  // If failed connecting to WiFi network...
  if(WiFi.status()!=WL_CONNECTED)
  {
    Serial.printf("[WIFI] === WiFi Connection Failed ===\n");
    Serial.printf("[WIFI] Final status: %d\n", WiFi.status());
    Serial.printf("[WIFI] Hardcoded tried: %s\n", triedHardcoded ? "YES" : "NO");
    Serial.printf("[WIFI] Available networks:\n");
    
    // Scan for networks to help debug
    int n = WiFi.scanNetworks();
    if(n == 0) {
      Serial.println("[WIFI] No networks found");
    } else {
      for(int i = 0; i < n && i < 5; ++i) { // Show first 5 networks
        Serial.printf("[WIFI]   %d: %s (RSSI: %d, Encrypted: %s)\n", 
                     i+1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), 
                     WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES");
      }
    }
    
    // WiFi connection failed
    drawScreen(status.c_str(), "No WiFi connection");
    Serial.println("[WIFI] === WiFi Connection Debug End (Failed) ===");
    // Done
    return(false);
  }

  Serial.println("[WIFI] === WiFi Connection Successful ===");
  Serial.printf("[WIFI] Connected to: %s\n", WiFi.SSID().c_str());
  Serial.printf("[WIFI] IP Address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("[WIFI] Subnet: %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("[WIFI] DNS: %s\n", WiFi.dnsIP().toString().c_str());
  Serial.printf("[WIFI] MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());
  Serial.printf("[WIFI] Channel: %d\n", WiFi.channel());
  Serial.println("[WIFI] === WiFi Connection Debug End (Success) ===");

  // WiFi connection succeeded
  drawScreen(
    ("Connected to WiFi network (" + WiFi.SSID() + ")").c_str(),
    ("IP : " + WiFi.localIP().toString() + " or atsmini.local").c_str()
  );
  // Done
  ajaxInterval = 1000;
  return(true);
}

// A per-device random password protects the WebUI on both AP and station networks.
// It is displayed on the receiver at first setup and whenever AP mode starts.
static void loadWebCredentials() {
  Preferences webPrefs;
  if (!webPrefs.begin("network", false, STORAGE_PARTITION)) return;
  loginUsername = webPrefs.getString("loginusername", "admin");
  loginPassword = webPrefs.getString("loginpassword", "");
  if (!loginUsername.length() || loginPassword.length() < 8) {
    loginUsername = "admin";
    loginPassword = String((uint32_t)esp_random(), HEX) +
                    String((uint32_t)esp_random(), HEX) +
                    String((uint32_t)esp_random(), HEX);
    webPrefs.putString("loginusername", loginUsername);
    webPrefs.putString("loginpassword", loginPassword);
    Serial.println("[WebUI] New admin credentials generated (see device screen)");
    drawScreen("WebUI login: admin", ("Password: " + loginPassword).c_str());
    delay(8000); // Physical access is required to see this value.
  }
  webPrefs.end();
  if (!webCsrfToken.length()) {
    webCsrfToken = String((uint32_t)esp_random(), HEX) +
                   String((uint32_t)esp_random(), HEX);
  }
}

#ifdef ENABLE_ASYNC_WEB
static bool webAuthenticate(AsyncWebServerRequest *request) {
  if (loginUsername.length() && loginPassword.length() &&
      request->authenticate(loginUsername.c_str(), loginPassword.c_str())) return true;
  request->requestAuthentication();
  return false;
}

static bool webValidPost(AsyncWebServerRequest *request) {
  if (!webAuthenticate(request)) return false;
  if (request->method() != HTTP_POST || !request->hasParam("csrf", true) ||
      request->getParam("csrf", true)->value() != webCsrfToken) {
    request->send(403, "text/plain", "Invalid POST or CSRF token");
    return false;
  }
  return true;
}

static String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}
#endif

// ---------------- Web Server ----------------
#ifdef ENABLE_ASYNC_WEB
// Initialize internal web server
static void webInit()
{
  if (webServerStarted) return;
  server.on("/", HTTP_GET, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webRadioPage());
  });

  server.on("/memory", HTTP_GET, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webMemoryPage());
  });

  server.on("/config", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if (!webAuthenticate(request)) return;
    request->send(200, "text/html; charset=utf-8", webConfigPage());
  });
  server.on("/ai", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if (!webAuthenticate(request)) return;
    request->send(200, "text/html; charset=utf-8", webAiPage());
  });
  server.on("/ai", HTTP_POST, webSetAi);

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // This method saves configuration form contents
  server.on("/setconfig", HTTP_POST, webSetConfig);

  // Start web server
  server.begin();
  webServerStarted = true;
}
#endif // ENABLE_ASYNC_WEB

#ifdef ENABLE_ASYNC_WEB
void webSetConfig(AsyncWebServerRequest *request)
{
  if (!webValidPost(request)) return;
  uint32_t prefsSave = 0;

  // Start modifying preferences
  prefs.begin("network", false, STORAGE_PARTITION);

  // Save user name and password
  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    String user = request->getParam("username", true)->value();
    String pass = request->getParam("password", true)->value();
    if(user.length() > 0 && user.length() < 64 && pass.length() >= 8 && pass.length() < 64) {
      loginUsername = user;
      loginPassword = pass;
      prefs.putString("loginusername", loginUsername);
      prefs.putString("loginpassword", loginPassword);
    }
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

  // Save time zone
  if(request->hasParam("utcoffset", true))
  {
    String utcOffset = request->getParam("utcoffset", true)->value();
    utcOffsetIdx = utcOffset.toInt();
    clockRefreshTime();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save theme
  if(request->hasParam("theme", true))
  {
    String theme = request->getParam("theme", true)->value();
    themeIdx = theme.toInt();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save scroll direction, tuning hold off, and menu zoom
  scrollDirection = request->hasParam("scroll", true)? -1 : 1;
  if (request->hasParam("holdoff", true))
    tuneHoldOff = request->getParam("holdoff", true)->value().toInt();
  zoomMenu        = request->hasParam("zoom", true);
  prefsSave |= SAVE_SETTINGS;

  // Done with the preferences
  prefs.end();

  // Save preferences immediately
  prefsRequestSave(prefsSave, true);

  // Show config page again
  request->redirect("/config");

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (wifiModeIdx>NET_AP_ONLY) && (WiFi.status()!=WL_CONNECTED))
    netRequestConnect();
}
#endif // ENABLE_ASYNC_WEB

#ifdef ENABLE_ASYNC_WEB
static const String webInputField(const String &name, const String &value, bool pass)
{
  String newValue = htmlEscape(value);

  return(
    "<INPUT TYPE='" + String(pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + newValue + "'>"
  );
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
"TH.HEADING"
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
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s (%s)</OPTION>",
      i, utcOffsetIdx==i? " SELECTED":"",
      utcOffsets[i].city, utcOffsets[i].desc
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
  String freq = currentMode == FM?
    String(currentFrequency / 100.0) + "MHz "
  : String(currentFrequency + currentBFO / 1000.0) + "kHz ";

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
"<H1>ATS-Mini Pocket Receiver</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/memory'>Memory</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>&nbsp;|&nbsp;<A HREF='/ai'>AI Models</A>"
"</P>"
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

static const String webMemoryPage()
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
"<H1>ATS-Mini Pocket Receiver Memory</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>Status</A>&nbsp;|&nbsp;<A HREF='/config'>Config</A>&nbsp;|&nbsp;<A HREF='/ai'>AI Models</A>"
"</P>"
"<TABLE COLUMNS=2>" + items + "</TABLE>"
);
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
  prefs.end();

  return webPage(
"<H1>ATS-Mini Config</H1>"
"<P ALIGN='CENTER'>"
  "<A HREF='/'>Status</A>"
  "&nbsp;|&nbsp;<A HREF='/memory'>Memory</A>"
  "&nbsp;|&nbsp;<A HREF='/ai'>AI Models</A>"
"</P>"
"<FORM ACTION='/setconfig' METHOD='POST'>"
  "<INPUT TYPE='hidden' NAME='csrf' VALUE='" + htmlEscape(webCsrfToken) + "'>"
  "<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Login Credentials</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Username</TD>"
    "<TD>" + webInputField("username", loginUsername) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("password", "", true) + " (enter a new password to change; 8+ characters)</TD>"
  "</TR>"
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
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Settings</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Time Zone</TD>"
    "<TD>"
      "<SELECT NAME='utcoffset'>" + webUtcOffsetSelector() + "</SELECT>"
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
    "<TD CLASS='LABEL'>Tuning Display Delay</TD>"
    "<TD><INPUT TYPE='NUMBER' NAME='holdoff' VALUE='" +
tuneHoldOff + "' MIN='0' MAX='255'></TD>"
  "</TR>"
   "<TR>"
    "<TD CLASS='LABEL'>Zoomed Menu</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='zoom' VALUE='on'" +
    (zoomMenu? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>"
    "<INPUT TYPE='SUBMIT' VALUE='Save'>"
  "</TH></TR>"
  "</TABLE>"
"</FORM>"
);
}
#endif // ENABLE_ASYNC_WEB


#ifdef ENABLE_ASYNC_WEB
static String aiInput(const String &id, const String &value) {
  return "<input id='" + id + "' name='" + id + "' type='text' maxlength='199' required"
         " style='width:100%;box-sizing:border-box' value='" + htmlEscape(value) + "'>";
}

static const String webAiPage() {
  AiServiceSettings chat = aiLoadSettings(AiService::Chat);
  AiServiceSettings image = aiLoadSettings(AiService::Image);
  String page = R"html(
    <h1>GalGame AI Providers</h1>
    <p style='text-align:center'><a href='/'>Status</a> | <a href='/config'>WiFi Settings</a></p>
    <p style='max-width:740px;margin:1em auto'>Configure text and image models independently.
       OpenAI-compatible JSON APIs and SiliconFlow are supported. Keys are stored only in the
       device's NVS storage, never in the public firmware. Use a trusted network.</p>
  )html";
  page += "<form method='POST' action='/ai'><input type='hidden' name='csrf' value='" +
          htmlEscape(webCsrfToken) + "'><input type='hidden' name='service' value='chat'>";
  page += "<table><tr><th colspan='2' class='HEADING'>Chat / Narrative Model</th></tr>";
  page += "<tr><td>Provider preset</td><td><select onchange=\"document.getElementById('chat_url').value=this.value\">"
          "<option value=''>Select (or use your own URL)</option>"
          "<option value='https://api.siliconflow.cn/v1/chat/completions'>SiliconFlow</option>"
          "<option value='https://api.openai.com/v1/chat/completions'>OpenAI</option>"
          "<option value='https://api.deepseek.com/chat/completions'>DeepSeek</option>"
          "<option value='https://openrouter.ai/api/v1/chat/completions'>OpenRouter</option>"
          "</select></td></tr>";
  page += "<tr><td>Chat API URL</td><td>" + aiInput("chat_url", chat.url) + "</td></tr>";
  page += "<tr><td>Chat model ID</td><td>" + aiInput("chat_model", chat.model) + "</td></tr>";
  page += "<tr><td>API key</td><td><input type='password' name='chat_key' autocomplete='new-password' maxlength='511' "
          "placeholder='Leave empty to keep existing key'> " +
          String(chat.apiKey.length() ? "Saved" : "Not set") + "</td></tr>";
  page += "<tr><td>Remove saved key</td><td><input type='checkbox' name='clear_key' value='1'></td></tr>";
  page += "<tr><td colspan='2' style='text-align:center'><button type='submit'>Save chat model</button></td></tr></table></form>";
  page += "<form method='POST' action='/ai'><input type='hidden' name='csrf' value='" +
          htmlEscape(webCsrfToken) + "'><input type='hidden' name='service' value='image'>";
  page += "<table><tr><th colspan='2' class='HEADING'>Image Generation Model (optional)</th></tr>";
  page += "<tr><td>Provider preset</td><td><select onchange=\"document.getElementById('img_url').value=this.value\">"
          "<option value=''>Select (or use your own URL)</option>"
          "<option value='https://api.siliconflow.cn/v1/images/generations'>SiliconFlow</option>"
          "<option value='https://api.openai.com/v1/images/generations'>OpenAI</option>"
          "</select></td></tr>";
  page += "<tr><td>Image API URL</td><td>" + aiInput("img_url", image.url) + "</td></tr>";
  page += "<tr><td>Image model ID</td><td>" + aiInput("img_model", image.model) + "</td></tr>";
  page += "<tr><td>Request format</td><td><select name='img_format'>"
          "<option value='siliconflow'" + String(image.imageFormat != "openai" ? " selected" : "") +
          ">SiliconFlow (image_size: 480x288)</option>"
          "<option value='openai'" + String(image.imageFormat == "openai" ? " selected" : "") +
          ">OpenAI compatible (size: 1024x1024)</option></select></td></tr>";
  page += "<tr><td>API key</td><td><input type='password' name='img_key' autocomplete='new-password' maxlength='511' "
          "placeholder='Leave empty to keep existing key'> " +
          String(image.apiKey.length() ? "Saved" : "Not set") + "</td></tr>";
  page += "<tr><td>Remove saved key</td><td><input type='checkbox' name='clear_key' value='1'></td></tr>";
  page += "<tr><td colspan='2' style='text-align:center'><button type='submit'>Save image model</button></td></tr></table></form>";
  page += "<p style='text-align:center'>A missing image model disables image requests but story generation still works.</p>"
          "<p style='text-align:center'>Keys are sent to the configured HTTPS host as Bearer tokens. "
          "HTTP URLs are allowed only for keyless local services. "
          "Do not expose this device's HTTP WebUI to the public internet.</p>";
  return webPage(page);
}

static void webSetAi(AsyncWebServerRequest *request) {
  if (!webValidPost(request)) return;
  auto field = [request](const char *name) -> String {
    return request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
  };
  String service = field("service");
  if (service != "chat" && service != "image") {
    request->send(400, "text/plain", "Unknown AI service");
    return;
  }
  bool isChat = service == "chat";
  String error;
  bool ok = aiSaveSettings(isChat ? AiService::Chat : AiService::Image,
                           field(isChat ? "chat_url" : "img_url"),
                           field(isChat ? "chat_model" : "img_model"),
                           field(isChat ? "chat_key" : "img_key"),
                           request->hasParam("clear_key", true),
                           isChat ? String() : field("img_format"), error);
  if (!ok) {
    request->send(400, "text/plain; charset=utf-8", error);
    return;
  }
  request->redirect("/ai");
}
#endif // ENABLE_ASYNC_WEB
