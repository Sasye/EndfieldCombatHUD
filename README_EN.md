# EndfieldCombatHUD

English | [中文](README.md)

Enhances the combat HUD in *Arknights: Endfield* with Buff/Debuff details, Combo Skill cooldowns, and Ultimate charge progress. Hover over a status icon to view its duration, stack count, and attribute changes.

## User Agreement and Disclaimer

<details>
<summary>Please read this agreement carefully before downloading, installing, or using this plugin. <b>By using this plugin, you acknowledge that you have fully read, understood, and agreed to all of the following terms.</b></summary>

### 1. Open-Source License and End-User Rights

- This plugin is fully open-sourced on GitHub under the **AGPL-3.0** license. Users may freely use, modify, and distribute the source code of this plugin in compliance with the license.
- End Users may use and distribute this plugin **without any restrictions**, provided they do not modify it. This right is not affected by whether the user violates this agreement.

### 2. Anti-Fraud Statement

- You **must not** openly sell this plugin **itself** on online retail platforms without providing the GitHub repository address and after-sales service.
- This plugin is entirely free and open-source on GitHub. If you obtained it through a paid purchase, please be aware that it is freely available on GitHub.

### 3. Content Compliance and Conduct

- This plugin does not contain any game art assets. Users acknowledge and agree that the official animations, scenes, models, and other assets built into *Arknights: Endfield* are copyrighted by Hypergryph and are not covered by the AGPL-3.0 license.

### 4. Risks and Disclaimer

- This project is for educational, technical research, and communication purposes only. All Arknights game data assets used in this plugin are copyrighted by Hypergryph. Using this tool may violate the game's terms of service and carries a risk of account suspension. For any loss directly or indirectly caused by using this plugin (including but not limited to account bans, game data corruption, etc.), **this project assumes no legal or financial liability**. Users bear all risks and are strongly advised to use it on a test account.

</details>

## Download

Required plugin manager: [Applepie Manager](https://github.com/zonemodAE/ApplepieManager)

Download the latest release from [Releases](https://github.com/Sasye/EndfieldBetterBuffBar/releases), or build it yourself from source.

## Installation

First install the required plugin manager, [Applepie Manager](https://github.com/zonemodAE/ApplepieManager), then copy the following file to the game directory (the folder containing `Endfield.exe`):

```
bin/EndfieldCombatHUD.dll  → Game directory/plugin/EndfieldCombatHUD.dll
```

> If you **have not installed this type of plugin before**, you may need to create the `plugin` folder yourself.

## Usage

1. Launch the game after installing the files as described above.
2. Enter a combat scenario. Once buff/debuff icons appear on the status bar, hover your mouse cursor over them.
3. A detailed attribute panel for that specific status will instantly pop up. The panel supports dynamic multi-resolution scaling, adapting to 720p - 4K screens.
4. During combat, a bracket-shaped HUD flanking the crosshair shows Combo Skill CDs (left) and Ultimate charge (right). It auto-hides when out of combat or when the UI fades.

While Manager shows `Enabling`, click **Cancel enable** to cancel preparation. Once cleanup finishes and the state becomes `Disabled`, click **Enable** to try again. Use **Disable** to close an active HUD.

## Basic Settings

Use Manager's plugin settings to change the Skill HUD key, Buff HUD key, visual effects, and language. Changes are saved and applied automatically. Defaults are F11/F12, effects off, and Chinese. Click the current key name and press the desired key; Esc cancels and Clear unbinds it.
