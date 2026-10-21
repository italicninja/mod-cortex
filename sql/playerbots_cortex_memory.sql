-- Cortex memory (mod-cortex, docs/bot-ai-overhaul.md §4.2): what each Cortex-arm bot has seen and learned, one
-- text blob per bot, written every Cortex.SaveMinutes and at shutdown. Database: acore_playerbots.
-- Base schema for a fresh database; an existing one is migrated by 2026_10_17_00_rename_memory_table.sql.
CREATE TABLE IF NOT EXISTS `playerbots_cortex_memory` (
  `guid` INT UNSIGNED NOT NULL,
  `level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bot level when saved; a higher level than the bot has means it was reset',
  `updated` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix seconds',
  `data` MEDIUMTEXT NOT NULL,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
