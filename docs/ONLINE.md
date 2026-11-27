# Online play (experimental test build)

Cemu can play online on [Pretendo Network](https://pretendo.network/) with files dumped from **your own** Wii U.
Nintendo's own Wii U servers are closed, so the Xbox frontend only offers Offline, Pretendo and Custom.

## 1. Dump the files from your Wii U
Follow the official Cemu guide: <https://cemu.cfw.guide/online-play.html>. You need:

- `otp.bin` (1024 bytes) and `seeprom.bin` (512 bytes)
- your account folder `mlc01/usr/save/system/act/800000xx` (linked to a Pretendo Network ID, see Pretendo's guide)
- the certificate title `mlc01/sys/title/0005001b/10054000`

Never share these files: they belong to your console. They are never uploaded or logged by Cemu.

## 2. Put them on the USB drive
The drive Cemu uses for exports (the one with your games):

```
<USB>/CemuOnline/otp.bin
<USB>/CemuOnline/seeprom.bin
<USB>/CemuOnline/mlc01/usr/save/system/act/800000xx/account.dat   (the whole folder)
<USB>/CemuOnline/mlc01/sys/title/0005001b/10054000/...             (the whole folder)
```

## 3. In Cemu
Settings > Online play (experimental):

1. **Find online files on USB** copies them into Cemu's storage. An account that already exists is moved to
   `online_backup/<time>/` in Cemu's data folder first. The status line shows what is still missing.
2. **Account**: pick the account you dumped (accounts without a linked online ID say "offline only").
3. **Network service**: Pretendo. "Custom" appears if you put your own `network_services.xml` in Cemu's data folder.
4. Start the game. Log lines start with `Online:` and `IOSU_CRYPTO:`.
