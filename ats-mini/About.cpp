#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include <LittleFS.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <qrcode.h>

static void displayQRCode(esp_qrcode_handle_t){}

static void drawAboutCommon(uint8_t){
  spr.fillSprite(TH.bg);
  spr.setTextDatum(TL_DATUM);
  int y=10;
  spr.setTextColor(TH.text, TH.bg); spr.drawString("ATS-MINI with GalGame", 4, y, 4); y+=38;
  spr.setTextColor(TH.text_muted, TH.bg); spr.drawString("Made by B5-Software", 4, y, 2); y+=20;
  spr.setTextColor(TH.text, TH.bg); spr.drawString("https://github.com/B5-Software", 4, y, 2); y+=20;
  spr.setTextColor(TH.text_muted, TH.bg); spr.drawString("MIT license & upstream base:", 4, y, 2); y+=18;
  spr.setTextColor(TH.text, TH.bg); spr.drawString("https://github.com/esp32-si4732/ats-mini", 4, y, 2); y+=24;
  spr.setTextColor(TH.text_muted, TH.bg); spr.drawString("(Press to exit)", 4, y, 2);
}

//
// Show HELP screen
//
void drawAboutHelp(uint8_t a){ drawAboutCommon(a); spr.pushSprite(0,0);} // 保留接口

//
// Show SYSTEM screen
//
static void drawAboutSystem(uint8_t a){ drawAboutCommon(a); spr.pushSprite(0,0);} // 保留接口

//
// Show AUTHORS screen
//
static void drawAboutAuthors(uint8_t a){ drawAboutCommon(a); spr.pushSprite(0,0);} // 保留接口

//
// Draw ABOUT screens
//
void drawAbout(){ drawAboutCommon(0); spr.pushSprite(0,0);} // 单页
