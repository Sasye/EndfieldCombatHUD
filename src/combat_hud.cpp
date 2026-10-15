#define APPLEPIE_PLUGIN_IMPL
#include "applepie_mgr.h"
#include "applepie_hud.h"
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

extern "C" __declspec(dllexport) void DummyExport() {}
static std::atomic<bool> g_langZh = true; // true=Chinese, false=English

// Visibility is runtime state; the key bindings are persistent settings.
static std::atomic<bool> g_showSkillHud = true;
static std::atomic<bool> g_showBuffHud = true;
static std::atomic<bool> g_visualEnhance = false; // ready-state glow/pulse (default OFF)
static std::atomic<int> g_vkSkillHud = VK_F11;  // 0x7A
static std::atomic<int> g_vkBuffHud  = VK_F12;  // 0x7B

// Language selector: picks Chinese or English string at zero cost
#define L(zh, en) (g_langZh ? (zh) : (en))

static HANDLE g_logHandle = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;

void Log(const char *fmt, ...) {
  if (g_logHandle == INVALID_HANDLE_VALUE)
    return;
  char buf[4096];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
  va_end(args);
  if (len < 0) return;
  if (len > (int)sizeof(buf) - 3) len = (int)sizeof(buf) - 3;
  buf[len++] = '\r';
  buf[len++] = '\n';
  EnterCriticalSection(&g_logLock);
  DWORD written;
  WriteFile(g_logHandle, buf, (DWORD)len, &written, NULL);
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

// Forward declarations for config functions (defined in Plugin Manager section)
static APH_Result SyncConfig();
static std::atomic<bool> g_configVisualDirty{false};

struct HudKeyEdge {
  int binding = -1;
  bool previous = false;
  bool Poll(int key, bool down) {
    if (key != binding) { binding = key; previous = down; return false; }
    const bool pressed = down && !previous;
    previous = down;
    return pressed;
  }
};

// ============================================================================
// Tooltip State (shared between hook thread and overlay thread)
// ============================================================================
static HWND g_gameHwnd = nullptr;
static HWND g_overlayHwnd = nullptr;
static std::atomic<bool> g_running = true;

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
  if (!g_pluginActive || !RuntimeOpen())
    return;
  if (!g_sceneActive || GetTickCount() - g_lastHpTick.load() > 1500) return;
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

  // Resolution scaling: base design at 1440p — use game window dimensions
  float scale = 1.0f;
  if (g_gameH > 0) scale = (float)g_gameH / 1440.0f;
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
  if (tx + panelW > g_gameW) tx = g_tooltipX - panelW - (int)(5 * scale);

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

// Repaint flag — set by tooltip/skill change detection, consumed by timer render
static bool g_needRepaint = true;

// Compute the tight bounding rect of all overlay content in game-window coordinates.
// Overlay only covers this region, dramatically reducing DWM compositing area
// (the main cause of input/render latency at high resolutions).
static RECT ComputeContentRect(int gW, int gH) {
  float scale = (float)gH / 1440.0f;
  int cx = gW / 2;
  int cy = (int)(gH * 0.49f);

  // Skill HUD bounds
  int blockGap = (int)(150 * scale);
  int barW = (int)(120 * scale);
  int curveOff = (int)(15 * scale);
  int textH = (int)(19 * scale);
  int barH = (int)(5 * scale);
  int slotGap = (int)(22 * scale);
  int slotH = textH + barH + slotGap;
  int totalH = 4 * slotH;
  int margin = (int)(30 * scale);

  int sL = cx - blockGap - barW - curveOff - margin;
  int sR = cx + blockGap + barW + curveOff + margin;
  int sT = cy - totalH / 2 - margin;
  int sB = cy + totalH / 2 + margin;

  // Buff icon area (bottom-left) + tooltip space
  float iconSpacing = gH * 0.0317f;
  float firstIconX = gW * 0.422f;
  float iconY = gH * 0.946f;
  float iconSz = gH * 0.025f;
  int bL = (int)(firstIconX - iconSz);
  int bR = (int)(firstIconX + iconSpacing * 30 + iconSz + 500 * scale); // tooltip room
  int bT = (int)(iconY - iconSz - 10);
  int bB = (int)(iconY + iconSz + 300 * scale); // tooltip height

  // Union
  RECT r;
  r.left   = max(0L, (long)min(sL, bL));
  r.right  = min((long)gW, (long)max(sR, bR));
  r.top    = max(0L, (long)min(sT, bT));
  r.bottom = min((long)gH, (long)max(sB, bB));
  return r;
}

// Render overlay content directly via GetDC+BitBlt (no InvalidateRect, no WM_PAINT)
// Transparency is handled by SetLayeredWindowAttributes(LWA_COLORKEY) set at creation.
// This avoids both DWM dirty notifications AND ULW_COLORKEY compatibility issues.
static HDC    s_memDC = NULL;
static HBITMAP s_memBmp = NULL;
static HBITMAP s_oldBmp = NULL;
static int    s_memW = 0, s_memH = 0;

static void RenderOverlay(HWND hwnd) {
  RECT cr;
  GetClientRect(hwnd, &cr);
  int w = cr.right, h = cr.bottom;
  if (w <= 0 || h <= 0) return;

  HDC winDC = GetDC(hwnd);
  if (!winDC) return;

  // Recreate cached bitmap only if window size changed
  if (!s_memDC || s_memW != w || s_memH != h) {
    if (s_memDC) {
      SelectObject(s_memDC, s_oldBmp);
      DeleteObject(s_memBmp);
      DeleteDC(s_memDC);
    }
    s_memDC = CreateCompatibleDC(winDC);
    s_memBmp = CreateCompatibleBitmap(winDC, w, h);
    s_oldBmp = (HBITMAP)SelectObject(s_memDC, s_memBmp);
    s_memW = w;
    s_memH = h;
  }

  // Coordinate mapping: game-window coords → overlay-local bitmap coords
  // DrawTooltip/DrawSkillHud use game-window coordinates unchanged;
  // SetWindowOrgEx shifts the logical origin so (g_overlayOX, g_overlayOY)
  // maps to device (0, 0) in the bitmap.
  SetWindowOrgEx(s_memDC, g_overlayOX, g_overlayOY, NULL);

  // Clear bitmap (in mapped coordinates)
  RECT clearRect = {g_overlayOX, g_overlayOY, g_overlayOX + w, g_overlayOY + h};
  FillRect(s_memDC, &clearRect, (HBRUSH)GetStockObject(BLACK_BRUSH));

  // Draw all content using game-window coordinates
  DrawTooltip(s_memDC);
  DrawSkillHud(s_memDC, hwnd);

  // Reset origin for BitBlt (device coordinates)
  SetWindowOrgEx(s_memDC, 0, 0, NULL);
  BitBlt(winDC, 0, 0, w, h, s_memDC, 0, 0, SRCCOPY);

  ReleaseDC(hwnd, winDC);
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  if (msg == WM_PAINT) {
    // Direct-render mode: render content here when system requests repaint
    // (e.g. after ShowWindow restores visibility)
    ValidateRect(hwnd, NULL); // prevent infinite WM_PAINT loop
    RenderOverlay(hwnd);
    return 0;
  }
  if (msg == WM_ERASEBKGND)
    return 1;
  if (msg == WM_TIMER) {
    if (!g_pluginActive || !RuntimeOpen()) {
      ShowWindow(hwnd, SW_HIDE);
      return 0;
    }
    if (g_configVisualDirty.exchange(false)) g_needRepaint = true;
    // Timer fires ~30fps on overlay thread - do hit testing here with pure
    // Win32
    if (!IsWindow(g_gameHwnd)) {
      g_running = false;
      PostQuitMessage(0);
      return 0;
    }

    // Visibility: show overlay only when game (or its child window) is foreground
    {
      static DWORD s_lastFgCheckTick = 0;
      DWORD now = GetTickCount();
      if (now - s_lastFgCheckTick > 100) {
        s_lastFgCheckTick = now;
        HWND fg = GetForegroundWindow();
        bool gameActive = false;
        if (fg == g_gameHwnd) {
          gameActive = true;
        } else if (fg) {
          // Check if foreground window belongs to same process
          // (handles in-game overlays, popup dialogs, etc.)
          DWORD fgPid = 0;
          GetWindowThreadProcessId(fg, &fgPid);
          DWORD gamePid = 0;
          GetWindowThreadProcessId(g_gameHwnd, &gamePid);
          if (fgPid == gamePid)
            gameActive = true;
        }
        if (!gameActive) {
          if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
          return 0;
        } else if (!IsWindowVisible(hwnd)) {
          ShowWindow(hwnd, SW_SHOWNA);
          g_needRepaint = true;
        }
      }
    }

    // Hotkey detection (edge-triggered: fires once per keypress)
    {
      static HudKeyEdge skillEdge, buffEdge;
      static uint64_t hotkeyEpoch = 0;
      if (hotkeyEpoch != g_epoch) {
        skillEdge = {}; buffEdge = {}; hotkeyEpoch = g_epoch;
      }
      const int skillKey = g_vkSkillHud.load(), buffKey = g_vkBuffHud.load();
      const bool curSkill = skillKey && (GetAsyncKeyState(skillKey) & 0x8000) != 0;
      const bool curBuff = buffKey && (GetAsyncKeyState(buffKey) & 0x8000) != 0;
      if (skillEdge.Poll(skillKey, curSkill)) {
        g_showSkillHud = !g_showSkillHud;
        g_needRepaint = true;
        Log("[HOTKEY] Skill HUD: %s", g_showSkillHud ? "ON" : "OFF");
      }
      if (buffEdge.Poll(buffKey, curBuff)) {
        g_showBuffHud = !g_showBuffHud;
        g_needRepaint = true;
        Log("[HOTKEY] Buff HUD: %s", g_showBuffHud ? "ON" : "OFF");
      }
    }

    // Sync overlay to content-tight region (not fullscreen)
    // DWM compositing cost is proportional to overlay area; at 4K a fullscreen
    RECT gr;
    GetClientRect(g_gameHwnd, &gr);
    g_gameW = gr.right;
    g_gameH = gr.bottom;
    POINT gpt = {0, 0};
    ClientToScreen(g_gameHwnd, &gpt);

    // Full game window overlay — content-tight sizing clips tooltips
    // drawn at cursor position. DWM compositing cost is negligible.
    g_overlayOX = 0;
    g_overlayOY = 0;
    int ovW = g_gameW;
    int ovH = g_gameH;
    if (ovW < 100) ovW = 100;
    if (ovH < 100) ovH = 100;

    RECT curOv;
    GetWindowRect(g_overlayHwnd, &curOv);
    int curW = curOv.right - curOv.left;
    int curH = curOv.bottom - curOv.top;
    if (curOv.left != gpt.x ||
        curOv.top  != gpt.y ||
        curW != ovW || curH != ovH) {
      SetWindowPos(hwnd, HWND_TOPMOST,
                   gpt.x, gpt.y,
                   ovW, ovH, SWP_NOACTIVATE);
      g_needRepaint = true;
    }

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
        if (foundIdx >= 0)
          visibleMap[visibleCount++] = foundIdx;
      }
    } else {
      // Fallback: only show buffs that came from display scan (source==2)
      // Raw hook buffs (source 0/1) may include hidden weapon/internal buffs
      // that the game never displays in its buff bar
      for (int i = 0; i < g_buffCount; i++) {
        if (g_buffs[i].source == 2)
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


    // Log throttle: only fire once per hover session
    static uint32_t s_lastLogUid = 0;
    static int s_lastLogEnhance = -1;

    if (newHover && g_showBuffHud && hoveredVisIdx >= 0 && hoveredVisIdx < visibleCount) {
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

        // Multiply static attributes by stack count.
        // The AttributeModifierLoader always stores per-stack base values;
        // the game engine applies the multiplier internally but our raw
        // loader read gets only base values.
        if (ab.enhanceCnt > 1) {
          int multiplier = ab.enhanceCnt;
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
          // Also multiply blackboard values.
          // Skip 'duration' and 'rate' keys (those are per-instance constants).
          for (int b = 0; b < ab.bbCount; b++) {
            const char *k = ab.bb[b].key;
            if (strcmp(k, "duration") == 0) continue;
            if (strcmp(k, "rate") == 0) continue;
            if (strcmp(k, "max_stack") == 0) continue;
            ab.bb[b].value *= multiplier;
          }
        }

        // Format duration string
      char durStr[128];
      if (ab.duration >= 100000.0f) {
        snprintf(durStr, sizeof(durStr), "%s", L("\xe6\xb0\xb8\xe4\xb9\x85", "Permanent")); // 永久
      } else if (ab.lifeTime > 0.0f) {
        snprintf(durStr, sizeof(durStr), "%.1fs / %.1fs", ab.lifeTime, ab.duration);
      } else {
        snprintf(durStr, sizeof(durStr), "%.1fs", ab.duration);
      }
      // Derive friendly display name from icon
      const char *displayName = ab.id;
      // Icon-based type name mapping
      struct { const char *pat; const char *name; } iconNames[] = {
        {"atk_up",           "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b\xe6\x8f\x90\xe5\x8d\x87"},
        {"def_up",           "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b\xe6\x8f\x90\xe5\x8d\x87"},
        {"hp_up",            "\xe7\x94\x9f\xe5\x91\xbd\xe6\x8f\x90\xe5\x8d\x87"},
        {"spd_up",           "\xe9\x80\x9f\xe5\xba\xa6\xe6\x8f\x90\xe5\x8d\x87"},
        {"crit_up",          "\xe6\x9a\xb4\xe5\x87\xbb\xe6\x8f\x90\xe5\x8d\x87"},
        {"pulse_dmg_up",     "\xe7\x94\xb5\xe7\xa3\x81\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"fire_dmg_up",      "\xe7\x81\xab\xe7\x84\xb0\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"cryst_dmg_up",     "\xe5\xaf\x92\xe5\x86\xb7\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"natural_dmg_up",   "\xe8\x87\xaa\xe7\x84\xb6\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"ether_dmg_up",     "\xe8\xb6\x85\xe5\x9f\x9f\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"physical_dmg_up",  "\xe7\x89\xa9\xe7\x90\x86\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87"},
        {"spell_enhance",    "\xe6\xb3\x95\xe6\x9c\xaf\xe5\xa2\x9e\xe5\xb9\x85"},
        {"fire_enhance",     "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85"},
        {"pulse_enhance",    "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85"},
        {"cryst_enhance",    "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
        {"natural_enhance",  "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85"},
        {"ether_enhance",    "\xe8\xb6\x85\xe5\x9f\x9f\xe5\xa2\x9e\xe5\xb9\x85"},
        {"physical_enhance", "\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85"},
        {"shelter",          "\xe5\xba\x87\xe6\x8a\xa4"},
        {"shield",           "\xe6\x8a\xa4\xe7\x9b\xbe"},
        {"heal",             "\xe6\xb2\xbb\xe7\x96\x97"},
        {"regen",            "\xe5\x86\x8d\xe7\x94\x9f"},
        {"atk_down",         "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b\xe4\xb8\x8b\xe9\x99\x8d"},
        {"def_down",         "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b\xe4\xb8\x8b\xe9\x99\x8d"},
        {"vuln",             "\xe6\x98\x93\xe4\xbc\xa4"},
        {"weaken",           "\xe5\xbc\xb1\xe5\x8c\x96"},
        {"slow",             "\xe5\x87\x8f\xe9\x80\x9f"},
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
            strncpy(friendlyName, g_langZh ? in.name : in.pat, 127);
            break;
          }
        }
      }
      // Priority 3: Try buff ID pattern
      if (!friendlyName[0]) {
        for (auto &in : iconNames) {
          if (strstr(ab.id, in.pat)) {
            strncpy(friendlyName, g_langZh ? in.name : in.pat, 127);
            break;
          }
        }
      }

      // Force read iconName (volatile prevents optimizer from eliding)
      volatile char ic0 = ab.iconName[0];
      (void)ic0;

      snprintf(g_tooltipText, sizeof(g_tooltipText),
               "%s%s%s\n"
               "%s: %s\n"
               "%s: %d\n",
               friendlyName[0] ? friendlyName : "",
               friendlyName[0] ? " | " : "",
               ab.id,
               L("\xe6\x97\xb6\xe9\x97\xb4", "Time"), durStr,
               L("\xe5\xb1\x82\xe6\x95\xb0", "Stack"), ab.enhanceCnt);

      bool logOnce = (ab.instUid != s_lastLogUid || ab.enhanceCnt != s_lastLogEnhance);
      if (logOnce) {
        s_lastLogUid = ab.instUid;
        s_lastLogEnhance = ab.enhanceCnt;
        Log("[HOVER] buff=%s enhance=%d bb=%d", ab.id, ab.enhanceCnt, ab.bbCount);
        for (int b = 0; b < ab.bbCount; b++)
          Log("  bb[%d] key='%s' val=%.6f", b, ab.bb[b].key, ab.bb[b].value);
        for (int j = 0; j < 94; j++) {
          if (ab.attrs.add[j] != 0) Log("  add[%d]=%f", j, ab.attrs.add[j]);
          if (ab.attrs.baseAdd[j] != 0) Log("  baseAdd[%d]=%f", j, ab.attrs.baseAdd[j]);
          if (ab.attrs.finalAdd[j] != 0) Log("  finalAdd[%d]=%f", j, ab.attrs.finalAdd[j]);
          if (ab.attrs.baseFinalAdd[j] != 0) Log("  baseFinalAdd[%d]=%f", j, ab.attrs.baseFinalAdd[j]);
          if (ab.attrs.mul[j] != 0) Log("  mul[%d]=%f", j, ab.attrs.mul[j]);
          if (ab.attrs.baseMul[j] != 0) Log("  baseMul[%d]=%f", j, ab.attrs.baseMul[j]);
          if (ab.attrs.finalScl[j] != 0) Log("  finalScl[%d]=%f", j, ab.attrs.finalScl[j]);
          if (ab.attrs.baseFinalScl[j] != 0) Log("  baseFinalScl[%d]=%f", j, ab.attrs.baseFinalScl[j]);
        }
      }
      // Helper: determine if AttributeType is a flat value (absolute number, not percentage)
      auto isFlatAttr = [](int attrType) -> bool {
        // AttrTypes that represent absolute values (HP, ATK, DEF, abilities, etc.)
        int flat[] = {0, 1, 2, 3, 8, 11, 12, 15, 18, 20, 21, 22, 34, 39, 40, 41, 42, 45, 46, 90};
        for (int i = 0; i < sizeof(flat)/sizeof(flat[0]); i++) {
          if (attrType == flat[i]) return true;
        }
        return false;
      };

      // Display each modifier array separately with zone label
      // j = array index, at = AttributeType from SATM mapping
      #define SHOW_ADD(arr, label) \
        if (ab.attrs.arr[j] != 0.0f) { \
          char buf[160]; \
          if (isFlatAttr(at)) { \
            snprintf(buf, sizeof(buf), " %+.1f %s (%s)\n", \
                     ab.attrs.arr[j], GetAttrName(at), label); \
          } else { \
            snprintf(buf, sizeof(buf), " %+.1f%% %s (%s)\n", \
                     ab.attrs.arr[j] * 100.0f, GetAttrName(at), label); \
          } \
          tooltip_cat(buf); \
        }
      #define SHOW_PCT(arr, label) \
        if (ab.attrs.arr[j] != 0.0f) { \
          char buf[160]; \
          snprintf(buf, sizeof(buf), " %+.1f%% %s (%s)\n", \
                   ab.attrs.arr[j] * 100.0f, GetAttrName(at), label); \
          tooltip_cat(buf); \
        }
      #define SHOW_SCL(arr, label) \
        if (ab.attrs.arr[j] != 0.0f && ab.attrs.arr[j] != 1.0f) { \
          char buf[160]; \
          snprintf(buf, sizeof(buf), " x%.0f%% %s (%s)\n", \
                   ab.attrs.arr[j] * 100.0f, GetAttrName(at), label); \
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
        int cnt = 0;
        if (found) for (int i = 0; i < len; i++) if (arr[i] == first) cnt++;
        return found && cnt >= 10;
      };
      if (isUniformScale(ab.attrs.mul, 94))         memset(ab.attrs.mul, 0, sizeof(ab.attrs.mul));
      if (isUniformScale(ab.attrs.finalScl, 94))     memset(ab.attrs.finalScl, 0, sizeof(ab.attrs.finalScl));
      if (isUniformScale(ab.attrs.baseFinalScl, 94)) memset(ab.attrs.baseFinalScl, 0, sizeof(ab.attrs.baseFinalScl));
      if (isUniformScale(ab.attrs.baseMul, 94))      memset(ab.attrs.baseMul, 0, sizeof(ab.attrs.baseMul));

      for (int j = 0; j < 94; j++) {
        int at = j;  // array index IS the AttributeType directly
        // 8 modifier zones per 数据导论 2.2
        // Base phase (基础阶段)
        SHOW_ADD(baseAdd,      L("\xe5\x9f\xba\xe7\xa1\x80\xe5\x8a\xa0\xe7\xae\x97", "Base Add"))           // 基础加算
        SHOW_PCT(baseMul,      L("\xe5\x9f\xba\xe7\xa1\x80\xe7\x99\xbe\xe5\x88\x86\xe6\xaf\x94", "Base Pct"))  // 基础百分比
        SHOW_ADD(baseFinalAdd, L("\xe5\x9f\xba\xe7\xa1\x80\xe6\x9c\x80\xe7\xbb\x88\xe5\x8a\xa0\xe7\xae\x97", "Base Final Add"))  // 基础最终加算
        SHOW_SCL(baseFinalScl, L("\xe5\x9f\xba\xe7\xa1\x80\xe6\x9c\x80\xe7\xbb\x88\xe5\x80\x8d\xe7\x8e\x87", "Base Final Mult")) // 基础最终倍率
        // Main phase (主修正阶段)
        SHOW_ADD(add,          L("\xe5\x8a\xa0\xe7\xae\x97", "Addition"))       // 加算
        SHOW_SCL(mul,          L("\xe4\xb9\x98\xe7\xae\x97", "Multiplier"))     // 乘算
        SHOW_ADD(finalAdd,     L("\xe6\x9c\x80\xe7\xbb\x88\xe5\x8a\xa0\xe7\xae\x97", "Final Add"))    // 最终加算
        SHOW_SCL(finalScl,     L("\xe6\x9c\x80\xe7\xbb\x88\xe5\x80\x8d\xe7\x8e\x87", "Final Mult"))   // 最终倍率
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
                  const char *dispLabel = g_langZh ? s.label : s.pat;
                  snprintf(buf, sizeof(buf),
                    " %s: %s%.1f%%\n",
                    dispLabel, L("\xe5\x87\x8f\xe4\xbc\xa4", "DR "), (1.0 - rateVal) * 100.0);
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
            {"physical_enhance",
              "\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_physical",
              "\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85"},
            {"fire_enhance",
              "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_fire",
              "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85"},
            {"pulse_enhance",
              "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85"},
            {"electric_enhance",
              "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_electric",
              "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85"},
            {"cryst_enhance",
              "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
            {"ice_enhance",
              "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_ice",
              "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_crystal",
              "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85"},
            {"natural_enhance",
              "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85"},
            {"nature_enhance",
              "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_nature",
              "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85"},
            {"ether_enhance",
              "\xe8\xb6\x85\xe5\x9f\x9f\xe5\xa2\x9e\xe5\xb9\x85"},
            {"enhance_ether",
              "\xe8\xb6\x85\xe5\x9f\x9f\xe5\xa2\x9e\xe5\xb9\x85"},
          };
          const char *sources[] = { ab.id, ab.iconName };
          for (auto src : sources) {
            if (!src[0]) continue;
            for (auto &m : enhMap) {
              if (strstr(src, m.pat)) {
                char buf[200];
                const char *dispLabel = g_langZh ? m.label : m.pat;
                snprintf(buf, sizeof(buf), " %s: +%.1f%%\n", dispLabel, rateVal * 100.0);
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
              " %s: +%.1f%%\n", L("\xe5\xa2\x9e\xe5\xb9\x85", "Enhance"), rateVal * 100.0);
            tooltip_cat(buf);
            handled = true;
          }
          }
        }
        // Show remaining non-rate blackboard entries with semantic labels
        struct { const char *raw; const char *label; bool isPct; } keyMap[] = {
          // === Shield (固定值) ===
          {"shield_def_rate", "\xe6\x8a\xa4\xe7\x9b\xbe(\xe9\x98\xb2\xe5\xbe\xa1\xc3\x97)", false},
          {"shield_atk_rate", "\xe6\x8a\xa4\xe7\x9b\xbe(\xe6\x94\xbb\xe5\x87\xbb\xc3\x97)", false},
          {"shield_hp_rate",  "\xe6\x8a\xa4\xe7\x9b\xbe(\xe7\x94\x9f\xe5\x91\xbd\xc3\x97)", false},
          {"shield_base",     "\xe6\x8a\xa4\xe7\x9b\xbe\xe5\x9f\xba\xe7\xa1\x80\xe5\x80\xbc", false},
          {"FinalShield",     "\xe6\x8a\xa4\xe7\x9b\xbe", false},
          {"shield",          "\xe6\x8a\xa4\xe7\x9b\xbe", false},
          // === Base stat ratios (独立乘区) ===
          {"atk_ratio",       "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", true},
          {"def_ratio",       "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", true},
          {"hp_ratio",        "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", true},
          {"hp_percent",      "\xe7\x94\x9f\xe5\x91\xbd", true},
          {"heal_ratio",      "\xe6\xb2\xbb\xe7\x96\x97\xe6\xaf\x94\xe4\xbe\x8b", true},
          {"damage",          "\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"heal",            "\xe6\xb2\xbb\xe7\x96\x97\xe9\x87\x8f", true},
          // === Base stats (基础百分比: modifierType 6 = baseMultiplier) ===
          {"atk",             "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", true},
          {"attack",          "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", true},
          {"def",             "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", true},
          {"defend",          "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", true},
          {"max_hp",          "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", true},
          // === Buff stat ups (乘算: modifierType 1 = multiplier) ===
          {"atk_up",          "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", true},
          {"def_up",          "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", true},
          {"hp_up",           "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", true},
          // === Speed & cooldown (乘算) ===
          {"move_speed",      "\xe7\xa7\xbb\xe5\x8a\xa8\xe9\x80\x9f\xe5\xba\xa6", true},
          {"move_speed_scalar","\xe7\xa7\xbb\xe5\x8a\xa8\xe9\x80\x9f\xe5\xba\xa6", true},
          {"attack_rate",     "\xe6\x94\xbb\xe5\x87\xbb\xe9\x80\x9f\xe5\xba\xa6", true},
          {"skill_cooldown",  "\xe6\x8a\x80\xe8\x83\xbd\xe5\x86\xb7\xe5\x8d\xb4", true},
          {"combo_cd_scalar", "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe5\x86\xb7\xe5\x8d\xb4", true},
          {"combo_cd",        "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe5\x86\xb7\xe5\x8d\xb4", false},
          // === Elemental damage increase (独立乘区: 伤害公式中的增伤区) ===
          {"physical_dmg_up", "\xe7\x89\xa9\xe7\x90\x86\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"fire_dmg_up",     "\xe7\x81\xab\xe7\x84\xb0\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"pulse_dmg_up",    "\xe7\x94\xb5\xe7\xa3\x81\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"cryst_dmg_up",    "\xe5\xaf\x92\xe5\x86\xb7\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"natural_dmg_up",  "\xe8\x87\xaa\xe7\x84\xb6\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"ether_dmg_up",    "\xe8\xb6\x85\xe5\x9f\x9f\xe4\xbc\xa4\xe5\xae\xb3", true},
          // === Skill type damage increase (独立乘区) ===
          {"normal_atk_dmg",  "\xe6\x99\xae\xe6\x94\xbb\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"normal_skill_dmg","\xe6\x88\x98\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"combo_skill_dmg", "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"ult_dmg",         "\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"dmg_scale",       "\xe4\xbc\xa4\xe5\xae\xb3\xe5\x80\x8d\xe7\x8e\x87", true},
          // === Enhanced (增幅, 独立乘区) per element ===
          {"physical_enhanced","\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"fire_enhanced",   "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"pulse_enhanced",  "\xe7\x94\xb5\xe7\xa3\x81\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"cryst_enhanced",  "\xe5\xaf\x92\xe5\x86\xb7\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"natural_enhanced","\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85", true},
          {"ether_enhanced",  "\xe8\xb6\x85\xe5\x9f\x9f\xe5\xa2\x9e\xe5\xb9\x85", true},
          // === Vulnerable (脆弱, 独立乘区) per element ===
          {"physical_vulnerable","\xe7\x89\xa9\xe7\x90\x86\xe8\x84\x86\xe5\xbc\xb1", true},
          {"fire_vulnerable", "\xe7\x81\xab\xe7\x84\xb0\xe8\x84\x86\xe5\xbc\xb1", true},
          {"pulse_vulnerable","\xe7\x94\xb5\xe7\xa3\x81\xe8\x84\x86\xe5\xbc\xb1", true},
          {"cryst_vulnerable","\xe5\xaf\x92\xe5\x86\xb7\xe8\x84\x86\xe5\xbc\xb1", true},
          {"natural_vulnerable","\xe8\x87\xaa\xe7\x84\xb6\xe8\x84\x86\xe5\xbc\xb1", true},
          {"ether_vulnerable","\xe8\xb6\x85\xe5\x9f\x9f\xe8\x84\x86\xe5\xbc\xb1", true},
          // === Burst damage (爆发, 独立乘区) per element ===
          {"fire_burst_dmg",  "\xe7\x81\xab\xe7\x84\xb0\xe7\x88\x86\xe5\x8f\x91", true},
          {"pulse_burst_dmg", "\xe7\x94\xb5\xe7\xa3\x81\xe7\x88\x86\xe5\x8f\x91", true},
          {"cryst_burst_dmg", "\xe5\xaf\x92\xe5\x86\xb7\xe7\x88\x86\xe5\x8f\x91", true},
          {"natural_burst_dmg","\xe8\x87\xaa\xe7\x84\xb6\xe7\x88\x86\xe5\x8f\x91", true},
          // === Abnormal damage (异常伤害, 独立乘区) per element ===
          {"fire_abnormal_dmg","\xe7\x87\x83\xe7\x83\xa7\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"pulse_abnormal_dmg","\xe5\xaf\xbc\xe7\x94\xb5\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"cryst_abnormal_dmg","\xe5\x86\xbb\xe7\xbb\x93\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"natural_abnormal_dmg","\xe8\x85\x90\xe8\x9a\x80\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"inflict_dmg",     "\xe5\xbc\x82\xe5\xb8\xb8\xe4\xbc\xa4\xe5\xae\xb3", true},
          // === Damage taken (承伤, 独立乘区) ===
          {"physical_dmg_taken","\xe7\x89\xa9\xe7\x90\x86\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"fire_dmg_taken",  "\xe7\x81\xab\xe7\x84\xb0\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"pulse_dmg_taken", "\xe7\x94\xb5\xe7\xa3\x81\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"cryst_dmg_taken", "\xe5\xaf\x92\xe5\x86\xb7\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"natural_dmg_taken","\xe8\x87\xaa\xe7\x84\xb6\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"ether_dmg_taken", "\xe8\xb6\x85\xe5\x9f\x9f\xe6\x89\xbf\xe4\xbc\xa4", true},
          // === Resist (抗性, 加算) ===
          {"physical_resist", "\xe7\x89\xa9\xe7\x90\x86\xe6\x8a\x97\xe6\x80\xa7", true},
          {"fire_resist",     "\xe7\x81\xab\xe7\x84\xb0\xe6\x8a\x97\xe6\x80\xa7", true},
          {"pulse_resist",    "\xe7\x94\xb5\xe7\xa3\x81\xe6\x8a\x97\xe6\x80\xa7", true},
          {"cryst_resist",    "\xe5\xaf\x92\xe5\x86\xb7\xe6\x8a\x97\xe6\x80\xa7", true},
          {"natural_resist",  "\xe8\x87\xaa\xe7\x84\xb6\xe6\x8a\x97\xe6\x80\xa7", true},
          {"ether_resist",    "\xe8\xb6\x85\xe5\x9f\x9f\xe6\x8a\x97\xe6\x80\xa7", true},
          // === Poise/Stagger (独立乘区) ===
          {"poise_dmg_up",    "\xe5\xa4\xb1\xe8\xa1\xa1\xe5\x80\xbc\xe8\xbe\x93\xe5\x87\xba", true},
          {"poise_dmg_taken", "\xe5\xa4\xb1\xe8\xa1\xa1\xe5\x80\xbc\xe6\x89\xbf\xe5\x8f\x97", true},
          {"broken_dmg",      "\xe5\xaf\xb9\xe5\xa4\xb1\xe8\xa1\xa1\xe7\x9b\xae\xe6\xa0\x87\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"break_dmg_taken", "\xe5\xa4\x84\xe5\x86\xb3\xe6\x89\xbf\xe4\xbc\xa4", true},
          {"knockdown_time",  "\xe5\x80\x92\xe5\x9c\xb0\xe6\x97\xb6\xe9\x97\xb4", false},
          // === Crit (加算: modifierType 0 = addition) ===
          {"crit_rate",       "\xe6\x9a\xb4\xe5\x87\xbb\xe7\x8e\x87", true},
          {"crit_damage",     "\xe6\x9a\xb4\xe5\x87\xbb\xe4\xbc\xa4\xe5\xae\xb3", true},
          // === Heal & shield output (独立乘区) ===
          {"heal_output",     "\xe6\xb2\xbb\xe7\x96\x97\xe6\x95\x88\xe6\x9e\x9c\xe5\x8a\xa0\xe6\x88\x90", true},
          {"heal_taken",      "\xe8\xa2\xab\xe6\xb2\xbb\xe7\x96\x97\xe5\x8a\xa0\xe6\x88\x90", true},
          {"shield_output",   "\xe6\x8a\xa4\xe7\x9b\xbe\xe9\x87\x8f\xe5\x8a\xa0\xe6\x88\x90", true},
          {"shield_taken",    "\xe5\x8f\x97\xe6\x8a\xa4\xe7\x9b\xbe\xe5\x8a\xa0\xe6\x88\x90", true},
          // === SP & ATB ===
          {"ult_sp_gain",     "\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80\xe5\x85\x85\xe8\x83\xbd\xe6\x95\x88\xe7\x8e\x87", true},
          {"atb_cost",        "\xe6\x8a\x80\xe5\x8a\x9b\xe6\xb6\x88\xe8\x80\x97", false},
          {"life_steal",      "\xe5\x90\xb8\xe8\xa1\x80", true},
          // === Weakness/Shelter (独立乘区) ===
          {"weakness_dmg",    "\xe8\x99\x9a\xe5\xbc\xb1\xe7\xb3\xbb\xe6\x95\xb0", true},
          {"shelter_dmg",     "\xe5\xba\x87\xe6\x8a\xa4\xe7\xb3\xbb\xe6\x95\xb0", true},
          // === Status effects (独立乘区) ===
          {"probability",     "\xe6\xa6\x82\xe7\x8e\x87", true},
          {"vuln_rate",       "\xe6\x98\x93\xe4\xbc\xa4", true},
          {"weaken_rate",     "\xe5\xbc\xb1\xe5\x8c\x96", true},
          {"slow_rate",       "\xe5\x87\x8f\xe9\x80\x9f", true},
          {"slow_action",     "\xe5\x8a\xa8\xe4\xbd\x9c\xe5\x87\x8f\xe9\x80\x9f", true},
          // === Infliction (源石技艺强度, 固定值) ===
          {"infliction_enhance","\xe6\xba\x90\xe7\x9f\xb3\xe6\x8a\x80\xe8\x89\xba\xe5\xbc\xba\xe5\xba\xa6", false},
          // === Combo zone (独立乘区) ===
          {"combo_dmg",       "\xe8\xbf\x9e\xe5\x87\xbb\xe5\xa2\x9e\xe4\xbc\xa4", true},
          // === Normal/Crit ups (独立乘区) ===
          {"normal_dmg_up",   "\xe6\x99\xae\xe6\x94\xbb\xe5\xa2\x9e\xe4\xbc\xa4", true},
          {"crit_rate_up",    "\xe6\x9a\xb4\xe5\x87\xbb\xe7\x8e\x87\xe6\x8f\x90\xe5\x8d\x87", true},
          {"crit_rate_up_dynamic", "\xe6\x9a\xb4\xe5\x87\xbb\xe7\x8e\x87(\xe5\x8a\xa8\xe6\x80\x81)", true},
          // === Numbered variant keys (weapon buffs, 乘算) ===
          {"atk_up2",         "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", true},
          {"def_up2",         "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", true},
          {"hp_up2",          "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", true},
          {"crit_rate2",      "\xe6\x9a\xb4\xe5\x87\xbb\xe7\x8e\x87", true},
          {"crit_damage2",    "\xe6\x9a\xb4\xe5\x87\xbb\xe4\xbc\xa4\xe5\xae\xb3", true},
          // === Physical damage (物理伤害) ===
          {"phy_dmg_up",       "\xe7\x89\xa9\xe7\x90\x86\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"phy_dmg_up_mult",  "\xe7\x89\xa9\xe7\x90\x86\xe4\xbc\xa4\xe5\xae\xb3", true},
          {"phy_spell_up",     "\xe6\xba\x90\xe7\x9f\xb3\xe6\x8a\x80\xe8\x89\xba\xe5\xbc\xba\xe5\xba\xa6", false},
          // === Generic damage up ===
          {"dmg_up",           "\xe4\xbc\xa4\xe5\xae\xb3\xe6\x8f\x90\xe5\x8d\x87", true},
        };
        for (int b = 0; b < ab.bbCount; b++) {
          const char *k = ab.bb[b].key;
          double v = ab.bb[b].value;
          if (strcmp(k, "duration") == 0 || strcmp(k, "duration2") == 0 || strcmp(k, "rate") == 0) continue;
          if (strcmp(k, "max_stack") == 0 || strcmp(k, "stack") == 0) continue;
          if (strcmp(k, "level") == 0 || strcmp(k, "lv") == 0 || strcmp(k, "prob") == 0) continue;
          if (strcmp(k, "cd") == 0 || strcmp(k, "priority") == 0) continue;
          if (v == 0.0) continue;
          // Try semantic mapping
          bool found = false;
          char buf[200];
          for (auto &km : keyMap) {
            if (strcmp(k, km.raw) == 0) {
              const char *dispLabel = g_langZh ? km.label : km.raw;
              if (km.isPct)
                snprintf(buf, sizeof(buf), " %s: %+.1f%%\n", dispLabel, v * 100.0);
              else
                snprintf(buf, sizeof(buf), " %s: %+.1f\n", dispLabel, v);
              tooltip_cat(buf);
              found = true;
              break;
            }
          }
          if (!found) {
            // Show unmapped key with raw name for identification
            if (v > -1.0 && v < 1.0)
              snprintf(buf, sizeof(buf), " [%s]: %+.1f%%\n", k, v * 100.0);
            else
              snprintf(buf, sizeof(buf), " [%s]: %+.1f\n", k, v);
            tooltip_cat(buf);
            if (logOnce) Log("[UNMAPPED_KEY] buff=%s key='%s' val=%.6f", ab.id, k, v);
          }
        }
      }

      // If tooltip has no effect info, show raw blackboard entries as fallback
      {
        bool hasEffect = false;
        int lineCount = 0;
        for (const char *p = g_tooltipText; *p; p++)
          if (*p == '\n') lineCount++;
        if (lineCount > 3) hasEffect = true;
        if (!hasEffect) {
          if (logOnce) {
            Log("[NO_EFFECT] buff=%s icon=%s bbCount=%d", ab.id, ab.iconName, ab.bbCount);
          }
          // Show raw bb entries in tooltip as fallback
          for (int b = 0; b < ab.bbCount; b++) {
            const char *k = ab.bb[b].key;
            double v = ab.bb[b].value;
            if (strcmp(k, "duration") == 0 || strcmp(k, "rate") == 0) continue;
            if (strcmp(k, "max_stack") == 0 || strcmp(k, "stack") == 0) continue;
            if (strcmp(k, "level") == 0 || strcmp(k, "prob") == 0) continue;
            if (v == 0.0) continue;
            char buf[200];
            if (v > -1.0 && v < 1.0 && v != 0.0)
              snprintf(buf, sizeof(buf), " %s: %+.1f%%\n", k, v * 100.0);
            else
              snprintf(buf, sizeof(buf), " %s: %.1f\n", k, v);
            tooltip_cat(buf);
            if (logOnce) Log("  bb[%d] key='%s' value=%.6f", b, ab.bb[b].key, ab.bb[b].value);
          }
          // Also log non-zero attributes
          if (logOnce) {
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
      }

      // Position tooltip near cursor
      g_tooltipX = cursor.x;
      g_tooltipY = cursor.y;
      } else {
        newHover = false; // invalid or missing buff
      }
    } else {
      newHover = false;
      // Reset log throttle when mouse leaves buff area
      s_lastLogUid = 0;
      s_lastLogEnhance = -1;
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
      g_needRepaint = true;
    } else if (newHover) {
      if (strcmp(s_lastTooltipText, g_tooltipText) != 0 || 
          s_lastTooltipX != g_tooltipX || 
          s_lastTooltipY != g_tooltipY) {
        strcpy(s_lastTooltipText, g_tooltipText);
        s_lastTooltipX = g_tooltipX;
        s_lastTooltipY = g_tooltipY;
        g_needRepaint = true;
      }
    }
    // Skill HUD: check for data changes
    {
      static SkillHudState s_lastSkillState = {};
      SkillHudState curState;
      EnterCriticalSection(&g_skillLock);
      curState = g_skillHud;
      LeaveCriticalSection(&g_skillLock);
      if (memcmp(&curState, &s_lastSkillState, sizeof(curState)) != 0) {
        s_lastSkillState = curState;
        g_needRepaint = true;
      }
    }
    // Track visibility state changes — force repaint when HUD should hide/show
    {
      // Consume hook-triggered visibility change
      if (g_visibilityChanged.exchange(false)) {
        g_needRepaint = true;
      }
      // Also detect gradual alpha changes (fade animations)
      static bool s_lastVisible = true;
      bool visible = g_sceneActive && (g_cgAlpha >= 0.1f);
      if (visible != s_lastVisible) {
        s_lastVisible = visible;
        g_needRepaint = true;
      }
    }
    // Render via direct BitBlt when content changed
    if (g_needRepaint) {
      g_needRepaint = false;
      RenderOverlay(hwnd);
    }
    return 0;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Hooks - MINIMAL work, no Unity API calls, no Win32 calls
// ============================================================================

static constexpr auto oProgressBar = ProgressHook::Original;

void hkProgressBar(void *self, BuffRef p1, bool p2, int32_t p3, void *mi) {
  // Only process buffs from the active character's HpBar
  // When g_activeHpBar is null (before first LateTick), skip all —
  // display scan fallback will read visible buffs directly from cells
  if (!g_activeHpBar || (uintptr_t)self != g_activeHpBar.load()) {
    oProgressBar(self, p1, p2, p3, mi);
    return;
  }
  uintptr_t isAdd = (uintptr_t)p2;
  ActiveBuff ab = {};
  ReadBuffData(&p1, &ab);



  if (isAdd && ab.buffObj) {
    EnterCriticalSection(&g_buffLock);
    // Merge same-id entries: ProgressBar fires multiple times for the same buff
    // type (e.g. each rest interaction creates a new instance).
    // Replace the old entry to prevent non-stacking buffs from accumulating.
    bool merged = false;
    for (int i = 0; i < g_buffCount; i++) {
      if (g_buffs[i].instUid == ab.instUid) {
        // Exact same instance — update in place
        ab.source = g_buffs[i].source;
        ab.buffObj = nullptr;
        g_buffs[i] = ab;
        merged = true;
        break;
      } else if (strcmp(g_buffs[i].id, ab.id) == 0 && strcmp(g_buffs[i].iconName, ab.iconName) == 0) {
        // Same buff type via ProgressBar — replace old instance
        g_buffs[i].buffObj = nullptr;
        g_buffs[i].instUid = ab.instUid;
        g_buffs[i].duration = ab.duration;
        g_buffs[i].lifeTime = ab.lifeTime;
        merged = true;
        break;
      }
    }
    if (!merged && g_buffCount < 64) {
      ab.source = 0; // ProgressBar = buff
      ab.buffObj = nullptr;
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
static constexpr auto oBuffIconChange = BuffIconHook::Original;
void hkBuffIconChange(void *self, BuffRef p1, bool p2, int32_t p3, void *mi) {
  // Only process buffs from the active character's HpBar
  if (!g_activeHpBar || (uintptr_t)self != g_activeHpBar.load()) {
    oBuffIconChange(self, p1, p2, p3, mi);
    return;
  }
  uintptr_t isAdd = (uintptr_t)p2;
  ActiveBuff ab = {};
  ReadBuffData(&p1, &ab);



  if (isAdd && ab.buffObj) {
    EnterCriticalSection(&g_buffLock);
    // Check for duplicate instUid only (old proven logic)
    bool found = false;
    for (int i = 0; i < g_buffCount; i++) {
      if (g_buffs[i].instUid == ab.instUid) {
        found = true;
        break;
      }
      // If same buff type already tracked via ProgressBar, skip to avoid
      // duplicate count. Ether buffs fire through BOTH ProgressBar and
      // BuffIconChange. Equipsuit only fires through BuffIconChange.
      if (g_buffs[i].source == 0 && strcmp(g_buffs[i].id, ab.id) == 0
          && ab.iconName[0] && strcmp(g_buffs[i].iconName, ab.iconName) == 0) {
        found = true;
        break;
      }
    }
    if (!found && g_buffCount < 64) {
      ab.source = 1; // BuffIcon = debuff
      ab.buffObj = nullptr;
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

static constexpr auto oEnhance = EnhanceHook::Original;
static constexpr auto oGpuiEnhance = GpuiEnhanceHook::Original;

void hkEnhanceCommon(BuffRef p1) {
  ActiveBuff ab = {};
  ReadBuffData(&p1, &ab);

  EnterCriticalSection(&g_buffLock);
  for (int i = 0; i < g_buffCount; i++) {
    if (g_buffs[i].instUid == ab.instUid) {
      g_buffs[i].enhanceCnt = ab.enhanceCnt;
      g_buffs[i].trueEnhanceCnt = ab.enhanceCnt;
      break;
    }
    // Also match by id — display scan may have swapped the tracked instUid
    if (strcmp(g_buffs[i].id, ab.id) == 0) {
      g_buffs[i].enhanceCnt = ab.enhanceCnt;
      g_buffs[i].trueEnhanceCnt = ab.enhanceCnt;
      // Don't break — update all entries with this id
    }
  }
  LeaveCriticalSection(&g_buffLock);
}

void hkEnhance(void *self, BuffRef p1, void *mi) {
  hkEnhanceCommon(p1);
  oEnhance(self, p1, mi);
}
void hkGpuiEnhance(void *self, BuffRef p1, void *mi) {
  hkEnhanceCommon(p1);
  oGpuiEnhance(self, p1, mi);
}

// Hook GPUIBuffNode stack operations (passthrough — stack counts read from m_stackBuffsDict)
static constexpr auto oAddStack = AddStackHook::Original;
void hkAddStack(void *self, BuffRef p1, bool playAnim, void *buffData, void *group, void *mi) {
  oAddStack(self, p1, playAnim, buffData, group, mi);
}
static constexpr auto oRemoveStack = RemoveStackHook::Original;
void hkRemoveStack(void *self, BuffRef p1, int32_t finishReason, void *buffData, void *group, void *mi) {
  oRemoveStack(self, p1, finishReason, buffData, group, mi);
}

// Hook for _ClearMainChar - clears all buffs when entering dungeon/switching char
static constexpr auto oClearMainChar = ClearHook::Original;
void hkClearMainChar(void *self, void *mi) {
  EnterCriticalSection(&g_buffLock);
  g_buffCount = 0;
  g_displayCount = 0;
  LeaveCriticalSection(&g_buffLock);
  // The HpBar being cleared IS the new active character's HpBar
  g_activeHpBar = (uintptr_t)self;
  Log("[CLEAR] All buffs cleared, activeHpBar=%p", self);
  oClearMainChar(self, mi);
}

static constexpr auto oLateTick = LateTickHook::Original;

void hkLateTick(void *self, float dt, void *mi) {
  oLateTick(self, dt, mi);

  // Plugin Manager: skip processing when paused
  if (!GameReadAllowed()) return;

  // Track which MainCharHpBar is active (the one LateTick is called on)
  if (g_activeHpBar.exchange((uintptr_t)self) != (uintptr_t)self) {
    EnterCriticalSection(&g_buffLock);
    g_buffCount = g_displayCount = 0;
    LeaveCriticalSection(&g_buffLock);
    s_cgResolved = false;
    s_cgGetAlpha = nullptr;
  }
  // Discover pre-existing panels once per binding. Subsequent OnHide/OnShow
  // events remain authoritative even when a hidden component keeps ticking.
  if (g_mainCharHpBarPtr != self) {
    g_mainCharHpBarPtr = self; // identity comparison only, never dereferenced
    g_sceneActive = PanelActive(self);
    g_cgAlpha = 1.0f;
    s_cgResolved = false;
    s_cgGetAlpha = nullptr;
  }
  g_lastHpTick = GetTickCount();

  // Read CanvasGroup alpha on game thread (safe) — overlay thread reads g_cgAlpha
  if (g_offHpBar_fadeCtrl >= 0) {
    void *fadeCtrl = SReadPtr(self, g_offHpBar_fadeCtrl);
    if (fadeCtrl) {
      int cgOff = (g_offFade_cg >= 0) ? g_offFade_cg : 0x20;
      void *cgObj = SReadPtr(fadeCtrl, cgOff);
      if (cgObj) {
        if (!s_cgResolved) {
          s_cgResolved = true;
          void *k = il2cpp_object_get_class(cgObj);
          if (k) {
            s_cgGetAlpha = FindMethod(k, "get_alpha", 0);
            if (!ValidateMethod(s_cgGetAlpha, "System.Single", {})) s_cgGetAlpha = nullptr;
          }
        }
        if (s_cgGetAlpha) {
          g_cgAlpha = SDirectFloat(s_cgGetAlpha, cgObj);
        }
      }
    }
  }

  // Wall-clock throttle: every ~1000ms (not frame-based, since at 200fps
  // a 60-frame counter fires every 300ms — 3x too often)
  DWORD now = GetTickCount();
  if (now - g_lastBuffScan < 1000) return;
  g_lastBuffScan = now;


  EnterCriticalSection(&g_buffLock);
  __try {
  // g_buffs contains value snapshots only; live cell references are borrowed
  // and copied during this callback, never held for the next one.
  // Read display order from buff node's m_orderedBuffCellList
  // Try GPUIBuffNode first (game update switched to GPUI), then UIBuffNode fallback
  if (g_getBuffInstanceUid) {
    void *buffNode = nullptr;
    int listOffset = 0;
    bool buffNodeIsGpui = false;
    void* emptyNode = nullptr;
    int emptyListOffset = 0;
    bool emptyIsGpui = false;

    // Prefer reflected MainCharHpBar/GPUIBuffNode fields. Legacy offsets remain
    // available only when reflection could not resolve a field.
    __try {
      void *gpui = nullptr;
      if (g_offHpBar_gpuiBuffNode >= 0)
        gpui = *(void **)((char *)self + g_offHpBar_gpuiBuffNode);
      else
        gpui = *(void **)((char *)self + 0xD8);

      if (gpui) {
        int gpuiListOffset = g_gpuiOrderedListOffset > 0
            ? (int)g_gpuiOrderedListOffset : 0xB0;
        void *ol = *(void **)((char *)gpui + gpuiListOffset);
        if (ol) {
          int sz = *(int *)((char *)ol + 0x18);
          if (sz > 0 && sz <= 64) {
            buffNode = gpui;
            listOffset = gpuiListOffset;
            buffNodeIsGpui = true;
          } else if (sz == 0) {
            emptyNode = gpui; emptyListOffset = gpuiListOffset; emptyIsGpui = true;
          }
        }
      }
    } __except(1) {}

    // Fallback: reflected UIBuffNode and its m_orderedBuffCellList.
    if (!buffNode && g_orderedListOffset > 0) {
      __try {
        void *uibn = nullptr;
        if (g_offHpBar_buffNode >= 0)
          uibn = *(void **)((char *)self + g_offHpBar_buffNode);
        else
          uibn = *(void **)((char *)self + 0xD0);
        if (uibn) {
          void *ol = *(void **)((char *)uibn + g_orderedListOffset);
          if (ol) {
            int sz = *(int *)((char *)ol + 0x18);
            if (sz > 0 && sz <= 64) {
              buffNode = uibn;
              listOffset = (int)g_orderedListOffset;
            } else if (sz == 0 && !emptyNode) {
              emptyNode = uibn; emptyListOffset = (int)g_orderedListOffset;
            }
          }
        }
      } __except(1) {}
    }
    // Additive legacy branch for builds where UIBuffNode reflection is absent.
    if (!buffNode && g_orderedListOffset == 0) {
      __try {
        void *uibn = nullptr;
        if (g_offHpBar_buffNode >= 0)
          uibn = *(void **)((char *)self + g_offHpBar_buffNode);
        else
          uibn = *(void **)((char *)self + 0xD0);
        if (uibn) {
          void *ol = *(void **)((char *)uibn + 0xA8);
          if (ol) {
            int sz = *(int *)((char *)ol + 0x18);
            if (sz > 0 && sz <= 64) {
              buffNode = uibn;
              listOffset = 0xA8;
            } else if (sz == 0 && !emptyNode) {
              emptyNode = uibn; emptyListOffset = 0xA8;
            }
          }
        }
      } __except(1) {}
    }

    // A nonempty legacy node takes precedence over an unused empty GPUI node.
    // If both are empty, publish that empty snapshot to discard stale buffs.
    if (!buffNode && emptyNode) {
      buffNode = emptyNode; listOffset = emptyListOffset; buffNodeIsGpui = emptyIsGpui;
    }
    if (buffNode) {
      __try {
        void *orderedList = *(void **)((char *)buffNode + listOffset);
        if (orderedList) {
          void *items = *(void **)((char *)orderedList + 0x10);
          int size = *(int *)((char *)orderedList + 0x18);
          if ((items || size == 0) && size >= 0 && size <= 64) {
            int newDisplayCount = 0;
            // Temporary array to rebuild g_buffs from display
            ActiveBuff newBuffs[64];
            int newBuffCount = 0;

            // Read stack counts from the reflected node-specific stack table.
            // DynamicFastLookupCollection.m_list is resolved lazily from its
            // constructed runtime class; List/array layouts are IL2CPP ABI.
            struct StackEntry { char id[128]; int count; };
            StackEntry stackCounts[32];
            int stackCountN = 0;
            __try {
              int stackDictOffset = buffNodeIsGpui
                  ? (g_gpuiStackBuffsOffset > 0
                      ? (int)g_gpuiStackBuffsOffset : 0xA8)
                  : (g_uiStackBuffsOffset > 0
                      ? (int)g_uiStackBuffsOffset : 0xA0);
              void *stackDict =
                  *(void **)((char *)buffNode + stackDictOffset);
              if (stackDict) {
                static void *s_lookupClass = nullptr;
                static int s_lookupListOffset = -2;
                if (il2cpp_object_get_class) {
                  void *lookupClass = il2cpp_object_get_class(stackDict);
                  if (lookupClass && lookupClass != s_lookupClass) {
                    const char *listNames[] = {"m_list", "_list", "list"};
                    s_lookupClass = lookupClass;
                    s_lookupListOffset =
                        FindFieldInHierarchy(lookupClass, listNames, 3);
                    Log("[DIAG] DynamicFastLookupCollection.m_list offset: 0x%X",
                        s_lookupListOffset);
                  }
                }
                int lookupListOffset = s_lookupListOffset >= 0
                    ? s_lookupListOffset : 0x18;
                void *mList =
                    *(void **)((char *)stackDict + lookupListOffset);
                if (mList) {
                  int listSize = *(int *)((char *)mList + 0x18);
                  void *listItems = *(void **)((char *)mList + 0x10);
                  if (listItems && listSize > 0 && listSize <= 32) {
                    // Each ValueTuple<String, List<>> is 16 bytes (2 refs)
                    // Array starts at listItems + 0x20 (array header: 0x10 klass + 0x08 monitor + 0x08 length)
                    // Actually: managed array: +0x10 = klass, +0x18 = length, +0x20 = data
                    char *base = (char *)listItems + 0x20;
                    for (int si = 0; si < listSize && stackCountN < 32; si++) {
                      // ValueTuple<String, List>: 2 object references = 16 bytes each
                      void *keyStr = *(void **)(base + si * 16);
                      void *valList = *(void **)(base + si * 16 + 8);
                      if (keyStr && valList) {
                        char idBuf[128] = {};
                        ReadStr(keyStr, idBuf, sizeof(idBuf));
                        int count = *(int *)((char *)valList + 0x18); // List._size
                        if (idBuf[0] && count > 0) {
                          strncpy(stackCounts[stackCountN].id, idBuf, 127);
                          stackCounts[stackCountN].count = count;

                          stackCountN++;
                        }
                      }
                    }
                  }
                }
              }
            } __except(1) {}

            void **elements = items ? (void **)((char *)items + 0x20) : nullptr;
            // These are metadata caches keyed by the actual concrete class.
            static void *cellClassCache = nullptr, *uidMethod = nullptr;
            static int cellBuffOffset = -1;
            for (int ci = 0; ci < size; ++ci) {
              void *cell = elements[ci];
              if (!cell) continue;
              void *klass = il2cpp_object_get_class(cell);
              if (klass != cellClassCache) {
                cellClassCache = klass;
                uidMethod = nullptr;
                for (void *k = klass; k && !uidMethod; k = il2cpp_class_get_parent(k))
                  uidMethod = FindMethod(k, "get_buffInstanceUid", 0);
                const char *names[] = {"m_buffPtr", "_buffPtr", "buffPtr"};
                cellBuffOffset = FindFieldInHierarchy(klass, names, 3);
              }
              void *boxedUid = Invoke(uidMethod ? uidMethod : g_getBuffInstanceUid, cell);
              if (!boxedUid) continue;
              uint32_t uid = *(uint32_t*)((char*)boxedUid + 16);
              g_displayUids[newDisplayCount++] = uid;
              ActiveBuff ab{};
              bool refreshed = false;
              if (cellBuffOffset >= 0) {
                BuffRef ref = *(BuffRef*)((char*)cell + cellBuffOffset);
                // A recycled cell's ObjectPtr must still describe this UID.
                if (ref.object && ref.uid == uid) {
                  ReadBuffData(&ref, &ab);
                  refreshed = ab.id[0] != 0;
                }
              }
              if (!refreshed) {
                for (int bi = 0; bi < g_buffCount; ++bi) {
                  if (g_buffs[bi].instUid == uid) { ab = g_buffs[bi]; break; }
                }
              }
              ab.instUid = uid;
              ab.buffObj = nullptr; // snapshot only
              ab.source = 2;
              for (int si = 0; si < stackCountN; ++si)
                if (!strcmp(stackCounts[si].id, ab.id)) {
                  ab.enhanceCnt = stackCounts[si].count;
                  break;
                }
              newBuffs[newBuffCount++] = ab;
            }
            // Replace g_buffs entirely with display-scanned list
            // Buffs not in the game's cell list are not for the current character
            memcpy(g_buffs, newBuffs, sizeof(ActiveBuff) * newBuffCount);
            g_buffCount = newBuffCount;
            g_displayCount = newDisplayCount;
          }
        }
      } __except (1) { /* ignore */ }
    }
  }
  // Lazy-load s_attributesToModify mapping (static field only available after game init)
  // Re-check if only a few entries were loaded (game populates array gradually)
  if (g_satmField && il2cpp_field_static_get_value && (!g_satmLoaded || g_satmLen < 10)) {
    void* satmArr = nullptr;
    il2cpp_field_static_get_value(g_satmField, &satmArr);
    if (satmArr) {
      int32_t sLen = *(int32_t *)((char *)satmArr + 0x18);
      if (sLen > g_satmLen && sLen <= 96) {
        int32_t *sData = (int32_t *)((char *)satmArr + 0x20);
        for (int si = 0; si < sLen; si++)
          g_satmMap[si] = sData[si];
        g_satmLen = sLen;
        g_satmLoaded = true;
        Log("[OK] s_attributesToModify loaded: %d entries", sLen);
        for (int si = 0; si < sLen && si < 10; si++)
          Log("  arr[%d] -> AttrType %d (%s)", si, g_satmMap[si], GetAttrName(g_satmMap[si]));
        if (sLen > 10)
          Log("  ... (%d more entries)", sLen - 10);
      }
    }
  }
  } __finally { LeaveCriticalSection(&g_buffLock); }
}

// ============================================================================
// Overlay Thread - completely separate from game thread
// ============================================================================
static APH_Result RunOverlay(uint64_t epoch) {
  // Set per-thread DPI awareness so we get real pixel coordinates
  // (without this, coordinates are virtualized on 125%/150%/200% displays)
  typedef DPI_AWARENESS_CONTEXT (WINAPI *tSetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
  HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
  if (hUser32) {
    auto pSetDpi = (tSetThreadDpiAwarenessContext)GetProcAddress(hUser32, "SetThreadDpiAwarenessContext");
    if (pSetDpi) {
      pSetDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
      Log("[OK] DPI awareness set to Per-Monitor V2");
    } else {
      Log("[WARN] SetThreadDpiAwarenessContext not available (old Windows?)");
    }
  }

  // Find game window belonging to THIS process (not any other Unity app)
  DWORD myPid = GetCurrentProcessId();
  g_gameHwnd = nullptr;
  while (EnableOpen(epoch) && !g_gameHwnd && GetTickCount64() < g_enableDeadline) {
    struct FindCtx { DWORD pid; HWND result; } ctx = { myPid, nullptr };
    EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
      auto *c = (FindCtx *)lp;
      DWORD wndPid = 0;
      GetWindowThreadProcessId(hw, &wndPid);
      if (wndPid != c->pid) return TRUE; // skip other processes
      char cls[64] = {};
      GetClassNameA(hw, cls, sizeof(cls));
      if (strcmp(cls, "UnityWndClass") == 0) {
        c->result = hw;
        return FALSE; // found
      }
      return TRUE;
    }, (LPARAM)&ctx);
    g_gameHwnd = ctx.result;
    if (!g_gameHwnd) WaitForSingleObject(g_cancel, 50);
  }
  if (!g_gameHwnd || !EnableOpen(epoch)) return APH_E_UNAVAILABLE;
  g_gameThread = GetWindowThreadProcessId(g_gameHwnd, nullptr);
  Log("[OK] Game window found: %p (PID=%u)", g_gameHwnd, myPid);

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = OverlayWndProc;
  wc.hInstance = GetModuleHandle(NULL);
  wc.lpszClassName = "BuffTooltipOverlay";
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return APH_E_UNAVAILABLE;

  RECT clientRect;
  GetClientRect(g_gameHwnd, &clientRect);
  POINT pt = {0, 0};
  ClientToScreen(g_gameHwnd, &pt);

  g_overlayHwnd = CreateWindowExA(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
      wc.lpszClassName, "", WS_POPUP, pt.x, pt.y, clientRect.right,
      clientRect.bottom, NULL, NULL, wc.hInstance, NULL);
  if (!g_overlayHwnd) return APH_E_UNAVAILABLE;
  SetLayeredWindowAttributes(g_overlayHwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

  Log("[OK] Overlay window created: %p", g_overlayHwnd);

  // Comprehensive diagnostics for invisible-HUD debugging
  {
    // Game window info
    LONG style = GetWindowLongA(g_gameHwnd, GWL_STYLE);
    LONG exStyle = GetWindowLongA(g_gameHwnd, GWL_EXSTYLE);
    RECT gameRect;
    GetWindowRect(g_gameHwnd, &gameRect);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int gameW = gameRect.right - gameRect.left;
    int gameH = gameRect.bottom - gameRect.top;

    // DPI info
    typedef UINT (WINAPI *tGetDpiForWindow)(HWND);
    UINT gameDpi = 96, overlayDpi = 96;
    if (hUser32) {
      auto pGetDpi = (tGetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
      if (pGetDpi) {
        gameDpi = pGetDpi(g_gameHwnd);
        overlayDpi = pGetDpi(g_overlayHwnd);
      }
    }

    // Overlay window info
    RECT overlayRect;
    GetWindowRect(g_overlayHwnd, &overlayRect);
    int ovW = overlayRect.right - overlayRect.left;
    int ovH = overlayRect.bottom - overlayRect.top;

    // Foreground window info
    HWND fg = GetForegroundWindow();
    char fgClass[128] = {};
    if (fg) GetClassNameA(fg, fgClass, sizeof(fgClass));

    Log("[DIAG] Game: %dx%d at (%d,%d) style=0x%X exStyle=0x%X dpi=%u",
        gameW, gameH, (int)gameRect.left, (int)gameRect.top,
        (int)style, (int)exStyle, gameDpi);
    Log("[DIAG] Overlay: %dx%d at (%d,%d) dpi=%u client=%dx%d",
        ovW, ovH, (int)overlayRect.left, (int)overlayRect.top,
        overlayDpi, (int)clientRect.right, (int)clientRect.bottom);
    Log("[DIAG] Screen: %dx%d scale=%d%% fg=%p class=%s",
        screenW, screenH, (int)(gameDpi * 100 / 96),
        fg, fgClass[0] ? fgClass : "(null)");

    if (gameDpi != overlayDpi)
      Log("[WARN] DPI mismatch: game=%u overlay=%u — coordinates may be wrong", gameDpi, overlayDpi);
    if (fg != g_gameHwnd)
      Log("[INFO] Foreground window is not the game (class=%s) — this is normal with launcher overlays", fgClass);
  }

  APH_Result result = APH_E_UNAVAILABLE;
  if (SetTimer(g_overlayHwnd, 1, 33, NULL)) {
    AcquireSRWLockExclusive(&g_workerLock);
    if (EnableOpen(epoch)) {
      result = g_client.CompleteEnable(epoch, APH_OK);
      if (result == APH_OK) g_pluginActive = true;
    }
    ReleaseSRWLockExclusive(&g_workerLock);
  }
  if (result == APH_OK) {
    ShowWindow(g_overlayHwnd, SW_SHOWNOACTIVATE);
    g_needRepaint = true;
    bool quit = false;
    while (!quit && RuntimeOpen() && WaitForSingleObject(g_cancel, 0) == WAIT_TIMEOUT) {
      DWORD wait = MsgWaitForMultipleObjects(1, &g_cancel, FALSE, 100, QS_ALLINPUT);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
      MSG msg{};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { quit = true; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }
  KillTimer(g_overlayHwnd, 1);
  DestroyWindow(g_overlayHwnd);
  g_overlayHwnd = nullptr;
  if (s_memDC) {
    SelectObject(s_memDC, s_oldBmp);
    DeleteObject(s_memBmp);
    DeleteDC(s_memDC);
    s_memDC = nullptr; s_memBmp = s_oldBmp = nullptr;
    s_memW = s_memH = 0;
  }
  if (s_skillFont) { DeleteObject(s_skillFont); s_skillFont = nullptr; s_skillLastH = 0; }
  UnregisterClassA(wc.lpszClassName, wc.hInstance);
  return result;
}

// ============================================================================
// Main Thread - Il2Cpp init + hooks only
// ============================================================================
static bool ResolveHudMetadata(void** asms, size_t ac) {
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
      if (bfname && (strcmp(bfname, "m_data") == 0 ||
                     strcmp(bfname, "<buffData>k__BackingField") == 0)) {
        g_buffDataOffset = bfoff;
      }
      if (bfname && (strcmp(bfname, "<id>k__BackingField") == 0 ||
                     strcmp(bfname, "m_id") == 0)) {
        g_buffIdOffset = bfoff;
      }
      if (bfname && (strcmp(bfname, "<blackboard>k__BackingField") == 0 ||
                     strcmp(bfname, "m_blackboard") == 0)) {
        g_buffBlackboardOffset = bfoff;
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

    // Resolve the remaining Buff fields through the full hierarchy if they
    // were not declared directly on the current game version's Buff class.
    if (g_buffDataOffset == 0) {
      const char *dataNames[] = {"m_data", "<buffData>k__BackingField"};
      int off = FindFieldInHierarchy(buffClass, dataNames, 2);
      if (off >= 0) g_buffDataOffset = (size_t)off;
    }
    if (g_buffIdOffset == 0) {
      const char *idNames[] = {"<id>k__BackingField", "m_id"};
      int off = FindFieldInHierarchy(buffClass, idNames, 2);
      if (off >= 0) g_buffIdOffset = (size_t)off;
    }
    if (g_buffBlackboardOffset == 0) {
      const char *bbNames[] = {"<blackboard>k__BackingField", "m_blackboard"};
      int off = FindFieldInHierarchy(buffClass, bbNames, 2);
      if (off >= 0) g_buffBlackboardOffset = (size_t)off;
    }

    Log("[OK] Buff class resolved (loaderOffset=0x%X)", (int)g_loaderOffset);
    if (g_buffDataOffset > 0)
      Log("[OK] Buff data field offset: 0x%X", (int)g_buffDataOffset);
    else
      Log("[WARN] Buff data field not found, using legacy offset 0x18");
    if (g_buffIdOffset > 0)
      Log("[OK] Buff id backing field offset: 0x%X", (int)g_buffIdOffset);
    else
      Log("[WARN] Buff id backing field not found, using legacy offset 0x140");
    if (g_buffBlackboardOffset > 0)
      Log("[OK] Buff blackboard backing field offset: 0x%X",
          (int)g_buffBlackboardOffset);
    else
      Log("[WARN] Buff blackboard backing field not found, using legacy offset 0x158");

  }

  // Resolve the two nested fields used by the buff icon-name path.
  void *buffDataClass = FindClass("Beyond.Gameplay.Core", "BuffData", asms, ac);
  if (buffDataClass) {
    const char *iconConfigNames[] = {"iconConfig", "m_iconConfig", "_iconConfig"};
    int off = FindFieldInHierarchy(buffDataClass, iconConfigNames, 3);
    if (off >= 0) g_buffDataIconConfigOffset = (size_t)off;
  }
  if (g_buffDataIconConfigOffset > 0)
    Log("[OK] BuffData.iconConfig offset: 0x%X",
        (int)g_buffDataIconConfigOffset);
  else
    Log("[WARN] BuffData.iconConfig not found, using legacy offset 0x18");

  void *buffIconConfigClass =
      FindClass("Beyond.Gameplay.Core", "BuffIconConfig", asms, ac);
  if (buffIconConfigClass) {
    const char *spritePathNames[] = {"_spritePath", "m_spritePath", "spritePath"};
    int off = FindFieldInHierarchy(buffIconConfigClass, spritePathNames, 3);
    if (off >= 0) g_buffIconSpritePathOffset = (size_t)off;
  }
  if (g_buffIconSpritePathOffset > 0)
    Log("[OK] BuffIconConfig sprite path offset: 0x%X",
        (int)g_buffIconSpritePathOffset);
  else
    Log("[WARN] BuffIconConfig sprite path not found, using legacy offset 0x10");

  void *amlClass = FindClass("", "AttributeModifierLoader", asms, ac);
  if (amlClass) {
    g_getAttrAdd = FindMethod(amlClass, "get_attributeAdditions", 0);
    g_getAttrMul = FindMethod(amlClass, "get_attributeMultipliers", 0);

    // Enumerate all fields to resolve modifier array offsets dynamically
    void* amlIter = nullptr;
    void* amlField;
    Log("  AttributeModifierLoader fields:");
    while ((amlField = il2cpp_class_get_fields(amlClass, &amlIter))) {
      const char* afname = il2cpp_field_get_name(amlField);
      int afoff = (int)il2cpp_field_get_offset(amlField);
      if (!afname) continue;
      Log("    [0x%X] %s", afoff, afname);

      // Resolve s_attributesToModify static field
      if (strcmp(afname, "s_attributesToModify") == 0) {
        g_satmField = amlField;
      }
      // Resolve modifier array offsets by field name
      // Game uses m_attribute* prefix naming convention
      if (strcmp(afname, "m_attributeAdditions") == 0 || strcmp(afname, "m_addition") == 0)
        g_offAmlAdd = afoff;
      else if (strcmp(afname, "m_attributeMultipliers") == 0 || strcmp(afname, "m_multiplier") == 0)
        g_offAmlMul = afoff;
      else if (strcmp(afname, "m_attributeFinalAdditions") == 0 || strcmp(afname, "m_finalAddition") == 0)
        g_offAmlFinalAdd = afoff;
      else if (strcmp(afname, "m_attributeFinalScalars") == 0 || strcmp(afname, "m_finalMultiplier") == 0)
        g_offAmlFinalScl = afoff;
      else if (strcmp(afname, "m_attributeBaseAdditions") == 0 || strcmp(afname, "m_baseAddition") == 0)
        g_offAmlBaseAdd = afoff;
      else if (strcmp(afname, "m_attributeBaseMultipliers") == 0 || strcmp(afname, "m_baseMultiplier") == 0)
        g_offAmlBaseMul = afoff;
      else if (strcmp(afname, "m_attributeBaseFinalAdditions") == 0 || strcmp(afname, "m_baseFinalAddition") == 0)
        g_offAmlBaseFinalAdd = afoff;
      else if (strcmp(afname, "m_attributeBaseFinalScalars") == 0 || strcmp(afname, "m_baseFinalMultiplier") == 0)
        g_offAmlBaseFinalScl = afoff;
    }
    Log("[OK] AttributeModifierLoader resolved: add=0x%X mul=0x%X finalAdd=0x%X finalScl=0x%X baseAdd=0x%X baseMul=0x%X baseFinalAdd=0x%X baseFinalScl=0x%X",
      g_offAmlAdd, g_offAmlMul, g_offAmlFinalAdd, g_offAmlFinalScl,
      g_offAmlBaseAdd, g_offAmlBaseMul, g_offAmlBaseFinalAdd, g_offAmlBaseFinalScl);
  }


  void *hpClass = FindClass("Beyond.UI", "MainCharHpBar", asms, ac);
  void *m;
  m = FindMethod(hpClass, "_OnProgressBarBuffChange", 3);
  if (!QueueHudHook<ProgressHook>(m, 1, "ProgressBar", hkProgressBar,
        "void(self,ObjectPtr<Buff>16,bool,i32,MethodInfo*)", {"Beyond.ObjectPtr<Beyond.Gameplay.Core.Buff>", "System.Boolean", "Beyond.Gameplay.Core.Buff.FinishReason"})) return false;
  m = FindMethod(hpClass, "_OnBuffIconChange", 3);
  if (!QueueHudHook<BuffIconHook>(m, 2, "BuffIcon", hkBuffIconChange,
        "void(self,ObjectPtr<Buff>16,bool,i32,MethodInfo*)", {"Beyond.ObjectPtr<Beyond.Gameplay.Core.Buff>", "System.Boolean", "Beyond.Gameplay.Core.Buff.FinishReason"})) return false;
  m = FindMethod(hpClass, "_OnBuffEnhanceChanged", 1);
  if (!QueueHudHook<EnhanceHook>(m, 3, "Enhance", hkEnhance,
        "void(self,ObjectPtr<Buff>16,MethodInfo*)", {"Beyond.ObjectPtr<Beyond.Gameplay.Core.Buff>"})) return false;
  m = FindMethod(hpClass, "LateTick", 1);
  if (!QueueHudHook<LateTickHook>(m, 8, "LateTick", hkLateTick,
        "void(self,f32,MethodInfo*)", {"System.Single"})) return false;
  m = FindMethod(hpClass, "_ClearMainChar", 0);
  if (!QueueHudHook<ClearHook>(m, 7, "ClearMainChar", hkClearMainChar,
        "void(self,MethodInfo*)", {})) return false;

  // Resolve MainCharHpBar.buffNode field offset dynamically
  if (hpClass) {
    const char *bnNames[] = {"buffNode", "_buffNode", "m_buffNode"};
    const char *bnMatch = nullptr;
    g_offHpBar_buffNode = FindFieldInHierarchy(hpClass, bnNames, 3, &bnMatch);
    if (g_offHpBar_buffNode >= 0)
      Log("[OK] MainCharHpBar.buffNode offset=0x%X (field=%s)", g_offHpBar_buffNode, bnMatch);
    else
      Log("[WARN] MainCharHpBar.buffNode NOT found, display order will use fallback");

    const char *gpuiNames[] = {
      "gpuiBuffNode", "_gpuiBuffNode", "m_gpuiBuffNode"
    };
    const char *gpuiMatch = nullptr;
    g_offHpBar_gpuiBuffNode =
        FindFieldInHierarchy(hpClass, gpuiNames, 3, &gpuiMatch);
    if (g_offHpBar_gpuiBuffNode >= 0)
      Log("[OK] MainCharHpBar.gpuiBuffNode offset=0x%X (field=%s)",
          g_offHpBar_gpuiBuffNode, gpuiMatch);
    else
      Log("[WARN] MainCharHpBar.gpuiBuffNode NOT found, using legacy offset 0xD8");
  }

  // Resolve UIBuffCell.get_buffInstanceUid for display order tracking
  void *buffCellClass = FindClass("Beyond.UI", "UIBuffCell", asms, ac);
  if (buffCellClass) {
    g_getBuffInstanceUid = FindMethod(buffCellClass, "get_buffInstanceUid", 0);
    Log("[OK] UIBuffCell.get_buffInstanceUid: %p", g_getBuffInstanceUid);
  }
  if (!g_getBuffInstanceUid) {
    void *gpuiBuffCellClass =
        FindClass("Beyond.UI", "GPUIBuffCell", asms, ac);
    if (gpuiBuffCellClass) {
      g_getBuffInstanceUid =
          FindMethod(gpuiBuffCellClass, "get_buffInstanceUid", 0);
      Log("[OK] GPUIBuffCell.get_buffInstanceUid fallback: %p",
          g_getBuffInstanceUid);
    }
  }

  // Hook GPUIBuffNode._OnBuffEnhanceChanged for stack tracking
  void *gpuiBuffNodeClass = FindClass("Beyond.UI", "GPUIBuffNode", asms, ac);
  if (gpuiBuffNodeClass) {
    const char *orderedNames[] = {
      "m_orderedBuffCellList", "_orderedBuffCellList"
    };
    int orderedOff =
        FindFieldInHierarchy(gpuiBuffNodeClass, orderedNames, 2);
    if (orderedOff >= 0)
      g_gpuiOrderedListOffset = (size_t)orderedOff;

    const char *stackNames[] = {"m_stackBuffsDict", "_stackBuffsDict"};
    int stackOff = FindFieldInHierarchy(gpuiBuffNodeClass, stackNames, 2);
    if (stackOff >= 0)
      g_gpuiStackBuffsOffset = (size_t)stackOff;

    if (g_gpuiOrderedListOffset > 0)
      Log("[OK] GPUIBuffNode.m_orderedBuffCellList offset: 0x%X",
          (int)g_gpuiOrderedListOffset);
    else
      Log("[WARN] GPUIBuffNode ordered list not found, using legacy offset 0xB0");
    if (g_gpuiStackBuffsOffset > 0)
      Log("[OK] GPUIBuffNode.m_stackBuffsDict offset: 0x%X",
          (int)g_gpuiStackBuffsOffset);
    else
      Log("[WARN] GPUIBuffNode stack dictionary not found, using legacy offset 0xA8");

    m = FindMethod(gpuiBuffNodeClass, "_OnBuffEnhanceChanged", 1);
    if (!QueueHudHook<GpuiEnhanceHook>(m, 4, "GpuiEnhance", hkGpuiEnhance,
        "void(self,ObjectPtr<Buff>16,MethodInfo*)", {"Beyond.ObjectPtr<Beyond.Gameplay.Core.Buff>"})) return false;
    Log("[OK] GPUIBuffNode._OnBuffEnhanceChanged hooked: %p", m);
  } else {
    Log("[WARN] GPUIBuffNode class not found");
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

    if (g_orderedListOffset == 0) {
      const char *orderedNames[] = {
        "m_orderedBuffCellList", "_orderedBuffCellList"
      };
      int off = FindFieldInHierarchy(buffNodeClass, orderedNames, 2);
      if (off >= 0) g_orderedListOffset = (size_t)off;
    }

    const char *stackNames[] = {"m_stackBuffsDict", "_stackBuffsDict"};
    int stackOff = FindFieldInHierarchy(buffNodeClass, stackNames, 2);
    if (stackOff >= 0) g_uiStackBuffsOffset = (size_t)stackOff;
    if (g_uiStackBuffsOffset > 0)
      Log("[OK] UIBuffNode.m_stackBuffsDict offset: 0x%X",
          (int)g_uiStackBuffsOffset);
    else
      Log("[WARN] UIBuffNode stack dictionary not found, using legacy offset 0xA0");
  }

  // Hook GPUIBuffNode stack methods for independent-instance stacking
  if (gpuiBuffNodeClass) {
    m = FindMethod(gpuiBuffNodeClass, "_AddStackBuffIconInternal", 4);
    if (!QueueHudHook<AddStackHook>(m, 5, "AddStack", hkAddStack,
        "void(self,ObjectPtr<Buff>16,bool,BuffData*,BuffCellGroup*,MethodInfo*)", {"Beyond.ObjectPtr<Beyond.Gameplay.Core.Buff>", "System.Boolean", "Beyond.Gameplay.Core.BuffData", "Beyond.UI.GPUIBuffNode.BuffCellGroup"})) return false;
    Log("[OK] GPUIBuffNode._AddStackBuffIconInternal hooked: %p", m);
    m = FindMethod(gpuiBuffNodeClass, "_RemoveStackBuffIconInternal", 4);
    if (!QueueHudHook<RemoveStackHook>(m, 6, "RemoveStack", hkRemoveStack,
        "void(self,ObjectPtr<Buff>16,i32,BuffData*,BuffCellGroup*,MethodInfo*)", {"Beyond.ObjectPtr<Beyond.Gameplay.Core.Buff>", "Beyond.Gameplay.Core.Buff.FinishReason", "Beyond.Gameplay.Core.BuffData", "Beyond.UI.GPUIBuffNode.BuffCellGroup"})) return false;
    Log("[OK] GPUIBuffNode._RemoveStackBuffIconInternal hooked: %p", m);
  }

  if (!buffClass || !hpClass || !g_getId || !g_getDuration || !g_getLifeTime) return false;
  return InitSkillHooks(asms, ac);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

// ============================================================================
// Basic settings. The old file is read only for the first import.
// ============================================================================
static char g_configPath[MAX_PATH] = {};
static SRWLOCK g_configLock = SRWLOCK_INIT;
static APH_ConfigApi g_configApi{};
static HANDLE g_configChanged;
static bool g_configImportPending;

static void InitConfigPath() {
    if (g_configPath[0]) return;
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    std::string d(p);
    size_t pos = d.find_last_of("\\/");
    if (pos != std::string::npos) d = d.substr(0, pos + 1);
    snprintf(g_configPath, sizeof(g_configPath), "%splugin\\combat_hud_config.txt", d.c_str());
}

static APH_Result SyncConfig() {
    if (!g_configApi.snapshot) return APH_E_UNAVAILABLE;
    AcquireSRWLockExclusive(&g_configLock);
    __try {
        APH_ConfigSnapshot state{};
        APH_ConfigValue values[4]{};
        APH_Result r = g_configApi.snapshot(g_client.owner(), sizeof(state), &state, 4, values);
        if (r != APH_OK) return r;
        if (g_configImportPending) {
            if (state.pending) return APH_PENDING;
            if (state.last_result != APH_OK) return state.last_result;
            g_configImportPending = false;
        }
        g_vkSkillHud = values[0].integer; g_vkBuffHud = values[1].integer;
        g_visualEnhance = values[2].integer != 0; g_langZh = !strcmp(values[3].text, "zh");
        g_configVisualDirty = true;
        return APH_OK;
    } __finally { ReleaseSRWLockExclusive(&g_configLock); }
}
static void APH_CALL ConfigChanged(APH_Owner, void*) {
    // The notification updates atomics only; the overlay consumes the repaint
    // flag on its own thread. No window or game operation runs in this callback.
    SyncConfig();
    SetEvent(g_configChanged);
}

static APH_Result ReadLegacyConfig(APH_ConfigValue (&values)[4], bool& found) {
    found = false;
    FILE* f = fopen(g_configPath, "rb");
    if (!f) return errno == ENOENT ? APH_OK : APH_E_IO;
    found = true;
    char line[512]; APH_Result result = APH_OK;
    while (fgets(line, sizeof(line), f)) {
        char key[128], value[128];
        const char* input = strlen(line) >= 3 && !memcmp(line, "\xef\xbb\xbf", 3) ? line + 3 : line;
        if (sscanf(input, " %127[^= \t] = %127s", key, value) != 2) continue;
        int index = !strcmp(key, "toggle_skill_hud") ? 0 : !strcmp(key, "toggle_buff_hud") ? 1 : !strcmp(key, "visual_enhance") ? 2 : -1;
        if (index < 0) continue;
        errno = 0; char* end = nullptr; const long number = strtol(value, &end, 0);
        if (errno || end == value || *end || number < 0 || number > (index == 2 ? 1 : 255)) { result = APH_E_ARGUMENT; break; }
        values[index].integer = int(number);
    }
    if (ferror(f)) result = APH_E_IO;
    fclose(f);
    return result;
}

static APH_Result RegisterConfig() {
    APH_Result r = g_client.Service(g_configApi);
    if (r != APH_OK) return r;
    if (!g_configApi.set_notifier) return APH_E_UNAVAILABLE;
    HMODULE module = nullptr; wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&RegisterConfig), &module)) return APH_E_UNAVAILABLE;
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return APH_E_UNAVAILABLE;
    wchar_t* slash = wcsrchr(path, L'\\'); if (!slash) return APH_E_UNAVAILABLE;
    if (wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"config\\applepie.combat_hud.ini")) return APH_E_UNAVAILABLE;
    const DWORD attributes = GetFileAttributesW(path);
    const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    if (error && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return APH_E_IO;
    APH_ConfigField fields[4]{};
    const char* keys[]{"toggle_skill_hud", "toggle_buff_hud", "visual_enhance", "language"};
    const char* labels[]{"Skill HUD key / 技能 HUD 快捷键", "Buff HUD key / Buff HUD 快捷键", "Visual effects / 视觉增强", "Language / 语言"};
    const APH_ConfigOption languages[]{{"zh"}, {"en"}};
    for (unsigned i = 0; i < 4; ++i) {
        auto& f = fields[i]; f.size = sizeof(f);
        f.type = f.initial.type = i < 2 ? APH_CONFIG_KEY : i == 2 ? APH_CONFIG_BOOL : APH_CONFIG_ENUM;
        strcpy_s(f.key, keys[i]); strcpy_s(f.label, labels[i]);
        if (i < 2) {
            f.initial.integer = i == 0 ? VK_F11 : VK_F12;
            strcpy_s(f.description, "Click and press a key; Clear disables it / 点击后按键，清除可禁用");
        }
    }
    fields[3].options = languages; fields[3].option_count = 2; strcpy_s(fields[3].initial.text, "zh");
    APH_ConfigValue imported[4]{};
    for (unsigned i = 0; i < 4; ++i) imported[i] = fields[i].initial;
    bool importLegacy = false;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        r = ReadLegacyConfig(imported, importLegacy);
        if (r != APH_OK) return r;
    }
    const APH_ConfigSchema schema{sizeof(schema), 4, fields};
    r = g_configApi.register_schema(g_client.owner(), &schema);
    if (r != APH_OK) return r;
    g_configChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_configChanged) return APH_E_UNAVAILABLE;
    r = g_configApi.set_notifier(g_client.owner(), ConfigChanged, nullptr);
    if (r != APH_OK) { CloseHandle(g_configChanged); g_configChanged = nullptr; return r; }
    if (importLegacy) {
        APH_ConfigSnapshot state{};
        r = g_configApi.snapshot(g_client.owner(), sizeof(state), &state, 0, nullptr);
        if (r != APH_OK) return r;
        AcquireSRWLockExclusive(&g_configLock);
        g_configImportPending = true;
        ReleaseSRWLockExclusive(&g_configLock);
        r = g_configApi.save(g_client.owner(), state.revision, 4, imported);
        if (r != APH_PENDING && r != APH_OK) return r;
        Log("[CONFIG] Importing legacy settings into Manager configuration");
        return APH_OK;
    }
    return SyncConfig();
}

static APH_Result SaveLanguage(bool chinese) {
    if (!g_configApi.snapshot) return APH_E_UNAVAILABLE;
    AcquireSRWLockExclusive(&g_configLock);
    __try {
        APH_ConfigSnapshot state{}; APH_ConfigValue values[4]{};
        APH_Result r = g_configApi.snapshot(g_client.owner(), sizeof(state), &state, 4, values);
        if (r != APH_OK) return r;
        if (state.pending || g_configImportPending) return APH_E_BUSY;
        const char* language = chinese ? "zh" : "en";
        if (!strcmp(values[3].text, language)) return APH_OK;
        strcpy_s(values[3].text, language);
        return g_configApi.save(g_client.owner(), state.revision, 4, values);
    } __finally { ReleaseSRWLockExclusive(&g_configLock); }
}

// ============================================================================
// Plugin Manager Interface
// ============================================================================


static AP_PluginInfo s_pluginInfo = {
    APPLEPIE_PLUGIN_API_VERSION,
    "combat_hud",
    "Combat HUD \xe6\x88\x98\xe6\x96\x97\xe4\xbf\xa1\xe6\x81\xaf",        // 战斗信息
    "\xe6\x98\xbe\xe7\xa4\xba" "Buff\xe8\xaf\xa6\xe6\x83\x85\xe4\xb8\x8e\xe5\xb1\x9e\xe6\x80\xa7\xe5\x8a\xa0\xe6\x88\x90",  // 显示Buff详情与属性加成
    "config/applepie.combat_hud.ini",
    true  // supports hot disable
};

APPLEPIE_PLUGIN_EXPORT AP_PluginInfo* AP_GetPluginInfo() {
    s_pluginInfo.displayName = L("Combat HUD \xe6\x88\x98\xe6\x96\x97\xe4\xbf\xa1\xe6\x81\xaf",
                                  "Combat HUD");
    s_pluginInfo.description = L("\xe6\x98\xbe\xe7\xa4\xba" "Buff\xe8\xaf\xa6\xe6\x83\x85\xe4\xb8\x8e\xe5\xb1\x9e\xe6\x80\xa7\xe5\x8a\xa0\xe6\x88\x90",
                                  "Display buff details and attribute bonuses");
    return &s_pluginInfo;
}

APPLEPIE_PLUGIN_EXPORT void AP_SetLanguage(const char* langCode) {
    const APH_Result result = SaveLanguage(langCode && langCode[0] == 'z');
    if (result < 0) Log("[LANG] Language save not accepted: %d", result);
}

APPLEPIE_PLUGIN_EXPORT int AP_GetHotkeys(AP_HotkeyInfo* outArray, int maxCount) {
    if (!outArray || maxCount <= 0) return 0;
    AP_HotkeyInfo hotkeys[2]{};
    hotkeys[0] = {
        L("\xe5\x88\x87\xe6\x8d\xa2\xe6\x8a\x80\xe8\x83\xbd\xe6\x98\xbe\xe7\xa4\xba", "Toggle Skill HUD"),  // 切换技能显示
        "toggle_skill_hud", g_vkSkillHud
    };
    hotkeys[1] = {
        L("\xe5\x88\x87\xe6\x8d\xa2" "Buff\xe6\x98\xbe\xe7\xa4\xba", "Toggle Buff HUD"),  // 切换Buff显示
        "toggle_buff_hud", g_vkBuffHud
    };
    int count = 2;
    if (count > maxCount) count = maxCount;
    for (int i = 0; i < count; i++) outArray[i] = hotkeys[i];
    return count;
}

APPLEPIE_PLUGIN_EXPORT bool AP_ReloadConfig() {
    return SyncConfig() == APH_OK;
}

// SDK lifecycle: one cancellable worker owns initialization and the overlay.
// It never invokes gameplay methods. Metadata queries run on an attached
// IL2CPP worker; actual getters execute only inside verified UI callbacks.
static APH_Result InitializeHud(uint64_t epoch) {
  while (EnableOpen(epoch) && GetTickCount64() < g_enableDeadline) {
    if (Resolve()) break;
    // A loaded PE's exports do not appear later. Fail closed if a required
    // readiness probe is absent, rather than falling back to domain_get.
    if (hGA) return APH_E_UNAVAILABLE;
    WaitForSingleObject(g_cancel, 50);
  }
  if (!hGA || !EnableOpen(epoch) || GetTickCount64() >= g_enableDeadline)
    return APH_E_UNAVAILABLE;
  bool stopInstalled = false;
  APH_NativeProc shutdown = (APH_NativeProc)GetProcAddress(hGA, "il2cpp_shutdown");
  if (shutdown) {
    APH_Result r = ShutdownHook::Install(epoch, shutdown, "void() C-export", "il2cpp.runtime.shutdown");
    if (r != APH_OK) return r;
    stopInstalled = true;
  }
  Log("[APH] Waiting for IL2CPP corlib and an existing registered VM thread before domain/attach/metadata");
  while (EnableOpen(epoch) && GetTickCount64() < g_enableDeadline && !WorkerAttachReady())
    WaitForSingleObject(g_cancel, 50);
  if (!EnableOpen(epoch) || GetTickCount64() >= g_enableDeadline) {
    Log("[APH] IL2CPP startup wait ended before readiness: epoch=%llu", (unsigned long long)epoch);
    return APH_E_UNAVAILABLE;
  }
  Log("[APH] IL2CPP worker readiness confirmed; starting metadata discovery");
  void* domain = nullptr;
  while (EnableOpen(epoch) && GetTickCount64() < g_enableDeadline) {
    domain = il2cpp_domain_get();
    if (domain) break;
    WaitForSingleObject(g_cancel, 50);
  }
  if (!domain || !EnableOpen(epoch) || GetTickCount64() >= g_enableDeadline)
    return APH_E_UNAVAILABLE;
  void* thread = il2cpp_thread_attach(domain);
  if (!thread) return APH_E_UNAVAILABLE;
  __try {
    size_t count = 0;
    void** assemblies = nullptr;
    while (EnableOpen(epoch) && GetTickCount64() < g_enableDeadline) {
      assemblies = il2cpp_domain_get_assemblies(domain, &count);
      if (assemblies && count && FindClass("Beyond.UI", "MainCharHpBar", assemblies, count)) break;
      WaitForSingleObject(g_cancel, 50);
    }
    if (!assemblies || !count || !EnableOpen(epoch) || GetTickCount64() >= g_enableDeadline)
      return APH_E_UNAVAILABLE;
    void* application = FindClass("UnityEngine", "Application", assemblies, count);
    void* quit = FindMethod(application, "Internal_ApplicationQuit", 0);
    if (ValidateMethod(quit, "System.Void", {}, true)) {
      APH_Result r = QuitHook::Install(epoch, (APH_NativeProc)((MInfo*)quit)->mp,
          "void(MethodInfo*) static", "unity.Application.Internal_ApplicationQuit");
      if (r != APH_OK) return r;
      stopInstalled = true;
    }
    if (!stopInstalled) return APH_E_UNAVAILABLE;
    if (!ResolveHudMetadata(assemblies, count)) return APH_E_SIGNATURE;
    return EnableOpen(epoch) ? APH_OK : APH_E_REVOKED;
  } __finally {
    // Once shutdown starts the runtime owns destruction of its threads.
    if (RuntimeOpen()) il2cpp_thread_detach(thread);
  }
}

static DWORD WINAPI HudWorker(void* parameter) {
  const uint64_t epoch = (uint64_t)(uintptr_t)parameter;
  APH_Result result = APH_E_FAULT;
  try {
    result = APH_PENDING;
    while (EnableOpen(epoch) && GetTickCount64() < g_enableDeadline) {
      result = SyncConfig();
      if (result != APH_PENDING) break;
      const ULONGLONG now = GetTickCount64();
      if (now >= g_enableDeadline) break;
      const HANDLE signals[]{g_cancel, g_configChanged};
      if (WaitForMultipleObjects(2, signals, FALSE, DWORD(g_enableDeadline - now)) != WAIT_OBJECT_0 + 1) {
        result = APH_E_UNAVAILABLE; break;
      }
    }
    if (!EnableOpen(epoch) || GetTickCount64() >= g_enableDeadline) result = APH_E_UNAVAILABLE;
    if (result == APH_OK) result = InitializeHud(epoch);
    if (result == APH_OK) result = RunOverlay(epoch);
  } catch (...) { result = APH_E_FAULT; } // local C++ boundary, /EHsc only
  g_pluginActive = false;
  if (result != APH_OK) {
    Log("[APH] Enable epoch=%llu ended: %d", (unsigned long long)epoch, result);
    g_client.CompleteEnable(epoch, result); // stale completion is rejected by Core
  } else {
    g_client.Disable(); // also drain after window destruction / runtime stop
  }
  return 0;
}

static void ResetHudSnapshots() {
  // Called only with native business drained AND our worker signalled.
  g_activeHpBar = 0;
  g_gameThread = 0;
  g_gameHwnd = nullptr;
  g_mainCharHpBarPtr = nullptr;
  g_buffCount = g_displayCount = 0;
  memset(g_buffs, 0, sizeof(g_buffs));
  g_skillHud = MakeEmptyHudState();
  memset(g_comboState, 0, sizeof(g_comboState));
  memset(g_lastSkillUpdate, 0, sizeof(g_lastSkillUpdate));
  g_lastBuffScan = 0;
  g_sceneActive = g_combatActive = false;
  g_cgAlpha = 0.0f;
  g_lastHpTick = g_lastComboTick = 0;
  g_uspMethodsResolved = s_cgResolved = false;
  g_mGetUltimateSp = g_mGetMaxUltimateSp = s_cgGetAlpha = nullptr;
  g_satmLoaded = false;
  g_satmLen = 0;
  g_tooltipText[0] = 0;
  g_showTooltip = false;
  g_needRepaint = true;
}

static void APH_CALL DisablingHud(APH_Owner, uint64_t, void*) {
  AcquireSRWLockExclusive(&g_workerLock);
  g_pluginActive = false;
  SetEvent(g_cancel);
  ReleaseSRWLockExclusive(&g_workerLock);
}
static APH_Result APH_CALL RestoreHud(APH_Owner, const APH_Cleanup* cleanup, void*) {
  if (!cleanup || cleanup->domains) return APH_E_ARGUMENT;
  AcquireSRWLockExclusive(&g_workerLock);
  __try {
    if (g_worker) {
      DWORD wait = WaitForSingleObject(g_worker, 0);
      if (wait == WAIT_TIMEOUT) return APH_PENDING;
      if (wait != WAIT_OBJECT_0) return APH_E_NEEDS_RESTART;
      CloseHandle(g_worker);
      g_worker = nullptr;
    }
    ResetHudSnapshots();
    return APH_OK;
  } __finally { ReleaseSRWLockExclusive(&g_workerLock); }
}

extern "C" __declspec(dllexport) const APH_Manifest APH_PluginManifest = {
  sizeof(APH_Manifest), APH_ABI_MAJOR, APH_ABI_MINOR, APH_ARCH_X64,
  APH_PLUGIN_LIFECYCLE | APH_PLUGIN_LATE_LOAD | APH_PLUGIN_MODULE_OBSERVER,
  APH_CAP_CORE | APH_CAP_HOOKS, 0, "combat_hud", "Endfield Combat HUD"
};
extern "C" __declspec(dllexport) APH_Result APH_CALL APH_Attach(const APH_AttachArgs* args) {
  APH_Result r = g_client.Attach(args);
  if (r != APH_OK) return r;
  r = g_client.Service(g_runtime);
  if (r != APH_OK) return r;
  InitializeCriticalSection(&g_logLock);
  InitializeCriticalSection(&g_buffLock);
  InitializeCriticalSection(&g_skillLock);
  g_cancel = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  if (!g_cancel) return APH_E_UNAVAILABLE;
  const APH_LifecycleDesc life{sizeof(life), 60000, DisablingHud, RestoreHud, nullptr};
  r = g_client.RegisterLifecycle(life);
  if (r != APH_OK) return r;
  try {
    InitConfigPath();
    char logPath[MAX_PATH]{};
    strcpy_s(logPath, g_configPath);
    char* slash = strrchr(logPath, '\\');
    if (slash) strcpy_s(slash + 1, sizeof(logPath) - (slash + 1 - logPath), "buff_sniff_log.txt");
    g_logHandle = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    r = RegisterConfig();
  } catch (...) { return APH_E_FAULT; }
  return r;
}
extern "C" __declspec(dllexport) APH_Result APH_CALL APH_Enable(APH_Owner owner, uint64_t epoch) {
  if (owner != g_client.owner() || !epoch || !g_cancel) return APH_E_ARGUMENT;
  AcquireSRWLockExclusive(&g_workerLock);
  __try {
    if (g_worker || g_stopping.load()) return APH_E_UNAVAILABLE;
    g_epoch = epoch;
    g_enableDeadline = GetTickCount64() + 45000;
    g_pluginActive = false;
    g_running = true;
    ResetEvent(g_cancel);
    g_worker = CreateThread(nullptr, 0, HudWorker, (void*)(uintptr_t)epoch, 0, nullptr);
    if (!g_worker) { SetEvent(g_cancel); return APH_E_UNAVAILABLE; }
    return APH_PENDING;
  } __finally { ReleaseSRWLockExclusive(&g_workerLock); }
}
