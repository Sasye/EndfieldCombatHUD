# EndfieldCombatHUD

English | [中文](README.md)

Enhances the combat HUD in *Arknights: Endfield* with a detailed buff/debuff inspection feature and real-time combat information display. Hovering over a buff icon in the status bar will display a detailed attribute panel that reveals the exact duration, stack count, and precise attribute modifiers (e.g., +10.0% ATK, +9.0% Spell Enhance, etc.). During combat, a bracket-shaped HUD flanking the crosshair shows Combo Skill cooldowns and Ultimate charge progress.

## User Agreement & Disclaimer

<details>
<summary>Please read this agreement carefully before downloading, installing, or using this plugin. <b>By using this plugin, you acknowledge that you have fully read, understood, and agreed to all of the following terms.</b></summary>

### 1. Open Source License & End User Rights
- This plugin is fully open-sourced on GitHub under the **AGPL-3.0** license. Users may freely use, modify, and distribute the source code of this plugin in compliance with the license.
- End Users may use and distribute this plugin **without any restrictions**, provided they do not modify it. This right is not affected by whether the user violates this agreement.

### 2. Anti-Fraud Statement
- You **must not** openly sell this plugin **itself** on online retail platforms without providing the GitHub repository address and after-sales service.
- This plugin is entirely free and open-source on GitHub. If you obtained it through a paid purchase, please be aware that it is freely available on GitHub.

### 3. Content Compliance & Conduct
- This plugin does not contain any game art assets. Users acknowledge and agree that the official animations, scenes, models, and other assets built into *Arknights: Endfield* are copyrighted by Hypergryph and are not covered by the AGPL-3.0 license.

### 4. Risk & Disclaimer
- This project is for educational, technical research, and communication purposes only. All Arknights game data assets used in this plugin are copyrighted by Hypergryph. Using this tool may violate the game's terms of service and carries a risk of account suspension. For any loss directly or indirectly caused by using this plugin (including but not limited to account bans, game data corruption, etc.), **this project assumes no legal or financial liability**. Users bear all risks and are strongly advised to use it on a test account.

</details>

## Installation

Copy the following files to the game directory (same folder as `Endfield.exe`):

```
bin/EndfieldCombatHUD.dll  → Game Directory/plugin/EndfieldCombatHUD.dll
bin/vulkan-1.dll           → Game Directory/vulkan-1.dll
bin/d3dcompiler_47.dll     → Game Directory/d3dcompiler_47.dll
```

> **Note**: `d3dcompiler_47.dll` (DirectX) and `vulkan-1.dll` (Vulkan) are proxy loaders. You only need the one matching your rendering API, or both. If you are also using other plugins that share these proxy loaders, there is no need to overwrite them.

> If you have **never installed a plugin of this type before**, you may need to create the `plugin` folder yourself.

## Usage

1. Launch the game after installing the files as described above.
2. Enter a combat scenario. Once buff/debuff icons appear on the status bar, hover your mouse cursor over them.
3. A detailed attribute panel for that specific status will instantly pop up. The panel supports dynamic multi-resolution scaling, adapting to 720p - 4K screens.
4. During combat, a bracket-shaped HUD flanking the crosshair shows Combo Skill CDs (left) and Ultimate charge (right). It auto-hides when out of combat or when the UI fades.
