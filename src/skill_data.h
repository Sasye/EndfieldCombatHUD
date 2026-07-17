#pragma once
// ============================================================================
// skill_data.h — Synchro Skill CD / Ultimate Charge HUD
// Phase 2: Hook SkillButton.PreTick to capture per-character data
// Phase 3: GDI overlay rendering of skill HUD
// ============================================================================
#include <cstdint>
#include <cstring>
#include <windows.h>

// Game window dimensions and overlay offset (set by timer handler in combat_hud.cpp)
static int g_gameW = 0, g_gameH = 0;
static int g_overlayOX = 0, g_overlayOY = 0;

// Forward decls from combat_hud.cpp / il2cpp_api.h
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

static SkillHudState MakeEmptyHudState() {
  SkillHudState s = {};
  for (int i = 0; i < 4; i++) s.synchro[i].isEmpty = true;
  return s;
}

static SkillHudState g_skillHud = MakeEmptyHudState();
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
// Dynamically resolved field offsets (set by InitSkillHooks)
// ============================================================================
static int g_offSB_charIndex = -1;      // SkillButton.m_charIndex
static int g_offSB_isEmpty = -1;        // SkillButton.m_isEmpty
static int g_offSB_abilityPtr = -1;     // SkillButton.m_charAbilityPtr
static int g_offSB_uspFill = -1;        // SkillButton.m_uspTargetFill
static int g_offAS_skills = -1;         // AbilitySystem.m_skills
static int g_offSkill_type = -1;        // Skill.<skillType>k__BackingField
static int g_offHpBar_fadeCtrl = -1;    // MainCharHpBar fade controller (resolved at runtime)
static int g_offFade_cg = -1;           // FadeCtrl -> CanvasGroup (resolved at runtime)
static int g_offHpBar_buffNode = -1;    // MainCharHpBar.buffNode (resolved at runtime)

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

// Invoke a 0-param method that returns a boxed float (SLOW — uses il2cpp_runtime_invoke)
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

// Direct call for 0-param float-returning methods (FAST — no invoke overhead)
// Uses MInfo->mp native pointer directly, bypassing boxing/unboxing/GC.
static float SDirectFloat(void *method, void *obj) {
  if (!method || !obj) return 0.0f;
  void *fn = ((MInfo *)method)->mp;
  if (!fn) return 0.0f;
  __try {
    typedef float (*tFn)(void *, void *);
    return ((tFn)fn)(obj, method);
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
static volatile float g_cgAlpha = 1.0f;      // CanvasGroup alpha (written by game thread)
static volatile bool g_visibilityChanged = false; // Set by hooks, consumed by timer
static void *g_mainCharHpBarPtr = nullptr;

// CanvasGroup alpha: method pointer cache (re-resolved when HpBar instance changes)
static void *s_cgGetAlpha = nullptr;
static bool  s_cgResolved = false;

// Hook: MainCharHpBar.OnShow — scene HUD appears
typedef void (*tHpBarOnShow)(void *self, void *methodInfo);
static tHpBarOnShow oHpBarOnShow = nullptr;

void hkHpBarOnShow(void *self, void *methodInfo) {
  oHpBarOnShow(self, methodInfo);
  // Always invalidate CG cache — CanvasGroup object changes when UI is recreated
  if (g_mainCharHpBarPtr != self) {
    s_cgResolved = false;
    s_cgGetAlpha = nullptr;
  }
  g_mainCharHpBarPtr = self;
  g_sceneActive = true;
  g_cgAlpha = 1.0f;
  g_visibilityChanged = true;
}

// Hook: MainCharHpBar.OnHide — scene HUD hidden (menu open etc)
typedef void (*tHpBarOnHide)(void *self, void *methodInfo);
static tHpBarOnHide oHpBarOnHide = nullptr;

void hkHpBarOnHide(void *self, void *methodInfo) {
  oHpBarOnHide(self, methodInfo);
  g_sceneActive = false;
  g_cgAlpha = 0.0f;
  g_visibilityChanged = true;
}

// Hook: ComboSkillPanel._OnBattleTeamChanged — entering combat
typedef void (*tComboPanelBattleChanged)(void *self, void *methodInfo);
static tComboPanelBattleChanged oComboPanelBattleChanged = nullptr;

// Forward-declare combo skill cache (used by hooks below, defined in CD reading section)
static void *g_comboSkillObj[4];
static void *g_comboAbilityPtr[4];
static SkillSlotStatus g_comboState[4];

void hkComboPanelBattleChanged(void *self, void *methodInfo) {
  oComboPanelBattleChanged(self, methodInfo);
  // Invalidate all cached skill pointers — handles dungeon restart
  // where game destroys and recreates all ability system objects
  for (int i = 0; i < 4; i++) {
    g_comboSkillObj[i] = nullptr;
    g_comboAbilityPtr[i] = nullptr;
    g_comboState[i] = {};
  }
  // Reset CG alpha cache so it re-resolves with new UI objects
  s_cgResolved = false;
  s_cgGetAlpha = nullptr;
  // Clear HUD data so stale values don't persist (all slots default to empty)
  EnterCriticalSection(&g_skillLock);
  g_skillHud = MakeEmptyHudState();
  LeaveCriticalSection(&g_skillLock);
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

// (combo cache variables declared above, before hooks)
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
    if (g_offAS_skills < 0) return;
    void *skillsCol = SReadPtr(abilityPtr, g_offAS_skills);
    if (!skillsCol) return;
    void *items = SReadPtr(skillsCol, 0x10); // List._items (fixed Unity layout)
    int32_t count = SReadInt32(skillsCol, 0x18); // List._size (fixed)
    if (!items || count <= 0 || count > 50) return;

    for (int si = 0; si < count && si < 30; si++) {
      void *skill = SReadPtr(items, 0x20 + si * 8); // Array data offset (fixed)
      if (!skill) continue;
      void *k = il2cpp_object_get_class(skill);
      const char *cn = k ? il2cpp_class_get_name(k) : "";
      if (strcmp(cn, "Skill") != 0) continue;
      int32_t stype = (g_offSkill_type >= 0) ? SReadInt32(skill, g_offSkill_type) : -1;
      if (stype == 6) { // ComboSkill
        g_comboSkillObj[charIdx] = skill;
        g_comboAbilityPtr[charIdx] = abilityPtr;
        float cdTot = g_mSkillGetCooldown ?
            SInvokeFloat(g_mSkillGetCooldown, skill) : 0.0f;
        static void *s_lastLoggedSkill[4] = {};
        if (s_lastLoggedSkill[charIdx] != skill) {
          s_lastLoggedSkill[charIdx] = skill;
          Log("[SkillHUD] ComboSkill found for char=%d obj=%p cdTotal=%.1f",
              charIdx, skill, cdTot);
        }
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
  // Call original first — always, for game stability
  oSkillBtnPreTick(self, deltaTime, methodInfo);

  __try {
    if (g_offSB_charIndex < 0) return; // offsets not resolved yet
    int32_t charIdx = SReadInt32(self, g_offSB_charIndex);
    if (charIdx < 0 || charIdx >= 4) return;

    // Throttle: only update HUD data ~30fps per character slot.
    // At 200fps this reduces il2cpp_runtime_invoke calls from 3200/s to ~480/s.
    static DWORD s_lastUpdate[4] = {};
    DWORD now = GetTickCount();
    if (now - s_lastUpdate[charIdx] < 33) return;
    s_lastUpdate[charIdx] = now;

    bool empty = (g_offSB_isEmpty >= 0) ? SReadBool(self, g_offSB_isEmpty) : false;

    SkillSlotStatus synchro = {};
    UltSlotStatus ult = {};
    synchro.isEmpty = empty;

    if (!empty) {
      void *abilityPtr = (g_offSB_abilityPtr >= 0) ? SReadPtr(self, g_offSB_abilityPtr) : nullptr;

      // --- Combo Skill CD (连携技) ---
      ReadComboCd(abilityPtr, charIdx);
      synchro = g_comboState[charIdx];

      // --- Ultimate SP (终结技充能) ---
      float uspFill = (g_offSB_uspFill >= 0) ? SReadFloat(self, g_offSB_uspFill) : 0.0f;
      ult.chargePercent = uspFill;
      ult.ready = (uspFill >= 0.999f);

      // Try to get actual USP values from ability system
      if (abilityPtr) {
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

  // 2. Resolve SkillButton field offsets dynamically
  g_offSB_charIndex = FindField(skillBtnClass, "m_charIndex");
  g_offSB_isEmpty = FindField(skillBtnClass, "m_isEmpty");
  g_offSB_abilityPtr = FindField(skillBtnClass, "m_charAbilityPtr");
  g_offSB_uspFill = FindField(skillBtnClass, "m_uspTargetFill");
  Log("[SkillHUD] SkillButton offsets: charIndex=0x%X isEmpty=0x%X abilityPtr=0x%X uspFill=0x%X",
      g_offSB_charIndex, g_offSB_isEmpty, g_offSB_abilityPtr, g_offSB_uspFill);
  if (g_offSB_charIndex < 0 || g_offSB_isEmpty < 0 || g_offSB_abilityPtr < 0) {
    Log("[WARN] SkillButton field resolution failed, dumping hierarchy:");
    DumpFieldsHierarchy(skillBtnClass);
  }

  // 3. Find Skill class and resolve CD methods + type offset
  void *skillClass = FindClass("Beyond.Gameplay.Core", "Skill", assemblies, count);
  if (skillClass) {
    g_mSkillGetCdRemaining = FindMethod(skillClass, "get_cdRemainingTime", 0);
    g_mSkillGetCooldown = FindMethod(skillClass, "get_cooldown", 0);
    g_offSkill_type = FindField(skillClass, "<skillType>k__BackingField");
    Log("[SkillHUD] Skill.get_cdRemainingTime=%p, get_cooldown=%p, typeOffset=0x%X",
        g_mSkillGetCdRemaining, g_mSkillGetCooldown, g_offSkill_type);
  } else {
    Log("[SkillHUD] Skill class not found");
  }

  // 4. Find AbilitySystem and resolve m_skills offset
  void *abilitySysClass = FindClass("Beyond.Gameplay.Core", "AbilitySystem", assemblies, count);
  if (abilitySysClass) {
    g_offAS_skills = FindField(abilitySysClass, "m_skills");
    Log("[SkillHUD] AbilitySystem.m_skills offset=0x%X", g_offAS_skills);
  } else {
    Log("[SkillHUD] AbilitySystem class not found");
  }

  // 5. Hook MainCharHpBar.OnShow / OnHide for battle detection
  void *hpBarClass = FindClass("Beyond.UI", "MainCharHpBar", assemblies, count);
  if (hpBarClass) {
    // Resolve fade controller field (walk hierarchy for inherited fields)
    // The field name may be m_fadeCtrl, _fadeCtrl, m_canvasGroupCtrl, etc.
    const char *fadeNames[] = {"m_hudFadeController", "m_fadeCtrl", "_fadeCtrl",
                               "m_bottomFadeCtrl", "m_canvasGroupCtrl", "_canvasGroupCtrl"};
    const char *matchedName = nullptr;
    g_offHpBar_fadeCtrl = FindFieldInHierarchy(hpBarClass, fadeNames, 6, &matchedName);
    if (g_offHpBar_fadeCtrl >= 0) {
      Log("[SkillHUD] HpBar fadeCtrl offset=0x%X (field=%s)", g_offHpBar_fadeCtrl, matchedName);
      // Also resolve FadeController -> CanvasGroup
      void *fadeCtrlClass = FindClass("Beyond.UI", "UIHudFadeController", assemblies, count);
      if (fadeCtrlClass) {
        g_offFade_cg = FindField(fadeCtrlClass, "m_canvasGroup");
        Log("[SkillHUD] FadeCtrl.m_canvasGroup offset=0x%X", g_offFade_cg);
      }
    } else {
      Log("[WARN] HpBar fadeCtrl not found by name, dumping hierarchy:");
      DumpFieldsHierarchy(hpBarClass);
    }
    void *showMethod = FindMethod(hpBarClass, "OnShow", 0);
    if (showMethod)
      Hook(showMethod, "HpBarOnShow", (void *)hkHpBarOnShow,
           (void **)&oHpBarOnShow);
    void *hideMethod = FindMethod(hpBarClass, "OnHide", 0);
    if (hideMethod)
      Hook(hideMethod, "HpBarOnHide", (void *)hkHpBarOnHide,
           (void **)&oHpBarOnHide);
  }

  // 6. Hook ComboSkillPanel for actual combat detection
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

  // Only show HUD when scene is active and CanvasGroup alpha is sufficient
  // (g_cgAlpha is updated by hkLateTick on the game thread — thread-safe read)
  if (!g_sceneActive) return;
  if (g_cgAlpha < 0.1f) return;

  // Use game window dimensions for layout (overlay may be smaller due to content-tight sizing)
  // Coordinate mapping via SetWindowOrgEx in RenderOverlay handles the offset.
  int screenW = g_gameW;
  int screenH = g_gameH;
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
      COLORREF clr = RGB(220, 220, 220);
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
      COLORREF clr = RGB(220, 220, 220);
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

