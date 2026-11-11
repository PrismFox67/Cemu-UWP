# Xbox compatibility list

Games tested on Xbox Series X|S with this fork (D3D12 renderer, Xbox frontend). Results from other games are welcome:
open an issue with the **Xbox / D3D12 bug report form** or comment on an existing one, and say which build you used.

Status: **Perfect** (no known issues) · **Playable** (small issues) · **Issues** (playable with notable bugs) ·
**Broken** (doesn't start or can't be played).

| Game | Resolution / FPS | Status | Notes | Reporter | Build |
|---|---|---|---|---|---|
| Donkey Kong Country: Tropical Freeze | ? | Issues | Some objects and shadows completely black, speckle on water. | PrismFox67 | 0.1.127 |
| LEGO Dimensions | ? | Broken | Heavy graphical glitches, unplayable (screenshots pending). Toy Pad: in-game menu. | The Primadonna (Discord) | 2026-11-09 |
| Mario Kart DS (Wii U Virtual Console) | - | Broken | Mario Kart DS crashes at launch; logs being checked. | PrismFox67 | 0.1.127 |
| Mario Kart 8 | 1440p 60 | Playable | Minor visual bugs ([issue #2](https://github.com/PrismFox67/Cemu-UWP/issues/2)). 4K can run out of memory on the largest tracks. | MagnusBRA | before 39c5467 |
| Mario Tennis: Ultra Smash | 1440p 60 | Perfect | Same as on PC. | MagnusBRA | before 39c5467 |
| Minecraft: Wii U Edition | ? | Playable | Runs decently. Save and Exit crashed on older builds, fixed by 0.1.214. Crashes at boot without the Mii system files (0005001b-10056000): the downloaded pack "Workarounds/Mii Crash Fix" fixes it (v688 only, no Miis in split screen); Cemu turns it on once by itself when those files are missing. Runs nicely with it and the Resolution pack (k, Discord, e1060b4). | The Primadonna (Discord) | 0.1.214 |
| Need for Speed: Most Wanted U | ? | Broken | Closes right after the intro video, when gameplay starts ([issue #3](https://github.com/PrismFox67/Cemu-UWP/issues/3)). | GuilhermeLCv | 2026-10-21 |
| New Super Mario Bros. U | 1440p 60 | Perfect | | MagnusBRA | before 39c5467 |
| Pokkén Tournament | 1440p 60 | Playable | Naming keyboard couldn't be used with a controller; fixed in 0f235bd, retest. | MagnusBRA | before 39c5467 |
| Skylanders: Spyro's Adventure | ? | Issues | Works decently (English patch), graphical glitches on character models. Figures: in-game menu **Skylander**. | The Primadonna (Discord) | 2026-11-09 |
| Sonic Lost World | ? | Issues | Title screen crash fixed in 664e165; crashes during some stages on Series S ([issue #7](https://github.com/PrismFox67/Cemu-UWP/issues/7)). | gabrielvalerio701-lang | ? |
| Splatoon | ? | Issues | "Doesn't work properly", details pending ([issue #2](https://github.com/PrismFox67/Cemu-UWP/issues/2)). | MagnusBRA | before 39c5467 |
| Super Mario 3D World | 1080p | Issues | Runs well, graphical bugs ([issue #2](https://github.com/PrismFox67/Cemu-UWP/issues/2)). Mic blowing: map "Blow into mic (hold)". | MagnusBRA | before 39c5467 |
| Super Smash Bros. for Wii U | ? | Playable | Lag while shaders compile on the first runs. Player 2 set to "Wii U GamePad" did nothing; 0.1.142 switches players 2-4 to Pro Controller, retest. | MagnusBRA, Discord tester | 0.1.142 |
| The Legend of Zelda: Breath of the Wild | 1440p 30+ (Series S) | Playable | Magnesis, Stasis and Sheikah Slate pins work since 0.1.139 ([issue #2](https://github.com/PrismFox67/Cemu-UWP/issues/2)). Dark/blocky terrain patches ([issue #5](https://github.com/PrismFox67/Cemu-UWP/issues/5)). Motion shrines not tested yet; gyro: in-game menu **Motion**. | Sales-Bruno, daniel221086, Atlas091 | 0.1.139 |
| The Legend of Zelda: Twilight Princess HD | ? | Issues | Broken shadows ([issue #8](https://github.com/PrismFox67/Cemu-UWP/issues/8)). | gabrielvalerio701-lang | 0.1.142 |
| The Legend of Zelda: Wind Waker HD | ? | Playable | Some pause menu letters missing or distorted ([issue #9](https://github.com/PrismFox67/Cemu-UWP/issues/9)). Naming keyboard fixed in 0f235bd. | MagnusBRA, jovovichbruno-gif, gabrielvalerio701-lang | 0.1.142 |
| Yoshi's Woolly World | ? | Playable | Works decently. | The Primadonna (Discord) | 2026-11-09 |
