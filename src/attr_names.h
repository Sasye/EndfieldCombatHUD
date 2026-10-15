// clang-format off
// Forward declaration: language flag defined in combat_hud.cpp
extern std::atomic<bool> g_langZh;

static const char *GetAttrName(int idx) {
  static const char *const ATTR_NAMES_ZH[] = {
    "\xe7\xad\x89\xe7\xba\xa7",                                                   // 0  等级
    "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd",                           // 1  最大生命
    "\xe6\x94\xbb\xe5\x87\xbb\xe5\x8a\x9b",                                       // 2  攻击力
    "\xe9\x98\xb2\xe5\xbe\xa1\xe5\x8a\x9b",                                       // 3  防御力
    "\xe7\x89\xa9\xe7\x90\x86\xe5\x8f\x97\xe4\xbc\xa4",                           // 4  物理受伤
    "\xe7\x81\xab\xe7\x84\xb0\xe5\x8f\x97\xe4\xbc\xa4",                           // 5  火焰受伤
    "\xe7\x94\xb5\xe8\x84\x89\xe5\x86\xb2\xe5\x8f\x97\xe4\xbc\xa4",               // 6  电脉冲受伤
    "\xe7\xbb\x93\xe6\x99\xb6\xe5\x8f\x97\xe4\xbc\xa4",                           // 7  结晶受伤
    "\xe4\xbd\x93\xe9\x87\x8d",                                                   // 8  体重
    "\xe6\x9a\xb4\xe5\x87\xbb\xe7\x8e\x87",                                       // 9  暴击率
    "\xe6\x9a\xb4\xe5\x87\xbb\xe4\xbc\xa4\xe5\xae\xb3",                           // 10 暴击伤害
    "\xe4\xbb\x87\xe6\x81\xa8",                                                   // 11 仇恨
    "\xe6\x99\xae\xe6\x94\xbb\xe8\x8c\x83\xe5\x9b\xb4",                           // 12 普攻范围
    "\xe7\xa7\xbb\xe9\x80\x9f",                                                   // 13 移速
    "\xe8\xbd\xac\xe5\x90\x91\xe9\x80\x9f\xe5\xba\xa6",                           // 14 转向速度
    "\xe6\x94\xbb\xe5\x87\xbb\xe9\x80\x9f\xe5\xba\xa6",                           // 15 攻击速度
    "\xe6\x8a\x80\xe8\x83\xbd\xe5\x86\xb7\xe5\x8d\xb4\xe5\x80\x8d\xe7\x8e\x87",   // 16 技能冷却倍率
    "\xe6\x99\xae\xe6\x94\xbb\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe6\x88\x90",   // 17 普攻伤害加成
    "\xe6\xaf\x8f\xe7\xa7\x92\xe5\x9b\x9e\xe8\xa1\x80",                           // 18 每秒回血
    "\xe6\xaf\x94\xe4\xbe\x8b\xe5\x9b\x9e\xe8\xa1\x80",                           // 19 比例回血
    "\xe6\x9c\x80\xe5\xa4\xa7\xe5\x89\x8a\xe9\x9f\xa7",                           // 20 最大削韧
    "\xe5\x89\x8a\xe9\x9f\xa7\xe6\x81\xa2\xe5\xa4\x8d\xe6\x97\xb6\xe9\x97\xb4",   // 21 削韧恢复时间
    "\xe6\x9c\x80\xe5\xa4\xa7\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80\xe8\x83\xbd\xe9\x87\x8f", // 22 最大终结技能量
    "\xe5\xb8\xa6\xe5\x89\x8a\xe9\x9f\xa7\xe5\x8f\x97\xe4\xbc\xa4",               // 23 带削韧受伤
    "\xe5\x89\x8a\xe9\x9f\xa7\xe5\x8f\x97\xe5\x87\xbb",                           // 24 削韧受击
    "\xe7\x89\xa9\xe7\x90\x86\xe5\xbc\x82\xe5\xb8\xb8\xe4\xbc\xa4\xe5\xae\xb3",   // 25 物理异常伤害
    "\xe5\x89\x8a\xe9\x9f\xa7\xe8\xbe\x93\xe5\x87\xba",                           // 26 削韧输出
    "\xe7\xa0\xb4\xe9\x98\xb2\xe5\x8f\x97\xe4\xbc\xa4",                           // 27 破防受伤
    "\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3",               // 28 终结技伤害
    "\xe6\xb2\xbb\xe7\x96\x97\xe8\xbe\x93\xe5\x87\xba",                           // 29 治疗输出
    "\xe5\x8f\x97\xe6\xb2\xbb\xe7\x96\x97\xe5\x8a\xa0\xe6\x88\x90",               // 30 受治疗加成
    "\xe5\x89\x8a\xe9\x9f\xa7\xe6\x81\xa2\xe5\xa4\x8d\xe5\x80\x8d\xe7\x8e\x87",   // 31 削韧恢复倍率
    "\xe6\x88\x98\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3",                           // 32 战技伤害
    "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe4\xbc\xa4\xe5\xae\xb3",               // 33 连携技伤害
    "\xe5\x87\xbb\xe5\x80\x92\xe6\x97\xb6\xe9\x97\xb4\xe5\xa2\x9e\xe5\x8a\xa0",   // 34 击倒时间增加
    "\xe7\x81\xab\xe5\x85\x83\xe7\xb4\xa0\xe7\x88\x86\xe5\x8f\x91\xe4\xbc\xa4\xe5\xae\xb3",   // 35 火元素爆发伤害
    "\xe7\x94\xb5\xe5\x85\x83\xe7\xb4\xa0\xe7\x88\x86\xe5\x8f\x91\xe4\xbc\xa4\xe5\xae\xb3",   // 36 电元素爆发伤害
    "\xe6\x99\xb6\xe5\x85\x83\xe7\xb4\xa0\xe7\x88\x86\xe5\x8f\x91\xe4\xbc\xa4\xe5\xae\xb3",   // 37 晶元素爆发伤害
    "\xe8\x87\xaa\xe7\x84\xb6\xe5\x85\x83\xe7\xb4\xa0\xe7\x88\x86\xe5\x8f\x91\xe4\xbc\xa4\xe5\xae\xb3", // 38 自然元素爆发伤害
    "\xe5\x8a\x9b\xe9\x87\x8f",                                                   // 39 力量
    "\xe6\x95\x8f\xe6\x8d\xb7",                                                   // 40 敏捷
    "\xe6\x99\xba\xe5\x8a\x9b",                                                   // 41 智力
    "\xe6\x84\x8f\xe5\xbf\x97",                                                   // 42 意志
    "\xe7\x94\x9f\xe5\x91\xbd\xe5\x81\xb7\xe5\x8f\x96",                           // 43 生命偷取
    "\xe7\xbb\x88\xe7\xbb\x93\xe6\x8a\x80\xe5\x85\x85\xe8\x83\xbd",               // 44 终结技充能
    "ATB\xe6\xb6\x88\xe8\x80\x97",                                                // 45 ATB消耗
    "\xe6\x8a\x80\xe8\x83\xbd\xe5\x86\xb7\xe5\x8d\xb4\xe5\xa2\x9e\xe5\x8a\xa0",   // 46 技能冷却增加
    "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe5\x86\xb7\xe5\x8d\xb4\xe5\x80\x8d\xe7\x8e\x87", // 47 连携技冷却倍率
    "\xe8\x87\xaa\xe7\x84\xb6\xe5\x8f\x97\xe4\xbc\xa4",                           // 48 自然受伤
    "\xe7\x82\xb9\xe7\x87\x83\xe4\xbc\xa4\xe5\xae\xb3",                           // 49 点燃伤害
    "\xe7\x89\xa9\xe7\x90\x86\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe6\x88\x90",   // 50 物理伤害加成
    "\xe7\x81\xab\xe7\x84\xb0\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe6\x88\x90",   // 51 火焰伤害加成
    "\xe7\x94\xb5\xe8\x84\x89\xe5\x86\xb2\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe6\x88\x90", // 52 电脉冲伤害加成
    "\xe7\xbb\x93\xe6\x99\xb6\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe6\x88\x90",   // 53 结晶伤害加成
    "\xe8\x87\xaa\xe7\x84\xb6\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe6\x88\x90",   // 54 自然伤害加成
    "\xe4\xbb\xa5\xe5\xa4\xaa\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe6\x88\x90",   // 55 以太伤害加成
    "\xe7\x81\xab\xe5\xbc\x82\xe5\xb8\xb8\xe4\xbc\xa4\xe5\xae\xb3",               // 56 火异常伤害
    "\xe7\x94\xb5\xe5\xbc\x82\xe5\xb8\xb8\xe4\xbc\xa4\xe5\xae\xb3",               // 57 电异常伤害
    "\xe6\x99\xb6\xe5\xbc\x82\xe5\xb8\xb8\xe4\xbc\xa4\xe5\xae\xb3",               // 58 晶异常伤害
    "\xe8\x87\xaa\xe7\x84\xb6\xe5\xbc\x82\xe5\xb8\xb8\xe4\xbc\xa4\xe5\xae\xb3",   // 59 自然异常伤害
    "\xe4\xbb\xa5\xe5\xa4\xaa\xe5\x8f\x97\xe4\xbc\xa4",                           // 60 以太受伤
    "\xe5\xaf\xb9\xe7\xa0\xb4\xe9\x98\xb2\xe7\x9b\xae\xe6\xa0\x87\xe4\xbc\xa4\xe5\xae\xb3", // 61 对破防目标伤害
    "\xe5\xbc\xb1\xe5\x8c\x96",                                                   // 62 弱化
    "\xe5\xba\x87\xe6\x8a\xa4",                                                   // 63 庇护
    "\xe7\x89\xa9\xe7\x90\x86\xe5\xa2\x9e\xe5\xb9\x85",                           // 64 物理增幅
    "\xe7\x81\xab\xe7\x84\xb0\xe5\xa2\x9e\xe5\xb9\x85",                           // 65 火焰增幅
    "\xe7\x94\xb5\xe8\x84\x89\xe5\x86\xb2\xe5\xa2\x9e\xe5\xb9\x85",               // 66 电脉冲增幅
    "\xe7\xbb\x93\xe6\x99\xb6\xe5\xa2\x9e\xe5\xb9\x85",                           // 67 结晶增幅
    "\xe8\x87\xaa\xe7\x84\xb6\xe5\xa2\x9e\xe5\xb9\x85",                           // 68 自然增幅
    "\xe4\xbb\xa5\xe5\xa4\xaa\xe5\xa2\x9e\xe5\xb9\x85",                           // 69 以太增幅
    "\xe7\x89\xa9\xe7\x90\x86\xe6\x98\x93\xe4\xbc\xa4",                           // 70 物理易伤
    "\xe7\x81\xab\xe7\x84\xb0\xe6\x98\x93\xe4\xbc\xa4",                           // 71 火焰易伤
    "\xe7\x94\xb5\xe8\x84\x89\xe5\x86\xb2\xe6\x98\x93\xe4\xbc\xa4",               // 72 电脉冲易伤
    "\xe7\xbb\x93\xe6\x99\xb6\xe6\x98\x93\xe4\xbc\xa4",                           // 73 结晶易伤
    "\xe8\x87\xaa\xe7\x84\xb6\xe6\x98\x93\xe4\xbc\xa4",                           // 74 自然易伤
    "\xe4\xbb\xa5\xe5\xa4\xaa\xe6\x98\x93\xe4\xbc\xa4",                           // 75 以太易伤
    "STR-ATK",                                                                     // 76
    "AGI-ATK",                                                                     // 77
    "WIS-ATK",                                                                     // 78
    "WIL-ATK",                                                                     // 79
    "\xe7\x89\xa9\xe7\x90\x86\xe6\x8a\x97\xe6\x80\xa7",                           // 80 物理抗性
    "\xe8\x87\xaa\xe7\x84\xb6\xe6\x8a\x97\xe6\x80\xa7",                           // 81 自然抗性
    "\xe7\xbb\x93\xe6\x99\xb6\xe6\x8a\x97\xe6\x80\xa7",                           // 82 结晶抗性
    "\xe7\x94\xb5\xe8\x84\x89\xe5\x86\xb2\xe6\x8a\x97\xe6\x80\xa7",               // 83 电脉冲抗性
    "\xe7\x81\xab\xe7\x84\xb0\xe6\x8a\x97\xe6\x80\xa7",                           // 84 火焰抗性
    "\xe4\xbb\xa5\xe5\xa4\xaa\xe6\x8a\x97\xe6\x80\xa7",                           // 85 以太抗性
    "\xe5\x87\x8f\xe9\x80\x9f",                                                   // 86 减速
    "\xe7\x89\xa9\xe6\xb3\x95\xe5\xbc\x82\xe5\xb8\xb8\xe5\xa2\x9e\xe5\xbc\xba",   // 87 物法异常增强
    "\xe6\x8a\xa4\xe7\x9b\xbe\xe8\xbe\x93\xe5\x87\xba",                           // 88 护盾输出
    "\xe5\x8f\x97\xe6\x8a\xa4\xe7\x9b\xbe\xe5\x8a\xa0\xe6\x88\x90",               // 89 受护盾加成
    "\xe6\x99\xae\xe6\x94\xbb\xe5\x90\xaf\xe5\x8a\xa8\xe8\xb7\x9d\xe7\xa6\xbb",   // 90 普攻启动距离
    "\xe7\xa9\xba\xe4\xb8\xad\xe7\xa7\xbb\xe9\x80\x9f",                           // 91 空中移速
    "\xe5\x85\xb3\xe9\x94\xae\xe8\xaf\x8d\xe5\x8a\xa0\xe9\x80\x9f",               // 92 关键词加速
    "\xe8\xbf\x9e\xe6\x90\xba\xe6\x8a\x80\xe5\x86\xb7\xe5\x8d\xb4\xe6\x81\xa2\xe5\xa4\x8d", // 93 连携技冷却恢复
  };
  static const char *const ATTR_NAMES_EN[] = {
    "Level",                   // 0
    "Max HP",                  // 1
    "ATK",                     // 2
    "DEF",                     // 3
    "Physical Taken",          // 4
    "Fire Taken",              // 5
    "Pulse Taken",             // 6
    "Crystal Taken",           // 7
    "Weight",                  // 8
    "CRIT Rate",               // 9
    "CRIT DMG",                // 10
    "Aggro",                   // 11
    "Atk Range",               // 12
    "Move SPD",                // 13
    "Turn SPD",                // 14
    "ATK SPD",                 // 15
    "Skill CD Mult",           // 16
    "Normal ATK DMG",          // 17
    "HP Regen/s",              // 18
    "HP Regen %",              // 19
    "Max Poise",               // 20
    "Poise Recovery",          // 21
    "Max Ult SP",              // 22
    "Poise DMG Taken",         // 23
    "Poise Hit Taken",         // 24
    "Phys Anomaly DMG",        // 25
    "Poise Output",            // 26
    "Break DMG Taken",         // 27
    "Ultimate DMG",            // 28
    "Heal Output",             // 29
    "Heal Taken",              // 30
    "Poise Recv Mult",         // 31
    "Skill DMG",               // 32
    "Combo DMG",               // 33
    "Knockdown Time+",         // 34
    "Fire Burst DMG",          // 35
    "Pulse Burst DMG",         // 36
    "Crystal Burst DMG",       // 37
    "Nature Burst DMG",        // 38
    "STR",                     // 39
    "AGI",                     // 40
    "WIS",                     // 41
    "WIL",                     // 42
    "Life Steal",              // 43
    "Ult SP Gain",             // 44
    "ATB Cost",                // 45
    "Skill CD Add",            // 46
    "Combo CD Mult",           // 47
    "Nature Taken",            // 48
    "Ignite DMG",              // 49
    "Physical DMG+",           // 50
    "Fire DMG+",               // 51
    "Pulse DMG+",              // 52
    "Crystal DMG+",            // 53
    "Nature DMG+",             // 54
    "Ether DMG+",              // 55
    "Fire Anomaly DMG",        // 56
    "Pulse Anomaly DMG",       // 57
    "Crystal Anomaly DMG",     // 58
    "Nature Anomaly DMG",      // 59
    "Ether Taken",             // 60
    "Break Target DMG",        // 61
    "Weaken",                  // 62
    "Shelter",                 // 63
    "Physical Enhance",        // 64
    "Fire Enhance",            // 65
    "Pulse Enhance",           // 66
    "Crystal Enhance",         // 67
    "Nature Enhance",          // 68
    "Ether Enhance",           // 69
    "Physical Vuln",           // 70
    "Fire Vuln",               // 71
    "Pulse Vuln",              // 72
    "Crystal Vuln",            // 73
    "Nature Vuln",             // 74
    "Ether Vuln",              // 75
    "STR-ATK",                 // 76
    "AGI-ATK",                 // 77
    "WIS-ATK",                 // 78
    "WIL-ATK",                 // 79
    "Physical RES",            // 80
    "Nature RES",              // 81
    "Crystal RES",             // 82
    "Pulse RES",               // 83
    "Fire RES",                // 84
    "Ether RES",               // 85
    "Slow",                    // 86
    "Anomaly Enhance",         // 87
    "Shield Output",           // 88
    "Shield Taken",            // 89
    "Atk Init Dist",           // 90
    "Air Move SPD",            // 91
    "Keyword Accel",           // 92
    "Combo CD Recovery",       // 93
  };
  static const int N = sizeof(ATTR_NAMES_ZH) / sizeof(ATTR_NAMES_ZH[0]);
  const char *const *names = g_langZh ? ATTR_NAMES_ZH : ATTR_NAMES_EN;
  if (idx >= 0 && idx < N)
    return names[idx];
  static char fallback[32];
  snprintf(fallback, sizeof(fallback), "Attr[%d]", idx);
  return fallback;
}
// clang-format on
