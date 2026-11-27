# Privacy

Cemu-UWP is a free, open-source hobby project. It has no accounts, no ads, no payments and no analytics.
**The current release does not collect or send any personal data to us.** This page lists everything the app
does over the network so you can check it.

## What the app connects to
| What | When | Who receives it |
|---|---|---|
| Update check | When the launcher opens, to say if a newer build exists | GitHub (`PrismFox67/Cemu-UWP` releases) |
| Box art | When a game in your folders has no cached cover (turn off in Settings → Box art) | GameTDB (`art.gametdb.com`) |
| Graphic packs | When you download or update the community packs | GitHub (`cemu-project/cemu_graphic_packs`) |
| Phone remote | Only after you turn it on in Settings. Listens on your local network; nothing leaves your home network | Your phone |

These requests are ordinary web downloads: the site you download from sees your IP address, like any website
visit. We don't run any server that receives data from the app.

## What stays on your console
Settings, logs, saves, box-art cache, `keys.txt` and controller profiles stay in the app's own storage on your
console or USB drive. Logs are only shared if **you** attach them to a GitHub issue or Discord message. Logs don't
contain keys; remove anything you don't want to share before posting.

## Future features
Planned optional features (anonymous play counts, online play through Pretendo) will be **off until you opt in**,
and this page will be updated, with the exact data listed, before any build that has them is released.
Opting out will always be possible in Settings, and anonymous data you sent can be deleted from the app.

## Contact
Questions or deletion requests: open an issue at https://github.com/PrismFox67/Cemu-UWP/issues.
