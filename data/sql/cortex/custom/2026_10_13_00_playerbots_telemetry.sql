-- Bot telemetry (patch 0003-bot-telemetry). Written by BotTelemetry, read by scripts/bot-report.
CREATE TABLE IF NOT EXISTS `cortex_telemetry` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `ts` BIGINT UNSIGNED NOT NULL COMMENT 'unix ms',
  `guid` INT UNSIGNED NOT NULL,
  `event` VARCHAR(12) NOT NULL,
  `state` VARCHAR(12) NOT NULL DEFAULT '',
  `info` VARCHAR(48) NOT NULL DEFAULT '',
  `a` INT NOT NULL DEFAULT 0,
  `b` INT NOT NULL DEFAULT 0,
  `c` INT NOT NULL DEFAULT 0,
  `map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `zone` INT UNSIGNED NOT NULL DEFAULT 0,
  `x` FLOAT NOT NULL DEFAULT 0,
  `y` FLOAT NOT NULL DEFAULT 0,
  `z` FLOAT NOT NULL DEFAULT 0,
  `level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_ts` (`ts`),
  KEY `idx_event_ts` (`event`, `ts`),
  KEY `idx_guid_ts` (`guid`, `ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
