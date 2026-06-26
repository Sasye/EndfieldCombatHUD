#define _CRT_SECURE_NO_WARNINGS
#include "MinHook.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

extern "C" __declspec(dllexport) void DummyExport() {}

static FILE *g_logFile = nullptr;
static CRITICAL_SECTION g_logLock;

void Log(const char *fmt, ...) {
  if (!g_logFile)
    return;
  EnterCriticalSection(&g_logLock);
  va_list args;
  va_start(args, fmt);
  vfprintf(g_logFile, fmt, args);
  va_end(args);
  fprintf(g_logFile, "\n");
  fflush(g_logFile);
  LeaveCriticalSection(&g_logLock);
}

// ============================================================================
// Il2Cpp API + Buff Data (extracted to headers)
// ============================================================================
#include "il2cpp_api.h"
#include "buff_data.h"
#include "attr_names.h"
#include "buff_names.h"
#include "skill_data.h"
// #include "f9_dump.h"

// ============================================================================
// Tooltip State (shared between hook thread and overlay thread)
// ============================================================================
static HWND g_gameHwnd = nullptr;
static HWND g_overlayHwnd = nullptr;
static volatile bool g_running = true;

static char g_tooltipText[2048] = {0};
static bool g_showTooltip = false;
static int g_tooltipX = 0, g_tooltipY = 0;

// Safe append to g_tooltipText — silently truncates if buffer would overflow
static void tooltip_cat(const char *src) {
  size_t cur = strlen(g_tooltipText);
  size_t srcLen = strlen(src);
  if (cur + srcLen >= sizeof(g_tooltipText)) return;
  memcpy(g_tooltipText + cur, src, srcLen + 1);
}
// ============================================================================
// Overlay Window (runs on its own thread - no interference with game)
// ============================================================================
static void DrawTooltip(HDC hdc) {
  if (!g_showTooltip || g_tooltipText[0] == '\0')
    return;

  // Parse tooltip text into lines
  char lines[20][256];
  int lineCount = 0;
  {
    const char *p = g_tooltipText;
    while (*p && lineCount < 20) {
      const char *nl = strchr(p, '\n');
      int len = nl ? (int)(nl - p) : (int)strlen(p);
      if (len > 255) len = 255;
      memcpy(lines[lineCount], p, len);
      lines[lineCount][len] = 0;
      lineCount++;
      if (!nl) break;
      p = nl + 1;
    }
  }
  if (lineCount == 0) return;

  // Resolution scaling: base design at 1440p
  HWND wnd = WindowFromDC(hdc);
  float scale = 1.0f;
  if (wnd) {
    RECT wr; GetClientRect(wnd, &wr);
    if (wr.bottom > 0) scale = wr.bottom / 1440.0f;
  }
  if (scale < 0.5f) scale = 0.5f;
  if (scale > 3.0f) scale = 3.0f;

  int padX = (int)(14 * scale);
  int padY = (int)(10 * scale);
  int cursorOff = (int)(15 * scale);
  int minPanelW = (int)(200 * scale);
  int maxTextW = (int)(360 * scale);
  int cornerR = (int)(6 * scale);

  static float s_lastScale = -1.0f;
  static HFONT s_hFontTitle = NULL;
  static HFONT s_hFontBody = NULL;
  static HFONT s_hFontSmall = NULL;
  static HPEN s_borderPen = NULL;
  static HBRUSH s_panelBrush = NULL;

  if (scale != s_lastScale) {
    if (s_hFontTitle) DeleteObject(s_hFontTitle);
    if (s_hFontBody) DeleteObject(s_hFontBody);
    if (s_hFontSmall) DeleteObject(s_hFontSmall);
    if (s_borderPen) DeleteObject(s_borderPen);
    if (s_panelBrush) DeleteObject(s_panelBrush);

    int titleSize = (int)(22 * scale);
    int bodySize  = (int)(16 * scale);
    int smallSize = (int)(14 * scale);
    int borderW = (int)(2 * scale);
    if (borderW < 1) borderW = 1;

    s_hFontTitle = CreateFontW(titleSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    s_hFontBody = CreateFontW(bodySize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    s_hFontSmall = CreateFontW(smallSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
      
    s_borderPen = CreatePen(PS_SOLID, borderW, RGB(40, 80, 140));
    s_panelBrush = CreateSolidBrush(RGB(12, 16, 28));
    
    s_lastScale = scale;
  }

  // Measure all lines to determine panel size
  int maxW = 0;
  int totalH = padY;
  struct LineInfo { int h; int font; };
  LineInfo li[20];

  for (int i = 0; i < lineCount; i++) {
    wchar_t wl[256];
    MultiByteToWideChar(CP_UTF8, 0, lines[i], -1, wl, 256);
    HFONT f;
    if (i == 0) { f = s_hFontTitle; li[i].font = 0; }
    else { f = s_hFontBody; li[i].font = 1; }
    if (i == 0 && strstr(lines[0], " | ")) { /* title has friendly name */ }

    SelectObject(hdc, f);
    RECT mr = {0, 0, maxTextW, 100};
    DrawTextW(hdc, wl, -1, &mr, DT_CALCRECT | DT_SINGLELINE);
    li[i].h = mr.bottom + (int)(4 * scale);
    if (mr.right > maxW) maxW = mr.right;
    totalH += li[i].h;
    if (i == 0) totalH += (int)(22 * scale);
    if (i == 2) totalH += (int)(6 * scale);
  }

  int panelW = maxW + padX * 2 + (int)(12 * scale);
  int panelH = totalH + padY + (int)(8 * scale);
  if (panelW < minPanelW) panelW = minPanelW;

  // Position: top-right of cursor
  int tx = g_tooltipX + cursorOff;
  int ty = g_tooltipY - panelH - (int)(5 * scale);
  if (ty < 0) ty = g_tooltipY + cursorOff;
  if (wnd) {
    RECT sr; GetClientRect(wnd, &sr);
    if (tx + panelW > sr.right) tx = g_tooltipX - panelW - (int)(5 * scale);
  }

  int px = tx, py = ty;

  // === Draw panel background ===
  SelectObject(hdc, s_borderPen);
  SelectObject(hdc, s_panelBrush);
  RoundRect(hdc, px, py, px + panelW, py + panelH, cornerR, cornerR);

  // === Draw text line by line ===
  SetBkMode(hdc, TRANSPARENT);
  int curY = py + padY;

  for (int i = 0; i < lineCount; i++) {
    wchar_t wl[256];
    MultiByteToWideChar(CP_UTF8, 0, lines[i], -1, wl, 256);

    if (i == 0) {
      // Title line - split at " | " if present
      char *sep = strstr(lines[0], " | ");
      if (sep) {
        // Friendly name in cyan
        *sep = 0;
        wchar_t wName[128];
        MultiByteToWideChar(CP_UTF8, 0, lines[0], -1, wName, 128);
        SelectObject(hdc, s_hFontTitle);
        SetTextColor(hdc, RGB(80, 210, 240));
        RECT r1 = {px + padX, curY, px + panelW - padX, curY + (int)(30 * scale)};
        DrawTextW(hdc, wName, -1, &r1, DT_SINGLELINE | DT_VCENTER);

        // Buff ID in small gray below
        curY += li[i].h;
        wchar_t wId[200];
        MultiByteToWideChar(CP_UTF8, 0, sep + 3, -1, wId, 200);
        SelectObject(hdc, s_hFontSmall);
        SetTextColor(hdc, RGB(100, 110, 130));
        RECT r2 = {px + padX, curY, px + panelW - padX, curY + (int)(20 * scale)};
        DrawTextW(hdc, wId, -1, &r2, DT_SINGLELINE);
        curY += (int)(14 * scale);
        *sep = ' '; // restore
      } else {
        // No friendly name, just show ID in cyan
        SelectObject(hdc, s_hFontTitle);
        SetTextColor(hdc, RGB(80, 210, 240));
        RECT r = {px + padX, curY, px + panelW - padX, curY + (int)(30 * scale)};
        DrawTextW(hdc, wl, -1, &r, DT_SINGLELINE | DT_VCENTER);
        curY += li[i].h;
      }
      // Separator line after title
      HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(40, 60, 90));
      SelectObject(hdc, sepPen);
      MoveToEx(hdc, px + padX, curY, NULL);
      LineTo(hdc, px + panelW - padX, curY);
      curY += (int)(6 * scale);
      DeleteObject(sepPen);
    } else if (i == 1 || i == 2) {
      // Time and stacks - white
      SelectObject(hdc, s_hFontBody);
      SetTextColor(hdc, RGB(200, 210, 230));
      RECT r = {px + padX, curY, px + panelW - padX, curY + (int)(24 * scale)};
      DrawTextW(hdc, wl, -1, &r, DT_SINGLELINE);
      curY += li[i].h;
      if (i == 2) {
        // Thin separator before effects
        HPEN sepPen2 = CreatePen(PS_SOLID, 1, RGB(35, 50, 75));
        SelectObject(hdc, sepPen2);
        MoveToEx(hdc, px + padX, curY, NULL);
        LineTo(hdc, px + panelW - padX, curY);
        curY += (int)(4 * scale);
        DeleteObject(sepPen2);
      }
    } else {
      // Effect lines - golden/green for values
      SelectObject(hdc, s_hFontBody);
      // Detect if line has percentage or number value
      if (strncmp(lines[i], " *", 2) == 0)
        SetTextColor(hdc, RGB(130, 140, 150)); // gray for internal blackboard params
      else if (strstr(lines[i], "%") || strstr(lines[i], "+"))
        SetTextColor(hdc, RGB(255, 210, 80));  // golden for buffs
      else if (strstr(lines[i], "-"))
        SetTextColor(hdc, RGB(255, 100, 100)); // red for debuffs
      else
        SetTextColor(hdc, RGB(160, 230, 160)); // green for misc
      RECT r = {px + padX, curY, px + panelW - padX, curY + (int)(24 * scale)};
      DrawTextW(hdc, wl, -1, &r, DT_SINGLELINE);
      curY += li[i].h;
    }
  }

  // Cleanup
  SelectObject(hdc, (HFONT)GetStockObject(DEFAULT_GUI_FONT));
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    // Clear entire window to black (= transparent via LWA_COLORKEY)
    RECT cr;
    GetClientRect(hwnd, &cr);
    HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &cr, clearBrush);
    DeleteObject(clearBrush);
    // Draw tooltip on top (non-black pixels will be visible)
    DrawTooltip(hdc);
    // Draw skill HUD (synchro CD + ultimate charge)
    DrawSkillHud(hdc, hwnd);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_ERASEBKGND)
    return 1;
  if (msg == WM_TIMER) {
    // Timer fires ~60fps on overlay thread - do hit testing here with pure
    // Win32
    if (!IsWindow(g_gameHwnd)) {
      g_running = false;
      PostQuitMessage(0);
      return 0;
    }

    // Hide overlay if game is not in foreground (prevents taskbar hiding/blocking when alt-tabbed)
    HWND fg = GetForegroundWindow();
    if (fg != g_gameHwnd && fg != hwnd) {
      if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
      return 0;
    } else {
      if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNA);
    }

    // Sync overlay position with game window
    RECT gr;
    GetClientRect(g_gameHwnd, &gr);
    POINT gpt = {0, 0};
    ClientToScreen(g_gameHwnd, &gpt);
    SetWindowPos(hwnd, HWND_TOPMOST, gpt.x, gpt.y, gr.right, gr.bottom,
                 SWP_NOACTIVATE);

    // Get mouse position in game client coords
    POINT cursor;
    GetCursorPos(&cursor);
    ScreenToClient(g_gameHwnd, &cursor);

    int gameW = gr.right;
    int gameH = gr.bottom;

    // ---- Buff icon region estimation ----
    // The buff icons are displayed near the bottom-left of the screen,
    // below the HP bar. Based on typical 1920x1080 / 2560x1440 layouts:
    // Approximate region: X in [gameW*0.05, gameW*0.25], Y in [gameH*0.88,
    // gameH*0.98] Each icon is ~36x36 px with some spacing, arranged
    // horizontally.
    //
    // We use a simple approach: check if cursor is in the buff icon region,
    // then determine which buff index based on X position.

    // Calibrated from F9 measurements on 2560x1440:
    //   buff[0] center = (1081, 1360)
    //   buff[3] center = (1218, 1363)
    //   icon spacing = (1218-1081)/3 = 45.7px = gameH * 0.0317
    //   Y center = 1362 = gameH * 0.946
    //   X start (icon0 center) = 1081 = gameW * 0.422
    float iconSpacing = gameH * 0.0317f;
    float iconSize = gameH * 0.025f; // 36px at 1440p
    float firstIconCenterX = gameW * 0.422f;
    float iconCenterY = gameH * 0.946f;
    float halfIcon = iconSize / 2.0f;

    // Detection region: from half-icon before first icon to well past potential
    // last icon
    float buffRegionLeft = firstIconCenterX - halfIcon;
    float buffRegionRight =
        firstIconCenterX + iconSpacing * 30 + halfIcon; // up to 30 icons
    float buffRegionTop = iconCenterY - halfIcon - 5;
    float buffRegionBottom = iconCenterY + halfIcon + 5;

    // F9 debug: log cursor position for calibration (removed for release)

    bool newHover = false;
    int hoveredVisIdx = -1;

    EnterCriticalSection(&g_buffLock);

    // Build visible buff list (exclude permanent buffs with huge duration - no
    // HUD icon)
    int visibleMap[64];
    int visibleCount = 0;
    // Use game's actual display order if available
    if (g_displayCount > 0) {
      for (int i = 0; i < g_displayCount && i < 64; i++) {
        int foundIdx = -1;
        for (int bi = 0; bi < g_buffCount; bi++) {
          if (g_buffs[bi].instUid == g_displayUids[i]) {
            foundIdx = bi;
            break;
          }
        }
        visibleMap[visibleCount++] = foundIdx;
      }
    } else {
      // Fallback: debuffs first, then buffs
      for (int i = 0; i < g_buffCount; i++) {
        if (g_buffs[i].duration < 100000.0f && g_buffs[i].source == 1)
          visibleMap[visibleCount++] = i;
      }
      for (int i = 0; i < g_buffCount; i++) {
        if (g_buffs[i].duration < 100000.0f && g_buffs[i].source == 0)
          visibleMap[visibleCount++] = i;
      }
    }

    if (visibleCount > 0 && cursor.y >= (int)buffRegionTop &&
        cursor.y <= (int)buffRegionBottom) {

      // Check each visible icon individually by its center position
      for (int vIdx = 0; vIdx < visibleCount; vIdx++) {
        float iconCX = firstIconCenterX + vIdx * iconSpacing;
        if (cursor.x >= (int)(iconCX - halfIcon) &&
            cursor.x <= (int)(iconCX + halfIcon)) {
          hoveredVisIdx = vIdx;
          newHover = true;
          break;
        }
      }
    }

    if (newHover && hoveredVisIdx >= 0 && hoveredVisIdx < visibleCount) {
      int bIdx = visibleMap[hoveredVisIdx];
      if (bIdx >= 0 && bIdx < g_buffCount) {
        ActiveBuff ab = g_buffs[bIdx]; // Copy for aggregation
        


        // Count how many independent instances of this buff exist in memory
        int matchingInstances = 0;
        for (int i = 0; i < g_buffCount; i++) {
          if (strcmp(g_buffs[i].id, ab.id) == 0 && strcmp(g_buffs[i].iconName, ab.iconName) == 0) {
            matchingInstances++;
          }
        }

        // Multiply static attributes ONLY when the UI shows more stacks than the engine tracks.
        // If trueEnhanceCnt == enhanceCnt, the engine already scaled the AttributeModifierLoader
        // to reflect all stacks (e.g. character buffs). If trueEnhanceCnt < enhanceCnt, the UI
        // is visually grouping independent instances whose loaders only contain base values.
        // Do NOT multiply blackboard values, as the engine dynamically updates them to the total.
        if (ab.trueEnhanceCnt > 0 && ab.enhanceCnt > ab.trueEnhanceCnt) {
          int multiplier = ab.enhanceCnt / ab.trueEnhanceCnt;
          for (int j = 0; j < 96; j++) {
            ab.attrs.add[j] *= multiplier;
            ab.attrs.baseAdd[j] *= multiplier;
            ab.attrs.finalAdd[j] *= multiplier;
            ab.attrs.baseFinalAdd[j] *= multiplier;
            ab.attrs.mul[j] *= multiplier;
            ab.attrs.baseMul[j] *= multiplier;
            ab.attrs.finalScl[j] *= multiplier;
            ab.attrs.baseFinalScl[j] *= multiplier;
          }
        }

        // Format duration string
      char durStr[128];
      if (ab.duration >= 100000.0f) {
        snprintf(durStr, sizeof(durStr), "\xe6\xb0\xb8\xe4\xb9\x85"); // yong jiu
      } else if (ab.lifeTime > 0.0f) {
        snprintf(durStr, sizeof(durStr), "%.1fs / %.1fs", ab.lifeTime, ab.duration);
      } else {
        snprintf(durStr, sizeof(durStr), "%.1fs", ab.duration);
      }
      // Derive friendly display name from icon
      const char *displayName = ab.id;
      // Icon-based type name mapping
      struct { const char *pat; const char *name; } iconNames[] = {
        {"atk_up",          "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b\xe6\x8f\x90\xe5\x8d\x87"},
        {"def_up",          "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b\xe6\x8f\x90\xe5\x8d\x87"},
        {"hp_up",           "\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc\xe6\x8f\x90\xe5\x8d\x87"},
        {"spd_up",          "\xe9\x80\x9f\xe5\xba\xa6\xe6\x8f\x90\xe5\x8d\x87"},
        {"crit_up",         "\xe6\x9a\xb4\xe5\x87\xbb\xe6\x8f\x90\xe5\x8d\x87"},
        {"pulse_dmg_up",    "\xe7\x94\xb5\xe7\xa3\x81\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"fire_dmg_up",     "\xe7\x81\xab\xe7\x84\xb0\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"cryst_dmg_up",    "\xe5\xaf\x92\xe5\x86\xb7\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"natural_dmg_up",  "\xe8\x87\xaa\xe7\x84\xb6\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"ether_dmg_up",    "\xe4\xbb\xa5\xe5\xa4\xaa\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"physical_dmg_up", "\xe7\x89\xa9\xe7\x90\x86\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"spell_enhance",   "\xe6\xb3\x95\xe6\x9c\xaf\xe5\xa2\x9e\xe5\xb9\x85"},
        {"fire_enhance",    "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85"},
        {"pulse_enhance",   "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85"},
        {"cryst_enhance",   "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
        {"natural_enhance", "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85"},
        {"ether_enhance",   "\xe4\xbb\xa5\xe5\xa4\xaa\xe5\xa2\x9e\xe5\xb9\x85"},
        {"physical_enhance","\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85"},
        {"shelter",         "\xe5\xba\x87\xe6\x8a\xa4"},
        {"shield",          "\xe6\x8a\xa4\xe7\x9b\xbe"},
        {"heal",            "\xe6\xb2\xbb\xe7\x96\x97"},
        {"regen",           "\xe5\x86\x8d\xe7\x94\x9f"},
        {"atk_down",        "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b\xe9\x99\x8d\xe4\xbd\x8e"},
        {"def_down",        "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b\xe9\x99\x8d\xe4\xbd\x8e"},
        {"vuln",            "\xe6\x98\x93\xe4\xbc\xa4"},
        {"weaken",          "\xe5\xbc\xb1\xe5\x8c\x96"},
        {"slow",            "\xe5\x87\x8f\xe9\x80\x9f"},
      };
      char friendlyName[128] = {};
      // Priority 1: Extract character name + skill type from buff string ID
      {
        ExtractBuffName(ab.id, friendlyName, sizeof(friendlyName));
      }
      // Priority 2: Try to match icon name pattern
      if (!friendlyName[0]) {
        for (auto &in : iconNames) {
          if (ab.iconName[0] && strstr(ab.iconName, in.pat)) {
            strncpy(friendlyName, in.name, 127);
            break;
          }
        }
      }
      // Priority 3: Try buff ID pattern
      if (!friendlyName[0]) {
        for (auto &in : iconNames) {
          if (strstr(ab.id, in.pat)) {
            strncpy(friendlyName, in.name, 127);
            break;
          }
        }
      }

      // Force read iconName (volatile prevents optimizer from eliding)
      volatile char ic0 = ab.iconName[0];
      (void)ic0;

      snprintf(g_tooltipText, sizeof(g_tooltipText),
               "%s%s%s\n"
               "\xe6\x97\xb6\xe9\x97\xb4: %s\n"
               "\xe5\xb1\x82\xe6\x95\xb0: %d\n",
               friendlyName[0] ? friendlyName : "",
               friendlyName[0] ? " | " : "",
               ab.id, durStr, ab.enhanceCnt);

      // Helper: determine if AttributeType is a flat value
      auto isFlatAttr = [](int j) -> bool {
        int flat[] = {0, 1, 2, 3, 8, 11, 12, 15, 18, 20, 21, 22, 34, 39, 40, 41, 42, 45, 46, 90};
        for (int i = 0; i < sizeof(flat)/sizeof(flat[0]); i++) {
          if (j == flat[i]) return true;
        }
        return false;
      };

      // Display each modifier array separately with zone label
      // Helper: show non-zero additive entries
      #define SHOW_ADD(arr, label) \
        if (ab.attrs.arr[j] != 0.0f) { \
          char buf[160]; \
          if (isFlatAttr(j)) { \
            snprintf(buf, sizeof(buf), " %+.1f %s (%s)\n", \
                     ab.attrs.arr[j], GetAttrName(j), label); \
          } else { \
            snprintf(buf, sizeof(buf), " %+.1f%% %s (%s)\n", \
                     ab.attrs.arr[j] * 100.0f, GetAttrName(j), label); \
          } \
          tooltip_cat(buf); \
        }
      // Helper: show non-zero percentage entries
      #define SHOW_PCT(arr, label) \
        if (ab.attrs.arr[j] != 0.0f) { \
          char buf[160]; \
          snprintf(buf, sizeof(buf), " %+.1f%% %s (%s)\n", \
                   ab.attrs.arr[j] * 100.0f, GetAttrName(j), label); \
          tooltip_cat(buf); \
        }
      // Helper: show scalar entries that deviate from 1.0
      #define SHOW_SCL(arr, label) \
        if (ab.attrs.arr[j] != 0.0f && ab.attrs.arr[j] != 1.0f) { \
          char buf[160]; \
          snprintf(buf, sizeof(buf), " x%.0f%% %s (%s)\n", \
                   ab.attrs.arr[j] * 100.0f, GetAttrName(j), label); \
          tooltip_cat(buf); \
        }

      // Filter out uniform scale arrays (all entries same value = internal engine factor, not per-attr)
      auto isUniformScale = [](float *arr, int len) -> bool {
        float first = 0.0f;
        bool found = false;
        for (int i = 0; i < len; i++) {
          if (arr[i] != 0.0f && arr[i] != 1.0f) {
            if (!found) { first = arr[i]; found = true; }
            else if (arr[i] != first) return false;
          }
        }
        // Uniform if we found 10+ identical non-trivial values
        int cnt = 0;
        if (found) for (int i = 0; i < len; i++) if (arr[i] == first) cnt++;
        return found && cnt >= 10;
      };
      if (isUniformScale(ab.attrs.mul, 94))         memset(ab.attrs.mul, 0, sizeof(ab.attrs.mul));
      if (isUniformScale(ab.attrs.finalScl, 94))     memset(ab.attrs.finalScl, 0, sizeof(ab.attrs.finalScl));
      if (isUniformScale(ab.attrs.baseFinalScl, 94)) memset(ab.attrs.baseFinalScl, 0, sizeof(ab.attrs.baseFinalScl));
      if (isUniformScale(ab.attrs.baseMul, 94))      memset(ab.attrs.baseMul, 0, sizeof(ab.attrs.baseMul));

      for (int j = 0; j < 94; j++) {
        // 4 additive arrays - flat values
        SHOW_ADD(baseAdd,     "\xe5\x9f\xba\xe7\xa1\x80")
        SHOW_ADD(add,         "\xe5\x9b\xba\xe5\xae\x9a")
        SHOW_ADD(baseFinalAdd,"\xe5\x9f\xba\xe7\xa1\x80\xe6\x9c\x80\xe7\xbb\x88")
        SHOW_ADD(finalAdd,    "\xe6\x9c\x80\xe7\xbb\x88")
        SHOW_PCT(baseMul,     "\xe7\x99\xbe\xe5\x88\x86\xe6\xaf\x94")
        SHOW_SCL(mul,         "\xe4\xb9\x98\xe5\x8c\xba")
        SHOW_SCL(finalScl,    "\xe6\x9c\x80\xe7\xbb\x88\xe7\xbc\xa9\xe6\x94\xbe")
        SHOW_SCL(baseFinalScl,"\xe5\x9f\xba\xe7\xa1\x80\xe7\xbc\xa9\xe6\x94\xbe")
      }

      #undef SHOW_ADD
      #undef SHOW_PCT
      #undef SHOW_SCL

      // Display blackboard entries with semantic mapping
      if (ab.bbCount > 0) {
        // Find rate value
        double rateVal = 0.0;
        for (int b = 0; b < ab.bbCount; b++)
          if (strcmp(ab.bb[b].key, "rate") == 0) { rateVal = ab.bb[b].value; break; }

        bool handled = false;
        if (rateVal != 0.0) {
          // Special buffs with non-enhance rate semantics
          // shelter: rate = damage taken multiplier, so reduction = (1-rate)*100%
          struct { const char *pat; const char *label; bool isDmgTaken; } specialMap[] = {
            {"shelter",
              "\xe5\xba\x87\xe6\x8a\xa4", true},
            {"damage_reduce",
              "\xe5\x87\x8f\xe4\xbc\xa4", true},
          };
          bool handled2 = false;
          const char *srcs[] = { ab.id, ab.iconName };
          for (auto src : srcs) {
            if (!src[0]) continue;
            for (auto &s : specialMap) {
              if (strstr(src, s.pat)) {
                char buf[200];
                if (s.isDmgTaken) {
                  // rate=0.10 => takes 10% dmg => 90% reduction
                  snprintf(buf, sizeof(buf),
                    " %s: \xe5\x87\x8f\xe4\xbc\xa4%.1f%%\n",
                    s.label, (1.0 - rateVal) * 100.0);
                }
                tooltip_cat(buf);
                handled2 = true;
                break;
              }
            }
            if (handled2) break;
          }
          handled = handled2;

          // Enhance buffs: rate = enhancement percentage
          if (!handled) {
          struct { const char *pat; const char *label; } enhMap[] = {
            {"spell_enhance",
              "\xe6\xb3\x95\xe6\x9c\xaf\xe5\xa2\x9e\xe5\xb9\x85(\xe7\x81\xab/\xe7\x94\xb5/\xe8\x87\xaa\xe7\x84\xb6/\xe5\x86\xb0)"},
            {"enhance_spell",
              "\xe6\xb3\x95\xe6\x9c\xaf\xe5\xa2\x9e\xe5\xb9\x85(\xe7\x81\xab/\xe7\x94\xb5/\xe8\x87\xaa\xe7\x84\xb6/\xe5\x86\xb0)"},
            {"physical_enhance", "\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_physical", "\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85"},
            {"fire_enhance", "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_fire", "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85"},
            {"pulse_enhance", "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85"},
            {"electric_enhance", "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_electric", "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85"},
            {"cryst_enhance", "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
            {"ice_enhance", "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_ice", "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_crystal", "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
            {"natural_enhance", "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85"},
            {"nature_enhance", "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_nature", "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85"},
            {"ether_enhance", "\xe8\xb6\x85\xe5\x9f\x9f\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_ether", "\xe8\xb6\x85\xe5\x9f\x9f\xe5\xa2\x9e\xe5\xb9\x85"},
          };
          const char *sources[] = { ab.id, ab.iconName };
          for (auto src : sources) {
            if (!src[0]) continue;
            for (auto &m : enhMap) {
              if (strstr(src, m.pat)) {
                char buf[200];
                snprintf(buf, sizeof(buf), " %s: +%.1f%%\n", m.label, rateVal * 100.0);
                tooltip_cat(buf);
                handled = true;
                break;
              }
            }
            if (handled) break;
          }
          // Generic fallback
          if (!handled) {
            char buf[200];
            snprintf(buf, sizeof(buf),
              " \xe5\xa2\x9e\xe5\xb9\x85: +%.1f%%\n", rateVal * 100.0);
            tooltip_cat(buf);
            handled = true;
          }
          }
        }
        // Show remaining non-rate blackboard entries with semantic labels
        struct { const char *raw; const char *label; bool isPct; } keyMap[] = {
          // === Shield ===
          {"shield_def_rate", "\xe6\x8a\xa4\xe7\x9b\xbe(\xe9\x98\xb2\xe5\xbe\xa1\xc3\x97)", false},
          {"shield_atk_rate", "\xe6\x8a\xa4\xe7\x9b\xbe(\xe6\x94\xbb\xe5\x87\xbb\xc3\x97)", false},
          {"shield_hp_rate",  "\xe6\x8a\xa4\xe7\x9b\xbe(\xe7\x94\x9f\xe5\x91\xbd\xc3\x97)", false},
          {"shield_base",     "\xe6\x8a\xa4\xe7\x9b\xbe\xe5\x9f\xba\xe7\xa1\x80\xe5\x80\xbc", false},
          {"FinalShield",     "\xe6\x8a\xa4\xe7\x9b\xbe", false},
          {"shield",          "\xe6\x8a\xa4\xe7\x9b\xbe", false},
          // === Base stat ratios ===
          {"atk_ratio",       "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", true},
          {"def_ratio",       "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", true},
          {"hp_ratio",        "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", true},
          {"hp_percent",      "\xe7\x94\x9f\xe5\x91\xbd", true},
          {"heal_ratio",      "\xe6\xb2\xbb\xe7\x96\x97\xe6\xaf\x94\xe4\xbe\x8b", true},
          {"damage",          "\xe4\xbc\xa4\xe5\xae\xb3", false},
          {"heal",            "\xe6\xb2\xbb\xe7\x96\x97\xe9\x87\x8f", false},
          // === Base stats (flat) ===
          {"atk",             "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", false},
          {"def",             "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", false},
          {"max_hp",          "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", false},
          {"atk_up",          "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", true},
          {"def_up",          "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", true},
          {"hp_up",           "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", true},
          // === Speed & cooldown ===
          {"move_speed",      "\xe7\xa7\xbb\xe5\x8a\xa8\xe9\x80\x9f\xe5\xba\xa6", true},
          {"move_speed_scalar","\xe7\xa7\xbb\xe5\x8a\xa8\xe9\x80\x9f\xe5\xba\xa6", true},
          {"attack_rate",     "\xe6\x94\xbb\xe5\x87\xbb\xe9\x80\x9f\xe5\xba\xa6", true},
          {"skill_cooldown",  "\xe6\x8a\x80\xe8\x83\xbd\xe5\x86\xb7\xe5\x8d\xb4", true},
          {"combo_cd_scalar", "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe5\x86\xb7\xe5\x8d\xb4", true},
          {"combo_cd",        "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe5\x86\xb7\xe5\x8d\xb4", false},
          // === Elemental damage increase ===
          {"physical_dmg_up", "\xe7\x89\xa9\xe7\x90\x86\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"fire_dmg_up",     "\xe7\x81\xab\xe7\x84\xb0\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"pulse_dmg_up",    "\xe7\x94\xb5\xe7\xa3\x81\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"cryst_dmg_up",    "\xe5\xaf\x92\xe5\x86\xb7\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"natural_dmg_up",  "\xe8\x87\xaa\xe7\x84\xb6\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"ether_dmg_up",    "\xe8\xb6\x85\xe5\x9f\x9f\xe4\xbc\xa4\xe5\xae\xb3", true},
          // === Skill type damage increase ===
          {"normal_atk_dmg",  "\xe6\x99\xae\xe6\x94\xbb\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"normal_skill_dmg","\xe6\x88\x98\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"combo_skill_dmg", "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"ult_dmg",         "\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"dmg_scale",       "\xe4\xbc\xa4\xe5\xae\xb3\xe5\x80\x8d\xe7\x8e\x87", true},
          // === Enhanced (增幅) per element ===
          {"physical_enhanced","\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"fire_enhanced",   "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"pulse_enhanced",  "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"cryst_enhanced",  "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"natural_enhanced","\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"ether_enhanced",  "\xe8\xb6\x85\xe5\x9f\x9f\xe5\xa2\x9e\xe5\xb9\x85", true},
          // === Vulnerable (脆弱) per element ===
          {"physical_vulnerable","\xe7\x89\xa9\xe7\x90\x86\xe8\x84\x86\xe5\xbc\xb1", true},
          {"fire_vulnerable", "\xe7\x81\xab\xe7\x84\xb0\xe8\x84\x86\xe5\xbc\xb1", true},
          {"pulse_vulnerable","\xe7\x94\xb5\xe7\xa3\x81\xe8\x84\x86\xe5\xbc\xb1", true},
          {"cryst_vulnerable","\xe5\xaf\x92\xe5\x86\xb7\xe8\x84\x86\xe5\xbc\xb1", true},
          {"natural_vulnerable","\xe8\x87\xaa\xe7\x84\xb6\xe8\x84\x86\xe5\xbc\xb1", true},
          {"ether_vulnerable","\xe8\xb6\x85\xe5\x9f\x9f\xe8\x84\x86\xe5\xbc\xb1", true},
          // === Burst damage (爆发伤害) per element ===
          {"fire_burst_dmg",  "\xe7\x81\xab\xe7\x84\xb0\xe7\x88\x86\xe5\x8f\x91", true},
          {"pulse_burst_dmg", "\xe7\x94\xb5\xe7\xa3\x81\xe7\x88\x86\xe5\x8f\x91", true},
          {"cryst_burst_dmg", "\xe5\xaf\x92\xe5\x86\xb7\xe7\x88\x86\xe5\x8f\x91", true},
          {"natural_burst_dmg","\xe8\x87\xaa\xe7\x84\xb6\xe7\x88\x86\xe5\x8f\x91", true},
          // === Abnormal damage (异常伤害) per element ===
          {"fire_abnormal_dmg","\xe7\x87\x83\xe7\x83\xa7\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"pulse_abnormal_dmg","\xe5\xaf\xbc\xe7\x94\xb5\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"cryst_abnormal_dmg","\xe5\x86\xbb\xe7\xbb\x93\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"natural_abnormal_dmg","\xe8\x85\x90\xe8\x9a\x80\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"inflict_dmg",     "\xe5\xbc\x82\xe5\xb8\xb8\xe4\xbc\xa4\xe5\xae\xb3", true},
          // === Damage taken (承伤系数) ===
          {"physical_dmg_taken","\xe7\x89\xa9\xe7\x90\x86\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"fire_dmg_taken",  "\xe7\x81\xab\xe7\x84\xb0\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"pulse_dmg_taken", "\xe7\x94\xb5\xe7\xa3\x81\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"cryst_dmg_taken", "\xe5\xaf\x92\xe5\x86\xb7\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"natural_dmg_taken","\xe8\x87\xaa\xe7\x84\xb6\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"ether_dmg_taken", "\xe8\xb6\x85\xe5\x9f\x9f\xe6\x89\xbf\xe4\xbc\xa4", true},
          // === Resist (抗性) ===
          {"physical_resist", "\xe7\x89\xa9\xe7\x90\x86\xe6\x8a\x97\xe6\x80\xa7", true},
          {"fire_resist",     "\xe7\x81\xab\xe7\x84\xb0\xe6\x8a\x97\xe6\x80\xa7", true},
          {"pulse_resist",    "\xe7\x94\xb5\xe7\xa3\x81\xe6\x8a\x97\xe6\x80\xa7", true},
          {"cryst_resist",    "\xe5\xaf\x92\xe5\x86\xb7\xe6\x8a\x97\xe6\x80\xa7", true},
          {"natural_resist",  "\xe8\x87\xaa\xe7\x84\xb6\xe6\x8a\x97\xe6\x80\xa7", true},
          {"ether_resist",    "\xe8\xb6\x85\xe5\x9f\x9f\xe6\x8a\x97\xe6\x80\xa7", true},
          // === Poise/Stagger ===
          {"poise_dmg_up",    "\xe5\xa4\xb1\xe8\xa1\xa1\xe5\x80\xbc\xe8\xbe\x93\xe5\x87\xba", true},
          {"poise_dmg_taken", "\xe5\xa4\xb1\xe8\xa1\xa1\xe5\x80\xbc\xe6\x89\xbf\xe5\x8f\x97", true},
          {"broken_dmg",      "\xe5\xaf\xb9\xe5\xa4\xb1\xe8\xa1\xa1\xe7\x9b\xae\xe6\xa0\x87\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"break_dmg_taken", "\xe5\xa4\x84\xe5\x86\xb3\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"knockdown_time",  "\xe5\x80\x92\xe5\x9c\xb0\xe6\x97\xb6\xe9\x97\xb4", false},
          // === Crit ===
          {"crit_rate",       "\xe6\x9a\xb4\xe5\x87\xbb\xe7\x8e\x87", true},
          {"crit_damage",     "\xe6\x9a\xb4\xe5\x87\xbb\xe4\xbc\xa4\xe5\xae\xb3", true},
          // === Heal & shield output ===
          {"heal_output",     "\xe6\xb2\xbb\xe7\x96\x97\xe6\x95\x88\xe6\x9e\x9c\xe5\x8a\xa0\xe6\x88\x90", true},
          {"heal_taken",      "\xe8\xa2\xab\xe6\xb2\xbb\xe7\x96\x97\xe5\x8a\xa0\xe6\x88\x90", true},
          {"shield_output",   "\xe6\x8a\xa4\xe7\x9b\xbe\xe9\x87\x8f\xe5\x8a\xa0\xe6\x88\x90", true},
          {"shield_taken",    "\xe5\x8f\x97\xe6\x8a\xa4\xe7\x9b\xbe\xe5\x8a\xa0\xe6\x88\x90", true},
          // === SP & ATB ===
          {"ult_sp_gain",     "\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80\xe5\x85\x85\xe8\x83\xbd\xe6\x95\x88\xe7\x8e\x87", true},
          {"atb_cost",        "\xe6\x8a\x80\xe5\x8a\x9b\xe6\xb6\x88\xe8\x80\x97", false},
          {"life_steal",      "\xe5\x90\xb8\xe8\xa1\x80", true},
          // === Weakness/Shelter ===
          {"weakness_dmg",    "\xe8\x99\x9a\xe5\xbc\xb1\xe7\xb3\xbb\xe6\x95\xb0", true},
          {"shelter_dmg",     "\xe5\xba\x87\xe6\x8a\xa4\xe7\xb3\xbb\xe6\x95\xb0", true},
          // === Status effects ===
          {"probability",     "\xe6\xa6\x82\xe7\x8e\x87", true},
          {"vuln_rate",       "\xe6\x98\x93\xe4\xbc\xa4", true},
          {"weaken_rate",     "\xe5\xbc\xb1\xe5\x8c\x96", true},
          {"slow_rate",       "\xe5\x87\x8f\xe9\x80\x9f", true},
          {"slow_action",     "\xe5\x8a\xa8\xe4\xbd\x9c\xe5\x87\x8f\xe9\x80\x9f", true},
          // === Infliction (源石技艺强度) ===
          {"infliction_enhance","\xe6\xba\x90\xe7\x9f\xb3\xe6\x8a\x80\xe8\x89\xba\xe5\xbc\xba\xe5\xba\xa6", false},
          // === Combo zone ===
          {"combo_dmg",       "\xe8\xbf\x9e\xe5\x87\xbb\xe5\xa2\x9e\xe4\xbc\xa4", true},
        };
        for (int b = 0; b < ab.bbCount; b++) {
          const char *k = ab.bb[b].key;
          double v = ab.bb[b].value;
          if (strcmp(k, "duration") == 0 || strcmp(k, "rate") == 0) continue;
          if (v == 0.0) continue;
          // Try semantic mapping
          bool found = false;
          char buf[200];
          for (auto &km : keyMap) {
            if (strcmp(k, km.raw) == 0) {
              if (km.isPct)
                snprintf(buf, sizeof(buf), " %s: %+.1f%%\n", km.label, v * 100.0);
              else
                snprintf(buf, sizeof(buf), " %s: %.2f\n", km.label, v);
              tooltip_cat(buf);
              found = true;
              break;
            }
          }
          // Unmapped blackboard keys are intentionally hidden.
          // They are typically internal script parameters (e.g. cooldowns, trigger flags)
          // that clutter the UI and confuse players.
        }
      }

      // If tooltip has no effect info, dump blackboard to log for diagnostics
      {
        bool hasEffect = false;
        int lineCount = 0;
        for (const char *p = g_tooltipText; *p; p++)
          if (*p == '\n') lineCount++;
        if (lineCount > 3) hasEffect = true;
        if (!hasEffect) {
          Log("[NO_EFFECT] buff=%s icon=%s bbCount=%d", ab.id, ab.iconName, ab.bbCount);
          for (int b = 0; b < ab.bbCount; b++)
            Log("  bb[%d] key='%s' value=%.6f", b, ab.bb[b].key, ab.bb[b].value);
          // Also log non-zero attributes
          for (int j = 0; j < 94; j++) {
            if (ab.attrs.add[j] != 0) Log("  attr add[%d]=%f", j, ab.attrs.add[j]);
            if (ab.attrs.baseAdd[j] != 0) Log("  attr baseAdd[%d]=%f", j, ab.attrs.baseAdd[j]);
            if (ab.attrs.mul[j] != 0) Log("  attr mul[%d]=%f", j, ab.attrs.mul[j]);
            if (ab.attrs.baseMul[j] != 0) Log("  attr baseMul[%d]=%f", j, ab.attrs.baseMul[j]);
            if (ab.attrs.finalAdd[j] != 0) Log("  attr finalAdd[%d]=%f", j, ab.attrs.finalAdd[j]);
            if (ab.attrs.baseFinalAdd[j] != 0) Log("  attr baseFinalAdd[%d]=%f", j, ab.attrs.baseFinalAdd[j]);
            if (ab.attrs.finalScl[j] != 0) Log("  attr finalScl[%d]=%f", j, ab.attrs.finalScl[j]);
            if (ab.attrs.baseFinalScl[j] != 0) Log("  attr baseFinalScl[%d]=%f", j, ab.attrs.baseFinalScl[j]);
          }
        }
      }

      // Position tooltip near cursor
      g_tooltipX = cursor.x;
      g_tooltipY = cursor.y;
      } else {
        newHover = false; // invalid or missing buff
      }
    } else {
      newHover = false;
    }
    LeaveCriticalSection(&g_buffLock);

    static char s_lastTooltipText[2048] = {0};
    static int s_lastTooltipX = 0;
    static int s_lastTooltipY = 0;

    if (newHover != g_showTooltip) {
      g_showTooltip = newHover;
      if (newHover) {
        strcpy(s_lastTooltipText, g_tooltipText);
        s_lastTooltipX = g_tooltipX;
        s_lastTooltipY = g_tooltipY;
      }
      InvalidateRect(hwnd, NULL, TRUE);
    } else if (newHover) {
      if (strcmp(s_lastTooltipText, g_tooltipText) != 0 || 
          s_lastTooltipX != g_tooltipX || 
          s_lastTooltipY != g_tooltipY) {
        strcpy(s_lastTooltipText, g_tooltipText);
        s_lastTooltipX = g_tooltipX;
        s_lastTooltipY = g_tooltipY;
        InvalidateRect(hwnd, NULL, TRUE);
      }
    }
    // Always invalidate for skill HUD continuous refresh
    InvalidateRect(hwnd, NULL, TRUE);
    return 0;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Hooks - MINIMAL work, no Unity API calls, no Win32 calls
// ============================================================================
typedef void (*tProgressBar)(void *, void *, void *, void *, void *);
static tProgressBar oProgressBar = nullptr;
void hkProgressBar(void *self, void *p1, void *p2, void *p3, void *mi) {
  uintptr_t isAdd = (uintptr_t)p2;
  ActiveBuff ab = {};
  ReadBuffData(p1, &ab);
  if (isAdd && ab.buffObj) {
    EnterCriticalSection(&g_buffLock);
    if (g_buffCount < 64) {
      ab.source = 0; // ProgressBar = buff
      g_buffs[g_buffCount++] = ab;
    }
    LeaveCriticalSection(&g_buffLock);
  } else {
    EnterCriticalSection(&g_buffLock);
    for (int i = 0; i < g_buffCount; i++) {
      if (g_buffs[i].instUid == ab.instUid) {
        g_buffs[i] = g_buffs[--g_buffCount];
        break;
      }
    }
    LeaveCriticalSection(&g_buffLock);
  }
  oProgressBar(self, p1, p2, p3, mi);
}

// Same logic for _OnBuffIconChange (debuffs like elemental attachments use this
// path)
typedef void (*tBuffIconChange)(void *, void *, void *, void *, void *);
static tBuffIconChange oBuffIconChange = nullptr;
void hkBuffIconChange(void *self, void *p1, void *p2, void *p3, void *mi) {
  uintptr_t isAdd = (uintptr_t)p2;
  ActiveBuff ab = {};
  ReadBuffData(p1, &ab);
  if (isAdd && ab.buffObj) {
    // Check for duplicate (same buff might go through both paths)
    EnterCriticalSection(&g_buffLock);
    bool found = false;
    for (int i = 0; i < g_buffCount; i++) {
      if (g_buffs[i].instUid == ab.instUid) {
        found = true;
        break;
      }
    }
    if (!found && g_buffCount < 64) {
      ab.source = 1; // BuffIcon = debuff
      g_buffs[g_buffCount++] = ab;
    }
    LeaveCriticalSection(&g_buffLock);
  } else {
    EnterCriticalSection(&g_buffLock);
    for (int i = 0; i < g_buffCount; i++) {
      if (g_buffs[i].instUid == ab.instUid) {
        g_buffs[i] = g_buffs[--g_buffCount];
        break;
      }
    }
    LeaveCriticalSection(&g_buffLock);
  }
  oBuffIconChange(self, p1, p2, p3, mi);
}

typedef void (*tEnhance)(void *, void *, void *);
static tEnhance oEnhance = nullptr;
void hkEnhance(void *self, void *p1, void *mi) {
  ActiveBuff ab = {};
  ReadBuffData(p1, &ab);
  EnterCriticalSection(&g_buffLock);
  for (int i = 0; i < g_buffCount; i++) {
    if (g_buffs[i].instUid == ab.instUid) {
      g_buffs[i].enhanceCnt = ab.enhanceCnt;
      g_buffs[i].trueEnhanceCnt = ab.enhanceCnt; // Keep engine-side count in sync
      break;
    }
  }
  LeaveCriticalSection(&g_buffLock);
  oEnhance(self, p1, mi);
}

// Hook for _ClearMainChar - clears all buffs when entering dungeon/switching
// char
typedef void (*tClearMainChar)(void *, void *);
static tClearMainChar oClearMainChar = nullptr;
void hkClearMainChar(void *self, void *mi) {
  EnterCriticalSection(&g_buffLock);
  g_buffCount = 0;
  LeaveCriticalSection(&g_buffLock);
  Log("[CLEAR] All buffs cleared");
  oClearMainChar(self, mi);
}

typedef void (*tLateTick)(void *, float, void *);
static tLateTick oLateTick = nullptr;
void hkLateTick(void *self, float dt, void *mi) {
  oLateTick(self, dt, mi);

  // Every ~60 frames (~1 sec), validate buff lifetimes + read display order
  static int s_frameCounter = 0;
  if (++s_frameCounter >= 60) {
    s_frameCounter = 0;
    EnterCriticalSection(&g_buffLock);
    // Remove expired buffs + refresh lifeTime + refresh attrs for tooltip
    for (int i = g_buffCount - 1; i >= 0; i--) {
      if (g_buffs[i].buffObj && g_getLifeTime) {
        float life = UnboxFloat(Invoke(g_getLifeTime, g_buffs[i].buffObj));
        g_buffs[i].lifeTime = life; // Save for tooltip display
        if (g_buffs[i].duration < 100000.0f && life <= 0.0f) {
          g_buffs[i] = g_buffs[--g_buffCount];
          continue;
        }
      }
      // Re-read enhanceCnt from the engine to keep trueEnhanceCnt fresh
      // (it may change after the initial hook, e.g. character buffs enhance over time)
      if (g_buffs[i].buffObj && g_getEnhanceCnt) {
        __try {
          void *boxedCnt = Invoke(g_getEnhanceCnt, g_buffs[i].buffObj);
          if (boxedCnt) {
            g_buffs[i].trueEnhanceCnt = *(int *)((char *)boxedCnt + 16);
          }
        } __except(1) {}
      }
      // Re-read attributes from loader (may not be filled at hook time)
      if (g_buffs[i].buffObj && g_loaderOffset > 0) {
        __try {
          void *loader = *(void **)((char *)g_buffs[i].buffObj + g_loaderOffset);
          if (loader) {
            auto readArr = [](void *ldr, int off, float *dst, int max) {
              void *arr = *(void **)((char *)ldr + off);
              if (!arr) return;
              int32_t len = *(int32_t *)((char *)arr + 0x18);
              if (len <= 0 || len > max) return;
              double *data = (double *)((char *)arr + 0x20);
              for (int j = 0; j < len && j < max; j++)
                dst[j] = (float)data[j];
            };
            readArr(loader, 0x18, g_buffs[i].attrs.add, 96);
            readArr(loader, 0x38, g_buffs[i].attrs.baseAdd, 96);
            readArr(loader, 0x20, g_buffs[i].attrs.finalAdd, 96);
            readArr(loader, 0x40, g_buffs[i].attrs.baseFinalAdd, 96);
            readArr(loader, 0x10, g_buffs[i].attrs.mul, 96);
            readArr(loader, 0x30, g_buffs[i].attrs.baseMul, 96);
            readArr(loader, 0x28, g_buffs[i].attrs.finalScl, 96);
            readArr(loader, 0x48, g_buffs[i].attrs.baseFinalScl, 96);
          }
        } __except(1) {}
      }
    }

    // Read display order from UIBuffNode.m_orderedBuffCellList
    if (g_orderedListOffset > 0 && g_getBuffInstanceUid) {
      void *buffNode = *(void **)((char *)self + 0xD0);
      if (buffNode) {
        __try {
          void *orderedList =
              *(void **)((char *)buffNode + g_orderedListOffset);
          if (orderedList) {
            // C# List<T>: _items at +0x10, _size at +0x18
            void *items = *(void **)((char *)orderedList + 0x10);
            int size = *(int *)((char *)orderedList + 0x18);
            if (items && size >= 0 && size <= 64) {
              int newDisplayCount = 0;
              // Array elements start at offset 0x20 in Il2Cpp array, each is a pointer
              void **elements = (void **)((char *)items + 0x20);
              for (int ci = 0; ci < size; ci++) {
                void *cell = elements[ci];
                if (!cell) continue;
                // Get buffInstanceUid from cell
                void *boxedUid = Invoke(g_getBuffInstanceUid, cell);
                if (!boxedUid) continue;
                uint32_t cellUid = *(uint32_t *)((char *)boxedUid + 16);
                g_displayUids[newDisplayCount++] = cellUid;
                
                // Extract stack count from UIBuffCell._buffStackCountText (offset 0x30)
                int uiStackCount = 1;
                static bool s_stackDbgLogged = false;
                __try {
                  void *textComp = *(void **)((char *)cell + 0x30);
                  if (!s_stackDbgLogged)
                    Log("[STACK] cell=%p textComp(0x30)=%p", cell, textComp);
                  if (textComp) {
                    void *klass = il2cpp_object_get_class(textComp);
                    if (klass) {
                      const char *className = il2cpp_class_get_name(klass);
                      const char *classNs = il2cpp_class_get_namespace ? il2cpp_class_get_namespace(klass) : "";
                      if (!s_stackDbgLogged)
                        Log("[STACK] textComp class: %s.%s", classNs ? classNs : "", className ? className : "?");
                      // Try get_text on this class and parents
                      void *curr = klass;
                      void *getText = nullptr;
                      while (curr && !getText) {
                        getText = FindMethod(curr, "get_text", 0);
                        if (!getText) {
                          if (!s_stackDbgLogged) {
                            const char *cn = il2cpp_class_get_name(curr);
                            Log("[STACK]   no get_text in %s", cn ? cn : "?");
                          }
                          curr = il2cpp_class_get_parent(curr);
                        }
                      }
                      if (!s_stackDbgLogged)
                        Log("[STACK] getText method: %p", getText);
                      if (getText) {
                        void *sysStr = Invoke(getText, textComp);
                        if (!s_stackDbgLogged)
                          Log("[STACK] sysStr: %p", sysStr);
                        if (sysStr) {
                          char textBuf[32];
                          int len = ReadStr(sysStr, textBuf, sizeof(textBuf));
                          if (!s_stackDbgLogged)
                            Log("[STACK] text='%s' len=%d", textBuf, len);
                          if (len > 0) {
                            int s = atoi(textBuf);
                            if (s > 0) uiStackCount = s;
                          }
                        }
                      }
                    }
                  }
                  s_stackDbgLogged = true;
                } __except(1) {
                  if (!s_stackDbgLogged)
                    Log("[STACK] EXCEPTION reading stack text");
                  s_stackDbgLogged = true;
                }

                // Fallback: if the buff isn't in our array (e.g. bypassed normal events)
                // Extract it directly from UIBuffCell.m_buffPtr (offset 0x58)
                bool found = false;
                for (int bi = 0; bi < g_buffCount; bi++) {
                  if (g_buffs[bi].instUid == cellUid) {
                    if (uiStackCount > 1) g_buffs[bi].enhanceCnt = uiStackCount;
                    found = true;
                    break;
                  }
                }
                if (!found && g_buffCount < 64) {
                  void *buffPtr = *(void **)((char *)cell + 0x58);
                  if (buffPtr) {
                    struct { void *b; uint32_t u; } dummy = { buffPtr, cellUid };
                    ActiveBuff ab = {};
                    ReadBuffData(&dummy, &ab);
                    ab.source = 2; // Source = LateTick fallback
                    if (uiStackCount > 1) ab.enhanceCnt = uiStackCount;
                    g_buffs[g_buffCount++] = ab;
                  }
                }
              }
              g_displayCount = newDisplayCount;

              // Remove any tracked buffs NOT in the game's display list
              // (handles dungeon entry/exit, scene changes, etc.)
              for (int bi = g_buffCount - 1; bi >= 0; bi--) {
                if (g_buffs[bi].duration >= 100000.0f)
                  continue; // keep permanent/hidden
                bool inDisplay = false;
                for (int di = 0; di < newDisplayCount; di++) {
                  if (g_buffs[bi].instUid == g_displayUids[di]) {
                    inDisplay = true;
                    break;
                  }
                }
                if (!inDisplay) {
                  g_buffs[bi] = g_buffs[--g_buffCount];
                }
              }
            } else if (size == 0) {
              // Game list is empty - clear everything visible
              g_displayCount = 0;
              for (int bi = g_buffCount - 1; bi >= 0; bi--) {
                if (g_buffs[bi].duration < 100000.0f) {
                  g_buffs[bi] = g_buffs[--g_buffCount];
                }
              }
            }
          }
        } __except (1) { /* ignore */
        }
      }
    }
    LeaveCriticalSection(&g_buffLock);
  }

  // Lazy-load s_attributesToModify mapping (static field only available after game init)
  if (!g_satmLoaded && g_satmField && il2cpp_field_static_get_value) {
    void* satmArr = nullptr;
    il2cpp_field_static_get_value(g_satmField, &satmArr);
    if (satmArr) {
      int32_t sLen = *(int32_t *)((char *)satmArr + 0x18);
      if (sLen > 0 && sLen <= 96) {
        int32_t *sData = (int32_t *)((char *)satmArr + 0x20);
        for (int si = 0; si < sLen; si++)
          g_satmMap[si] = sData[si];
        g_satmLen = sLen;
        g_satmLoaded = true;
        Log("[OK] s_attributesToModify loaded: %d entries", sLen);
        for (int si = 0; si < sLen; si++)
          Log("  arr[%d] -> AttrType %d (%s)", si, g_satmMap[si], GetAttrName(g_satmMap[si]));
      }
    }
  }

  // F9BuffDump removed for release
}

// ============================================================================
// Overlay Thread - completely separate from game thread
// ============================================================================
DWORD WINAPI OverlayThread(LPVOID) {
  // Wait for game window
  while (g_running && !g_gameHwnd) {
    g_gameHwnd = FindWindowA("UnityWndClass", NULL);
    Sleep(500);
  }
  if (!g_gameHwnd)
    return 0;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = OverlayWndProc;
  wc.hInstance = GetModuleHandle(NULL);
  wc.lpszClassName = "BuffTooltipOverlay";
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  RegisterClassA(&wc);

  RECT clientRect;
  GetClientRect(g_gameHwnd, &clientRect);
  POINT pt = {0, 0};
  ClientToScreen(g_gameHwnd, &pt);

  g_overlayHwnd = CreateWindowExA(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
      wc.lpszClassName, "", WS_POPUP | WS_VISIBLE, pt.x, pt.y, clientRect.right,
      clientRect.bottom, NULL, NULL, wc.hInstance, NULL);
  SetLayeredWindowAttributes(g_overlayHwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

  Log("[OK] Overlay window created: %p", g_overlayHwnd);

  // Start a timer for hit-testing at ~60fps
  SetTimer(g_overlayHwnd, 1, 16, NULL);

  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  if (g_overlayHwnd)
    DestroyWindow(g_overlayHwnd);
  return 0;
}

// ============================================================================
// Main Thread - Il2Cpp init + hooks only
// ============================================================================
DWORD WINAPI MainThread(LPVOID) {
  char p[MAX_PATH];
  GetModuleFileNameA(NULL, p, MAX_PATH);
  std::string d(p);
  size_t pos = d.find_last_of("\\/");
  if (pos != std::string::npos)
    d = d.substr(0, pos + 1);
  g_logFile = fopen((d + "plugin\\buff_sniff_log.txt").c_str(), "w");

  Log("=== EndfieldBetterBuffBar Phase 2d: Lightweight Overlay ===");
  for (int i = 0; i < 120; i++) {
    if (GetModuleHandleW(L"GameAssembly.dll"))
      break;
    Sleep(1000);
  }
  Sleep(15000);
  if (!Resolve()) {
    Log("[FATAL] resolve failed");
    return 1;
  }

  void *dom = il2cpp_domain_get();
  il2cpp_thread_attach(dom);
  size_t ac = 0;
  void **asms = il2cpp_domain_get_assemblies(dom, &ac);

  void *buffClass = FindClass("Beyond.Gameplay.Core", "Buff", asms, ac);
  if (buffClass) {
    g_getId = FindMethod(buffClass, "get_id", 0);
    g_getDuration = FindMethod(buffClass, "get_duration", 0);
    g_getLifeTime = FindMethod(buffClass, "get_lifeTime", 0);
    g_getEnhanceCnt = FindMethod(buffClass, "get_enhanceCnt", 0);
    // Resolve m_attributeModifierLoader field offset
    void* bfIter = nullptr;
    void* bfield;
    Log("  Buff fields:");
    while ((bfield = il2cpp_class_get_fields(buffClass, &bfIter))) {
      const char* bfname = il2cpp_field_get_name(bfield);
      size_t bfoff = il2cpp_field_get_offset(bfield);
      Log("    [0x%X] %s", (int)bfoff, bfname ? bfname : "?");
      if (bfname && strcmp(bfname, "m_attributeModifierLoader") == 0) {
        g_loaderOffset = bfoff;
      }
    }
    if (g_loaderOffset > 0)
      Log("[OK] Buff.m_attributeModifierLoader offset: 0x%X", (int)g_loaderOffset);
    else
      Log("[WARN] m_attributeModifierLoader not found in Buff fields, checking parent...");
    // Check parent class fields if not found
    if (g_loaderOffset == 0) {
      void* parentClass = il2cpp_class_get_parent(buffClass);
      while (parentClass && g_loaderOffset == 0) {
        const char* pname = il2cpp_class_get_name(parentClass);
        Log("  Parent: %s", pname ? pname : "?");
        void* pfIter = nullptr;
        void* pfield;
        while ((pfield = il2cpp_class_get_fields(parentClass, &pfIter))) {
          const char* pfname = il2cpp_field_get_name(pfield);
          size_t pfoff = il2cpp_field_get_offset(pfield);
          Log("    [0x%X] %s", (int)pfoff, pfname ? pfname : "?");
          if (pfname && strcmp(pfname, "m_attributeModifierLoader") == 0) {
            g_loaderOffset = pfoff;
          }
        }
        parentClass = il2cpp_class_get_parent(parentClass);
      }
      if (g_loaderOffset > 0)
        Log("[OK] Found in parent: offset 0x%X", (int)g_loaderOffset);
    }
    Log("[OK] Buff class resolved (loaderOffset=0x%X)", (int)g_loaderOffset);

  }

  void *amlClass = FindClass("", "AttributeModifierLoader", asms, ac);
  if (amlClass) {
    g_getAttrAdd = FindMethod(amlClass, "get_attributeAdditions", 0);
    g_getAttrMul = FindMethod(amlClass, "get_attributeMultipliers", 0);
    // Dump all AML field offsets
    Log("  AML fields:");
    void* amlIter = nullptr;
    void* amlField;
    while ((amlField = il2cpp_class_get_fields(amlClass, &amlIter))) {
      const char* afname = il2cpp_field_get_name(amlField);
      size_t afoff = il2cpp_field_get_offset(amlField);
      Log("    [0x%X] %s", (int)afoff, afname ? afname : "?");
      // Read s_attributesToModify static field (maps array index -> AttributeType)
      if (afname && strcmp(afname, "s_attributesToModify") == 0) {
        g_satmField = amlField; // Save for lazy loading in LateTick
        void* satmArr = nullptr;
        il2cpp_field_static_get_value(amlField, &satmArr);
        if (satmArr) {
          int32_t sLen = *(int32_t *)((char *)satmArr + 0x18);
          Log("    s_attributesToModify: arr=%p len=%d", satmArr, sLen);
          if (sLen > 0 && sLen <= 96) {
            // This is likely an int[] or AttributeType[] array
            int32_t *sData = (int32_t *)((char *)satmArr + 0x20);
            for (int si = 0; si < sLen; si++) {
              Log("      satm[%d] = %d", si, sData[si]);
            }
          }
        } else {
          Log("    s_attributesToModify: null (static class may not be initialized)");
        }
      }
    }
    Log("[OK] AttributeModifierLoader resolved");
  }

  // Dump GEnums.AttributeType enum values
  void *attrTypeClass = FindClass("Beyond.GEnums", "AttributeType", asms, ac);
  if (!attrTypeClass)
    attrTypeClass = FindClass("", "AttributeType", asms, ac);
  if (attrTypeClass && il2cpp_field_static_get_value) {
    Log("========== AttributeType Enum Dump ==========");
    void *fieldIter = nullptr;
    void *field;
    while ((field = il2cpp_class_get_fields(attrTypeClass, &fieldIter))) {
      const char *fname = il2cpp_field_get_name(field);
      if (!fname || strcmp(fname, "value__") == 0)
        continue;
      int32_t val = 0;
      il2cpp_field_static_get_value(field, &val);
      Log("  [%d] %s", val, fname);
    }
    Log("========== End Enum Dump ==========");
  } else {
    Log("[WARN] AttributeType enum not found");
  }

  // ========== Phase 1: Skill/Ult Class Discovery Dump ==========
  // Comprehensive scan for synchro skill CD and ultimate charge data sources.
  // Results are written to buff_sniff_log.txt for manual analysis.
  DumpSkillRelatedClasses(asms, ac);
  DumpBeyondUIClasses(asms, ac);
  DumpBeyondGameplayClasses(asms, ac);

  void *hpClass = FindClass("Beyond.UI", "MainCharHpBar", asms, ac);
  // Full MainCharHpBar dump — find skill/ult references
  DumpMainCharHpBarFull(hpClass);
  if (MH_Initialize() != MH_OK) {
    Log("[FATAL] MH_Initialize failed");
    return 1;
  }
  InitializeCriticalSection(&g_buffLock);

  void *m;
  if ((m = FindMethod(hpClass, "_OnProgressBarBuffChange", 3)))
    Hook(m, "ProgressBar", (void *)hkProgressBar, (void **)&oProgressBar);
  if ((m = FindMethod(hpClass, "_OnBuffIconChange", 3)))
    Hook(m, "BuffIcon", (void *)hkBuffIconChange, (void **)&oBuffIconChange);
  if ((m = FindMethod(hpClass, "_OnBuffEnhanceChanged", 1)))
    Hook(m, "Enhance", (void *)hkEnhance, (void **)&oEnhance);
  if ((m = FindMethod(hpClass, "LateTick", 1)))
    Hook(m, "LateTick", (void *)hkLateTick, (void **)&oLateTick);
  if ((m = FindMethod(hpClass, "_ClearMainChar", 0)))
    Hook(m, "ClearMainChar", (void *)hkClearMainChar, (void **)&oClearMainChar);

  // Resolve UIBuffCell.get_buffInstanceUid for display order tracking
  void *buffCellClass = FindClass("Beyond.UI", "UIBuffCell", asms, ac);
  if (buffCellClass) {
    g_getBuffInstanceUid = FindMethod(buffCellClass, "get_buffInstanceUid", 0);
    Log("[OK] UIBuffCell.get_buffInstanceUid: %p", g_getBuffInstanceUid);
    
    // Dump UIBuffCell fields to find the grouped buffs list
    Log("  UIBuffCell fields:");
    void* iter = nullptr;
    void* f;
    while ((f = il2cpp_class_get_fields(buffCellClass, &iter))) {
      const char* fname = il2cpp_field_get_name(f);
      size_t foff = il2cpp_field_get_offset(f);
      Log("    [0x%X] %s", (int)foff, fname ? fname : "?");
    }
  }

  // Resolve UIBuffNode.m_orderedBuffCellList field offset
  void *buffNodeClass = FindClass("Beyond.UI", "UIBuffNode", asms, ac);
  if (buffNodeClass) {
    void *fieldIter = nullptr;
    void *field;
    while ((field = il2cpp_class_get_fields(buffNodeClass, &fieldIter))) {
      const char *fname = il2cpp_field_get_name(field);
      if (fname && strcmp(fname, "m_orderedBuffCellList") == 0) {
        g_orderedListOffset = il2cpp_field_get_offset(field);
        Log("[OK] UIBuffNode.m_orderedBuffCellList offset: 0x%X",
            (int)g_orderedListOffset);
        break;
      }
    }
  }

  Log("Init complete. Hooks active.");

  // ========== Phase 2: Skill HUD hooks ==========
  InitSkillHooks(asms, ac);

  // Start overlay thread AFTER game is fully loaded
  CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
  Log("Overlay thread started.");

  return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
  if (r == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    InitializeCriticalSection(&g_logLock);
    CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
  }
  return TRUE;
}
