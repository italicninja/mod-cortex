-- Applied 2026-10-22 ~03:00 UTC, before the build's dbimport (which would otherwise re-run the rewritten module SQL):
-- the four playerbots tables in the core DBs become cortex_*, their updater records take the new hashes, and views
-- keep the old names working until nothing reads them.
RENAME TABLE
   `acore_characters`.`playerbots_names` TO `acore_characters`.`cortex_names`,
   `acore_characters`.`playerbots_arena_team_names` TO `acore_characters`.`cortex_arena_team_names`,
   `acore_characters`.`playerbots_guild_names` TO `acore_characters`.`cortex_guild_names`,
   `acore_world`.`playerbots_rpg_races` TO `acore_world`.`cortex_rpg_races`;
UPDATE `acore_characters`.`updates` SET `name` = '0000_playerbots_arena_team_names.sql', `hash` = '26776BD337C7772F7E44AD4328F303A0D78C0C16' WHERE `name` = '0000_playerbots_arena_team_names.sql';
UPDATE `acore_characters`.`updates` SET `name` = '0000_playerbots_guild_names.sql', `hash` = '2B766B6B04EA3C142189B24AFAB386BBC0CCAD54' WHERE `name` = '0000_playerbots_guild_names.sql';
UPDATE `acore_characters`.`updates` SET `name` = '0000_playerbots_names.sql', `hash` = '163BAAFF868093483C5DD6DAD39345F0D7E04385' WHERE `name` = '0000_playerbots_names.sql';
UPDATE `acore_characters`.`updates` SET `name` = '2026_09_24_00_playerbots_arena_team_names_add_locale_columns.sql', `hash` = '02C366C85236325DC8BB803B5D89F5A7DF0B9828' WHERE `name` = '2026_09_24_00_playerbots_arena_team_names_add_locale_columns.sql';
UPDATE `acore_characters`.`updates` SET `name` = '2026_09_24_01_playerbots_guild_names_add_locale_columns.sql', `hash` = '603F06BE5EBA5DD358D0B9A71CFF3D06A8D3D92C' WHERE `name` = '2026_09_24_01_playerbots_guild_names_add_locale_columns.sql';
UPDATE `acore_characters`.`updates` SET `name` = '2026_09_24_02_playerbots_translated_french_arena_team_names.sql', `hash` = 'C938E0A1BBE5528F57C5D460E108E4B4460FB7F6' WHERE `name` = '2026_09_24_02_playerbots_translated_french_arena_team_names.sql';
UPDATE `acore_characters`.`updates` SET `name` = '2026_09_24_03_playerbots_translated_french_guilds_name.sql', `hash` = '39BDB52E2D90C3061055E4D7196D39A667E1BB2A' WHERE `name` = '2026_09_24_03_playerbots_translated_french_guilds_name.sql';
UPDATE `acore_world`.`updates` SET `name` = '0000_playerbots_rpg_races.sql', `hash` = '2ADF05FCFECB4028966451CC8EDAD972922AE609' WHERE `name` = '0000_playerbots_rpg_races.sql';
CREATE OR REPLACE VIEW acore_characters.playerbots_names AS SELECT * FROM acore_characters.cortex_names;
CREATE OR REPLACE VIEW acore_characters.playerbots_arena_team_names AS SELECT * FROM acore_characters.cortex_arena_team_names;
CREATE OR REPLACE VIEW acore_characters.playerbots_guild_names AS SELECT * FROM acore_characters.cortex_guild_names;
CREATE OR REPLACE VIEW acore_world.playerbots_rpg_races AS SELECT * FROM acore_world.cortex_rpg_races;
