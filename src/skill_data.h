#pragma once
// ============================================================================
// skill_data.h — Synchro Skill CD / Ultimate Charge HUD
// Phase 2: Hook SkillButton.PreTick to capture per-character data
// Phase 3: GDI overlay rendering of skill HUD
// ============================================================================
#include <cstdint>
#include <cstring>
#include <windows.h>

// Forward decls from better_buff_bar.cpp / il2cpp_api.h
extern void Log(const char *fmt, ...);

// ============================================================================
// Data structures (shared between game thread and overlay thread)
// ============================================================================
struct SkillSlotStatus {
  float cooldown;      // remaining CD (seconds), 0 = ready
  float maxCooldown;   // total CD duration (seconds)
  bool  ready;         // true if CD <= 0
  bool  valid;         // true if we have data for this slot
  bool  isEmpty;       // true if character slot is empty
};

struct UltSlotStatus {
  float charge;        // current SP value
  float maxCharge;     // max SP value
  float chargePercent; // 0.0 - 1.0
  bool  ready;         // true if fully charged
  bool  hasActualValues; // true if we have cur/max, not just percent
};

struct SkillHudState {
  SkillSlotStatus synchro[4];
  UltSlotStatus   ult[4];
  DWORD lastUpdateTick;  // GetTickCount() at last update
  bool  dataValid;
};

static SkillHudState g_skillHud = {};
static CRITICAL_SECTION g_skillLock;

// ============================================================================
// Resolved IL2CPP method handles
// ============================================================================
static void *g_mSkillGetCdRemaining = nullptr;  // Skill.get_cdRemainingTime
static void *g_mSkillGetCooldown = nullptr;      // Skill.get_cooldown
static void *g_mGetUltimateSp = nullptr;         // entity.get_ultimateSp
static void *g_mGetMaxUltimateSp = nullptr;      // entity.get_maxUltimateSp
static bool g_uspMethodsResolved = false;

// ============================================================================
// SEH-safe memory read helpers
// ============================================================================
static float SReadFloat(void *obj, size_t offset) {
  __try { return *(float *)((char *)obj + offset); }
  __except(1) { return 0.0f; }
}
static void *SReadPtr(void *obj, size_t offset) {
  __try { return *(void **)((char *)obj + offset); }
  __except(1) { return nullptr; }
}
static int32_t SReadInt32(void *obj, size_t offset) {
  __try { return *(int32_t *)((char *)obj + offset); }
  __except(1) { return -1; }
}
static bool SReadBool(void *obj, size_t offset) {
  __try { return *(bool *)((char *)obj + offset); }
  __except(1) { return false; }
}

// Invoke a 0-param method that returns a boxed float
static float SInvokeFloat(void *method, void *obj) {
  if (!method || !obj || !il2cpp_runtime_invoke) return 0.0f;
  __try {
    void *exc = nullptr;
    void *boxed = il2cpp_runtime_invoke(method, obj, nullptr, &exc);
    if (exc || !boxed) return 0.0f;
    // Boxed value type: klass(8) + monitor(8) + data
    return *(float *)((char *)boxed + 0x10);
  } __except(1) { return 0.0f; }
}

// ============================================================================
// SkillButton.PreTick Hook
// ============================================================================
typedef void (*tSkillBtnPreTick)(void *self, void *deltaTime, void *methodInfo);
static tSkillBtnPreTick oSkillBtnPreTick = nullptr;

// Try to lazy-resolve USP methods from the ability system entity class
static void TryResolveUspMethods(void *entityObj) {
  if (g_uspMethodsResolved || !entityObj) return;
  __try {
    void *klass = il2cpp_object_get_class(entityObj);
    if (!klass) return;

    // Walk the class hierarchy looking for get_ultimateSp
    void *searchClass = klass;
    int depth = 0;
    while (searchClass && depth < 10 && !g_mGetUltimateSp) {
      void *iter = nullptr;
      void *method;
      while ((method = il2cpp_class_get_methods(searchClass, &iter))) {
        const char *mname = il2cpp_method_get_name(method);
        if (!mname) continue;
        if (strcmp(mname, "get_ultimateSp") == 0 &&
            il2cpp_method_get_param_count(method) == 0) {
          g_mGetUltimateSp = method;
        }
        else if (strcmp(mname, "get_maxUltimateSp") == 0 &&
                 il2cpp_method_get_param_count(method) == 0) {
          g_mGetMaxUltimateSp = method;
        }
      }
      searchClass = il2cpp_class_get_parent ? il2cpp_class_get_parent(searchClass) : nullptr;
      depth++;
    }

    if (g_mGetUltimateSp && g_mGetMaxUltimateSp) {
      Log("[SkillHUD] Resolved USP methods: get_ultimateSp=%p, get_maxUltimateSp=%p",
          g_mGetUltimateSp, g_mGetMaxUltimateSp);
      g_uspMethodsResolved = true;
    }
  } __except(1) {
    // Failed to resolve - leave unresolved
  }
}

// ============================================================================
// Battle state detection:
// 1. MainCharHpBar.OnShow/OnHide — panel lifecycle (menus, scene changes)
// 2. ComboSkillPanel._OnBattleTeamChanged / OnRelease — actual combat
// HUD shows only when BOTH sceneActive AND combatActive
// ============================================================================

static volatile bool g_sceneActive = false;  // OnShow/OnHide
static volatile bool g_combatActive = false; // _OnBattleTeamChanged/OnRelease
static void *g_mainCharHpBarPtr = nullptr;

// Hook: MainCharHpBar.OnShow — scene HUD appears
typedef void (*tHpBarOnShow)(void *self, void *methodInfo);
static tHpBarOnShow oHpBarOnShow = nullptr;

void hkHpBarOnShow(void *self, void *methodInfo) {
  oHpBarOnShow(self, methodInfo);
  g_mainCharHpBarPtr = self;
  g_sceneActive = true;
}

// Hook: MainCharHpBar.OnHide — scene HUD hidden (menu open etc)
typedef void (*tHpBarOnHide)(void *self, void *methodInfo);
static tHpBarOnHide oHpBarOnHide = nullptr;

void hkHpBarOnHide(void *self, void *methodInfo) {
  oHpBarOnHide(self, methodInfo);
  g_sceneActive = false;
}

// Hook: ComboSkillPanel._OnBattleTeamChanged — entering combat
typedef void (*tComboPanelBattleChanged)(void *self, void *methodInfo);
static tComboPanelBattleChanged oComboPanelBattleChanged = nullptr;

void hkComboPanelBattleChanged(void *self, void *methodInfo) {
  oComboPanelBattleChanged(self, methodInfo);
  g_combatActive = true;
}

// Hook: ComboSkillPanel.OnRelease — leaving combat
typedef void (*tComboPanelRelease)(void *self, void *methodInfo);
static tComboPanelRelease oComboPanelRelease = nullptr;

void hkComboPanelRelease(void *self, void *methodInfo) {
  oComboPanelRelease(self, methodInfo);
  g_combatActive = false;
}

// ============================================================================
// Combo Skill CD reading — find type==6 Skill in m_skills List
// ============================================================================

static void *g_comboSkillObj[4] = {};  // cached Skill* per character
static void *g_comboAbilityPtr[4] = {}; // abilityPtr used when resolving
static SkillSlotStatus g_comboState[4] = {};

// Find the ComboSkill (type==6) from AbilitySystem.m_skills List
static void ResolveComboSkill(void *abilityPtr, int charIdx) {
  if (!abilityPtr || charIdx < 0 || charIdx >= 4) return;
  // Re-resolve if abilityPtr changed (character swap)
  if (g_comboAbilityPtr[charIdx] != abilityPtr) {
    g_comboSkillObj[charIdx] = nullptr;
    g_comboAbilityPtr[charIdx] = nullptr;
  }
  if (g_comboSkillObj[charIdx]) return; // already resolved

  __try {
    void *skillsCol = SReadPtr(abilityPtr, 0x230); // m_skills
    if (!skillsCol) return;
    void *items = SReadPtr(skillsCol, 0x10);
    int32_t count = SReadInt32(skillsCol, 0x18);
    if (!items || count <= 0 || count > 50) return;

    for (int si = 0; si < count && si < 30; si++) {
      void *skill = SReadPtr(items, 0x20 + si * 8);
      if (!skill) continue;
      void *k = il2cpp_object_get_class(skill);
      const char *cn = k ? il2cpp_class_get_name(k) : "";
      if (strcmp(cn, "Skill") != 0) continue;
      int32_t stype = SReadInt32(skill, 0xD8);
      if (stype == 6) { // ComboSkill
        g_comboSkillObj[charIdx] = skill;
        g_comboAbilityPtr[charIdx] = abilityPtr;
        float cdTot = g_mSkillGetCooldown ?
            SInvokeFloat(g_mSkillGetCooldown, skill) : 0.0f;
        Log("[SkillHUD] ComboSkill found for char=%d obj=%p cdTotal=%.1f",
            charIdx, skill, cdTot);
        return;
      }
    }
  } __except(1) {}
}

// Read combo CD every frame
static void ReadComboCd(void *abilityPtr, int charIdx) {
  if (charIdx < 0 || charIdx >= 4) return;

  ResolveComboSkill(abilityPtr, charIdx);

  void *skill = g_comboSkillObj[charIdx];
  if (skill && g_mSkillGetCdRemaining && g_mSkillGetCooldown) {
    float cdRem = SInvokeFloat(g_mSkillGetCdRemaining, skill);
    float cdTot = SInvokeFloat(g_mSkillGetCooldown, skill);
    g_comboState[charIdx].cooldown = cdRem;
    g_comboState[charIdx].maxCooldown = cdTot;
    g_comboState[charIdx].ready = (cdRem <= 0.01f);
    g_comboState[charIdx].valid = true;
    g_comboState[charIdx].isEmpty = false;
  } else {
    g_comboState[charIdx].valid = false;
  }
}

void hkSkillBtnPreTick(void *self, void *deltaTime, void *methodInfo) {
  // Call original first
  oSkillBtnPreTick(self, deltaTime, methodInfo);

  __try {
    // Read character index (int32 at 0x238)
    int32_t charIdx = SReadInt32(self, 0x238);
    if (charIdx < 0 || charIdx >= 4) return;

    // Check if slot is empty (bool at 0x278)
    bool empty = SReadBool(self, 0x278);

    SkillSlotStatus synchro = {};
    UltSlotStatus ult = {};
    synchro.isEmpty = empty;

    if (!empty) {
      // Read ability system pointer (for USP only)
      void *abilityPtr = SReadPtr(self, 0x258); // m_charAbilityPtr

      // --- Combo Skill CD (连携技) ---
      ReadComboCd(abilityPtr, charIdx);
      synchro = g_comboState[charIdx];

      // --- Ultimate SP (终结技充能) ---
      // Read m_uspTargetFill as fallback percentage
      float uspFill = SReadFloat(self, 0x288); // m_uspTargetFill
      ult.chargePercent = uspFill;
      ult.ready = (uspFill >= 0.999f);

      // Try to get actual USP values from ability system (abilityPtr read above)
      if (abilityPtr) {
        // Lazy-resolve USP methods on first valid encounter
        if (!g_uspMethodsResolved) {
          TryResolveUspMethods(abilityPtr);
        }
        if (g_mGetUltimateSp && g_mGetMaxUltimateSp) {
          float curSp = SInvokeFloat(g_mGetUltimateSp, abilityPtr);
          float maxSp = SInvokeFloat(g_mGetMaxUltimateSp, abilityPtr);
          if (maxSp > 0.01f) {
            ult.charge = curSp;
            ult.maxCharge = maxSp;
            ult.chargePercent = curSp / maxSp;
            ult.ready = (curSp >= maxSp - 0.01f);
            ult.hasActualValues = true;
          }
        }
      }
    }

    // Write to shared state (thread-safe)
    EnterCriticalSection(&g_skillLock);
    g_skillHud.synchro[charIdx] = synchro;
    g_skillHud.ult[charIdx] = ult;
    g_skillHud.lastUpdateTick = GetTickCount();
    g_skillHud.dataValid = true;
    LeaveCriticalSection(&g_skillLock);
  } __except(1) {
    // Silently ignore any crash in data capture
  }
}

// ============================================================================
// Init: Resolve classes & methods, install hooks
// Called from MainThread after MH_Initialize
// ============================================================================
static bool InitSkillHooks(void **assemblies, size_t count) {
  InitializeCriticalSection(&g_skillLock);

  // 1. Find SkillButton class and hook PreTick
  void *skillBtnClass = FindClass("Beyond.UI", "SkillButton", assemblies, count);
  if (!skillBtnClass) {
    Log("[SkillHUD] SkillButton class not found");
    return false;
  }
  Log("[SkillHUD] SkillButton class: %p", skillBtnClass);

  void *preTickMethod = FindMethod(skillBtnClass, "PreTick", 1);
  if (!preTickMethod) {
    Log("[SkillHUD] SkillButton.PreTick not found");
    return false;
  }

  Hook(preTickMethod, "SkillBtnPreTick", (void *)hkSkillBtnPreTick,
       (void **)&oSkillBtnPreTick);

  // 2. Find Skill class and resolve CD methods
  void *skillClass = FindClass("Beyond.Gameplay.Core", "Skill", assemblies, count);
  if (skillClass) {
    g_mSkillGetCdRemaining = FindMethod(skillClass, "get_cdRemainingTime", 0);
    g_mSkillGetCooldown = FindMethod(skillClass, "get_cooldown", 0);
    Log("[SkillHUD] Skill.get_cdRemainingTime=%p, get_cooldown=%p",
        g_mSkillGetCdRemaining, g_mSkillGetCooldown);
  } else {
    Log("[SkillHUD] Skill class not found");
  }

  // 3. Hook MainCharHpBar.OnShow / OnHide for battle detection
  void *hpBarClass = FindClass("Beyond.UI", "MainCharHpBar", assemblies, count);
  if (hpBarClass) {
    void *showMethod = FindMethod(hpBarClass, "OnShow", 0);
    if (showMethod)
      Hook(showMethod, "HpBarOnShow", (void *)hkHpBarOnShow,
           (void **)&oHpBarOnShow);
    void *hideMethod = FindMethod(hpBarClass, "OnHide", 0);
    if (hideMethod)
      Hook(hideMethod, "HpBarOnHide", (void *)hkHpBarOnHide,
           (void **)&oHpBarOnHide);
  }

  // 4. Hook ComboSkillPanel for actual combat detection
  void *comboPanelClass = FindClass("Beyond.UI", "ComboSkillPanel", assemblies, count);
  if (comboPanelClass) {
    Log("[SkillHUD] ComboSkillPanel class: %p", comboPanelClass);
    void *btcMethod = FindMethod(comboPanelClass, "_OnBattleTeamChanged", 0);
    if (btcMethod)
      Hook(btcMethod, "ComboPanelBattleChanged", (void *)hkComboPanelBattleChanged,
           (void **)&oComboPanelBattleChanged);
    void *relMethod = FindMethod(comboPanelClass, "OnRelease", 0);
    if (relMethod)
      Hook(relMethod, "ComboPanelRelease", (void *)hkComboPanelRelease,
           (void **)&oComboPanelRelease);
  } else {
    Log("[SkillHUD] ComboSkillPanel not found");
  }

  Log("[SkillHUD] Hooks installed successfully");
  return true;
}

// ============================================================================
// Phase 3: HUD Rendering (called from overlay WM_PAINT on overlay thread)
// ============================================================================

// Layout constants (designed for 1440p, scaled by factor)
static const int SKILL_HUD_BASE_H = 1440;
static const int SKILL_HUD_FONT_SIZE = 16;
static const int SKILL_HUD_BAR_W = 110;
static const int SKILL_HUD_BAR_H = 5;
static const int SKILL_HUD_SLOT_H = 38;
static const int SKILL_HUD_TITLE_H = 22;

// Cached fonts
static HFONT s_skillFont = nullptr;
static int   s_skillLastH = 0;

static void DrawSkillHud(HDC hdc, HWND hwnd) {
  // Check if data is valid and recent (within 3 seconds)
  SkillHudState local;
  EnterCriticalSection(&g_skillLock);
  local = g_skillHud;
  LeaveCriticalSection(&g_skillLock);

  if (!local.dataValid) return;

  // Only show HUD when scene is active
  if (!g_sceneActive) return;
  // Check if the bottom UI is visible via CanvasGroup alpha
  if (g_mainCharHpBarPtr) {
    void *fadeCtrl = SReadPtr(g_mainCharHpBarPtr, 0x198);
    if (fadeCtrl) {
      static void *s_cgGetAlpha = nullptr;
      static bool s_resolved = false;
      if (!s_resolved) {
        s_resolved = true;
        void *cgObj = SReadPtr(fadeCtrl, 0x20);
        if (cgObj) {
          void *k = il2cpp_object_get_class(cgObj);
          if (k) s_cgGetAlpha = FindMethod(k, "get_alpha", 0);
        }
      }
      if (s_cgGetAlpha) {
        void *cgObj = SReadPtr(fadeCtrl, 0x20);
        if (cgObj) {
          float alpha = SInvokeFloat(s_cgGetAlpha, cgObj);
          if (alpha < 0.1f) return; // UI faded out
        }
      }
    }
  }

  // Get window dimensions
  RECT wr;
  GetClientRect(hwnd, &wr);
  int screenW = wr.right;
  int screenH = wr.bottom;
  if (screenW < 100 || screenH < 100) return;

  float scale = (float)screenH / (float)SKILL_HUD_BASE_H;

  // Recreate font if scale changed
  if (s_skillLastH != screenH) {
    if (s_skillFont) DeleteObject(s_skillFont);
    s_skillFont = CreateFontW(
        -(int)(19 * scale), 0, 0, 0,
        FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    s_skillLastH = screenH;
  }

  SetBkMode(hdc, TRANSPARENT);
  HFONT oldFont = (HFONT)SelectObject(hdc, s_skillFont);

  // Helper: draw text with black outline for readability
  auto DrawOutlinedText = [&](HDC h, const wchar_t *text, RECT *rc, UINT fmt, COLORREF color) {
    int outPx = (int)(2.5f * scale);
    if (outPx < 2) outPx = 2;
    SetTextColor(h, RGB(0, 0, 0));
    int offsets[][2] = {{-outPx,0},{outPx,0},{0,-outPx},{0,outPx},{-outPx,-outPx},{outPx,-outPx},{-outPx,outPx},{outPx,outPx}};
    for (auto &o : offsets) {
      RECT orc = {rc->left + o[0], rc->top + o[1], rc->right + o[0], rc->bottom + o[1]};
      DrawTextW(h, text, -1, &orc, fmt);
    }
    SetTextColor(h, color);
    DrawTextW(h, text, -1, rc, fmt);
  };

  int barW = (int)(120 * scale);
  int barH = (int)(5 * scale);
  int textH = (int)(19 * scale);
  int slotGap = (int)(22 * scale);  // larger gap between slots
  int slotH = textH + barH + slotGap;

  // Center position — moved down
  int centerX = screenW / 2;
  int centerY = (int)(screenH * 0.49f);

  // Bracket curve offsets
  int curveOff[] = {
    (int)(15 * scale),  // slot 0: slight indent
    0,                  // slot 1: outermost
    0,                  // slot 2: outermost
    (int)(15 * scale)   // slot 3: slight indent
  };

  int blockGap = (int)(150 * scale); // larger gap from center

  // Count valid slots to center vertically
  int validCount = 0;
  for (int i = 0; i < 4; i++) {
    if (!local.synchro[i].isEmpty) validCount++;
  }
  if (validCount == 0) { SelectObject(hdc, oldFont); return; }

  int totalH = validCount * slotH - slotGap;
  int startY = centerY - totalH / 2;

  // =========== LEFT BRACKET (  : 连携技 CD ===========
  {
    int slotIdx = 0;
    for (int i = 0; i < 4; i++) {
      const SkillSlotStatus &s = local.synchro[i];
      if (s.isEmpty || !s.valid) continue;

      int y = startY + slotIdx * slotH;
      int x = centerX - blockGap - barW + curveOff[slotIdx];

      // Text: "ready" or "12.3s" — centered above bar
      wchar_t buf[64];
      if (s.ready) {
        wsprintfW(buf, L"ready");
        SetTextColor(hdc, RGB(220, 220, 220));
      } else {
        int cdSec = (int)(s.cooldown * 10);
        wsprintfW(buf, L"%d.%ds", cdSec / 10, cdSec % 10);
        SetTextColor(hdc, RGB(220, 220, 220));
      }
      COLORREF clr = s.ready ? RGB(220, 220, 220) : RGB(220, 220, 220);
      RECT lr = {x, y, x + barW, y + textH};
      DrawOutlinedText(hdc, buf, &lr, DT_SINGLELINE | DT_CENTER, clr);

      // Progress bar
      y += textH;
      RECT barBg = {x, y, x + barW, y + barH};
      HBRUSH bgBr = CreateSolidBrush(RGB(35, 38, 45));
      FillRect(hdc, &barBg, bgBr);
      DeleteObject(bgBr);

      float progress = 1.0f;
      if (s.maxCooldown > 0.01f) {
        progress = 1.0f - (s.cooldown / s.maxCooldown);
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
      }
      int fillW = (int)(barW * progress);
      if (fillW > 0) {
        RECT barFill = {x + barW - fillW, y, x + barW, y + barH};
        HBRUSH fillBr = CreateSolidBrush(
            s.ready ? RGB(220, 220, 220) : RGB(80, 160, 240));
        FillRect(hdc, &barFill, fillBr);
        DeleteObject(fillBr);
      }

      slotIdx++;
    }
  }

  // =========== RIGHT BRACKET )  : 终结技充能 ===========
  {
    int slotIdx = 0;
    for (int i = 0; i < 4; i++) {
      const UltSlotStatus &u = local.ult[i];
      const SkillSlotStatus &s = local.synchro[i];
      if (s.isEmpty) continue;

      int y = startY + slotIdx * slotH;
      int x = centerX + blockGap - curveOff[slotIdx];

      // Text: "ready" or "5/10 50%" — centered above bar
      wchar_t buf[64];
      if (u.ready) {
        wsprintfW(buf, L"ready");
        SetTextColor(hdc, RGB(220, 220, 220));
      } else if (u.hasActualValues && u.maxCharge > 0.01f) {
        int cur = (int)u.charge;
        int max = (int)u.maxCharge;
        int pct = (int)(u.chargePercent * 100.0f);
        wsprintfW(buf, L"%d/%d %d%%", cur, max, pct);
        SetTextColor(hdc, RGB(220, 220, 220));
      } else {
        int pct = (int)(u.chargePercent * 100.0f);
        wsprintfW(buf, L"%d%%", pct);
        SetTextColor(hdc, RGB(220, 220, 220));
      }
      COLORREF clr = u.ready ? RGB(220, 220, 220) : RGB(220, 220, 220);
      RECT lr = {x, y, x + barW, y + textH};
      DrawOutlinedText(hdc, buf, &lr, DT_SINGLELINE | DT_CENTER, clr);

      // Progress bar
      y += textH;
      RECT barBg = {x, y, x + barW, y + barH};
      HBRUSH bgBr = CreateSolidBrush(RGB(35, 38, 45));
      FillRect(hdc, &barBg, bgBr);
      DeleteObject(bgBr);

      float progress = u.chargePercent;
      if (progress < 0.0f) progress = 0.0f;
      if (progress > 1.0f) progress = 1.0f;
      int fillW = (int)(barW * progress);
      if (fillW > 0) {
        RECT barFill = {x, y, x + fillW, y + barH};
        HBRUSH fillBr = CreateSolidBrush(
            u.ready ? RGB(220, 220, 220) : RGB(240, 200, 60));
        FillRect(hdc, &barFill, fillBr);
        DeleteObject(fillBr);
      }

      slotIdx++;
    }
  }

  SelectObject(hdc, oldFont);
}

// ============================================================================
// Phase 1: Discovery dump functions (kept for reference, can be removed later)
// ============================================================================
// [SEH-safe wrappers - unchanged from previous version]
static void *SafeGetImage(void *assembly) {
  __try { return il2cpp_assembly_get_image(assembly); }
  __except(1) { return nullptr; }
}
static const char *SafeGetImageName(void *img) {
  __try { return il2cpp_image_get_name ? il2cpp_image_get_name(img) : "?"; }
  __except(1) { return "?"; }
}
static size_t SafeGetClassCount(void *img) {
  __try { return il2cpp_image_get_class_count(img); }
  __except(1) { return 0; }
}
static void *SafeGetClass(void *img, size_t idx) {
  __try { return il2cpp_image_get_class(img, idx); }
  __except(1) { return nullptr; }
}
static const char *SafeGetClassName(void *klass) {
  __try { return il2cpp_class_get_name(klass); }
  __except(1) { return nullptr; }
}
static const char *SafeGetClassNamespace(void *klass) {
  __try { return il2cpp_class_get_namespace ? il2cpp_class_get_namespace(klass) : ""; }
  __except(1) { return ""; }
}
static void *SafeGetParent(void *klass) {
  __try { return il2cpp_class_get_parent ? il2cpp_class_get_parent(klass) : nullptr; }
  __except(1) { return nullptr; }
}
static void *SafeGetFields(void *klass, void **iter) {
  __try { return il2cpp_class_get_fields(klass, iter); }
  __except(1) { return nullptr; }
}
static const char *SafeGetFieldName(void *field) {
  __try { return il2cpp_field_get_name(field); }
  __except(1) { return "?"; }
}
static size_t SafeGetFieldOffset(void *field) {
  __try { return il2cpp_field_get_offset(field); }
  __except(1) { return 0; }
}
static int SafeGetFieldFlags(void *field) {
  __try { return il2cpp_field_get_flags ? il2cpp_field_get_flags(field) : 0; }
  __except(1) { return 0; }
}
static void *SafeGetMethods(void *klass, void **iter) {
  __try { return il2cpp_class_get_methods(klass, iter); }
  __except(1) { return nullptr; }
}
static const char *SafeGetMethodName(void *method) {
  __try { return il2cpp_method_get_name(method); }
  __except(1) { return "?"; }
}
static uint32_t SafeGetMethodParamCount(void *method) {
  __try { return il2cpp_method_get_param_count(method); }
  __except(1) { return 0; }
}

static bool IsSkillRelatedName(const char *name, const char *ns) {
  if (!name) return false;
  auto containsCI = [](const char *haystack, const char *needle) -> bool {
    if (!haystack || !needle) return false;
    int hlen = (int)strlen(haystack);
    int nlen = (int)strlen(needle);
    if (nlen > hlen) return false;
    for (int i = 0; i <= hlen - nlen; i++) {
      bool match = true;
      for (int j = 0; j < nlen; j++) {
        char h = haystack[i + j]; char n = needle[j];
        if (h >= 'A' && h <= 'Z') h += 32;
        if (n >= 'A' && n <= 'Z') n += 32;
        if (h != n) { match = false; break; }
      }
      if (match) return true;
    }
    return false;
  };
  static const char *keywords[] = {
    "skill", "ultimate", "burst", "synchro", "combo",
    "cooldown", "energy", "charge", "gauge",
    "charslot", "teamslot", "HpBar", "ability",
  };
  for (auto kw : keywords) {
    if (containsCI(name, kw)) return true;
    if (ns && containsCI(ns, kw)) return true;
  }
  return false;
}

static void DumpClassFields(void *klass, const char *className) {
  if (!klass || !il2cpp_class_get_fields || !il2cpp_field_get_name) return;
  void *iter = nullptr; void *field; int count = 0;
  while ((field = SafeGetFields(klass, &iter)) != nullptr) {
    const char *fname = SafeGetFieldName(field);
    size_t foff = SafeGetFieldOffset(field);
    int flags = SafeGetFieldFlags(field);
    bool isStatic = (flags & 0x10) != 0;
    Log("    [0x%X] %s%s", (int)foff, fname ? fname : "?", isStatic ? " (static)" : "");
    count++;
    if (count > 200) { Log("    ... (truncated)"); break; }
  }
  if (count == 0) Log("    (no fields)");
}

static void DumpClassMethods(void *klass, const char *className) {
  if (!klass || !il2cpp_class_get_methods || !il2cpp_method_get_name) return;
  void *iter = nullptr; void *method; int count = 0;
  while ((method = SafeGetMethods(klass, &iter)) != nullptr) {
    const char *mname = SafeGetMethodName(method);
    uint32_t pc = SafeGetMethodParamCount(method);
    void *mp = nullptr;
    __try { mp = ((MInfo *)method)->mp; } __except(1) {}
    Log("    %s(%d params) ptr=%p", mname ? mname : "?", (int)pc, mp);
    count++;
    if (count > 200) { Log("    ... (truncated)"); break; }
  }
  if (count == 0) Log("    (no methods)");
}

static void DumpClassHierarchy(void *klass) {
  if (!klass) return;
  void *parent = SafeGetParent(klass); int depth = 0;
  while (parent && depth < 10) {
    const char *pname = SafeGetClassName(parent);
    const char *pns = SafeGetClassNamespace(parent);
    Log("    parent[%d]: %s.%s", depth, pns ? pns : "", pname ? pname : "?");
    parent = SafeGetParent(parent); depth++;
  }
}

static void DumpSkillRelatedClasses(void **assemblies, size_t assemblyCount) {
  Log("========== SKILL/ULT CLASS DISCOVERY DUMP ==========");
  Log("Scanning %zu assemblies for skill-related classes...", assemblyCount);
  int totalClasses = 0, matchedClasses = 0;
  for (size_t ai = 0; ai < assemblyCount; ai++) {
    void *img = SafeGetImage(assemblies[ai]);
    if (!img) continue;
    const char *imgName = SafeGetImageName(img);
    size_t classCount = SafeGetClassCount(img);
    if (classCount == 0 || classCount > 100000) continue;
    Log("  [Assembly %zu] %s: %zu classes", ai, imgName ? imgName : "?", classCount);
    for (size_t ci = 0; ci < classCount; ci++) {
      void *klass = SafeGetClass(img, ci);
      if (!klass) continue;
      const char *cname = SafeGetClassName(klass);
      const char *cns = SafeGetClassNamespace(klass);
      if (!cname) continue;
      totalClasses++;
      if (IsSkillRelatedName(cname, cns)) {
        matchedClasses++;
        Log("--------------------------------------");
        Log("CLASS: %s.%s  (image: %s)", cns ? cns : "", cname, imgName ? imgName : "?");
        Log("  [Hierarchy]"); DumpClassHierarchy(klass);
        Log("  [Fields]"); DumpClassFields(klass, cname);
        Log("  [Methods]"); DumpClassMethods(klass, cname);
      }
    }
  }
  Log("DISCOVERY SUMMARY: %d/%d classes matched", matchedClasses, totalClasses);
  Log("========== END SKILL/ULT DISCOVERY ==========");
}

static void DumpMainCharHpBarFull(void *hpClass) {
  if (!hpClass) return;
  Log("========== MainCharHpBar FULL DUMP ==========");
  const char *cname = SafeGetClassName(hpClass);
  Log("CLASS: %s", cname ? cname : "?");
  Log("  [Hierarchy]"); DumpClassHierarchy(hpClass);
  Log("  [ALL Fields]"); DumpClassFields(hpClass, "MainCharHpBar");
  if (il2cpp_class_get_parent) {
    void *parent = SafeGetParent(hpClass); int depth = 0;
    while (parent && depth < 5) {
      const char *pname = SafeGetClassName(parent);
      Log("  [Parent Fields: %s]", pname ? pname : "?");
      DumpClassFields(parent, pname ? pname : "?");
      parent = SafeGetParent(parent); depth++;
    }
  }
  Log("  [ALL Methods]"); DumpClassMethods(hpClass, "MainCharHpBar");
  Log("========== END MainCharHpBar DUMP ==========");
}

static void DumpBeyondUIClasses(void **assemblies, size_t assemblyCount) {
  Log("========== Beyond.UI TARGETED DUMP ==========");
  for (size_t ai = 0; ai < assemblyCount; ai++) {
    void *img = SafeGetImage(assemblies[ai]);
    if (!img) continue;
    size_t classCount = SafeGetClassCount(img);
    if (classCount == 0 || classCount > 100000) continue;
    for (size_t ci = 0; ci < classCount; ci++) {
      void *klass = SafeGetClass(img, ci);
      if (!klass) continue;
      const char *cns = SafeGetClassNamespace(klass);
      if (!cns || strcmp(cns, "Beyond.UI") != 0) continue;
      const char *cname = SafeGetClassName(klass);
      if (!cname) continue;
      Log("  Beyond.UI.%s", cname);
    }
  }
  Log("========== END Beyond.UI DUMP ==========");
}

static void DumpBeyondGameplayClasses(void **assemblies, size_t assemblyCount) {
  Log("========== Beyond.Gameplay TARGETED DUMP ==========");
  for (size_t ai = 0; ai < assemblyCount; ai++) {
    void *img = SafeGetImage(assemblies[ai]);
    if (!img) continue;
    size_t classCount = SafeGetClassCount(img);
    if (classCount == 0 || classCount > 100000) continue;
    for (size_t ci = 0; ci < classCount; ci++) {
      void *klass = SafeGetClass(img, ci);
      if (!klass) continue;
      const char *cns = SafeGetClassNamespace(klass);
      if (!cns) continue;
      if (strncmp(cns, "Beyond.Gameplay", 15) != 0) continue;
      const char *cname = SafeGetClassName(klass);
      if (!cname) continue;
      Log("  %s.%s", cns, cname);
    }
  }
  Log("========== END Beyond.Gameplay DUMP ==========");
}
