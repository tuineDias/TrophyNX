# Send your achievements to the TrophyNX Hub site

Press **R3** (click the right stick) in TrophyNX: the app sends every game you have progress in
(names, points, unlock dates, play time, game icon) to your own TrophyNX Hub site.

1. Deploy the Hub (Cloudflare Worker + site): see the Hub's README.
2. Create `sdmc:/switch/TrophyNX/sync.json`:

```json
{ "url": "https://YOUR-WORKER.workers.dev/api/sync", "token": "the same secret as SYNC_TOKEN", "user": "yourname" }
```

`url` must be https. `user` is 2-32 letters, digits, `_` or `-`; it is the name in the site's address.
3. Press **Y** to rescan, then **R3**. The toast at the bottom says how many games were sent.

Only data the app already shows on screen is sent. Locked hidden achievements are sent as `???`.
Nothing is read from or written to your game saves by this feature.
