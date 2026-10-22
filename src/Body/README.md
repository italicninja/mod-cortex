# Cortex's body (ported from mod-playerbots)

This directory is mod-playerbots, ported into mod-cortex on 2026-10-21 so that Cortex no longer depends on it
(wowproject `docs/cortex-standalone.md`). It is the bot runtime Cortex drives: sessions and the random bot population,
the action engine, class combat, group play, dungeons, raids and battlegrounds, and the features wowproject added to
playerbots (telemetry, routes, trips, professions, auction, corpse runs; patches 0001–0073).

- Source: https://github.com/mod-playerbots/mod-playerbots (upstream `master`), as carried by our fork
  https://github.com/italicninja/mod-playerbots branch `wowproject` at commit c44a19ef.
- License: GNU GPL v2 or later (`LICENSE`); authors and copyright: `AUTHORS.md` and the history of the upstream
  repository. Every file keeps its original notice.
- Its SQL is in `../../data/sql/` (playerbots DB, plus world and characters updates); its config is still
  `playerbots.conf` with `Playerbots.*` keys.

From here it is Cortex's code: it changes in this repository, and upstream playerbots is no longer merged in.
