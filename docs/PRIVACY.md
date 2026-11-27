# Privacy

Cemu-UWP is a free, open-source hobby project. It has no accounts, no ads, no payments and no analytics.
**The current release does not collect or send any personal data to us.** Test builds with the optional play stats send the anonymous data listed below, only if you opt in. This page lists everything the app
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

## Anonymous play stats (test builds, opt-in)
Asked once, the first time the launcher opens on a build that has it. Your answer is stored in `settings.xml`
(`stats_consent`) and kept across updates; if you say No you are not asked again. Change it any time in
Settings → Privacy. You are only asked again if the list below changes.

When it's **on**, the app sends this to our stats server, once when the launcher opens and every 5 minutes while a
game runs:

| field | example | what it is |
|---|---|---|
| `iid` | `3f9c...` (32 hex digits) | a random ID made on your console, not tied to your account, console or name |
| `tid` | `000500001010ec00` | the title ID of the running game (empty on the launcher) |
| `ver` | `0.1.262` | the Cemu version |
| `console` | `seriesX` | `seriesX`, `seriesS` or `pc` |

The server stores only these four values and the time rounded to the minute. It does not store IP addresses,
user agents, game file names, accounts or anything else. After 90 days the rows are reduced to daily totals
without IDs.

**Deleting your data:** Settings → Privacy → 'Delete my stats and reset ID' deletes everything stored for your
current ID and starts a new one. Turning stats off also deletes everything stored for your ID. Daily totals older
than 90 days no longer contain IDs, so they can't be linked to you.

Everyone, with stats on or off, may download the per-game totals ("12 playing now", "40 this week") shown in the
game list. That request carries no ID.

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
