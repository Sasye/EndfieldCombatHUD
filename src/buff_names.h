#pragma once
// Buff name lookup: string-ID pattern matching + character name extraction
// Since int_id is not available in client memory, we use buff string IDs.

// Forward declaration: language flag defined in combat_hud.cpp
extern std::atomic<bool> g_langZh;

// ============================================================================
// Character code -> name mapping (from CharacterNameIndex.json)
// ============================================================================
struct CharEntry { const char *code; const char *zhName; const char *enName; };
static const CharEntry g_charNames[] = {
  {"endminm", "\xe7\xae\xa1\xe7\x90\x86\xe5\x91\x98",   "Endmin"},       // 管理员
  {"endminf", "\xe7\xae\xa1\xe7\x90\x86\xe5\x91\x98",   "Endmin"},       // 管理员
  {"endmin",  "\xe7\xae\xa1\xe7\x90\x86\xe5\x91\x98",   "Endmin"},       // 管理员
  {"chen",    "\xe9\x99\x88\xe5\x8d\x83\xe8\xaf\xad",   "Chen"},         // 陈千语
  {"ikut",    "\xe5\xbc\xa7\xe5\x85\x89",               "Arclight"},     // 弧光
  {"azrila",  "\xe4\xbd\x99\xe7\x83\xac",               "Ember"},        // 余烬
  {"pelica",  "\xe4\xbd\xa9\xe4\xb8\xbd\xe5\x8d\xa1",   "Pelica"},       // 佩丽卡
  {"antal",   "\xe5\xae\x89\xe5\xa1\x94\xe5\xb0\x94",   "Antal"},        // 安塔尔
  {"wolfgd",  "\xe7\x8b\xbc\xe5\x8d\xab",               "Wolfguard"},    // 狼卫
  {"seraph",  "\xe8\xb5\x9b\xe5\xb8\x8c",               "Seraph"},       // 赛希
  {"avywen",  "\xe8\x89\xbe\xe7\xbb\xb4\xe6\x96\x87\xe5\xa8\x9c", "Avywenna"}, // 艾维文娜
  {"aglina",  "\xe6\xb4\x81\xe5\xb0\x94\xe4\xbd\xa9\xe5\xa1\x94", "Aglina"},   // 洁尔佩塔
  {"aurora",  "\xe6\x98\xbc\xe9\x9b\xaa",               "Aurora"},       // 昼雪
  {"lifeng",  "\xe9\xbb\x8e\xe9\xa3\x8e",               "Lifeng"},       // 黎风
  {"laevat",  "\xe8\x8e\xb1\xe4\xb8\x87\xe6\xb1\x80",   "Laevatain"},    // 莱万汀
  {"deepfin", "\xe9\x98\xbf\xe5\x88\x97\xe4\xbb\x80",   "Alesh"},        // 阿列什
  {"pograni", "\xe9\xaa\x8f\xe5\x8d\xab",               "Pograni"},      // 骏卫
  {"yvonne",  "\xe4\xbc\x8a\xe5\x86\xaf",               "Yvonne"},       // 伊冯
  {"bounda",  "\xe8\x90\xa4\xe7\x9f\xb3",               "Fluorite"},     // 萤石
  {"ardelia", "\xe8\x89\xbe\xe5\xb0\x94\xe9\xbb\x9b\xe6\x8b\x89", "Ardelia"},   // 艾尔黛拉
  {"dapan",   "\xe5\xa4\xa7\xe6\xbd\x98",               "Dapan"},        // 大潘
  {"karin",   "\xe7\xa7\x8b\xe6\xa0\x97",               "Karin"},        // 秋栗
  {"meurs",   "\xe5\x8d\xa1\xe5\xa5\x91\xe5\xb0\x94",   "Meurs"},        // 卡契尔
  {"whiten",  "\xe5\x9f\x83\xe7\x89\xb9\xe6\x8b\x89",   "Whiten"},       // 埃特拉
  {"lastrite","\xe5\x88\xab\xe7\xa4\xbc",               "Last Rite"},    // 别礼
  {"tangtang","\xe6\xb1\xa4\xe6\xb1\xa4",               "Tang"},         // 汤汤
  {"wulfa",   "\xe6\xb4\x9b\xe8\x8c\x9c",               "Wulfa"},        // 洛茜
  {"zhuangfy","\xe5\xba\x84\xe6\x96\xb9\xe5\xae\x9c",   "Zhuang"},       // 庄方宜
};

// ============================================================================
// Extract character name + skill type from buff string ID
// e.g. "buff_chr_0011_seraph_ultimate_effect_2" -> "赛希-终结技" / "Seraph-Ultimate"
// ============================================================================

// Skill type suffix patterns (longer/compound patterns first!)
struct SkillSuffix { const char *pat; const char *zhLabel; const char *enLabel; };
static const SkillSuffix g_skillTypes[] = {
  // Compound patterns (must come before short ones)
  {"_ultimateshield", "-\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80\xe6\x8a\xa4\xe7\x9b\xbe", "-Ult Shield"},   // -终结技护盾
  {"_skillshield",    "-\xe6\x88\x98\xe6\x8a\x80\xe6\x8a\xa4\xe7\x9b\xbe",               "-Skill Shield"}, // -战技护盾
  {"_comboshield",    "-\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\xa4\xe7\x9b\xbe",               "-Combo Shield"}, // -连携护盾
  {"_talentshield",   "-\xe5\xa4\xa9\xe8\xb5\x8b\xe6\x8a\xa4\xe7\x9b\xbe",               "-Talent Shield"},// -天赋护盾
  // Simple patterns
  {"_ultimate",  "-\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80",   "-Ultimate"},  // -终结技
  {"_skill",     "-\xe6\x88\x98\xe6\x8a\x80",               "-Skill"},     // -战技
  {"_combo",     "-\xe8\xbf\x9e\xe6\x90\xba",               "-Combo"},     // -连携
  {"_talent",    "-\xe5\xa4\xa9\xe8\xb5\x8b",               "-Talent"},    // -天赋
  {"_normal",    "-\xe6\x99\xae\xe6\x94\xbb",               "-Normal"},    // -普攻
  {"_passive",   "-\xe8\xa2\xab\xe5\x8a\xa8",               "-Passive"},   // -被动
  {"_shield",    "-\xe6\x8a\xa4\xe7\x9b\xbe",               "-Shield"},    // -护盾
};

// Check if pattern appears at a word boundary in suffix
// (pattern must be followed by '_', '\0', or be the end of a known compound)
static const char* FindSuffixPattern(const char* suffix, const char* pat) {
  size_t patLen = strlen(pat);
  const char *p = suffix;
  while ((p = strstr(p, pat)) != nullptr) {
    char next = p[patLen];
    if (next == '_' || next == '\0') return p;
    p += patLen;
  }
  return nullptr;
}

static bool ExtractBuffName(const char* buffId, char* out, int outSz) {
  // buff_chr_XXXX_<charcode>_<skilltype>_...
  if (!buffId || strncmp(buffId, "buff_chr_", 9) != 0) return false;
  const char *p = buffId + 9;
  while (*p >= '0' && *p <= '9') p++;
  if (*p == '_') p++;
  // Match character code
  for (auto &c : g_charNames) {
    size_t len = strlen(c.code);
    if (strncmp(p, c.code, len) == 0 && (p[len] == '_' || p[len] == '\0')) {
      // Start with character name (Chinese or English)
      const char *name = g_langZh ? c.zhName : c.enName;
      strncpy(out, name, outSz - 1);
      out[outSz - 1] = '\0';
      // Try to append skill type from remaining suffix
      const char *suffix = p + len; // e.g. "_ultimateshield" or "_ultimate_effect_2"
      for (auto &s : g_skillTypes) {
        // Compound patterns (no separator) use strstr directly
        // Simple patterns require word boundary
        bool isCompound = (strstr(s.pat, "shield") != nullptr && strlen(s.pat) > 7);
        const char *match = isCompound ? strstr(suffix, s.pat) : FindSuffixPattern(suffix, s.pat);
        if (match) {
          const char *label = g_langZh ? s.zhLabel : s.enLabel;
          size_t cur = strlen(out);
          strncpy(out + cur, label, outSz - 1 - cur);
          out[outSz - 1] = '\0';
          break;
        }
      }
      return true;
    }
  }
  return false;
}
