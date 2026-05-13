#pragma once
// ============================================================================
// Buff Data Structures and Reading Logic
// ============================================================================
#include <cstdint>
#include <windows.h>

// Forward decls from il2cpp_api.h
extern void *Invoke(void *method, void *obj, void **params);
extern int ReadStr(void *s, char *b, int sz);

// ============================================================================
// Method handles (resolved in InitThread)
// ============================================================================
static void *g_getId = nullptr, *g_getDuration = nullptr,
            *g_getLifeTime = nullptr;
static void *g_getEnhanceCnt = nullptr;
static void *g_getAttrAdd = nullptr, *g_getAttrMul = nullptr;
static size_t g_loaderOffset = 0;

// Index mapping for s_attributesToModify
static int32_t g_satmMap[96] = {0};
static int g_satmLen = 0;
static bool g_satmLoaded = false;
static void* g_satmField = nullptr;

// ============================================================================
// Data Structures
// ============================================================================
struct AttrMod {
  float add[96];
  float baseAdd[96];
  float finalAdd[96];
  float baseFinalAdd[96];
  float mul[96];
  float baseMul[96];
  float finalScl[96];
  float baseFinalScl[96];
};

struct BlackboardEntry {
  char key[64];
  double value;
};

struct ActiveBuff {
  void *buffObj;
  uint32_t instUid;
  char id[128];
  float duration;
  float lifeTime;
  int enhanceCnt;
  int trueEnhanceCnt;
  int source; // 0 = ProgressBar (buff), 1 = BuffIcon (debuff)

  AttrMod attrs;
  BlackboardEntry bb[8];
  int bbCount;
  char iconName[128];
};

// ============================================================================
// Global buff state
// ============================================================================
static ActiveBuff g_buffs[64];
static int g_buffCount = 0;
static CRITICAL_SECTION g_buffLock;

// Display order: UIDs of buffs in HUD display order
static uint32_t g_displayUids[64];
static int g_displayCount = 0;

// UIBuffNode resolution
static void *g_getBuffInstanceUid = nullptr;
static size_t g_orderedListOffset = 0;

// ============================================================================
// Unboxing helpers
// ============================================================================
static float UnboxFloat(void *boxed) {
  __try {
    return boxed ? *(float *)((char *)boxed + 16) : 0.0f;
  } __except (1) {
    return 0.0f;
  }
}

static int32_t UnboxInt(void *boxed) {
  __try {
    return boxed ? *(int32_t *)((char *)boxed + 16) : 0;
  } __except (1) {
    return 0;
  }
}

// ============================================================================
// Array reading
// ============================================================================
static void ReadDoubleArray(void *arrayObj, float *outArray, int maxLen) {
  __try {
    if (!arrayObj)
      return;
    int32_t len = *(int32_t *)((char *)arrayObj + 24);
    if (len <= 0 || len > maxLen)
      return;
    double *data = (double *)((char *)arrayObj + 32);
    for (int i = 0; i < len; i++)
      outArray[i] = (float)data[i];
  } __except (1) {
  }
}

// ============================================================================
// ReadBuffData — reads all buff fields from a game buff object
// ============================================================================
static void ReadBuffData(void *objPtr, ActiveBuff *out) {
  __try {
    out->buffObj = *(void **)objPtr;
    out->instUid = *(uint32_t *)((char *)objPtr + 8);
    out->id[0] = 0;
    out->iconName[0] = 0;
    out->duration = 0;
    out->lifeTime = 0;
    out->enhanceCnt = 0;
    out->trueEnhanceCnt = 0;

    out->bbCount = 0;
    memset(&out->attrs, 0, sizeof(out->attrs));

    if (!out->buffObj)
      return;
    void *r;
    if (g_getId && (r = Invoke(g_getId, out->buffObj)))
      ReadStr(r, out->id, sizeof(out->id));
    if (g_getDuration)
      out->duration = UnboxFloat(Invoke(g_getDuration, out->buffObj));
    if (g_getLifeTime)
      out->lifeTime = UnboxFloat(Invoke(g_getLifeTime, out->buffObj));
    if (g_getEnhanceCnt) {
      void *boxedCnt = Invoke(g_getEnhanceCnt, out->buffObj);
      if (boxedCnt) {
        out->enhanceCnt = *(int *)((char *)boxedCnt + 16);
        out->trueEnhanceCnt = out->enhanceCnt;
      }
    }

    if (g_loaderOffset > 0) {
      void *loader = *(void **)((char *)out->buffObj + g_loaderOffset);
      if (loader) {
        auto readArr = [](void *loader, int fieldOff, float *dst, int max) {
          void *arr = *(void **)((char *)loader + fieldOff);
          if (!arr) return;
          int32_t len = *(int32_t *)((char *)arr + 0x18);
          if (len <= 0 || len > max) return;
          double *data = (double *)((char *)arr + 0x20);
          for (int j = 0; j < len && j < max; j++)
            dst[j] = (float)data[j];
        };
        readArr(loader, 0x18, out->attrs.add, 96);
        readArr(loader, 0x38, out->attrs.baseAdd, 96);
        readArr(loader, 0x20, out->attrs.finalAdd, 96);
        readArr(loader, 0x40, out->attrs.baseFinalAdd, 96);
        readArr(loader, 0x10, out->attrs.mul, 96);
        readArr(loader, 0x30, out->attrs.baseMul, 96);
        readArr(loader, 0x28, out->attrs.finalScl, 96);
        readArr(loader, 0x48, out->attrs.baseFinalScl, 96);
      }
    }

    // Read blackboard DataPair[] from +0x158
    void *bb = *(void **)((char *)out->buffObj + 0x158);
    if (bb) {
      void *dpArr = *(void **)((char *)bb + 0x40);
      if (dpArr) {
        int32_t dpLen = *(int32_t *)((char *)dpArr + 0x18);
        if (dpLen > 0 && dpLen <= 8) {
          char *base = (char *)dpArr + 0x20;
          out->bbCount = 0;
          for (int d = 0; d < dpLen && out->bbCount < 8; d++) {
            char *ep = base + d * 32;
            void *sref = *(void **)(ep);
            if (!sref || (uintptr_t)sref < 0x10000) continue;
            int32_t slen = *(int32_t *)((char *)sref + 0x10);
            if (slen <= 0 || slen >= 60) continue;
            wchar_t *wc = (wchar_t *)((char *)sref + 0x14);
            WideCharToMultiByte(CP_UTF8, 0, wc, slen,
              out->bb[out->bbCount].key, 63, NULL, NULL);
            out->bb[out->bbCount].value = *(double *)(ep + 8);
            out->bbCount++;
          }
        }
      }
    }

    // Read icon name: m_data(+0x18) -> BuffIconConfig(+0x18) -> +0x10 (String)
    void *mdata = *(void **)((char *)out->buffObj + 0x18);
    if (mdata) {
      void *iconCfg = *(void **)((char *)mdata + 0x18);
      if (iconCfg && (uintptr_t)iconCfg > 0x10000) {
        void *iconStr = *(void **)((char *)iconCfg + 0x10);
        if (iconStr && (uintptr_t)iconStr > 0x10000) {
          int32_t sl = *(int32_t *)((char *)iconStr + 0x10);
          if (sl > 0 && sl < 120) {
            wchar_t *wc = (wchar_t *)((char *)iconStr + 0x14);
            WideCharToMultiByte(CP_UTF8, 0, wc, sl, out->iconName, 127, NULL, NULL);
          }
        }
      }
    }
  } __except (1) {
  }
}
