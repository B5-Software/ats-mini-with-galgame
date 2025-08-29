#pragma once
#include "Common.h"
#include <LittleFS.h>
#include <vector>
#include <ArduinoJson.h>

// GalGame command states (separate from main currentCmd) to avoid auto timeout
enum GalGameState {
  GG_IDLE=0,
  GG_START_MENU, // 新初始菜单：Start Game / Erase All Projects
  GG_PROJECT_MENU,
  GG_ERASE_ALL_CONFIRM, // 全部项目擦除确认
  GG_NEW_PROJECT_TYPE,
  GG_NEW_PROJECT_CONFIRM,
  GG_RUNNING,
  GG_OPTION_MENU,
  GG_IN_GAME_MENU,
  GG_DELETE_CONFIRM,
  GG_KNOWLEDGE_VIEW,
  GG_ABOUT
};

struct GalGameProjectMeta {
  String name;
  String synopsis;
  String genre;
  String dir; // directory name under /gg/
};

// Runtime context (trimmed conversation)
struct GalGameContextEntry { String role; String content; };

void galgameInit();
void galgameEnter();
bool galgameActive();
void galgameLoop();
void galgameEncoder(int dir, bool click, bool longPress, bool shortPress);
void galgameDraw();
void galgameLeave();
void galgameSetApiKey(const String &key);
void galgamePersistSettings();
void galgameLoadSettings();
// Serial helpers
int  galgameProjectCount();
String galgameProjectName(int idx);
bool galgameOpenProjectIndex(int idx);
bool galgameDeleteProjectIndex(int idx);
void galgameSetMaxRounds(int r);
void galgameTriggerSummarize();
