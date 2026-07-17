#include "MinHook.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

extern "C" __declspec(dllexport) void DummyExport() {}

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
    // Timer fires ~30fps on overlay thread - do hit testing here with pure
    // Win32
    if (!IsWindow(g_gameHwnd)) {
      g_running = false;
      PostQuitMessage(0);
      return 0;
    }

    // Z-order based visibility (throttled to every ~500ms to reduce overhead)
    // On 4K displays, the per-tick Z-order walk + full-screen bitmap ops are expensive
    {
      static DWORD s_lastZCheckTick = 0;
      DWORD now = GetTickCount();
      if (now - s_lastZCheckTick > 100) {
        s_lastZCheckTick = now;
      bool coveredByApp = false;
      // Walk Z-order from top, looking for a visible non-TOPMOST window above the game
      HWND w = GetTopWindow(NULL);
      while (w && w != g_gameHwnd) {
        if (w != hwnd && IsWindowVisible(w)) {
          LONG exStyle = GetWindowLongA(w, GWL_EXSTYLE);
          bool isOverlay = (exStyle & WS_EX_TOPMOST) || (exStyle & WS_EX_TOOLWINDOW);
          if (!isOverlay) {
            // A regular visible window is above the game — user alt-tabbed
            RECT wr;
            GetWindowRect(w, &wr);
            int wArea = (wr.right - wr.left) * (wr.bottom - wr.top);
            if (wArea > 50000) { // ignore tiny windows (notifications, tooltips)
              coveredByApp = true;
              break;
            }
          }
        }
        w = GetNextWindow(w, GW_HWNDNEXT);
      }
      if (coveredByApp) {
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
        return 0;
      } else if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNA);
        g_needRepaint = true;
      }
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
          // Also multiply blackboard values for independent-instance stacking.
          // Each instance holds its own base value — the engine doesn't merge
          // blackboard across independent instances.
          // Skip 'duration' and 'rate' keys (those are per-instance constants).
          for (int b = 0; b < ab.bbCount; b++) {
            const char *k = ab.bb[b].key;
            if (strcmp(k, "duration") == 0) continue;
            if (strcmp(k, "rate") == 0) continue;
            ab.bb[b].value *= multiplier;
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
          {"attack",           "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b", true},
          {"def",             "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", false},
          {"defend",           "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b", true},
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
          // === Normal/Crit (角色大招层数) ===
          {"normal_dmg_up",   "\xe6\x99\xae\xe6\x94\xbb\xe5\xa2\x9e\xe4\xbc\xa4", true},
          {"crit_rate_up",    "\xe6\x9a\xb4\xe5\x87\xbb\xe7\x8e\x87\xe6\x8f\x90\xe5\x8d\x87", true},
          {"crit_rate_up_dynamic", "\xe6\x9a\xb4\xe5\x87\xbb\xe7\x8e\x87(\xe5\x8a\xa8\xe6\x80\x81)", true},
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

      // If tooltip has no effect info, show raw blackboard entries as fallback
      {
        bool hasEffect = false;
        int lineCount = 0;
        for (const char *p = g_tooltipText; *p; p++)
          if (*p == '\n') lineCount++;
        if (lineCount > 3) hasEffect = true;
        if (!hasEffect) {
          Log("[NO_EFFECT] buff=%s icon=%s bbCount=%d", ab.id, ab.iconName, ab.bbCount);
          // Show raw bb entries in tooltip as fallback
          for (int b = 0; b < ab.bbCount; b++) {
            const char *k = ab.bb[b].key;
            double v = ab.bb[b].value;
            if (strcmp(k, "duration") == 0 || strcmp(k, "rate") == 0) continue;
            if (v == 0.0) continue;
            char buf[200];
            // Auto-detect percentage: values between -1 and 1 (exclusive) are likely percentages
            if (v > -1.0 && v < 1.0 && v != 0.0)
              snprintf(buf, sizeof(buf), " %s: %+.1f%%\n", k, v * 100.0);
            else
              snprintf(buf, sizeof(buf), " %s: %.1f\n", k, v);
            tooltip_cat(buf);
            Log("  bb[%d] key='%s' value=%.6f", b, ab.bb[b].key, ab.bb[b].value);
          }
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
      if (g_visibilityChanged) {
        g_visibilityChanged = false;
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
typedef void (*tProgressBar)(void *, void *, void *, void *, void *);
static tProgressBar oProgressBar = nullptr;
static void *g_activeHpBar = nullptr; // The MainCharHpBar of the currently active (controlled) character
void hkProgressBar(void *self, void *p1, void *p2, void *p3, void *mi) {
  // Only process buffs from the active character's HpBar
  // When g_activeHpBar is null (before first LateTick), skip all —
  // display scan fallback will read visible buffs directly from cells
  if (!g_activeHpBar || self != g_activeHpBar) {
    oProgressBar(self, p1, p2, p3, mi);
    return;
  }
  uintptr_t isAdd = (uintptr_t)p2;
  ActiveBuff ab = {};
  ReadBuffData(p1, &ab);



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
        g_buffs[i] = ab;
        merged = true;
        break;
      } else if (strcmp(g_buffs[i].id, ab.id) == 0 && strcmp(g_buffs[i].iconName, ab.iconName) == 0) {
        // Same buff type via ProgressBar — replace old instance
        g_buffs[i].buffObj = ab.buffObj;
        g_buffs[i].instUid = ab.instUid;
        g_buffs[i].duration = ab.duration;
        g_buffs[i].lifeTime = ab.lifeTime;
        merged = true;
        break;
      }
    }
    if (!merged && g_buffCount < 64) {
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
  // Only process buffs from the active character's HpBar
  if (!g_activeHpBar || self != g_activeHpBar) {
    oBuffIconChange(self, p1, p2, p3, mi);
    return;
  }
  uintptr_t isAdd = (uintptr_t)p2;
  ActiveBuff ab = {};
  ReadBuffData(p1, &ab);



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
static tEnhance oGpuiEnhance = nullptr;

void hkEnhanceCommon(void *p1) {
  ActiveBuff ab = {};
  ReadBuffData(p1, &ab);

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

void hkEnhance(void *self, void *p1, void *mi) {
  hkEnhanceCommon(p1);
  oEnhance(self, p1, mi);
}
void hkGpuiEnhance(void *self, void *p1, void *mi) {
  hkEnhanceCommon(p1);
  oGpuiEnhance(self, p1, mi);
}

// Hook GPUIBuffNode stack operations (passthrough — stack counts read from m_stackBuffsDict)
typedef void (*tAddStack)(void *, void *, bool, void *, void *, void *);
static tAddStack oAddStack = nullptr;
void hkAddStack(void *self, void *p1, bool playAnim, void *buffData, void *group, void *mi) {
  oAddStack(self, p1, playAnim, buffData, group, mi);
}
typedef void (*tRemoveStack)(void *, void *, int, void *, void *, void *);
static tRemoveStack oRemoveStack = nullptr;
void hkRemoveStack(void *self, void *p1, int finishReason, void *buffData, void *group, void *mi) {
  oRemoveStack(self, p1, finishReason, buffData, group, mi);
}

// Hook for _ClearMainChar - clears all buffs when entering dungeon/switching char
typedef void (*tClearMainChar)(void *, void *);
static tClearMainChar oClearMainChar = nullptr;
void hkClearMainChar(void *self, void *mi) {
  EnterCriticalSection(&g_buffLock);
  g_buffCount = 0;
  g_displayCount = 0;
  LeaveCriticalSection(&g_buffLock);
  // The HpBar being cleared IS the new active character's HpBar
  g_activeHpBar = self;
  Log("[CLEAR] All buffs cleared, activeHpBar=%p", self);
  oClearMainChar(self, mi);
}

typedef void (*tLateTick)(void *, float, void *);
static tLateTick oLateTick = nullptr;

void hkLateTick(void *self, float dt, void *mi) {
  oLateTick(self, dt, mi);

  // Track which MainCharHpBar is active (the one LateTick is called on)
  g_activeHpBar = self;

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
          if (k) s_cgGetAlpha = FindMethod(k, "get_alpha", 0);
        }
        if (s_cgGetAlpha) {
          g_cgAlpha = SDirectFloat(s_cgGetAlpha, cgObj);
        }
      }
    }
  }

  // Wall-clock throttle: every ~1000ms (not frame-based, since at 200fps
  // a 60-frame counter fires every 300ms — 3x too often)
  static DWORD s_lastTickMs = 0;
  DWORD now = GetTickCount();
  if (now - s_lastTickMs < 1000) return;
  s_lastTickMs = now;


  EnterCriticalSection(&g_buffLock);
  // Remove expired buffs + refresh lifeTime + refresh attrs for tooltip
  // NOTE: Do NOT check m_isFinished — the game recycles buff objects to pools
  // and sets m_isFinished even when the buff effect persists with a new instance.
  for (int i = g_buffCount - 1; i >= 0; i--) {
    // Refresh lifeTime (validates pointer via SEH)
    if (g_buffs[i].buffObj && g_getLifeTime) {
      __try {
        void *boxedLife = Invoke(g_getLifeTime, g_buffs[i].buffObj);
        if (boxedLife) {
          float life = *(float *)((char *)boxedLife + 0x10);
          g_buffs[i].lifeTime = life;
          if (g_buffs[i].duration < 100000.0f && life <= 0.0f) {
            g_buffs[i] = g_buffs[--g_buffCount];
            continue;
          }
        }
      } __except(1) {
        // Object pointer invalid (GC'd/recycled) — null it out but KEEP the buff.
        // Cached id/duration/lifeTime/attrs are still valid.
        // Buff will only be removed by explicit isAdd=false callback.
        g_buffs[i].buffObj = nullptr;
      }
    }
    // Re-read attributes from loader (pure memory read, no invoke)
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

  // Read display order from buff node's m_orderedBuffCellList
  // Try GPUIBuffNode first (game update switched to GPUI), then UIBuffNode fallback
  if (g_getBuffInstanceUid) {
    void *buffNode = nullptr;
    int listOffset = 0;

    // GPUIBuffNode at self+0xD8, its m_orderedBuffCellList at offset 0xB0
    __try {
      void *gpui = *(void **)((char *)self + 0xD8);

      if (gpui) {
        void *ol = *(void **)((char *)gpui + 0xB0);
        if (ol) {
          int sz = *(int *)((char *)ol + 0x18);
          if (sz > 0) {
            buffNode = gpui;
            listOffset = 0xB0;
          }
        }
      }
    } __except(1) {}

    // Fallback: UIBuffNode at self+0xD0, its m_orderedBuffCellList at g_orderedListOffset
    if (!buffNode && g_orderedListOffset > 0) {
      __try {
        void *uibn = *(void **)((char *)self + 0xD0);
        if (uibn) {
          void *ol = *(void **)((char *)uibn + g_orderedListOffset);
          if (ol) {
            int sz = *(int *)((char *)ol + 0x18);
            if (sz > 0) {
              buffNode = uibn;
              listOffset = (int)g_orderedListOffset;
            }
          }
        }
      } __except(1) {}
    }

    if (buffNode) {
      __try {
        void *orderedList = *(void **)((char *)buffNode + listOffset);
        if (orderedList) {
          void *items = *(void **)((char *)orderedList + 0x10);
          int size = *(int *)((char *)orderedList + 0x18);
          if (items && size >= 0 && size <= 64) {
            int newDisplayCount = 0;
            // Temporary array to rebuild g_buffs from display
            ActiveBuff newBuffs[64];
            int newBuffCount = 0;

            // Read stack counts from GPUIBuffNode.m_stackBuffsDict (0xA8)
            // DynamicFastLookupCollection -> m_list (+0x18) -> List<ValueTuple<String, List<ObjectPtr<Buff>>>>
            struct StackEntry { char id[128]; int count; };
            StackEntry stackCounts[32];
            int stackCountN = 0;
            __try {
              void *stackDict = *(void **)((char *)buffNode + 0xA8);
              if (stackDict) {
                void *mList = *(void **)((char *)stackDict + 0x18);
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

            void **elements = (void **)((char *)items + 0x20);
            // Resolve get_buffInstanceUid dynamically from first cell's class
            static void *s_gpuiGetUid = nullptr;
            static bool s_gpuiGetUidResolved = false;
            static int s_cellBuffPtrOffset = -2; // -2 = not resolved

            for (int ci = 0; ci < size; ci++) {
              void *cell = elements[ci];
              if (!cell) continue;

              // Resolve method from actual cell class (handles both UIBuffCell and GPUIBuffCell)
              if (!s_gpuiGetUidResolved) {
                s_gpuiGetUidResolved = true;
                void *cellClass = il2cpp_object_get_class(cell);
                void *cur = cellClass;
                while (cur && !s_gpuiGetUid) {
                  s_gpuiGetUid = FindMethod(cur, "get_buffInstanceUid", 0);
                  if (!s_gpuiGetUid) cur = il2cpp_class_get_parent(cur);
                }
                Log("[DIAG] Resolved cell get_buffInstanceUid: %p (class=%s)",
                    s_gpuiGetUid,
                    cellClass ? il2cpp_class_get_name(cellClass) : "null");
              }
              void *uidMethod = s_gpuiGetUid ? s_gpuiGetUid : g_getBuffInstanceUid;
              if (!uidMethod) continue;

              void *boxedUid = Invoke(uidMethod, cell);
              if (!boxedUid) continue;
              uint32_t cellUid = *(uint32_t *)((char *)boxedUid + 16);
              g_displayUids[newDisplayCount++] = cellUid;
              
              // Stack count is read from m_stackBuffsDict (stackCounts array above)
              // _buffStackCountText doesn't work for GPUI cells

              // Check if we already have this buff from hooks (match by instUid or by buff id)
              bool found = false;
              for (int bi = 0; bi < g_buffCount; bi++) {
                if (g_buffs[bi].instUid == cellUid) {
                  ActiveBuff copy = g_buffs[bi];
                  // Use m_stackBuffsDict count as primary source for stack count
                  for (int si = 0; si < stackCountN; si++) {
                    if (strcmp(stackCounts[si].id, copy.id) == 0) {
                      copy.enhanceCnt = stackCounts[si].count;
                      break;
                    }
                  }

                  newBuffs[newBuffCount++] = copy;
                  found = true;
                  break;
                }
              }
              // If not matched by instUid, try matching by buff id (object may have been recycled)
              if (!found) {
                for (int bi = 0; bi < g_buffCount; bi++) {
                  if (g_buffs[bi].id[0] && strcmp(g_buffs[bi].id, "") != 0) {
                    // We need the buff data from the cell to compare ids
                    // Read buff from cell first
                    void *cellBuff = nullptr;
                    if (s_cellBuffPtrOffset > 0) {
                      __try { cellBuff = *(void **)((char *)cell + s_cellBuffPtrOffset); } __except(1) {}
                    }
                    if (cellBuff) {
                      char cellId[128] = {};
                      __try {
                        void *idStr = *(void **)((char *)cellBuff + 0x140);
                        if (idStr) ReadStr(idStr, cellId, sizeof(cellId));
                      } __except(1) {}
                      if (cellId[0] && strcmp(g_buffs[bi].id, cellId) == 0) {
                        ActiveBuff copy = g_buffs[bi];
                        copy.instUid = cellUid;
                        copy.buffObj = cellBuff;
                        for (int si = 0; si < stackCountN; si++) {
                          if (strcmp(stackCounts[si].id, copy.id) == 0) {
                            copy.enhanceCnt = stackCounts[si].count;
                            break;
                          }
                        }

                        newBuffs[newBuffCount++] = copy;
                        found = true;
                        break;
                      }
                    }
                  }
                }
              }
              // Not found from hooks — read buff data directly from cell
              if (!found && newBuffCount < 64) {
                // Dynamically resolve m_buffPtr offset from cell's class
                if (s_cellBuffPtrOffset == -2) {
                  void *cellClass = il2cpp_object_get_class(cell);
                  const char *buffPtrNames[] = {"m_buffPtr", "_buffPtr", "buffPtr"};
                  s_cellBuffPtrOffset = FindFieldInHierarchy(cellClass, buffPtrNames, 3);
                  Log("[DIAG] Cell m_buffPtr offset: 0x%X (class=%s)",
                      s_cellBuffPtrOffset,
                      cellClass ? il2cpp_class_get_name(cellClass) : "null");
                }
                void *buffPtr = nullptr;
                if (s_cellBuffPtrOffset > 0) {
                  __try {
                    // ObjectPtr<Buff> is a struct: first field is the object pointer
                    buffPtr = *(void **)((char *)cell + s_cellBuffPtrOffset);
                  } __except(1) {}
                }
                if (buffPtr) {
                  struct { void *b; uint32_t u; } dummy = { buffPtr, cellUid };
                  ActiveBuff ab = {};
                  ReadBuffData(&dummy, &ab);
                  ab.source = 2; // from display scan
                  // Apply stack count from m_stackBuffsDict
                  for (int si = 0; si < stackCountN; si++) {
                    if (strcmp(stackCounts[si].id, ab.id) == 0) {
                      ab.enhanceCnt = stackCounts[si].count;
                      break;
                    }
                  }
                  newBuffs[newBuffCount++] = ab;
                } else {
                  // Create placeholder entry so display count matches
                  ActiveBuff ab = {};
                  ab.instUid = cellUid;
                  ab.source = 2;
                  ab.enhanceCnt = 1;
                  newBuffs[newBuffCount++] = ab;
                }
              }
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
  LeaveCriticalSection(&g_buffLock);

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
}

// ============================================================================
// Overlay Thread - completely separate from game thread
// ============================================================================
DWORD WINAPI OverlayThread(LPVOID) {
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

  // Initial render so the window isn't blank until first timer tick
  RenderOverlay(g_overlayHwnd);

  // Timer for hit-testing + direct render (~33ms = 30Hz polling)
  SetTimer(g_overlayHwnd, 1, 33, NULL);

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
  std::string logPath = d + "plugin\\buff_sniff_log.txt";
  g_logHandle = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);

  Log("=== EndfieldCombatHUD Phase 2d: Lightweight Overlay ===");
  for (int i = 0; i < 120; i++) {
    if (GetModuleHandleW(L"GameAssembly.dll"))
      break;
    Sleep(1000);
  }
  // Wait for IL2CPP to finish initializing exports (retry instead of fixed delay)
  {
    bool resolved = false;
    for (int attempt = 0; attempt < 30; attempt++) {
      Sleep(1000);
      if (Resolve()) {
        resolved = true;
        Log("[OK] IL2CPP resolved after %d seconds", attempt + 1);
        break;
      }
    }
    if (!resolved) {
      Log("[FATAL] IL2CPP resolve failed after 30 retries");
      return 1;
    }
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
    // Find s_attributesToModify static field (maps array index -> AttributeType)
    void* amlIter = nullptr;
    void* amlField;
    while ((amlField = il2cpp_class_get_fields(amlClass, &amlIter))) {
      const char* afname = il2cpp_field_get_name(amlField);
      if (afname && strcmp(afname, "s_attributesToModify") == 0) {
        g_satmField = amlField;
        break;
      }
    }
    Log("[OK] AttributeModifierLoader resolved");
  }


  void *hpClass = FindClass("Beyond.UI", "MainCharHpBar", asms, ac);
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

  // Resolve MainCharHpBar.buffNode field offset dynamically
  if (hpClass) {
    const char *bnNames[] = {"buffNode", "_buffNode", "m_buffNode"};
    const char *bnMatch = nullptr;
    g_offHpBar_buffNode = FindFieldInHierarchy(hpClass, bnNames, 3, &bnMatch);
    if (g_offHpBar_buffNode >= 0)
      Log("[OK] MainCharHpBar.buffNode offset=0x%X (field=%s)", g_offHpBar_buffNode, bnMatch);
    else
      Log("[WARN] MainCharHpBar.buffNode NOT found, display order will use fallback");
  }

  // Resolve UIBuffCell.get_buffInstanceUid for display order tracking
  void *buffCellClass = FindClass("Beyond.UI", "UIBuffCell", asms, ac);
  if (buffCellClass) {
    g_getBuffInstanceUid = FindMethod(buffCellClass, "get_buffInstanceUid", 0);
    Log("[OK] UIBuffCell.get_buffInstanceUid: %p", g_getBuffInstanceUid);
  }

  // Hook GPUIBuffNode._OnBuffEnhanceChanged for stack tracking
  void *gpuiBuffNodeClass = FindClass("Beyond.UI", "GPUIBuffNode", asms, ac);
  if (gpuiBuffNodeClass) {
    if ((m = FindMethod(gpuiBuffNodeClass, "_OnBuffEnhanceChanged", 1)))
      Hook(m, "GpuiEnhance", (void *)hkGpuiEnhance, (void **)&oGpuiEnhance);
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
  }

  // Hook GPUIBuffNode stack methods for independent-instance stacking
  if (gpuiBuffNodeClass) {
    if ((m = FindMethod(gpuiBuffNodeClass, "_AddStackBuffIconInternal", 4)))
      Hook(m, "AddStack", (void *)hkAddStack, (void **)&oAddStack);
    Log("[OK] GPUIBuffNode._AddStackBuffIconInternal hooked: %p", m);
    if ((m = FindMethod(gpuiBuffNodeClass, "_RemoveStackBuffIconInternal", 4)))
      Hook(m, "RemoveStack", (void *)hkRemoveStack, (void **)&oRemoveStack);
    Log("[OK] GPUIBuffNode._RemoveStackBuffIconInternal hooked: %p", m);
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
