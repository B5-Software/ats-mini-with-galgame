// Clean structured header
#include "Common.h"
#include "AIGalGame.h"
#include "AiConfig.h"
#include "Themes.h"
#include "Draw.h"
#include "Storage.h"
#include "Utils.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <cstring>

// ESP32 core 3.x places File in namespace fs. Provide alias for legacy-style code.
#if defined(ESP32)
using File = fs::File;
#endif

// Unified logging: print to Serial AND update on-screen single-line logs
static String fetchStatus; // 提前声明，供 mirrorOverlayToStatus 使用
static String uiLogLine=""; static uint32_t uiLogTs=0; // base log
static String uiLogOverlay=""; static uint32_t uiLogOverlayTs=0; // high priority short-lived log
static void ggUILogf(const char *fmt,...){ char buf[160]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap); uiLogLine=String(buf); uiLogTs=millis(); }
static void ggUILogOverlayf(const char *fmt,...){ char buf[160]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap); uiLogOverlay=String(buf); uiLogOverlayTs=millis(); }
static void mirrorOverlayToStatus(){ if(uiLogOverlay.length()) fetchStatus = uiLogOverlay.substring(0,60); }
static void ggLogDual(const char *tag,const char *fmt,...){ char msg[192]; va_list ap; va_start(ap,fmt); vsnprintf(msg,sizeof(msg),fmt,ap); va_end(ap); Serial.printf("[%s] %s\n", tag, msg); Serial.flush(); uiLogLine=String(msg); uiLogTs=millis(); }
#define GG_LOG(fmt, ...) ggLogDual("GG", fmt, ##__VA_ARGS__)
#define GG_LOG_ERROR(fmt, ...) ggLogDual("GG-ERR", fmt, ##__VA_ARGS__)
#define GG_LOG_HTTP(fmt, ...) ggLogDual("GG-HTTP", fmt, ##__VA_ARGS__)
#define GG_LOG_STATE(fmt, ...) ggLogDual("GG-STATE", fmt, ##__VA_ARGS__)
#define GG_LOG_JSON(fmt, ...) ggLogDual("GG-JSON", fmt, ##__VA_ARGS__)

static bool inputPressLocked=false; static bool lastPressActive=false; // long-press guard
static int parseRetryCount=0; static const int MAX_PARSE_RETRY=2;
#ifdef USE_TJPG_DECODER
#include <TJpg_Decoder.h>
// Compatibility: some versions name the struct JPGDRAW instead of JPEGDRAW
#ifndef JPEGDRAW
#ifdef JPGDRAW
#define JPEGDRAW JPGDRAW
#endif
#endif
#endif

#ifdef USE_PNG_DECODER
#include <PNGdec.h>
// PNG pixel type constants if not defined
#ifndef PNG_PIXEL_TRUECOLOR
#define PNG_PIXEL_TRUECOLOR 2
#endif
#ifndef PNG_PIXEL_TRUECOLOR_ALPHA
#define PNG_PIXEL_TRUECOLOR_ALPHA 6
#endif
static PNG png;
struct ThumbSpec { uint16_t w; uint16_t h; const char *file; };
static const ThumbSpec THUMBS[] = { {60,36,"thumb_60x36.bin"}, {120,72,"thumb_120x72.bin"} };
#endif

// NOTE: This module builds a minimal text+image AI GalGame using SiliconFlow APIs.
// Constraints: limited RAM, no full streaming; incremental fetch each turn.
// Images are fetched as base64 and NOT decoded to bitmap here (placeholder rectangle) to keep scope feasible.

static GalGameState ggState = GG_IDLE;
static std::vector<String> genres = {
  "School Life","Sci-Fi","Fantasy","Mystery","Romance","Cyberpunk","Steampunk","Post-Apocalyptic","Historical","Slice of Life","Adventure","Horror","Comedy","Drama"
};
static int genreIdx = 0;
static GalGameProjectMeta currentProject;
static std::vector<GalGameContextEntry> context;
static int scrollOffset = 0; // Lines scroll
static bool fetching = false;
static uint32_t lastFetchTs = 0;
// fetchStatus 已前移
static int retryCount = 0;
static const int MAX_RETRY = 3;
static String lastAIText;
static String imagePrompt; // last generated image prompt
static String lastAIReasoning; // reasoning/hints from AI
static String lastAIMemoryAdd; // last memory addition fragment
static bool haveImage = false;
static uint32_t imageColor = TFT_DARKGREY; // simple average color placeholder
static bool imageDecoded = false;
static String lastImageB64;
static String lastImageUrl; // for URL-based image responses
static bool imageFromUrl = false;
static size_t lastImageBytes = 0; // 下载/解码到的原始 JPEG 字节数
// Persistent thumbnail (60x36) so decoded JPEG stays visible after each frame clear
static bool imageThumbReady = false;
static uint16_t imageThumb[60*36];
static int jpgFullW=0, jpgFullH=0; // full JPEG dimensions for scaling
static int jpgScaledW=0, jpgScaledH=0; // scaled output size after TJpgDec scale
// Image format tracking
enum GGImageFormat { IMG_FMT_NONE=0, IMG_FMT_JPEG=1, IMG_FMT_PNG=2 };
static GGImageFormat lastImageFormat = IMG_FMT_NONE;
// Target expected AI image resolution (request / validation)
// Updated target image size to 480x288 so 60x36 thumbnail is exact /8 scaling
static const int TARGET_IMG_W = 480;
static const int TARGET_IMG_H = 288;
static bool fullscreenNeedsRedraw=false;
// Fullscreen decode state
static bool fullscreenDecodeActive=false; static int fsBaseX=0; static int fsBaseY=0; static int fsScaledW=0; static int fsScaledH=0;
static uint32_t fsLastSig=0; // cache signature to avoid redundant fullscreen re-decode
static bool fsCachedLogged=false; // one-time log for cached fullscreen reuse
static int projectMenuIdx = 0;
static std::vector<GalGameProjectMeta> projects;
static int inGameMenuIdx = 0;
static const char* inGameMenuItems[] = {"Save Project","Create Checkpoint","Restore Checkpoint","View Knowledge Base","Summarize Knowledge","Full Screen Picture","Delete Project","About","Restart System","Exit Game"};

// Extra state for full screen image view
enum ExtraGGState { GG_FULLSCREEN_IMG = 0x100, GG_RESTART_MENU = 0x101 }; // use high value to avoid clash
static std::vector<String> pendingOptions; // choice texts
static int optionIdx = 0;
static int restartMenuIdx = 0;
static const char* restartMenuItems[] = {"Normal Restart", "Download Mode Restart", "Cancel"};
static String knowledgeBase; // persisted long-term memory summary
static String sessionLog; // append narrative text
static int maxContextRounds = 10; // configurable
static const char *GG_PREFS_NS = "ggsettings"; // NVS namespace

// Serial command support
static String serialCommandBuffer = "";
static bool serialCommandMode = false;

static void printSerialHelp() {
  Serial.println("\n=== ATS-MINI GalGame Serial Commands ===");
  Serial.println("LEFT       - Simulate encoder turn left");
  Serial.println("RIGHT      - Simulate encoder turn right");  
  Serial.println("CLICK      - Simulate encoder short press");
  Serial.println("LONG       - Simulate encoder long press");
  Serial.println("STATUS     - Show current game status");
  Serial.println("HELP       - Show this help message");
  Serial.println("=========================================\n");
}

static void processSerialCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  
  if(cmd == "HELP") {
    printSerialHelp();
  } else if(cmd == "LEFT") {
    Serial.println("CMD: Encoder LEFT");
    galgameEncoder(-1, false, false, false);
  } else if(cmd == "RIGHT") {
    Serial.println("CMD: Encoder RIGHT");  
    galgameEncoder(1, false, false, false);
  } else if(cmd == "CLICK") {
    Serial.println("CMD: Encoder CLICK");
    galgameEncoder(0, false, false, true);
  } else if(cmd == "LONG") {
    Serial.println("CMD: Encoder LONG");
    galgameEncoder(0, false, true, false);
  } else if(cmd == "STATUS") {
    Serial.printf("Status: State=%d, Active=%s, Fetching=%s\n", 
                  (int)ggState, galgameActive()?"YES":"NO", fetching?"YES":"NO");
    Serial.printf("Project: %s\n", currentProject.name.c_str());
    Serial.printf("Image: %s, Decoded=%s\n", haveImage?"YES":"NO", imageDecoded?"YES":"NO");
  } else if(cmd.length() > 0) {
    Serial.println("Unknown command. Type HELP for available commands.");
  }
}

static void handleSerialCommands() {
  while(Serial.available()) {
    char c = Serial.read();
    if(c == '\n' || c == '\r') {
      if(serialCommandBuffer.length() > 0) {
        processSerialCommand(serialCommandBuffer);
        serialCommandBuffer = "";
      }
    } else if(c >= ' ' && c <= '~') { // printable characters
      serialCommandBuffer += c;
      if(serialCommandBuffer.length() > 50) { // prevent overflow
        serialCommandBuffer = "";
      }
    }
  }
}
// Option statistics & preference reinforcement (time–decayed weight)
struct OptionStat { String text; float weight; uint32_t lastMs; };
static std::vector<OptionStat> optionStats;
static uint32_t lastGlobalDecayMs=0;
// Spinner characters for loading animation
static uint8_t spinnerIdx=0; static const char spinnerChars[] = "|/-\\";
// Non-blocking retry scheduling state
static bool aiRetryScheduled=false; static uint32_t aiRetryDueMs=0; static int aiStatusCode=0; static String lastErrorField;

void galgamePersistSettings(){
  GG_LOG("Saving GalGame settings");
  if(!prefs.begin(GG_PREFS_NS,false,STORAGE_PARTITION)) {
    GG_LOG_ERROR("Failed to open preferences for writing");
    return;
  }
  prefs.putUChar("maxRounds", (uint8_t)maxContextRounds);
  prefs.end();
  GG_LOG("GalGame settings saved");
}

void galgameLoadSettings(){
  GG_LOG("Loading GalGame settings");

  // 直接以读写方式打开，便于在缺失键时写入默认值，避免 NOT_FOUND 日志
  if(!prefs.begin(GG_PREFS_NS,false,STORAGE_PARTITION)) {
    GG_LOG_ERROR("Failed to open/create preferences namespace");
    return;
  }

  // maxRounds 默认初始化
  if(!prefs.isKey("maxRounds")) {
    prefs.putUChar("maxRounds", (uint8_t)maxContextRounds);
    GG_LOG("Created default key: maxRounds=%d", maxContextRounds);
  } else {
    maxContextRounds = prefs.getUChar("maxRounds", maxContextRounds);
  }

  // 不再支持运行时修改 API Key, 忽略 apiKey 键
  prefs.end();
  GG_LOG("GalGame settings loaded: maxRounds=%d (API key hardcoded)", maxContextRounds);
}

// Utility paths
static String baseDir = "/gg"; // under LittleFS

static void ensureBaseDir() { if(!LittleFS.exists(baseDir)) LittleFS.mkdir(baseDir); }

static void loadProjects() {
  projects.clear();
  ensureBaseDir();
  GG_LOG("Loading projects from %s", baseDir.c_str());
  
  File dir = LittleFS.open(baseDir);
  if(!dir) {
    GG_LOG_ERROR("Failed to open base directory: %s", baseDir.c_str());
    return;
  }
  
  File f;
  while((f = dir.openNextFile())) {
    if(!f.isDirectory()) { f.close(); continue; }
    
    String dirName = String(f.name());
    GG_LOG("Found directory: %s", dirName.c_str());
    
    // 确保路径是绝对路径
    String fullPath = dirName.startsWith("/") ? dirName : (baseDir + "/" + dirName);
    String metaPath = fullPath + "/meta.json";
    
    GG_LOG("Trying to open meta file: %s", metaPath.c_str());
    
    File meta = LittleFS.open(metaPath, "r");
    if(meta) {
      size_t fsz = meta.size();
      size_t baseCap = fsz < 512 ? 1024 : (fsz < 2048 ? fsz * 2 : fsz * 3 / 2 + 1024);
      DeserializationError err; bool parsed=false;
      for(int attempt=0; attempt<3 && !parsed; ++attempt){
        size_t cap = baseCap + attempt * 1024;
        DynamicJsonDocument jd(cap);
        meta.seek(0);
        err = deserializeJson(jd, meta);
        if(!err){
          GalGameProjectMeta m; m.dir = fullPath; m.name = jd["name"].as<String>(); m.synopsis = jd["synopsis"].as<String>(); m.genre = jd["genre"].as<String>();
          projects.push_back(m);
          GG_LOG("Loaded project: %s (cap=%u)", m.name.c_str(), (unsigned)cap);
          parsed=true; break;
        } else if(err == DeserializationError::NoMemory){
          GG_LOG_ERROR("Parse meta.json %s NoMemory cap=%u retry", dirName.c_str(), (unsigned)cap);
        } else {
          GG_LOG_ERROR("Parse meta.json %s fail: %s cap=%u", dirName.c_str(), err.c_str(), (unsigned)cap); break;
        }
      }
      meta.close();
      if(!parsed){ GalGameProjectMeta m; m.dir=fullPath; m.name=dirName; m.synopsis="(meta error)"; m.genre=""; projects.push_back(m); }
    } else {
      GG_LOG("No meta.json found for directory: %s", dirName.c_str());
    }
    f.close();
  }
  dir.close();
  GG_LOG("Loaded %d projects total", projects.size());
}

// 递归删除目录 (项目擦除辅助)
static bool deleteProjectRecursive(const String &path){
  if(!LittleFS.exists(path)) return true;
  File dir = LittleFS.open(path);
  if(!dir){ GG_LOG_ERROR("delete open fail %s", path.c_str()); return false; }
  if(!dir.isDirectory()){ dir.close(); LittleFS.remove(path); return true; }
  File f; bool ok=true; while((f=dir.openNextFile())){ String name=f.name(); if(f.isDirectory()){ ok &= deleteProjectRecursive(String(name)); } else { if(!LittleFS.remove(name)) ok=false; } f.close(); }
  dir.close(); if(!LittleFS.rmdir(path)) ok=false; return ok;
}

// 从AI响应内容中提取JSON，支持多种格式
static String extractJsonFromContent(const String &content) {
  GG_LOG_JSON("Extracting JSON from content: %s", content.c_str());
  
  // 1. 尝试直接解析整个内容（纯JSON格式）
  DynamicJsonDocument testDoc(1024);
  if(!deserializeJson(testDoc, content)) {
    GG_LOG_JSON("Content is pure JSON");
    return content;
  }
  
  // 2. 查找JSON代码块标记：```json ... ```
  int jsonStart = content.indexOf("```json");
  if(jsonStart >= 0) {
    int contentStart = content.indexOf('\n', jsonStart) + 1;
    if(contentStart > 0) {
      int jsonEnd = content.indexOf("```", contentStart);
      if(jsonEnd > contentStart) {
        String extracted = content.substring(contentStart, jsonEnd);
        extracted.trim();
        GG_LOG_JSON("Found JSON code block: %s", extracted.c_str());
        return extracted;
      }
    }
  }
  
  // 3. 查找普通代码块标记：``` ... ```
  int codeStart = content.indexOf("```");
  if(codeStart >= 0) {
    int contentStart = content.indexOf('\n', codeStart) + 1;
    if(contentStart <= 0) contentStart = codeStart + 3; // 没有换行，直接跟内容
    
    int codeEnd = content.indexOf("```", contentStart);
    if(codeEnd > contentStart) {
      String extracted = content.substring(contentStart, codeEnd);
      extracted.trim();
      
      // 检查这个代码块是否是JSON
      if(!deserializeJson(testDoc, extracted)) {
        GG_LOG_JSON("Found JSON in code block: %s", extracted.c_str());
        return extracted;
      }
    }
  }
  
  // 4. 查找花括号包围的JSON
  int braceStart = content.indexOf('{');
  int braceEnd = content.lastIndexOf('}');
  
  if(braceStart >= 0 && braceEnd > braceStart) {
    String extracted = content.substring(braceStart, braceEnd + 1);
    
    // 验证是否为有效JSON
    if(!deserializeJson(testDoc, extracted)) {
      GG_LOG_JSON("Found JSON in braces: %s", extracted.c_str());
      return extracted;
    }
  }
  
  // 5. 查找方括号包围的JSON数组
  int arrayStart = content.indexOf('[');
  int arrayEnd = content.lastIndexOf(']');
  
  if(arrayStart >= 0 && arrayEnd > arrayStart) {
    String extracted = content.substring(arrayStart, arrayEnd + 1);
    
    // 验证是否为有效JSON
    if(!deserializeJson(testDoc, extracted)) {
      GG_LOG_JSON("Found JSON array: %s", extracted.c_str());
      return extracted;
    }
  }
  
  GG_LOG_ERROR("No valid JSON found in content");
  return "";
}

static String safeFileName(const String &s) {
  String r;
  for(char c: s) r += (isalnum(c)||c=='-'||c=='_')? c:'_';
  return r.substring(0,24);
}

static void saveMeta() {
  if(currentProject.dir=="") {
    GG_LOG_ERROR("Cannot save meta: project dir is empty");
    return;
  }
  
  GG_LOG("Saving meta for project: %s", currentProject.name.c_str());
  GG_LOG("Project directory: %s", currentProject.dir.c_str());
  
  DynamicJsonDocument jd(4096);
  jd["name"]=currentProject.name;
  jd["synopsis"]=currentProject.synopsis;
  jd["genre"]=currentProject.genre;
  jd["knowledge"] = knowledgeBase;
  JsonArray hist = jd.createNestedArray("context");
  for(auto &e: context) {
    JsonObject o = hist.createNestedObject();
    o["role"]=e.role; o["content"]=e.content;
  }
  jd["log"] = sessionLog;
  jd["max_rounds"] = maxContextRounds;
  jd["api_key_set"] = true; // 恒为 true: 使用硬编码密钥
  // Persist weighted preferences
  if(optionStats.size()){
    JsonArray prefs = jd.createNestedArray("prefs");
    for(auto &os: optionStats){ JsonObject po=prefs.createNestedObject(); po["text"]=os.text; po["w"]=os.weight; po["t"]=os.lastMs; }
  }
  // Thumbnails metadata (if exist)
#ifdef USE_PNG_DECODER
  JsonArray thumbs = jd.createNestedArray("thumbs");
  for(auto &spec: THUMBS){ String p = currentProject.dir+"/"+spec.file; if(LittleFS.exists(p)) { JsonObject t=thumbs.createNestedObject(); t["w"]=spec.w; t["h"]=spec.h; t["path"]=spec.file; } }
#endif

  String metaPath = currentProject.dir + "/meta.json";
  GG_LOG("Saving meta file to: %s", metaPath.c_str());
  
  File f = LittleFS.open(metaPath, "w");
  if(f) { 
    size_t written = serializeJson(jd, f); 
    f.close(); 
    GG_LOG("Meta file saved successfully, %zu bytes written", written);
  } else {
    GG_LOG_ERROR("Failed to open meta file for writing: %s", metaPath.c_str());
  }
}

static void loadMeta(const GalGameProjectMeta &p) {
  GG_LOG("Loading meta for project: %s from %s", p.name.c_str(), p.dir.c_str());
  
  currentProject = p; context.clear(); knowledgeBase=""; lastAIText=""; imagePrompt=""; scrollOffset=0; inGameMenuIdx=0; fetchStatus=""; fetching=false;
  
  String metaPath = p.dir + "/meta.json";
  GG_LOG("Opening meta file: %s", metaPath.c_str());
  
  File f = LittleFS.open(metaPath, "r");
  if(!f) {
    GG_LOG_ERROR("Failed to open meta file: %s", metaPath.c_str());
    return; 
  }
  
  DynamicJsonDocument jd(8192); 
  DeserializationError e = deserializeJson(jd, f); 
  f.close(); 
  
  if(e) {
    GG_LOG_ERROR("Failed to parse meta file: %s", e.c_str());
    return;
  }
  
  knowledgeBase = jd["knowledge"].as<String>();
  sessionLog = jd["log"].as<String>();
  if(jd.containsKey("max_rounds")) maxContextRounds = jd["max_rounds"].as<int>();
  if(jd.containsKey("context")) {
    for(JsonObject o: jd["context"].as<JsonArray>()) {
      GalGameContextEntry ce; ce.role=o["role"].as<String>(); ce.content=o["content"].as<String>(); context.push_back(ce);
    }
  }
  optionStats.clear();
  if(jd.containsKey("prefs")){
    for(JsonObject po: jd["prefs"].as<JsonArray>()){
      OptionStat os; os.text=po["text"].as<String>(); os.weight=po["w"].as<float>(); os.lastMs=po["t"].as<uint32_t>(); if(os.text.length()) optionStats.push_back(os);
    }
  }
  
  GG_LOG("Meta loaded successfully: knowledge=%d chars, context=%d entries, prefs=%d", 
         knowledgeBase.length(), context.size(), optionStats.size());
}

static void appendContext(const String &role,const String &content) {
  context.push_back({role,content});
  while(context.size()>40) context.erase(context.begin()); // absolute cap
  while(context.size()> (size_t)(maxContextRounds*2)) context.erase(context.begin());
}

static String buildSystemPrompt() {
  String sys = "You are an AI GalGame engine. ALWAYS reply with ONLY a single compact JSON object (no markdown fences, no extra text). Max 180 words narrative. Provide fields: text (story continuation), img_prompt (succinct visual description for an illustration), reasoning (short inner design explanation 1-2 sentences, player-visible), options (array max 3 concise branching choices 2-6 words), memory_add (array of NEW long-term memory facts distilled from current turn ONLY when truly important, else empty). Do NOT include markdown or backticks. Maintain continuity using knowledge base. Project meta and knowledge base follow. Knowledge base is curated facts, don't repeat inside memory_add. If unsafe material requested, steer to safe adventure.\n";
  sys += "ProjectName:"+currentProject.name+" Genre:"+currentProject.genre+" Synopsis:"+currentProject.synopsis+"\nKnowledgeBase:"+knowledgeBase+"\n";
  sys += "Return JSON keys in order: text,img_prompt,reasoning,options,memory_add. Do not invent extra keys.";
  // Inject top preference weighting hint
  if(optionStats.size()){
  std::vector<OptionStat> copy=optionStats; std::sort(copy.begin(),copy.end(),[](const OptionStat&a,const OptionStat&b){return a.weight>b.weight;});
  int lim = std::min(3,(int)copy.size());
  String prefs = " Player preference trending keywords (bias subtle, weave naturally):";
  for(int i=0;i<lim;i++){ prefs += " ["+copy[i].text+":"+String(copy[i].weight,1)+"]"; }
    sys += prefs;
  }
  return sys;
}

static bool httpPostJSON(const char* url, const String &payload, String &response) {
  GG_LOG_HTTP("=== HTTP POST DEBUG START ===");
  GG_LOG_HTTP("Target URL: %s", url);
  GG_LOG_HTTP("Payload size: %d bytes", payload.length());
  GG_LOG_HTTP("Free heap: %d bytes", ESP.getFreeHeap());
  GG_LOG_HTTP("WiFi status: %d (%s)", WiFi.status(), 
    WiFi.status() == WL_CONNECTED ? "CONNECTED" :
    WiFi.status() == WL_NO_SSID_AVAIL ? "NO_SSID" :
    WiFi.status() == WL_CONNECT_FAILED ? "CONNECT_FAILED" :
    WiFi.status() == WL_CONNECTION_LOST ? "CONNECTION_LOST" :
    WiFi.status() == WL_DISCONNECTED ? "DISCONNECTED" : "OTHER");
  
  if(WiFi.status()!=WL_CONNECTED) { 
    response="WiFi not connected"; 
    GG_LOG_ERROR("WiFi not connected, status: %d", WiFi.status());
    GG_LOG_HTTP("=== HTTP POST DEBUG END (WiFi fail) ===");
    return false; 
  }
  
  GG_LOG_HTTP("WiFi IP: %s", WiFi.localIP().toString().c_str());
  GG_LOG_HTTP("WiFi RSSI: %d dBm", WiFi.RSSI());
  
  HTTPClient http; 
  GG_LOG_HTTP("HTTPClient begin...");
  bool beginResult = http.begin(url);
  GG_LOG_HTTP("HTTPClient begin result: %d", beginResult);
  
  if(!beginResult) {
    response = "HTTP begin failed";
    GG_LOG_ERROR("HTTPClient begin failed");
    GG_LOG_HTTP("=== HTTP POST DEBUG END (begin fail) ===");
    return false;
  }
  
  http.addHeader("Content-Type","application/json");
  http.addHeader("Authorization", String("Bearer ")+AI_API_KEY); // 固定密钥
  http.setTimeout(30000); // 30 second timeout
  
  GG_LOG_HTTP("Headers set, API key length: %d", strlen(AI_API_KEY));
  GG_LOG_HTTP("Request payload preview (first 200 chars): %.200s", payload.c_str());
  
  GG_LOG_HTTP("Sending POST request...");
  unsigned long startTime = millis();
  int code = http.POST(payload);
  unsigned long endTime = millis();
  
  GG_LOG_HTTP("POST completed in %lu ms", endTime - startTime);
  
  if(code<=0) { 
    aiStatusCode=code; 
    response=http.errorToString(code); 
    GG_LOG_ERROR("HTTP error code: %d", code);
    GG_LOG_ERROR("HTTP error string: %s", response.c_str());
    
    // Additional error details
    if(code == HTTPC_ERROR_CONNECTION_REFUSED) GG_LOG_ERROR("Connection refused");
    else if(code == HTTPC_ERROR_SEND_HEADER_FAILED) GG_LOG_ERROR("Send header failed");
    else if(code == HTTPC_ERROR_SEND_PAYLOAD_FAILED) GG_LOG_ERROR("Send payload failed");
    else if(code == HTTPC_ERROR_NOT_CONNECTED) GG_LOG_ERROR("Not connected");
    else if(code == HTTPC_ERROR_CONNECTION_LOST) GG_LOG_ERROR("Connection lost");
    else if(code == HTTPC_ERROR_NO_STREAM) GG_LOG_ERROR("No stream");
    else if(code == HTTPC_ERROR_NO_HTTP_SERVER) GG_LOG_ERROR("No HTTP server");
    else if(code == HTTPC_ERROR_TOO_LESS_RAM) GG_LOG_ERROR("Too less RAM");
    else if(code == HTTPC_ERROR_ENCODING) GG_LOG_ERROR("Encoding error");
    else if(code == HTTPC_ERROR_STREAM_WRITE) GG_LOG_ERROR("Stream write error");
    else if(code == HTTPC_ERROR_READ_TIMEOUT) GG_LOG_ERROR("Read timeout");
    
    http.end(); 
    GG_LOG_HTTP("=== HTTP POST DEBUG END (error) ===");
    return false; 
  }
  
  String responseBody = http.getString();
  int responseSize = http.getSize();
  String contentType = http.header("Content-Type");
  
  response = String(code)+"\n"+responseBody;
  http.end(); 
  aiStatusCode=code; 
  
  GG_LOG_HTTP("HTTP response code: %d", code);
  GG_LOG_HTTP("Response size: %d bytes (reported: %d)", responseBody.length(), responseSize);
  GG_LOG_HTTP("Content-Type: %s", contentType.c_str());
  GG_LOG_HTTP("Response preview (first 500 chars): %.500s", responseBody.c_str());
  
  bool success = code>=200 && code<300;
  if(!success) {
    GG_LOG_ERROR("HTTP request failed with code %d", code);
    if(code == 400) GG_LOG_ERROR("Bad Request - check payload format");
    else if(code == 401) GG_LOG_ERROR("Unauthorized - check API key");
    else if(code == 403) GG_LOG_ERROR("Forbidden - check API permissions");
    else if(code == 404) GG_LOG_ERROR("Not Found - check URL");
    else if(code == 429) GG_LOG_ERROR("Rate Limited - too many requests");
    else if(code >= 500) GG_LOG_ERROR("Server Error - API service issue");
  }
  
  GG_LOG_HTTP("=== HTTP POST DEBUG END (success: %d) ===", success);
  return success;
}

static void parseAndStoreImage(const String &json) {
  // Reset all image state
  haveImage=false; imageDecoded=false; lastImageB64=""; lastImageUrl=""; imageFromUrl=false; lastImageBytes=0;
  imageThumbReady=false; jpgFullW=jpgFullH=0; jpgScaledW=jpgScaledH=0; memset(imageThumb,0xFF,sizeof(imageThumb));
  lastImageFormat = IMG_FMT_NONE;
  // Try structured JSON first
  DynamicJsonDocument dj(8192);
  DeserializationError jerr = deserializeJson(dj, json);
  if(!jerr && dj.is<JsonObject>()){
    JsonArray arr = dj["images"].as<JsonArray>();
    if(!arr.isNull() && arr.size()>0){
      JsonObject first = arr[0].as<JsonObject>();
      if(first.containsKey("url")){
        lastImageUrl = first["url"].as<String>(); // will set haveImage true after successful fetch
        imageThumbReady=false; imageDecoded=false; jpgFullW=jpgFullH=0; jpgScaledW=jpgScaledH=0; memset(imageThumb,0xFF,sizeof(imageThumb));
        GG_LOG_HTTP("IMG url: %s", lastImageUrl.c_str());
        // Fetch binary (改进持续读取，直到 Content-Length 满足或超时)
    HTTPClient hc; if(hc.begin(lastImageUrl)){
          hc.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
          hc.addHeader("Accept","image/jpeg,application/octet-stream;q=0.9,*/*;q=0.5");
          // 某些服务需要鉴权才能访问生成文件
          if(AI_API_KEY && strlen(AI_API_KEY)>4) hc.addHeader("Authorization", String("Bearer ")+AI_API_KEY);
          hc.addHeader("User-Agent","ATS-MINI-GalGame/1.0");
          int code = hc.GET();
          String ctype = hc.header("Content-Type");
          if(code==200){
            int expected = hc.getSize(); // -1 if unknown
            GG_LOG_HTTP("IMG GET 200 Len=%d Type=%s", expected, ctype.c_str());
            // If Content-Type missing, proceed and rely on SOI/EOI + size sniff; only abort when an explicit non-image type present
            if(!ctype.length()){
              GG_LOG_HTTP("IMG no Content-Type header; will sniff JPEG markers");
              int hcount = hc.headers();
              for(int i=0;i<hcount;i++){
                String hn = hc.headerName(i);
                String hv = hc.header(hn.c_str());
                GG_LOG_HTTP("HDR %s: %s", hn.c_str(), hv.c_str());
              }
            } else if(!ctype.startsWith("image")){
              GG_LOG_HTTP("IMG abort: content-type %s", ctype.c_str());
              int hcount = hc.headers();
              for(int i=0;i<hcount;i++){
                String hn = hc.headerName(i);
                String hv = hc.header(hn.c_str());
                GG_LOG_HTTP("HDR %s: %s", hn.c_str(), hv.c_str());
              }
              hc.end();
              return; // do not mark haveImage
            }
            File f = LittleFS.open(currentProject.dir+"/last_image.bin","w");
            if(f){
              WiFiClient * stream = hc.getStreamPtr();
        uint8_t buf[1024]; size_t total=0; uint32_t accR=0,accG=0,accB=0,sample=0; uint32_t lastRead=millis(); bool sawSOI=false; bool foundEOI=false; uint8_t prev=0; size_t eoiPos=0; uint8_t first8[8]; int firstFill=0;
              while(hc.connected()){
                if(stream->available()){
                  int r = stream->readBytes(buf,sizeof(buf)); if(r<=0) break; lastRead=millis();
                  // Detect SOI
                  if(!sawSOI && r>=2){ if(buf[0]==0xFF && buf[1]==0xD8) sawSOI=true; }
          if(firstFill<8){ int need=8-firstFill; int take=r<need?r:need; memcpy(first8+firstFill, buf, take); firstFill+=take; }
                  
                  // Don't truncate at EOI - download the complete file as specified by Content-Length
                  int writeLen = r;
                  for(int i=0;i<r;i++){
                    uint8_t b = buf[i];
                    if(prev==0xFF && b==0xD9){ foundEOI=true; eoiPos = total + i + 1; }
                    prev = b;
                  }
                  f.write(buf, writeLen); total += writeLen;
                  for(int i=0;i<writeLen;i+=64){ uint8_t v=buf[i]; accR+=v; accG+= (v*37+17)&0xFF; accB+= (v*73+5)&0xFF; sample++; }
                  
                  // Only break if we have expected length and reached it, or if no expected length and we found EOI
                  if(expected>0 && (int)total>=expected) {
                    GG_LOG_HTTP("IMG complete at %d bytes (expected %d)%s", (int)total, expected, foundEOI?" with EOI":"");
                    break;
                  } else if(expected<=0 && foundEOI) {
                    GG_LOG_HTTP("IMG EOI at %d bytes (no Content-Length)", (int)eoiPos);
                    break;
                  }
                  // 不再强制总大小上限，依赖 expected / timeout 退出
                } else {
                  if(millis()-lastRead>600) { GG_LOG_HTTP("IMG idle timeout"); break; }
                  delay(5);
                }
              }
              f.close();
              lastImageBytes = total;
              if(sample){ uint8_t r=(accR/sample)&0xFF, g=(accG/sample)&0xFF, b=(accB/sample)&0xFF; imageColor=tft.color565(r,g,b);} else imageColor=TFT_NAVY;
              if(!sawSOI){ GG_LOG_HTTP("IMG warn: no SOI marker"); }
              if(!foundEOI){ GG_LOG_HTTP("IMG warn: no EOI marker (truncated?)"); }
              if(sawSOI) lastImageFormat = IMG_FMT_JPEG; else if(firstFill>=8 && first8[0]==0x89 && first8[1]==0x50 && first8[2]==0x4E && first8[3]==0x47) lastImageFormat = IMG_FMT_PNG; else lastImageFormat = IMG_FMT_NONE;
              GG_LOG_HTTP("IMG header %02X %02X %02X %02X %02X %02X %02X %02X fmt=%d", firstFill>0?first8[0]:0, firstFill>1?first8[1]:0, firstFill>2?first8[2]:0, firstFill>3?first8[3]:0, firstFill>4?first8[4]:0, firstFill>5?first8[5]:0, firstFill>6?first8[6]:0, firstFill>7?first8[7]:0, (int)lastImageFormat);
              if(lastImageFormat==IMG_FMT_NONE || total<128){ GG_LOG_HTTP("IMG invalid, discard (fmt=%d size=%d)", (int)lastImageFormat, (int)total); LittleFS.remove(currentProject.dir+"/last_image.bin"); return; }
              haveImage=true; imageFromUrl=true; imageThumbReady=false; imageDecoded=false; jpgFullW=jpgFullH=0; jpgScaledW=jpgScaledH=0; memset(imageThumb,0xFF,sizeof(imageThumb));
              GG_LOG_HTTP("IMG fetched %d bytes (exp=%d)%s%s", (int)total, expected, foundEOI?" EOI":"", (!foundEOI && expected<0)?" unkLen":"");
            } else GG_LOG_HTTP("IMG file open fail");
          } else GG_LOG_HTTP("IMG GET code %d", code);
          hc.end();
        } else GG_LOG_HTTP("IMG begin fail");
        return; // done (URL path)
      }
      // fallback: maybe base64 field inside object
      if(first.containsKey("base64")) lastImageB64 = first["base64"].as<String>();
      else if(first.containsKey("b64_json")) lastImageB64 = first["b64_json"].as<String>();
    }
  }
  if(lastImageB64.length()==0){
    int keyPos = json.indexOf("base64"); if(keyPos<0) keyPos = json.indexOf("b64");
    if(keyPos<0){ GG_LOG_HTTP("IMG json: no base64 key/url"); return; }
    int q1 = json.indexOf('"', keyPos); if(q1<0) return; q1 = json.indexOf('"', q1+1); if(q1<0) return; int q2 = json.indexOf('"', q1+1); if(q2<0) return;
    lastImageB64 = json.substring(q1+1, q2);
  }
  if(lastImageB64.length()<100) {
    if(lastImageB64.length()) GG_LOG_HTTP("IMG b64 too short %d", lastImageB64.length());
    return; // nothing usable
  }
  GG_LOG_HTTP("IMG b64 length %d", lastImageB64.length());
  // Heuristic detect format (first b64 bytes after decode may not be available yet; quick peek by decoding minimal prefix?)
  imageThumbReady=false; imageDecoded=false; jpgFullW=jpgFullH=0; jpgScaledW=jpgScaledH=0; memset(imageThumb,0xFF,sizeof(imageThumb));
  haveImage = true; imageFromUrl=false; imageColor = TFT_NAVY; lastImageBytes = lastImageB64.length();
}

static bool summarizePending = false; // flag triggers async summarize
static uint32_t summarizeStartTs = 0; // time when async summarize started

static void startSummarizeAsync(){
  if(fetching || summarizePending) return; summarizePending=true; summarizeStartTs=millis(); fetchStatus="Summarize queued"; }

static void doSummarizeNow(){
  if(fetching) return; fetching=true; fetchStatus="Summarizing...";
  DynamicJsonDocument jd(8192); jd["model"]=AI_CHAT_MODEL; JsonArray msgs=jd.createNestedArray("messages");
  JsonObject sys=msgs.createNestedObject(); sys["role"]="system"; sys["content"]="Summarize narrative facts into concise bullet points (max 12)";
  JsonObject u=msgs.createNestedObject(); u["role"]="user"; u["content"] = sessionLog.substring(sessionLog.length()>6000? sessionLog.length()-6000:0);
  String payload; serializeJson(jd,payload); String resp; if(httpPostJSON(AI_CHAT_URL,payload,resp)) {
    int nl=resp.indexOf('\n'); String body = (nl>0)? resp.substring(nl+1):resp; DynamicJsonDocument dr(8192); if(!deserializeJson(dr,body)) {
      String add = dr["choices"][0]["message"]["content"].as<String>();
      if(add.length()) { knowledgeBase += "\n"+add; if(knowledgeBase.length()>6000) knowledgeBase.remove(0, knowledgeBase.length()-6000); saveMeta(); lastAIText="Knowledge +"; }
    }
  }
  fetching=false; fetchStatus="Done"; summarizePending=false; retryCount=0;
}

static void requestAI(); // fwd
static bool writeImageFileFromB64(const String &b64); // fwd
static void buildThumbnails(); // fwd
// forward decls removed (function defined earlier)

static const char b64table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64idx(char c){ const char* p=strchr(b64table,c); return p? (int)(p-b64table) : -1; }
static size_t b64decode(const String &in, std::unique_ptr<uint8_t[]> &out){
  size_t len=in.length(); size_t pad=0; if(len>=2){ if(in[len-1]=='=') pad++; if(in[len-2]=='=') pad++; }
  size_t outLen = (len/4)*3 - pad; out.reset(new uint8_t[outLen]); size_t oi=0; uint32_t buf=0; int valb=-8; for(char c: in){ if(c=='='||c=='\n' || c=='\r') break; int v=b64idx(c); if(v<0) continue; buf=(buf<<6)|v; valb+=6; if(valb>=0){ out[oi++]=(uint8_t)((buf>>valb)&0xFF); valb-=8; if(oi>=outLen) break; } } return oi; }

#ifdef USE_TJPG_DECODER
// TJpg_Decoder 1.0.8+ uses SketchCallback: bool(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
static bool tftJpgDrawCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap){
  static uint32_t cbCount=0; cbCount++; if(cbCount<4 || (cbCount & 0x3F)==0){ GG_LOG_HTTP("JPG cb #%lu blk %dx%d at (%d,%d)", (unsigned long)cbCount, w, h, x, y); }
  
  // Always build thumbnail if sized
  if(jpgScaledW>0 && jpgScaledH>0){
    const int TW=60, THH=36;
    if(w>0 && h>0){
      for(int py=0; py<h; py++){
        int globalY = y + py; if(globalY>=jpgScaledH) continue;
        int ty = (globalY * THH) / jpgScaledH; if(ty>=0 && ty<THH){
          uint16_t *srcLine = bitmap + py * w;
          for(int px=0; px<w; px++){
            int globalX = x + px; if(globalX>=jpgScaledW) continue;
            int tx = (globalX * TW) / jpgScaledW; if(tx>=0 && tx<TW){
              imageThumb[ty*TW + tx] = srcLine[px];
            }
          }
        }
      }
    }
  }
  
  // If fullscreen decode is active, draw directly to TFT region
  if(fullscreenDecodeActive){
    int dx = fsBaseX + x; int dy = fsBaseY + y;
    if(w>0 && h>0) tft.pushImage(dx, dy, w, h, bitmap);
  }
  return true;
}
#endif

static void tryDecodeImage(){
  if(!haveImage){ GG_LOG_HTTP("IMG decode skip: no image flag"); return; }
  if(imageDecoded){ GG_LOG_HTTP("IMG decode skip: already decoded"); return; }
  GG_LOG_HTTP("IMG decode begin (have=%d fromUrl=%d fmt=%d bytes=%lu thumb=%d decoded=%d)", haveImage?1:0, imageFromUrl?1:0, (int)lastImageFormat, (unsigned long)lastImageBytes, imageThumbReady?1:0, imageDecoded?1:0);

  // Case 1: binary file downloaded from URL already stored at last_image.bin
  if(imageFromUrl && lastImageUrl.length()){
    File f = LittleFS.open(currentProject.dir+"/last_image.bin","r");
    if(!f){ GG_LOG_HTTP("IMG decode: file missing"); imageDecoded=true; return; }
// JPEG decoder (URL file) - assume library present when USE_TJPG_DECODER
#ifdef USE_TJPG_DECODER
  size_t sz = f.size();
  if(sz==0){ GG_LOG_HTTP("IMG decode: size 0"); imageDecoded=true; f.close(); return; }
  if(sz>600*1024){ GG_LOG_HTTP("IMG decode: too large %d (>600KB)", (int)sz); imageDecoded=true; f.close(); return; }
  std::unique_ptr<uint8_t[]> buf(new uint8_t[sz]);
  size_t rd = f.read(buf.get(), sz); if(rd!=sz){ GG_LOG_HTTP("IMG decode: read mismatch %d/%d", (int)rd,(int)sz); }
  if(lastImageFormat==IMG_FMT_JPEG){
    if(!(buf[0]==0xFF && buf[1]==0xD8)){ GG_LOG_HTTP("IMG decode: missing SOI (fmt JPEG but magic %02X %02X)", buf[0], buf[1]); imageDecoded=true; f.close(); return; }
    
    // Progressive attempts to initialize JPEG decoder
    uint16_t fw=0, fh=0; 
    int result = -1;
    
    // Log the exact JPEG header for debugging
    GG_LOG_HTTP("JPEG header analysis: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", 
                buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
    
    // Attempt 1: Clean state  
    result = TJpgDec.getJpgSize(&fw,&fh, buf.get(), sz);
    GG_LOG_HTTP("JPEG attempt 1 (clean): result=%d (w=%d h=%d)", result, fw, fh);
    
    if(result != 0 && fw == 0){
      // Attempt 2: With callback set
      fw=0; fh=0;
      TJpgDec.setCallback(tftJpgDrawCallback);
      result = TJpgDec.getJpgSize(&fw,&fh, buf.get(), sz);
      GG_LOG_HTTP("JPEG attempt 2 (callback): result=%d (w=%d h=%d)", result, fw, fh);
    }
    if(result != 0 && fw == 0){
      // Attempt 3: With swap bytes enabled
      fw=0; fh=0;
      TJpgDec.setSwapBytes(true);
      result = TJpgDec.getJpgSize(&fw,&fh, buf.get(), sz);
      GG_LOG_HTTP("JPEG attempt 3 (swap): result=%d (w=%d h=%d)", result, fw, fh);
    }
    if(result != 0 && fw == 0){
      // Attempt 4: Reset everything and try minimal setup
      fw=0; fh=0;
      TJpgDec.setSwapBytes(false);
      TJpgDec.setCallback(nullptr);
      result = TJpgDec.getJpgSize(&fw,&fh, buf.get(), sz);
      GG_LOG_HTTP("JPEG attempt 4 (minimal): result=%d (w=%d h=%d)", result, fw, fh);
    }
    
    // Accept if we got valid dimensions, regardless of return code
    bool sizeOk = (fw > 0 && fh > 0 && fw <= 2048 && fh <= 2048);
    GG_LOG_HTTP("JPEG size check: %s (fw=%d fh=%d result=%d)", sizeOk?"ACCEPT":"REJECT", fw, fh, result);
    
    if(!sizeOk){ 
      GG_LOG_HTTP("IMG decode: ALL JPEG METHODS FAILED (size=%d, file seems intact)", (int)sz); 
      // Try to use the image as a colored block instead
      imageColor = tft.color565(100, 150, 200); // Blue placeholder
      imageDecoded=true; f.close(); return; 
    } 
    jpgFullW=fw; jpgFullH=fh;
    int scale=1; while(scale<8 && (jpgFullW/(scale*2))>=60 && (jpgFullH/(scale*2))>=36) scale*=2; 
    TJpgDec.setJpgScale(scale);
    jpgScaledW = jpgFullW/scale; jpgScaledH = jpgFullH/scale;
  GG_LOG_HTTP("IMG size %dx%d raw=%dB scale=%d -> %dx%d (JPEG) target=%dx%d", jpgFullW, jpgFullH, (int)sz, scale, jpgScaledW, jpgScaledH, TARGET_IMG_W, TARGET_IMG_H);
  if(jpgFullW!=TARGET_IMG_W || jpgFullH!=TARGET_IMG_H) GG_LOG_HTTP("IMG warn: expected %dx%d", TARGET_IMG_W, TARGET_IMG_H);
    memset(imageThumb,0xFF,sizeof(imageThumb)); // Initialize to white
    int r = TJpgDec.drawJpg(0,0, buf.get(), sz);
    if(r==0 || r==1){ imageDecoded=true; imageThumbReady=true; GG_LOG_HTTP("IMG decode OK JPEG (%d bytes r=%d)", (int)sz,r); }
  else { GG_LOG_HTTP("IMG decode fail code %d fw=%d fh=%d scale=%d bytes=%d", r, jpgFullW, jpgFullH, scale, (int)sz); }
  }
#else // !USE_TJPG_DECODER
    // No decoder compiled: simple colored rectangle
    tft.fillRect(260,0,60,36,imageColor); tft.drawRect(260,0,60,36,TH.scale_pointer); imageDecoded=true;
#endif
    // PNG path (independent of JPEG decoder) - build thumbnail simple nearest scaling
#ifdef USE_PNG_DECODER
    if(!imageDecoded && lastImageFormat==IMG_FMT_PNG){
      f.seek(0);
      size_t psz = f.size(); if(psz>0 && psz<600*1024){ std::unique_ptr<uint8_t[]> pbuf(new uint8_t[psz]); size_t prd=f.read(pbuf.get(),psz); if(prd==psz){
        if(png.openFLASH((uint8_t*)pbuf.get(), psz, nullptr)==PNG_SUCCESS){
          uint16_t pw=png.getWidth(); uint16_t ph=png.getHeight(); 
          int pixelType = png.getPixelType();
          int bpp = png.getBpp();
          GG_LOG_HTTP("PNG thumb info: %dx%d type=%d bpp=%d", pw, ph, pixelType, bpp);
          jpgFullW=pw; jpgFullH=ph; // reuse vars
          memset(imageThumb,0xFF,sizeof(imageThumb)); // Initialize to white instead of black
          struct ThumbDrawCtx { uint16_t *thumb; int tw,th; int srcH; };
          static ThumbDrawCtx tctx; tctx.thumb=imageThumb; tctx.tw=60; tctx.th=36; tctx.srcH=ph;
          void (*pcb)(PNGDRAW*) = [](PNGDRAW *pd){
            ThumbDrawCtx *C=(ThumbDrawCtx*)pd->pUser;
            int w=pd->iWidth; int y=pd->y;
            int ty = (int)((uint32_t)y * C->th / C->srcH);
            if(ty>=C->th) return; 
            uint16_t *row=C->thumb + ty*C->tw;
            
            // Robust PNG pixel format handling for thumbnails (avoid flower screen)
            uint8_t *pixels = (uint8_t*)pd->pPixels;
            int pixelType = pd->iPixelType; // 统一当作 24bit RGB 处理（文档要求）
            int bytesPerPixel = (pixelType==6)?4:3; // RGBA or RGB
            if(pd->iWidth==480 && C->tw==60 && C->th==36 && C->srcH==288){
              // Exact /8 downsample: pick near center pixel of each 8x8 block
              for(int bx=0; bx<60; ++bx){ int sx = bx*8 + 4; if(sx>=pd->iWidth) sx=pd->iWidth-1; int idx = sx*bytesPerPixel; uint8_t r=pixels[idx], g=pixels[idx+1], b=pixels[idx+2]; row[bx] = ((r>>3)<<11) | ((g>>2)<<5) | (b>>3); }
            } else {
              for(int sx=0; sx<w; sx++){ int tx=(int)((uint32_t)sx * C->tw / w); if(tx>=C->tw) continue; int idx = sx*bytesPerPixel; uint8_t r=pixels[idx], g=pixels[idx+1], b=pixels[idx+2]; row[tx] = ((r>>3)<<11) | ((g>>2)<<5) | (b>>3); }
            }
          };
          png.close(); // reopen with callback
          if(png.openFLASH((uint8_t*)pbuf.get(), psz, pcb)==PNG_SUCCESS){ 
            png.decode(&tctx,0); 
            imageDecoded=true; imageThumbReady=true; 
            // Debug: check first few pixels
            GG_LOG_HTTP("IMG decode OK PNG %dx%d -> thumb 60x36, first pixels: %04X %04X %04X", pw,ph, imageThumb[0], imageThumb[1], imageThumb[2]); 
          }
          png.close();
        } else { GG_LOG_HTTP("IMG PNG open fail"); }
      }}
    }
#endif
    f.close();
    return;
  }

  // Case 2: base64 kept in memory
  if(!lastImageB64.length()) return;
#ifdef USE_TJPG_DECODER
  {
    std::unique_ptr<uint8_t[]> bin; size_t sz = b64decode(lastImageB64, bin);
    if(sz>0){
  if(sz>=2 && bin[0]==0xFF && bin[1]==0xD8) lastImageFormat=IMG_FMT_JPEG; else if(sz>=8 && bin[0]==0x89 && bin[1]==0x50 && bin[2]==0x4E && bin[3]==0x47) lastImageFormat=IMG_FMT_PNG; else lastImageFormat=IMG_FMT_NONE;
  if(lastImageFormat==IMG_FMT_JPEG){
  TJpgDec.setCallback(tftJpgDrawCallback);
  uint16_t fw=0, fh=0; TJpgDec.getJpgSize(&fw,&fh, bin.get(), sz); jpgFullW=fw; jpgFullH=fh;
  int scale=1; while((jpgFullW/(scale*2))>=60 && (jpgFullH/(scale*2))>=36 && scale<8) scale*=2; if(scale>1) TJpgDec.setJpgScale(scale);
  jpgScaledW = jpgFullW/scale; jpgScaledH = jpgFullH/scale;
  GG_LOG_HTTP("IMG size %dx%d (b64) scale=%d -> %dx%d target=%dx%d", jpgFullW, jpgFullH, scale, jpgScaledW, jpgScaledH, TARGET_IMG_W, TARGET_IMG_H);
  if(jpgFullW!=TARGET_IMG_W || jpgFullH!=TARGET_IMG_H) GG_LOG_HTTP("IMG warn: expected %dx%d", TARGET_IMG_W, TARGET_IMG_H);
  memset(imageThumb,0xFF,sizeof(imageThumb));
  int r = TJpgDec.drawJpg(0,0, bin.get(), sz);
  if(r==0 || r==1){ imageDecoded=true; imageThumbReady=true; GG_LOG_HTTP("IMG decode b64 OK (%d bytes r=%d)", (int)sz,r); return; }
      else GG_LOG_HTTP("IMG decode b64 fail %d", r);
  }
    }
  }
#endif
#ifdef USE_PNG_DECODER
  if(!imageDecoded){
    std::unique_ptr<uint8_t[]> bin; size_t sz=b64decode(lastImageB64, bin);
    if(sz>0){
      if(sz>=8 && bin[0]==0x89 && bin[1]==0x50 && bin[2]==0x4E && bin[3]==0x47){
        lastImageFormat=IMG_FMT_PNG;
        if(png.openFLASH((uint8_t*)bin.get(), sz, nullptr)==PNG_SUCCESS){ uint16_t pw=png.getWidth(); uint16_t ph=png.getHeight(); png.close();
          struct B64PngThumbCtx { uint16_t *thumb; int tw,th; int srcH; };
          static B64PngThumbCtx btctx; btctx.thumb=imageThumb; btctx.tw=60; btctx.th=36; btctx.srcH=ph; memset(imageThumb,0xFF,sizeof(imageThumb)); // Initialize to white
          void (*tcb)(PNGDRAW*) = [](PNGDRAW *pd){ B64PngThumbCtx *C=(B64PngThumbCtx*)pd->pUser; uint16_t *src=(uint16_t*)pd->pPixels; int w=pd->iWidth; int y=pd->y; int ty=(int)((uint32_t)y * C->th / C->srcH); if(ty>=C->th) return; uint16_t *row=C->thumb + ty*C->tw; for(int sx=0;sx<w;sx++){ int tx=(int)((uint32_t)sx * C->tw / w); if(tx<C->tw) row[tx]=src[sx]; } };
          if(png.openFLASH((uint8_t*)bin.get(), sz, tcb)==PNG_SUCCESS){ png.decode(&btctx,0); png.close(); imageDecoded=true; imageThumbReady=true; GG_LOG_HTTP("IMG decode b64 PNG -> thumb 60x36"); }
        }
      }
    }
  }
#endif
  if(!imageDecoded){
    // Fallback hash to color
    uint32_t h=0; for(size_t i=0;i<lastImageB64.length(); i+= (lastImageB64.length()/32)+1) h = (h*131) + lastImageB64[i];
    uint8_t r=(h>>16)&0xFF, g=(h>>8)&0xFF, b=h&0xFF; imageColor = tft.color565(r,g,b); imageDecoded=true;
  }
}

// Draw fullscreen image (JPEG or PNG) with aspect-fit
static void drawFullscreenImage(){
  spr.fillSprite(TH.bg);
  spr.setTextDatum(TL_DATUM);
  bool drew=false;
#ifdef USE_TJPG_DECODER
#if defined(TJPGDECODER_H) || defined(_TJPG_DECODER_H_) || defined(JPEGDECODER_H)
  if(haveImage && lastImageFormat==IMG_FMT_JPEG){
    File f=LittleFS.open(currentProject.dir+"/last_image.bin","r");
    if(f){ size_t sz=f.size(); if(sz>0 && sz<900*1024){ std::unique_ptr<uint8_t[]> buf(new uint8_t[sz]); size_t rd=f.read(buf.get(),sz); if(rd==sz){
      if(buf[0]==0xFF && buf[1]==0xD8){ uint16_t fw=0,fh=0; if(TJpgDec.getJpgSize(&fw,&fh,buf.get(),sz)){
        uint32_t sig = ((uint32_t)fw<<16) ^ ((uint32_t)fh) ^ (uint32_t)sz;
        bool needDecode = fullscreenNeedsRedraw || (sig!=fsLastSig);
        if(needDecode){ int dscale=1; while(dscale<8 && (fw/(dscale*2))>=320 && (fh/(dscale*2))>=180) dscale*=2; if(dscale>1) TJpgDec.setJpgScale(dscale);
          jpgFullW=fw; jpgFullH=fh; jpgScaledW=fw/dscale; jpgScaledH=fh/dscale; fsScaledW=jpgScaledW; fsScaledH=jpgScaledH; fsBaseX=(320-fsScaledW)/2; fsBaseY=(180-fsScaledH)/2; if(fsBaseX<0) fsBaseX=0; if(fsBaseY<0) fsBaseY=0;
          fullscreenDecodeActive=true; tft.startWrite(); tft.fillRect(0,0,320,180,TH.bg); TJpgDec.setCallback(tftJpgDrawCallback); int rr= TJpgDec.drawJpg(fsBaseX,fsBaseY,buf.get(),sz); tft.endWrite(); fullscreenDecodeActive=false; GG_LOG_HTTP("FS decode %dx%d scale=%d -> %dx%d r=%d", fw,fh,dscale, jpgScaledW,jpgScaledH, rr); if(rr==0||rr==1){ imageThumbReady=true; imageDecoded=true; fsLastSig=sig; drew=true; }
          fullscreenNeedsRedraw=false; fsCachedLogged=false;
        } else { drew=true; if(!fsCachedLogged){ GG_LOG_HTTP("FS JPEG using cached decode"); fsCachedLogged=true; } }
      }
    }} f.close(); }
  }
#endif
#endif
#ifdef USE_PNG_DECODER
  if(!drew && haveImage && lastImageFormat==IMG_FMT_PNG){
    File f=LittleFS.open(currentProject.dir+"/last_image.bin","r");
    if(f){ size_t sz=f.size(); if(sz>0 && sz<900*1024){ std::unique_ptr<uint8_t[]> buf(new uint8_t[sz]); size_t rd=f.read(buf.get(),sz); if(rd==sz){
      if(buf[0]==0x89 && buf[1]==0x50 && buf[2]==0x4E && buf[3]==0x47){ 
        if(png.openFLASH((uint8_t*)buf.get(), sz, nullptr)==PNG_SUCCESS){ 
          uint16_t pw=png.getWidth(); uint16_t ph=png.getHeight(); 
          int pixelType = png.getPixelType();
          int bpp = png.getBpp();
          uint32_t sig = ((uint32_t)pw<<16) ^ ((uint32_t)ph) ^ (uint32_t)sz;
          bool needDecode = fullscreenNeedsRedraw || (sig!=fsLastSig);
          if(needDecode){ GG_LOG_HTTP("PNG info: %dx%d type=%d bpp=%d", pw, ph, pixelType, bpp); }
          png.close();
          if(needDecode){
            float sx = 320.0f/pw; float sy=180.0f/ph; float s=min(sx,sy); int dw=(int)(pw*s); int dh=(int)(ph*s); int ox=(320-dw)/2; int oy=(180-dh)/2; jpgScaledW=dw; jpgScaledH=dh; fsBaseX=ox; fsBaseY=oy;
            struct FSpngCtx { int dw,dh; int ox,oy; int pw,ph; };
            static FSpngCtx fctx; fctx.dw=dw; fctx.dh=dh; fctx.ox=ox; fctx.oy=oy; fctx.pw=pw; fctx.ph=ph;
            void (*fcb)(PNGDRAW*) = [](PNGDRAW *pd){ 
              FSpngCtx *C=(FSpngCtx*)pd->pUser; 
              int y=pd->y; 
              int dy=C->oy + (int)((uint32_t)y * C->dh / C->ph); 
              if(dy>=180) return; 
              static uint16_t lineBuf[320]; 
              static bool loggedFirst = false; if(!loggedFirst){ GG_LOG_HTTP("PNG FS: y=%d iBpp=%d iPixelType=%d iWidth=%d", pd->y, pd->iBpp, pd->iPixelType, pd->iWidth); loggedFirst=true; }
              uint8_t *pixels = (uint8_t*)pd->pPixels; int bytesPerPixel = (pd->iPixelType==6)?4:3; 
              for(int x=0; x<C->dw && x<320; x++){ int sx = (int)((uint32_t)x * C->pw / C->dw); if(sx >= pd->iWidth) { lineBuf[x]=0; continue; } int idx = sx*bytesPerPixel; uint8_t r=pixels[idx], g=pixels[idx+1], b=pixels[idx+2]; lineBuf[x] = ((r>>3)<<11) | ((g>>2)<<5) | (b>>3); }
              tft.pushImage(C->ox, dy, C->dw, 1, lineBuf);
            };
            if(png.openFLASH((uint8_t*)buf.get(), sz, fcb)==PNG_SUCCESS){ tft.startWrite(); tft.fillRect(0,0,320,180,TH.bg); png.decode(&fctx,0); tft.endWrite(); drew=true; imageDecoded=true; imageThumbReady=true; fsLastSig=sig; fullscreenNeedsRedraw=false; fsCachedLogged=false; GG_LOG_HTTP("FS decode PNG %dx%d -> %dx%d", pw,ph,dw,dh); }
            png.close();
          } else { drew=true; if(!fsCachedLogged){ GG_LOG_HTTP("FS PNG using cached decode"); fsCachedLogged=true; } }
        }
      }
    }} f.close(); }
  }
#endif
  if(drew){
    // Avoid overwriting decoded fullscreen image with sprite; draw footer directly
    tft.setTextColor(TH.menu_param, TH.bg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Click=Back Long=Menu",4,182,2);
    return;
  }
  // Fallback / placeholder rendering via sprite
  spr.fillSprite(TH.bg);
  if(!haveImage){ spr.setTextColor(TFT_RED,TH.bg); spr.drawString("No Image",4,4,2); }
  else if(imageThumbReady){ for(int ty=0; ty<36; ++ty){ for(int tx=0; tx<60; ++tx){ uint16_t c=imageThumb[ty*60+tx]; int x=(tx*320)/60; int x2=((tx+1)*320)/60; int y=(ty*180)/36; int y2=((ty+1)*180)/36; tft.fillRect(x,y,max(1,x2-x),max(1,y2-y),c);} } }
  else if(imageDecoded){ tft.fillRect(0,0,320,180,imageColor); }
  spr.setTextColor(TH.menu_param,TH.bg); spr.drawString("Click=Back Long=Menu",4,182,2); spr.pushSprite(0,0);
}

static void requestAI() {
  if(fetching || summarizePending) return; // don't mix
  aiRetryScheduled=false;
  fetching=true; lastFetchTs=millis(); fetchStatus="Querying LLM..."; lastAIText=""; imagePrompt=""; lastAIReasoning=""; lastAIMemoryAdd=""; parseRetryCount=0;
  DynamicJsonDocument jd(12288); jd["model"] = AI_CHAT_MODEL; JsonArray msgs = jd.createNestedArray("messages");
  JsonObject sys = msgs.createNestedObject(); sys["role"]="system"; sys["content"]=buildSystemPrompt();
  for(auto &e: context) { JsonObject m=msgs.createNestedObject(); m["role"]=e.role; m["content"]=e.content; }
  JsonObject userQ = msgs.createNestedObject(); userQ["role"]="user"; userQ["content"]="Continue the interactive visual novel. Produce JSON ONLY (no markdown).";
  String payload; serializeJson(jd,payload);
  String resp; bool ok = httpPostJSON(AI_CHAT_URL, payload, resp);
  int statusCode = -1; if(resp.length()){ int nl=resp.indexOf('\n'); if(nl>0) statusCode = resp.substring(0,nl).toInt(); }
  if(!ok){
    bool retryable = (statusCode==429) || (statusCode>=500 && statusCode<600) || statusCode==-1;
    int base = (statusCode==429? 600: 250);
    int backoffMs = base * (1 << min(retryCount,5)); backoffMs += random(0,120);
    fetchStatus = String("Err ")+statusCode+ (retryable? " sched ":" halt ")+ String(retryCount+1)+" @ "+backoffMs/1000.0+"s";
    fetching=false; if(retryable && retryCount++<MAX_RETRY){ aiRetryScheduled=true; aiRetryDueMs=millis()+backoffMs; } else { retryCount=0; }
    return;
  }
  // Extract body without status line
  int nl=resp.indexOf('\n'); String body = (nl>0)? resp.substring(nl+1):resp;
  retryCount=0; DynamicJsonDocument dr(12288); DeserializationError e = deserializeJson(dr, body);
  if(e){ fetchStatus="Top JSON Err"; fetching=false; return; }
  if(dr.containsKey("error")) { JsonVariant ev=dr["error"]; if(ev.is<JsonObject>()) { lastErrorField = ev["message"].as<String>(); fetchStatus = "API err:"+lastErrorField; fetching=false; return; } }
  if(!dr["choices"].is<JsonArray>()){ fetchStatus="No choices"; fetching=false; return; }
  JsonVariant msg = dr["choices"][0]["message"]["content"]; if(msg.isNull()){ fetchStatus="No content"; fetching=false; return; }
  String content = msg.as<String>(); if(content.length()<5){ fetchStatus="Short resp"; fetching=false; return; }
  // Attempt to extract JSON fragment
  bool parseSuccess=false; pendingOptions.clear();
  String jsonFrag = extractJsonFromContent(content);
  if(jsonFrag.length()==0){ int b1=content.indexOf('{'); int b2=content.lastIndexOf('}'); if(b1>=0 && b2>b1) jsonFrag=content.substring(b1,b2+1); }
  DynamicJsonDocument jt(12288); DeserializationError pe = deserializeJson(jt,jsonFrag); lastAIReasoning=""; lastAIMemoryAdd="";
  bool haveText=false, haveOpts=false, haveMem=false, haveImg=false;
  if(!pe && jt.is<JsonObject>()){
    if(jt.containsKey("text")){ lastAIText = jt["text"].as<String>(); haveText = lastAIText.length()>0; }
    if(jt.containsKey("img_prompt")){ imagePrompt = jt["img_prompt"].as<String>(); haveImg = imagePrompt.length()>0; }
    else if(jt.containsKey("image_prompt")){ imagePrompt=jt["image_prompt"].as<String>(); haveImg = imagePrompt.length()>0; }
    if(jt["options"].is<JsonArray>()){
      for(JsonVariant v: jt["options"].as<JsonArray>()) { String o=v.as<String>(); if(o.length()>0 && o.length()<40) pendingOptions.push_back(o); if(pendingOptions.size()>=3) break; }
      haveOpts = !pendingOptions.empty();
    }
    if(jt["memory_add"].is<JsonArray>()){
      for(JsonVariant mv: jt["memory_add"].as<JsonArray>()){
        String mem = mv.as<String>(); if(mem.length()>0 && mem.length()<120){ if(!knowledgeBase.endsWith(mem)) knowledgeBase += (knowledgeBase.length()?"\n":"") + mem; lastAIMemoryAdd += (lastAIMemoryAdd.length()?" \n":"") + mem; }
      }
      if(knowledgeBase.length()>6000) knowledgeBase.remove(0, knowledgeBase.length()-6000);
      haveMem = lastAIMemoryAdd.length()>0;
    }
    if(jt.containsKey("reasoning")) lastAIReasoning = jt["reasoning"].as<String>();
    parseSuccess = haveText && haveOpts && haveMem && haveImg; // 全部关键字段都要
  }
  if(!parseSuccess){
    GG_LOG_ERROR("Parse fail %d", parseRetryCount);
    fetching=false; fetchStatus="Bad JSON, Retrying...";
    if(parseRetryCount < MAX_PARSE_RETRY){ parseRetryCount++; aiRetryScheduled=true; aiRetryDueMs=millis()+250; return; }
    lastAIText = "(Parse failed)"; // fallback text
  }
  if(pendingOptions.empty()) fetchStatus+=" | no opts"; else fetchStatus += " | "+String(pendingOptions.size())+" opt";
  optionIdx=0; appendContext("assistant", lastAIText); sessionLog += lastAIText + "\n";
  if(imagePrompt.length()){
    fetchStatus="Gen Img..."; ggUILogOverlayf("IMG start %d", imagePrompt.length()); mirrorOverlayToStatus();
    haveImage=false; imageDecoded=false; lastImageB64="";
  DynamicJsonDocument ji(4096); ji["model"]=AI_IMG_MODEL; ji["prompt"]=imagePrompt; ji["image_size"]="480x288"; ji["num_inference_steps"]=20; ji["guidance_scale"]=7.5; ji["batch_size"]=1; // Kolors spec (480x288)
    String ip; serializeJson(ji,ip); ggUILogOverlayf("IMG post %d", ip.length()); mirrorOverlayToStatus();
    String ir; if(httpPostJSON(AI_IMAGE_URL, ip, ir)) {
      ggUILogOverlayf("IMG resp %d", ir.length()); mirrorOverlayToStatus();
      int nli=ir.indexOf('\n'); String bodyi=(nli>0)? ir.substring(nli+1):ir; // 修正变量名错误
      File f=LittleFS.open(currentProject.dir+"/last_image.json","w"); if(f){ f.print(bodyi); f.close(); }
      parseAndStoreImage(bodyi);
      ggUILogOverlayf("IMG flags img=%d url=%d b64len=%d", haveImage?1:0, imageFromUrl?1:0, lastImageB64.length()); mirrorOverlayToStatus();
      if(haveImage && lastImageB64.length()) {
        writeImageFileFromB64(lastImageB64); buildThumbnails(); ggUILogOverlayf("IMG b64 stored"); mirrorOverlayToStatus();
      } else if(haveImage && imageFromUrl){
        ggUILogOverlayf("IMG url fetched"); mirrorOverlayToStatus();
        tryDecodeImage(); // 立即尝试解码URL图片
      } else {
        ggUILogOverlayf("IMG none after parse"); mirrorOverlayToStatus();
      }
    } else { ggUILogOverlayf("IMG netfail"); mirrorOverlayToStatus(); }
  if(haveImage) fetchStatus = String("Img OK ") + String((unsigned long)lastImageBytes) + "B"; else if(fetchStatus.startsWith("Gen Img")) fetchStatus = "Img Fail"; // 保留结果
  } else {
    ggUILogOverlayf("IMG skip no prompt"); mirrorOverlayToStatus();
    fetchStatus="OK";
  }
  if(!imagePrompt.length() || haveImage || (!haveImage && imagePrompt.length())) { fetching=false; saveMeta(); }
}

static void newProjectGenerate() {
  // Simple single-shot name + synopsis generation using genre
  if(fetching) return; 
  fetching=true; fetchStatus="Generating Project...";
  
  GG_LOG_STATE("=== PROJECT GENERATION START ===");
  GG_LOG_STATE("Selected genre: %s (index: %d)", genres[genreIdx].c_str(), genreIdx);
  GG_LOG_STATE("Current project name before: '%s'", currentProject.name.c_str());
  GG_LOG_STATE("Current project synopsis before: '%s'", currentProject.synopsis.c_str());
  
  // Clear previous project data
  currentProject.name = "";
  currentProject.synopsis = "";
  
  DynamicJsonDocument jd(4096);
  jd["model"]=AI_CHAT_MODEL;
  JsonArray msgs = jd.createNestedArray("messages");
  JsonObject sys=msgs.createNestedObject(); 
  sys["role"]="system"; 
  sys["content"]="You generate concise game meta.";
  
  String userPrompt = String("Create a compelling English visual novel title (<35 chars) and a 1 sentence synopsis for genre: ")+genres[genreIdx]+". Return as JSON: {name:'',synopsis:''}";
  JsonObject u=msgs.createNestedObject(); 
  u["role"]="user"; 
  u["content"] = userPrompt;
  
  GG_LOG_JSON("System prompt: %s", sys["content"].as<String>().c_str());
  GG_LOG_JSON("User prompt: %s", userPrompt.c_str());
  GG_LOG_JSON("Using model: %s", AI_CHAT_MODEL);
  
  String payload; 
  size_t payloadSize = serializeJson(jd,payload);
  GG_LOG_HTTP("Serialized payload size: %zu bytes", payloadSize);
  GG_LOG_HTTP("Request payload: %s", payload.c_str());
  
  String resp;
  GG_LOG_HTTP("Calling httpPostJSON...");
  bool ok=httpPostJSON(AI_CHAT_URL,payload,resp);
  GG_LOG_HTTP("httpPostJSON returned: %s, response length: %d", ok ? "SUCCESS" : "FAILED", resp.length());
  
  if(ok) { 
    GG_LOG_HTTP("Full raw response: %s", resp.c_str());
    
    // Parse HTTP status line + body
    int nlPos = resp.indexOf('\n');
    String statusLine = (nlPos > 0) ? resp.substring(0, nlPos) : "";
    String body = (nlPos > 0) ? resp.substring(nlPos + 1) : resp;
    
    GG_LOG_HTTP("Status line: '%s'", statusLine.c_str());
    GG_LOG_HTTP("Response body: %s", body.c_str());
    
    if(body.length() == 0) {
      GG_LOG_ERROR("Empty response body");
      fetching=false; 
      fetchStatus = "Empty response";
      GG_LOG_STATE("=== PROJECT GENERATION END (empty body) ===");
      return;
    }
    
    DynamicJsonDocument dr(8192); 
    DeserializationError err = deserializeJson(dr, body);
    
    if(err) {
      GG_LOG_ERROR("JSON parsing failed: %s", err.c_str());
      GG_LOG_ERROR("Failed to parse body: %s", body.c_str());
      fetching=false; 
      fetchStatus = "JSON parse error";
      GG_LOG_STATE("=== PROJECT GENERATION END (JSON error) ===");
      return;
    }
    
    GG_LOG_JSON("JSON parsing succeeded, doc size: %zu", dr.memoryUsage());
    
    // Check for API error
    if(dr.containsKey("error")) {
      JsonVariant errorVar = dr["error"];
      if(errorVar.is<JsonObject>()) {
        String errorMsg = errorVar["message"].as<String>();
        String errorType = errorVar["type"].as<String>();
        GG_LOG_ERROR("API returned error - type: %s, message: %s", errorType.c_str(), errorMsg.c_str());
      } else {
        GG_LOG_ERROR("API returned error: %s", errorVar.as<String>().c_str());
      }
      fetching=false; 
      fetchStatus = "API error";
      GG_LOG_STATE("=== PROJECT GENERATION END (API error) ===");
      return;
    }
    
    // Check choices array
    if(!dr.containsKey("choices")) {
      GG_LOG_ERROR("Response missing 'choices' key");
      GG_LOG_ERROR("Available keys:");
      for(JsonPair kv : dr.as<JsonObject>()) {
        GG_LOG_ERROR("  - %s", kv.key().c_str());
      }
      fetching=false; 
      fetchStatus = "No choices key";
      GG_LOG_STATE("=== PROJECT GENERATION END (no choices) ===");
      return;
    }
    
    if(!dr["choices"].is<JsonArray>()) {
      GG_LOG_ERROR("'choices' is not an array");
      fetching=false; 
      fetchStatus = "Choices not array";
      GG_LOG_STATE("=== PROJECT GENERATION END (choices type) ===");
      return;
    }
    
    JsonArray choices = dr["choices"].as<JsonArray>();
    if(choices.size() == 0) {
      GG_LOG_ERROR("Choices array is empty");
      fetching=false; 
      fetchStatus = "Empty choices";
      GG_LOG_STATE("=== PROJECT GENERATION END (empty choices) ===");
      return;
    }
    
    GG_LOG_JSON("Found %d choices", choices.size());
    
    JsonObject firstChoice = choices[0].as<JsonObject>();
    if(!firstChoice.containsKey("message")) {
      GG_LOG_ERROR("First choice missing 'message' key");
      fetching=false; 
      fetchStatus = "No message key";
      GG_LOG_STATE("=== PROJECT GENERATION END (no message) ===");
      return;
    }
    
    JsonObject message = firstChoice["message"].as<JsonObject>();
    if(!message.containsKey("content")) {
      GG_LOG_ERROR("Message missing 'content' key");
      fetching=false; 
      fetchStatus = "No content key";
      GG_LOG_STATE("=== PROJECT GENERATION END (no content) ===");
      return;
    }
    
    String content = message["content"].as<String>();
    GG_LOG_JSON("AI content length: %d", content.length());
    GG_LOG_JSON("AI content: %s", content.c_str());
    
    if(content.length() < 10) {
      GG_LOG_ERROR("Content too short: %d chars", content.length());
      fetching=false; 
      fetchStatus = "Content too short";
      GG_LOG_STATE("=== PROJECT GENERATION END (short content) ===");
      return;
    }
    
    // Extract JSON from content using enhanced parser
    String jsonFrag = extractJsonFromContent(content);
    
    if(jsonFrag.length() == 0) {
      GG_LOG_ERROR("No valid JSON found in AI response");
      GG_LOG_ERROR("Original content: %s", content.c_str());
      fetching=false; 
      fetchStatus = "No JSON found";
      GG_LOG_STATE("=== PROJECT GENERATION END (no JSON) ===");
      return;
    }
    
    GG_LOG_JSON("Using extracted JSON: %s", jsonFrag.c_str());
    
    DynamicJsonDocument jm(1024); 
    DeserializationError metaErr = deserializeJson(jm, jsonFrag);
    
    if(metaErr) {
      GG_LOG_ERROR("Meta JSON parse failed: %s", metaErr.c_str());
      GG_LOG_ERROR("Failed fragment: %s", jsonFrag.c_str());
      fetching=false; 
      fetchStatus = "Meta parse error";
      GG_LOG_STATE("=== PROJECT GENERATION END (meta parse) ===");
      return;
    }
    
    GG_LOG_JSON("Meta JSON parsed successfully");
    
    // Check for required fields
    if(!jm.containsKey("name")) {
      GG_LOG_ERROR("Meta JSON missing 'name' field");
      GG_LOG_ERROR("Available meta keys:");
      for(JsonPair kv : jm.as<JsonObject>()) {
        GG_LOG_ERROR("  - %s", kv.key().c_str());
      }
      fetching=false; 
      fetchStatus = "No name field";
      GG_LOG_STATE("=== PROJECT GENERATION END (no name) ===");
      return;
    }
    
    if(!jm.containsKey("synopsis")) {
      GG_LOG_ERROR("Meta JSON missing 'synopsis' field");
      fetching=false; 
      fetchStatus = "No synopsis field";
      GG_LOG_STATE("=== PROJECT GENERATION END (no synopsis) ===");
      return;
    }
    
    String extractedName = jm["name"].as<String>();
    String extractedSynopsis = jm["synopsis"].as<String>();
    
    GG_LOG_JSON("Extracted name: '%s' (length: %d)", extractedName.c_str(), extractedName.length());
    GG_LOG_JSON("Extracted synopsis: '%s' (length: %d)", extractedSynopsis.c_str(), extractedSynopsis.length());
    
    if(extractedName.length() == 0) {
      GG_LOG_ERROR("Extracted name is empty");
      fetching=false; 
      fetchStatus = "Empty name";
      GG_LOG_STATE("=== PROJECT GENERATION END (empty name) ===");
      return;
    }
    
    if(extractedSynopsis.length() == 0) {
      GG_LOG_ERROR("Extracted synopsis is empty");
      fetching=false; 
      fetchStatus = "Empty synopsis";
      GG_LOG_STATE("=== PROJECT GENERATION END (empty synopsis) ===");
      return;
    }
    
    // Success! Set the values
    currentProject.name = extractedName;
    currentProject.synopsis = extractedSynopsis;
    
    GG_LOG("Project generated successfully!");
    GG_LOG("  Name: %s", currentProject.name.c_str());
    GG_LOG("  Synopsis: %s", currentProject.synopsis.c_str());
    GG_LOG("  Genre: %s", genres[genreIdx].c_str());
    
  } else {
    GG_LOG_ERROR("HTTP request failed completely");
    GG_LOG_ERROR("Response received: %s", resp.c_str());
  }
  
  fetching=false; 
  fetchStatus = currentProject.name.length() ? "OK" : "Failed";
  GG_LOG_STATE("Project generation finished with status: %s", fetchStatus.c_str());
  GG_LOG_STATE("Final project name: '%s'", currentProject.name.c_str());
  GG_LOG_STATE("Final project synopsis: '%s'", currentProject.synopsis.c_str());
  GG_LOG_STATE("=== PROJECT GENERATION END ===");
}

void galgameInit() { 
  GG_LOG("=== GalGame Initialization Start ===");
  
  // Print serial command help on startup
  Serial.println("\n" + String('=', 50));
  Serial.println("ATS-MINI GalGame System Started");
  Serial.println("Serial Commands Available - Type HELP for details");  
  Serial.println(String('=', 50) + "\n");
  
  // JPEG decoder setup
#ifdef USE_TJPG_DECODER
  GG_LOG("Init JPEG decoder (TJpgDec)");
  TJpgDec.setSwapBytes(true); // TFT_eSPI usually needs byte swap
  TJpgDec.setCallback(tftJpgDrawCallback); // older versions use setCallback name
  GG_LOG("TJpgDec callbacks set");
#endif
  
  // 网络状态检查
  GG_LOG("Checking network status...");
  GG_LOG("WiFi status: %d (%s)", WiFi.status(),
    WiFi.status() == WL_CONNECTED ? "CONNECTED" :
    WiFi.status() == WL_NO_SSID_AVAIL ? "NO_SSID" :
    WiFi.status() == WL_CONNECT_FAILED ? "CONNECT_FAILED" :
    WiFi.status() == WL_CONNECTION_LOST ? "CONNECTION_LOST" :
    WiFi.status() == WL_DISCONNECTED ? "DISCONNECTED" : "OTHER");
    
  if(WiFi.status() == WL_CONNECTED) {
    GG_LOG("WiFi connected to: %s", WiFi.SSID().c_str());
    GG_LOG("IP Address: %s", WiFi.localIP().toString().c_str());
    GG_LOG("RSSI: %d dBm", WiFi.RSSI());
  } else {
    GG_LOG_ERROR("WiFi not connected! AI features will not work.");
  }
  
  // 确保NVS命名空间存在
  GG_LOG("Initializing preferences...");
  if(!prefs.begin(GG_PREFS_NS, false, STORAGE_PARTITION)) {
    GG_LOG_ERROR("Failed to create/open GalGame preferences namespace");
  } else {
    // 确保有默认值
    if(!prefs.isKey("maxRounds")) {
      prefs.putUChar("maxRounds", (uint8_t)maxContextRounds);
      GG_LOG("Set default maxRounds in preferences");
    }
    prefs.end();
    GG_LOG("Preferences initialized successfully");
  }
  
  // 确保文件系统目录存在
  GG_LOG("Setting up file system...");
  ensureBaseDir(); 
  
  // 加载项目列表
  GG_LOG("Loading projects...");
  loadProjects(); 
  GG_LOG("Loaded %d projects", projects.size());
  
  // API配置检查
  GG_LOG("API Configuration:");
  GG_LOG("  Chat URL: %s", AI_CHAT_URL);
  GG_LOG("  Image URL: %s", AI_IMAGE_URL);
  GG_LOG("  Chat Model: %s", AI_CHAT_MODEL);
  GG_LOG("  Image Model: %s", AI_IMG_MODEL);
  GG_LOG("  API Key length: %d", strlen(AI_API_KEY));
  GG_LOG("  Max context rounds: %d", maxContextRounds);
  
  GG_LOG("=== GalGame Initialization Complete ===");
}

void galgameEnter() {
  galgameLoadSettings(); galgameInit(); ggState=GG_START_MENU; projectMenuIdx=0; scrollOffset=0; }

bool galgameActive() { return ggState!=GG_IDLE; }

void galgameLeave() { ggState=GG_IDLE; }
// 运行时不允许修改 API Key, 保留空实现以兼容旧调用
void galgameSetApiKey(const String &key){ (void)key; }
static void setMaxRounds(int r){ maxContextRounds = constrain(r,1,40); galgamePersistSettings(); }
// External wrappers
int galgameProjectCount(){ return projects.size(); }
String galgameProjectName(int idx){ if(idx<0||idx>=(int)projects.size()) return String(); return projects[idx].name; }
bool galgameOpenProjectIndex(int idx){ if(idx<0||idx>=(int)projects.size()) return false; loadMeta(projects[idx]); ggState=GG_RUNNING; lastAIText=""; requestAI(); return true; }
bool galgameDeleteProjectIndex(int idx){ if(idx<0||idx>=(int)projects.size()) return false; auto p=projects[idx]; deleteProjectRecursive(p.dir); loadProjects(); return true; }
void galgameSetMaxRounds(int r){ setMaxRounds(r); }
void galgameTriggerSummarize(){ startSummarizeAsync(); }

static void createProjectDir() {
  if(currentProject.name=="") currentProject.name="Untitled";
  
  String safeName = safeFileName(currentProject.name + "_" + String(millis()));
  currentProject.dir = baseDir + "/" + safeName;
  
  GG_LOG("Creating project directory: %s", currentProject.dir.c_str());
  
  if(!LittleFS.mkdir(currentProject.dir)) {
    GG_LOG_ERROR("Failed to create project directory: %s", currentProject.dir.c_str());
    return;
  }
  
  knowledgeBase=""; 
  context.clear(); 
  appendContext("user","Game starts."); 
  
  GG_LOG("Project directory created successfully");
  saveMeta();
}

static void drawFrameBox(int x,int y,int w,int h,uint16_t color){ spr.drawSmoothRoundRect(x,y,4,4,w,h,color,TH.bg); }

static void drawScrollingText(int x,int y,int w,int h,const String &text,int offset){
  spr.setTextDatum(TL_DATUM); spr.setTextColor(TH.text,TH.bg); int lineH=14; int visible= h/lineH; int printed=0; String tmp=text; tmp.replace("\r"," ");
  int startLine=offset; int curLine=0; String line=""; for(size_t i=0;i<tmp.length();++i){ char c=tmp[i]; if(c=='\n'){ if(curLine>=startLine && printed<visible) spr.drawString(line,x,y+lineH*printed,2),printed++; line=""; curLine++; }
    else { line += c; if(line.length()> (size_t)(w/8)) { if(curLine>=startLine && printed<visible) spr.drawString(line,x,y+lineH*printed,2),printed++; line=""; curLine++; } }
  }
  if(line.length() && curLine>=startLine && printed<visible) spr.drawString(line,x,y+lineH*printed,2);
  int totalLines = curLine+1; if(totalLines>visible){ spr.fillRect(x+w+2,y,5,h,TH.menu_bg); int barH = max(8, h * visible / totalLines); int barY = y + (h-barH)* startLine / max(1,(totalLines-visible)); spr.fillRoundRect(x+w+2,barY,5,barH,2,TH.menu_param);} }

void galgameDraw() {
  if(!galgameActive()) return; spr.fillSprite(TH.bg);
  spr.setFreeFont(&Orbitron_Light_24);
  switch(ggState){
  case GG_START_MENU: {
      const char *items[] = {"Start Game","Erase All Projects"};
      static int startIdx=0; // reuse across draws inside state
      // reuse projectMenuIdx variable for encoder; ensure sync
      // draw
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("GalGame",160,10,2);
      int total=2; int window=2; int start=0;
      for(int row=0; row<window; ++row){ int idx=row; int y=60+row*30; bool sel=(idx==projectMenuIdx); spr.setTextColor(sel?TFT_RED:TH.menu_item,TH.bg); spr.drawString(items[idx],160,y,2);} 
      spr.setTextDatum(TL_DATUM);
      break; }
  case GG_ERASE_ALL_CONFIRM: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TFT_RED,TH.bg); spr.drawString("Erase ALL Projects?",160,20,2);
      spr.setTextColor(TH.text,TH.bg); spr.drawString("This cannot be undone",160,50,2);
      spr.setTextColor(TH.menu_item,TH.bg); spr.drawString("Click=Confirm  Long=Cancel",160,110,2);
      break; }
  // (Knowledge view input handling occurs in encoder logic, not here)
  case GG_PROJECT_MENU: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg);
      spr.drawString("AI GalGame Projects",160,10,2);
      spr.setTextColor(TH.menu_item,TH.bg);
      int total = projects.size()+1; // + New
      int window=7; // leave space for footer
      int start = projectMenuIdx - window/2; if(start<0) start=0; if(start> total-window) start=max(0,total-window);
      for(int row=0; row<window && (start+row)<total; ++row){ int idx=start+row; int y=40+row*16; bool sel=(idx==projectMenuIdx); String label = (idx==0)? String("[ New Project ]"): projects[idx-1].name; spr.setTextColor(sel?TFT_RED:TH.menu_item,TH.bg); spr.drawString(label,160,y,2);} 
      if(total>window){ int trackY=40; int trackH=window*16; int barH = max(10, trackH * window / total); int barY = trackY + (trackH-barH) * projectMenuIdx / max(1,(total-1)); spr.fillRect(310,trackY,4,trackH,TH.menu_bg); spr.fillRect(310,barY,4,barH,TH.menu_param);} 
      spr.setTextDatum(BR_DATUM); spr.setTextColor(TH.text_muted,TH.bg); spr.drawString("Made by B5-Software",318,174,1); spr.setTextDatum(TL_DATUM);
      break; }
  case GG_NEW_PROJECT_TYPE: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("Select Genre",160,10,2);
      int totalG = genres.size(); int windowG=7; int startG = genreIdx - windowG/2; if(startG<0) startG=0; if(startG> totalG-windowG) startG = max(0,totalG-windowG);
      for(int row=0; row<windowG && (startG+row)<totalG; ++row){ int idx=startG+row; int y=40+row*16; bool sel=(idx==genreIdx); spr.setTextColor(sel?TFT_RED:TH.menu_item,TH.bg); spr.drawString(genres[idx],160,y,2);} 
      if(totalG>windowG){ int trackY=40; int trackH=windowG*16; int barH=max(10, trackH * windowG / totalG); int barY = trackY + (trackH-barH) * genreIdx / max(1,(totalG-1)); spr.fillRect(310,trackY,4,trackH,TH.menu_bg); spr.fillRect(310,barY,4,barH,TH.menu_param);} 
      break; }
    case GG_NEW_PROJECT_CONFIRM: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("Confirm",160,10,2);
      String meta = String("Name: ")+currentProject.name+"\n"+currentProject.synopsis+"\nClick=Confirm Long=Regen";
      drawScrollingText(10,40,300,100,meta,0);
      spr.setTextColor(TH.menu_param,TH.bg); spr.drawString(fetchStatus,160,150,2);
      break; }
    case GG_RUNNING: {
      spr.setTextDatum(TL_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString(currentProject.name,4,0,2);
      // status line with small icon
  if(fetching){ spr.setTextColor(TH.text_warn,TH.bg); spinnerIdx=(spinnerIdx+1)%4; spr.drawString(String(spinnerChars[spinnerIdx])+" "+fetchStatus,4,20,2);} else { spr.setTextColor(TH.menu_param,TH.bg); spr.drawString(fetchStatus,4,20,2);}    
      drawScrollingText(4,40,300,110,lastAIText,scrollOffset); // only story
      // image placeholder
      if(haveImage){
        // If not decoded or decoder absent, still show placeholder color block
        if(imageThumbReady){
          spr.pushImage(260,0,60,36,imageThumb);
        } else if(!imageDecoded){
          spr.fillRect(260,0,60,36,imageColor);
        } else if(!imageThumbReady){
          spr.fillRect(260,0,60,36,imageColor);
        }
        // Always draw border rectangle
        spr.drawRect(260,0,60,36,TH.scale_pointer);
      } else {
        spr.drawRect(260,0,60,36,TH.scale_pointer);
      }
  // Options are hidden in RUNNING view per new requirement
      break; }
    case GG_KNOWLEDGE_VIEW: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("Knowledge Base",160,10,2);
      drawScrollingText(4,40,300,110,knowledgeBase,scrollOffset);
      spr.setTextColor(TH.menu_param,TH.bg); spr.drawString("Click=Back Long=Menu",160,150,2);
      break; }
    case GG_ABOUT: {
      spr.setTextDatum(TL_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("About",4,0,2);
      // 复用 About.cpp 的精简文本（直接绘制以避免耦合）
      int y=24; spr.setTextColor(TH.text,TH.bg); spr.drawString("ATS-MINI with GalGame",4,y,2); y+=18;
      spr.setTextColor(TH.menu_item,TH.bg); spr.drawString("Made by B5-Software",4,y,2); y+=16;
      spr.setTextColor(TH.text,TH.bg); spr.drawString("github.com/B5-Software",4,y,2); y+=16;
      spr.setTextColor(TH.menu_item,TH.bg); spr.drawString("MIT & upstream:",4,y,2); y+=16;
      spr.setTextColor(TH.text,TH.bg); spr.drawString("esp32-si4732/ats-mini",4,y,2); y+=20;
      spr.setTextColor(TH.menu_param,TH.bg); spr.drawString("Click=Back",4,y,2);
      break; }
    case GG_DELETE_CONFIRM: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("Delete Project",160,10,2);
      String warn = String("Project: ")+ currentProject.name + "\nConfirm delete?";
      drawScrollingText(10,40,300,80,warn,0);
      spr.setTextColor(TFT_RED,TH.bg); spr.drawString("Click=Delete",160,130,2);
      spr.setTextColor(TH.menu_param,TH.bg); spr.drawString("Long=Cancel",160,150,2);
      break; }
    case GG_OPTION_MENU: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("Choose",160,10,2);
      for(int i=0;i<(int)pendingOptions.size();i++) { int y=40+i*20; bool sel=(i==optionIdx); if(sel){ spr.setTextColor(TFT_RED,TH.bg);} else spr.setTextColor(TH.menu_item,TH.bg); spr.drawString(pendingOptions[i],160,y,2);} break; }
    case GG_IN_GAME_MENU: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("Game Menu",160,10,2);
      int totalItems = sizeof(inGameMenuItems)/sizeof(inGameMenuItems[0]);
      int window=6; // 可见行数
      int start = inGameMenuIdx - window/2; if(start<0) start=0; if(start> totalItems-window) start = max(0,totalItems-window);
      for(int row=0; row<window && (start+row)<totalItems; ++row){ int idx = start+row; int y=40+row*20; bool sel = (idx==inGameMenuIdx); if(sel){ spr.setTextColor(TFT_RED,TH.bg);} else spr.setTextColor(TH.menu_item,TH.bg); spr.drawString(inGameMenuItems[idx],160,y,2);} 
      // 滚动条
      if(totalItems>window){ int barH = 100 * window / totalItems; if(barH<10) barH=10; int trackY=40; int trackH=window*20; int barY = trackY + (trackH-barH) * inGameMenuIdx / max(1,(totalItems-1)); spr.fillRect(310,trackY,4,trackH,TH.menu_bg); spr.fillRect(310,barY,4,barH,TH.menu_param);} 
      break; }
    case (GalGameState)GG_FULLSCREEN_IMG: {
      drawFullscreenImage();
      break; }
    case (GalGameState)GG_RESTART_MENU: {
      spr.setTextDatum(TC_DATUM); spr.setTextColor(TH.menu_hdr,TH.bg); spr.drawString("Restart System",160,10,2);
      int totalItems = sizeof(restartMenuItems)/sizeof(restartMenuItems[0]);
      for(int i=0; i<totalItems; i++) { 
        int y=50+i*25; 
        bool sel=(i==restartMenuIdx); 
        if(sel){ spr.setTextColor(TFT_RED,TH.bg);} else spr.setTextColor(TH.menu_item,TH.bg); 
        spr.drawString(restartMenuItems[i],160,y,2);
      } 
      spr.setTextColor(TH.text_muted,TH.bg); 
      spr.drawString("Click=Select Long=Cancel",160,150,2);
      break; }
    default: break;
  }
  // bottom UI log line
  uint16_t logColor = tft.color565(150,150,150);
  spr.setTextDatum(TL_DATUM); spr.setTextColor(logColor, TH.bg);
  spr.fillRect(0,168,320,12,TH.bg);
  uint32_t now=millis(); String toShow = (uiLogOverlay.length() && now-uiLogOverlayTs<4000)? uiLogOverlay: uiLogLine; if(toShow.length()) spr.drawString(toShow.substring(0,42),2,168,2);
  spr.pushSprite(0,0);
}

// Count wrapped lines for given width (approx 8px per char with font size used)
static int countWrappedLines(const String &text,int w){
  int charsPerLine = max(1, w/8);
  int lines=0; int col=0; for(size_t i=0;i<text.length();++i){ char c=text[i]; if(c=='\r') continue; if(c=='\n'){ lines++; col=0; continue; } col++; if(col>=charsPerLine){ lines++; col=0; } }
  if(col>0) lines++; return lines; }

static void regenerateTurn() { requestAI(); }

static void openInGameMenu(){ ggState=GG_IN_GAME_MENU; inGameMenuIdx=0; }

static void executeInGameMenu(){
  switch(inGameMenuIdx){
    case 0: saveMeta(); fetchStatus="Saved"; ggState=GG_RUNNING; break;
    case 1: { // checkpoint + auto summarize trigger
      File src=LittleFS.open(currentProject.dir+"/meta.json","r"); if(src){ File dst=LittleFS.open(currentProject.dir+"/checkpoint.json","w"); if(dst){ while(src.available()) dst.write(src.read()); dst.close(); } src.close(); fetchStatus="Checkpoint"; }
      startSummarizeAsync(); ggState=GG_RUNNING; break; }
    case 2: { File cp=LittleFS.open(currentProject.dir+"/checkpoint.json","r"); if(cp){ File dst=LittleFS.open(currentProject.dir+"/meta.json","w"); if(dst){ while(cp.available()) dst.write(cp.read()); dst.close(); } cp.close(); loadMeta(currentProject); fetchStatus="Restored"; } ggState=GG_RUNNING; }
      break;
    case 3: { // view knowledge
  scrollOffset=0; ggState=GG_KNOWLEDGE_VIEW; }
      break;
    case 4: { startSummarizeAsync(); ggState=GG_RUNNING; break; }
    case 5: { // Full Screen Picture
      if(haveImage) {
        // Ensure image is decoded before entering fullscreen
        if(!imageDecoded) {
          tryDecodeImage();
        }
        ggState = (GalGameState)GG_FULLSCREEN_IMG; 
        GG_LOG_STATE("Entering fullscreen mode");
  fullscreenNeedsRedraw = true; // force first draw
  fsCachedLogged = false; // reset cache log suppression
      } else { 
        fetchStatus="No Image"; 
        ggState=GG_RUNNING; 
      }
      break; }
    case 6: { // delete project
      ggState = GG_DELETE_CONFIRM; break; }
    case 7: { // About 内嵌
      scrollOffset=0; ggState=GG_ABOUT; fetchStatus=""; break; }
    case 8: { // Restart System - create submenu
      ggState=(GalGameState)GG_RESTART_MENU; restartMenuIdx=0; break; }
    case 9: galgameLeave(); break;
  }
}

void galgameLoop() {
  if(!galgameActive()) return;
  
  // Handle serial commands
  handleSerialCommands();
  
  // Passive global decay every ~5s to keep preference distribution adaptive
  if(millis() - lastGlobalDecayMs > 5000){
    lastGlobalDecayMs = millis();
    const float GLOBAL_DECAY = 0.995f; // slow decay
    for(auto &os: optionStats){ os.weight *= GLOBAL_DECAY; }
    optionStats.erase(std::remove_if(optionStats.begin(), optionStats.end(), [](const OptionStat &o){ return o.weight < 0.05f; }), optionStats.end());
  }
  if(summarizePending && !fetching) doSummarizeNow();
  else if(aiRetryScheduled && !fetching && millis()>=aiRetryDueMs) requestAI();
  else if(ggState==GG_RUNNING && !fetching && lastAIText=="" && !summarizePending) requestAI();
  // Fullscreen image interaction: short click back to running, long click opens menu
  if(ggState==(GalGameState)GG_FULLSCREEN_IMG){
    static bool lastButtonState = true; // HIGH when not pressed
    static uint32_t pressStartMs = 0;
    
    bool currentButtonState = digitalRead(ENCODER_PUSH_BUTTON);
    bool click = false, longp = false;
    
    if(!currentButtonState && lastButtonState) { // button pressed
      pressStartMs = millis();
    } else if(currentButtonState && !lastButtonState) { // button released
      uint32_t pressDuration = millis() - pressStartMs;
      if(pressDuration < 700) {
        click = true;
      }
    } else if(!currentButtonState && (millis() - pressStartMs) > 700) { // long press detected
      longp = true;
      pressStartMs = millis() + 1000; // prevent multiple long press detections
    }
    
    lastButtonState = currentButtonState;
    
    if(click){ 
      ggState=GG_RUNNING; 
      GG_LOG_STATE("Fullscreen exit via click");
    }
    else if(longp){ 
      openInGameMenu(); 
      GG_LOG_STATE("Fullscreen -> menu via long press");
    }
  }
  // Periodic redraw throttle handled outside; here just request immediate draw
  // Don't redraw over fullscreen image constantly
  static uint32_t lastDrawMs = 0;
  if(ggState != (GalGameState)GG_FULLSCREEN_IMG || (millis() - lastDrawMs > 100)) {
    if(ggState != (GalGameState)GG_FULLSCREEN_IMG) lastDrawMs = millis();
    galgameDraw();
  }
}

void galgameEncoder(int dir,bool click,bool longPress,bool shortPress){
  if(!galgameActive()) return;
  bool anyPress = click || longPress || shortPress;
  if(!anyPress && lastPressActive) inputPressLocked=false;
  lastPressActive = anyPress;
  if(inputPressLocked && anyPress) return;
  
  // Log input events
  if(dir != 0) GG_LOG_STATE("Encoder dir: %d in state %d", dir, (int)ggState);
  if(click) GG_LOG_STATE("Encoder click in state %d", (int)ggState);
  if(longPress) GG_LOG_STATE("Encoder longPress in state %d", (int)ggState);
  
  GalGameState oldState = ggState;
  
  switch(ggState){
    case GG_START_MENU:
      if(dir){ int total=2; projectMenuIdx = (projectMenuIdx + total + dir) % total; }
      if(click){ if(projectMenuIdx==0){ ggState=GG_PROJECT_MENU; } else if(projectMenuIdx==1){ ggState=GG_ERASE_ALL_CONFIRM; } }
      if(longPress){ galgameLeave(); }
      break;
    case GG_ERASE_ALL_CONFIRM:
      if(click){
        fetchStatus="Erasing..."; galgameDraw();
        File dir=LittleFS.open(baseDir); if(dir){ File f; while((f=dir.openNextFile())){ if(f.isDirectory()){ deleteProjectRecursive(String(f.name())); } f.close(); } dir.close(); }
        loadProjects(); fetchStatus="All erased"; delay(300); ggState=GG_START_MENU; projectMenuIdx=0; }
      if(longPress){ ggState=GG_START_MENU; projectMenuIdx=0; fetchStatus=""; }
      break;
    case GG_PROJECT_MENU:
      if(dir){ int total=projects.size()+1; projectMenuIdx = (projectMenuIdx + total + dir) % total; }
      if(click){ if(projectMenuIdx==0){ ggState=GG_NEW_PROJECT_TYPE; genreIdx=0; } else { loadMeta(projects[projectMenuIdx-1]); ggState=GG_RUNNING; lastAIText=""; requestAI(); } }
      if(longPress && projectMenuIdx>0){ // long press delete confirm path
        currentProject = projects[projectMenuIdx-1]; ggState=GG_DELETE_CONFIRM; }
      break;
    case GG_NEW_PROJECT_TYPE:
      if(dir){ genreIdx = (genreIdx + genres.size() + dir) % genres.size(); }
      if(click){ 
        GG_LOG_STATE("Creating new project with genre: %s", genres[genreIdx].c_str());
        currentProject=GalGameProjectMeta(); 
        currentProject.genre=genres[genreIdx]; 
        currentProject.name=""; 
        currentProject.synopsis=""; 
        newProjectGenerate(); 
        ggState=GG_NEW_PROJECT_CONFIRM; 
      }
      if(longPress){ ggState=GG_PROJECT_MENU; }
      break;
    case GG_NEW_PROJECT_CONFIRM:
      if(longPress){ 
        GG_LOG_STATE("Regenerating project name/synopsis");
        currentProject.name=""; 
        currentProject.synopsis=""; 
        newProjectGenerate(); 
      }
      if(click){ 
        if(currentProject.name=="") { 
          GG_LOG_STATE("No project name, generating again");
          newProjectGenerate(); 
        } else { 
          GG_LOG_STATE("Confirming project: %s", currentProject.name.c_str());
          createProjectDir(); 
          ggState=GG_RUNNING; 
          lastAIText=""; 
          requestAI(); 
          loadProjects(); 
        } 
      }
      break;
    case GG_RUNNING:
      if(dir){ // scroll dynamic bounds
        int total = countWrappedLines(lastAIText,300);
        int visible = 110/14; // from drawScrollingText
        int maxOff = max(0, total - visible);
        scrollOffset += (dir>0?1:-1);
        if(scrollOffset<0) scrollOffset=0; if(scrollOffset>maxOff) scrollOffset=maxOff;
      }
      if(click){ ggState=GG_OPTION_MENU; }
      if(longPress){ openInGameMenu(); inputPressLocked=true; }
      break;
    case GG_KNOWLEDGE_VIEW:
      if(dir){ int total = countWrappedLines(knowledgeBase,300); int visible=110/14; int maxOff=max(0,total-visible); scrollOffset += (dir>0?1:-1); if(scrollOffset<0) scrollOffset=0; if(scrollOffset>maxOff) scrollOffset=maxOff; }
      if(click){ ggState=GG_RUNNING; }
      if(longPress){ openInGameMenu(); }
      break;
    case GG_OPTION_MENU:
      if(dir && !pendingOptions.empty()) { optionIdx = (optionIdx + pendingOptions.size() + dir) % pendingOptions.size(); }
      if(click && !pendingOptions.empty()) {
        String chosen = pendingOptions[optionIdx];
        appendContext("user", String("Choice: ")+chosen);
        if(chosen.length()) {
          knowledgeBase += "\nCHOICE:" + chosen;
          if(knowledgeBase.length()>3000) knowledgeBase.remove(0, knowledgeBase.length()-3000);
          if(knowledgeBase.length()>1500) startSummarizeAsync();
      // Weighted preference update with time decay
      const uint32_t now = millis();
      const float BOOST=1.0f; const float DECAY_PER_SEC=0.995f;
      bool found=false; for(auto &os: optionStats){ if(os.text==chosen){
        float dtSec = (now - os.lastMs)/1000.0f; if(dtSec<0) dtSec=0; float decayFactor = powf(DECAY_PER_SEC, dtSec);
        os.weight = os.weight * decayFactor + BOOST; os.lastMs=now; found=true; break; } }
      if(!found && optionStats.size()<32){ optionStats.push_back({chosen,BOOST,now}); }
      if(optionStats.size() && (now/1000)%7==0){ std::vector<OptionStat> copy=optionStats; std::sort(copy.begin(),copy.end(),[](const OptionStat&a,const OptionStat&b){return a.weight>b.weight;}); String pref="Prefs:"; int lim=min(3,(int)copy.size()); for(int i=0;i<lim;i++){ pref+="["+copy[i].text+":"+String(copy[i].weight,1)+"]"; } if(pref.length()<140) knowledgeBase += "\n"+pref; }
        }
        pendingOptions.clear(); scrollOffset=0; requestAI(); ggState=GG_RUNNING; }
      if(longPress){ ggState=GG_RUNNING; }
      break;
    case GG_IN_GAME_MENU:
  if(dir){ int totalItems = sizeof(inGameMenuItems)/sizeof(inGameMenuItems[0]); inGameMenuIdx = (inGameMenuIdx + totalItems + dir) % totalItems; }
      if(click){ executeInGameMenu(); }
      if(longPress){ ggState=GG_RUNNING; }
      break;
    case GG_ABOUT:
      if(click){ ggState=GG_RUNNING; }
      if(longPress){ ggState=GG_IN_GAME_MENU; }
      break;
    case GG_DELETE_CONFIRM:
  if(click){ if(currentProject.dir.length()) { deleteProjectRecursive(currentProject.dir); loadProjects(); } galgameLeave(); }
  if(longPress){ ggState=GG_IN_GAME_MENU; }
      break;
    case (GalGameState)GG_RESTART_MENU:
      if(dir){ int totalItems = sizeof(restartMenuItems)/sizeof(restartMenuItems[0]); restartMenuIdx = (restartMenuIdx + totalItems + dir) % totalItems; }
      if(click){ 
        if(restartMenuIdx == 0) { // Normal Restart
          GG_LOG("Normal restart requested");
          ESP.restart();
        } else if(restartMenuIdx == 1) { // Download Mode Restart  
          GG_LOG("Download mode restart requested");
          // Set download mode and restart
          // On ESP32, we can use RTC memory or preferences to signal download mode
          Preferences prefs;
          prefs.begin("system", false);
          prefs.putBool("download_mode", true);
          prefs.end();
          ESP.restart();
        } else { // Cancel
          ggState=GG_IN_GAME_MENU; 
        }
      }
      if(longPress){ ggState=GG_IN_GAME_MENU; }
      break;
    default: break;
  }
  
  // Log state changes
  if(oldState != ggState) {
    GG_LOG_STATE("State changed from %d to %d", (int)oldState, (int)ggState);
  }
}

// Missing helper implementations (restored)
static bool writeImageFileFromB64(const String &b64){ if(!b64.length()) return false; std::unique_ptr<uint8_t[]> bin; size_t sz=b64decode(b64,bin); if(!sz) return false; File f=LittleFS.open(currentProject.dir+"/last_image.bin","w"); if(!f) return false; f.write(bin.get(),sz); f.close(); return true; }
// Unified thumbnail loader (validates header magic 'THMB' and dimensions)
static bool loadThumbnail(const String &dir,const char *file,uint16_t expectedW,uint16_t expectedH,std::unique_ptr<uint16_t[]> &out){
  File f=LittleFS.open(dir+"/"+file,"r"); if(!f) return false; if(f.size()<8){ f.close(); return false; }
  char magic[4]; if(f.read((uint8_t*)magic,4)!=4){ f.close(); return false; }
  if(!(magic[0]=='T'&&magic[1]=='H'&&magic[2]=='M'&&magic[3]=='B')){ f.close(); return false; }
  uint8_t wh[4]; if(f.read(wh,4)!=4){ f.close(); return false; }
  uint16_t w = wh[0] | (wh[1]<<8); uint16_t h = wh[2] | (wh[3]<<8); if(w!=expectedW || h!=expectedH){ f.close(); return false; }
  size_t need=(size_t)w*h*2; if(f.size() < 8 + need){ f.close(); return false; }
  out.reset(new uint16_t[w*h]); size_t rd=f.read((uint8_t*)out.get(), need); f.close(); return rd==need; }
// Multi-size thumbnail cache (PNG only)
static void buildThumbnails(){
#ifdef USE_PNG_DECODER
  if(!lastImageB64.length()) return;
  // Decode base64
  std::unique_ptr<uint8_t[]> bin; size_t sz=b64decode(lastImageB64,bin); if(sz<8) return;
  // Check PNG signature
  if(!(bin[0]==0x89 && bin[1]==0x50 && bin[2]==0x4E && bin[3]==0x47)) return;
  // First pass to obtain dimensions
  uint16_t pw=0, ph=0; if(png.openFLASH((uint8_t*)bin.get(), sz, nullptr)==PNG_SUCCESS){ pw=png.getWidth(); ph=png.getHeight(); png.close(); }
  if(pw==0 || ph==0) return;
  struct ThumbBuf { const ThumbSpec *spec; std::unique_ptr<uint16_t[]> pix; };
  ThumbBuf bufs[sizeof(THUMBS)/sizeof(THUMBS[0])];
  for(size_t i=0;i<sizeof(THUMBS)/sizeof(THUMBS[0]);++i){ bufs[i].spec=&THUMBS[i]; bufs[i].pix.reset(new uint16_t[THUMBS[i].w*THUMBS[i].h]); memset(bufs[i].pix.get(),0,THUMBS[i].w*THUMBS[i].h*2); }
  struct DrawCtx { ThumbBuf *bufs; size_t count; int srcH; };
  static DrawCtx dctx; dctx.bufs=bufs; dctx.count= sizeof(THUMBS)/sizeof(THUMBS[0]); dctx.srcH=ph;
  void (*cb)(PNGDRAW*) = [](PNGDRAW *pDraw){ DrawCtx *C=(DrawCtx*)pDraw->pUser; uint16_t *line=(uint16_t*)pDraw->pPixels; int sw=pDraw->iWidth; int y=pDraw->y; for(size_t bi=0; bi<C->count; ++bi){ auto &tb=*C->bufs[bi].spec; auto *dst=C->bufs[bi].pix.get(); int th=tb.h; int tw=tb.w; int ty = (int)((uint32_t)y * th / C->srcH); if(ty>=th) continue; uint16_t *row = dst + ty*tw; for(int sx=0;sx<sw;sx++){ int tx = (int)((uint32_t)sx * tw / sw); if(tx<tw) row[tx]=line[sx]; } } };
  if(png.openFLASH((uint8_t*)bin.get(), sz, cb)==PNG_SUCCESS){ png.decode(&dctx,0); png.close();
    // Write buffers
  for(auto &b: bufs){ String path = currentProject.dir+"/"+b.spec->file; File tf=LittleFS.open(path,"w"); if(tf){
    // Write 8-byte header: 'THMB' + w + h (little endian)
    uint8_t hdr[8] = {'T','H','M','B', (uint8_t)(b.spec->w & 0xFF), (uint8_t)(b.spec->w>>8), (uint8_t)(b.spec->h & 0xFF), (uint8_t)(b.spec->h>>8)};
    tf.write(hdr,8);
    tf.write((uint8_t*)b.pix.get(), b.spec->w*b.spec->h*2);
    tf.close(); }
  }
    saveMeta();
  }
#endif
}
