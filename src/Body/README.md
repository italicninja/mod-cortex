# Cortex's body

The bot runtime that Cortex's brain (`../`) drives: bot sessions and the random bot population, the action engine,
class combat, group play, classic raids and battlegrounds, and the features added for the wowproject realm
(telemetry, routes, trips, professions, auction, corpse runs).

- `Bot/`: sessions, login and logout, the random bot manager, the action engine, bot creation and gear.
- `Ai/`: class combat (9 classes), group play, classic raids (Molten Core, Onyxia, BWL, AQ20),
  battleground tactics.
- `Mgr/`: travel and movement, items, guilds, talents, gathering nodes, bot texts.
- `Util/`: telemetry (`BotTelemetry`) and helpers. `Db/`: the database pool and repositories. `Script/`: script
  and command registration.

Its SQL is in `../../data/sql/` (the `acore_cortex` database, plus world and characters updates), applied by the
built-in updater. Its config is `mod_cortex_bots.conf` (`Cortex.Bots.*` keys).

## Credits

This code began as [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) and was brought into
mod-cortex on 2026-10-21; it is developed here now. It is GPL-2.0 or later (`LICENSE`). Every file keeps its
original notice; authors and copyright are in `AUTHORS.md` and the upstream history.
