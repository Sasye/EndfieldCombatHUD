# EndfieldCombatHUD

English | [中文](README.md)

Enhances the combat HUD in *Arknights: Endfield* with a detailed buff/debuff inspection feature and real-time combat information display. Hovering over a buff icon in the status bar will display a detailed attribute panel that reveals the exact duration, stack count, and precise attribute modifiers (e.g., +10.0% ATK, +9.0% Spell Enhance, etc.). During combat, a bracket-shaped HUD flanking the crosshair shows Combo Skill cooldowns and Ultimate charge progress.

## Installation

Copy the following files to the game directory (same folder as `Endfield.exe`):

```
bin/EndfieldCombatHUD.dll  → Game Directory/plugin/EndfieldCombatHUD.dll
bin/vulkan-1.dll           → Game Directory/vulkan-1.dll
bin/d3dcompiler_47.dll     → Game Directory/d3dcompiler_47.dll
```

> **Note**: `d3dcompiler_47.dll` (DirectX) and `vulkan-1.dll` (Vulkan) are proxy loaders. You only need the one matching your rendering API, or both. If you are also using other plugins that share these proxy loaders, there is no need to overwrite them.

## Usage

1. Launch the game after installing the files as described above.
2. Enter a combat scenario. Once buff/debuff icons appear on the status bar, hover your mouse cursor over them.
3. A detailed attribute panel for that specific status will instantly pop up. The panel supports dynamic multi-resolution scaling, adapting to 720p - 4K screens.
4. During combat, a bracket-shaped HUD flanking the crosshair shows Combo Skill CDs (left) and Ultimate charge (right). It auto-hides when out of combat or when the UI fades.

## Disclaimer

This project is for educational and research purposes only. Use of this tool may violate the game's Terms of Service and carries a risk of account suspension. Use at your own risk.
