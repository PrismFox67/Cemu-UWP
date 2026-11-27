# Privacy

Cemu-UWP doesn't collect anything unless you say yes. This page lists exactly what goes over the network.

## Anonymous play stats (opt-in, experimental)
Asked once, the first time the launcher opens on a build that has it. Your answer is stored in `settings.xml`
(`stats_consent`) and kept across updates; if you say No you are not asked again. Change it any time in
Settings > Privacy. You are only asked again if the list below changes.

When it's **on**, Cemu sends this, once when the launcher opens and every 5 minutes while a game runs:

| field | example | what it is |
|---|---|---|
| `iid` | `3f9c...` (32 hex digits) | a random ID made on your console, not tied to your account, console or name. 'Reset my ID' makes a new one |
| `tid` | `000500001010ec00` | the title ID of the running game (empty on the launcher) |
| `ver` | `0.1.262` | the Cemu version |
| `console` | `seriesX` | `seriesX`, `seriesS` or `pc` |

The server stores only these four values and the time rounded to the minute. It does not store IP addresses,
user agents, game file names, accounts or anything else. After 90 days the rows are reduced to daily totals.

Everyone, with sharing on or off, may download the per-game totals ("12 playing now", "40 this week") shown in the
game list. That request carries no ID.

## Other network use
- Update check: asks GitHub for the newest build (GitHub sees your IP like any website visit).
- Box art: downloaded from GameTDB (can be turned off in Settings).
- Phone remote / phone companion / online play: only when you turn them on, see their notes in Settings.
