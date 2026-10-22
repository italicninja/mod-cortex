-- Bot personas: a bot keeps one persona and prefers "<name>:<persona>" texts.
-- The persona list and its texts are content, filled in separately.

CREATE TABLE IF NOT EXISTS `ai_playerbot_personas` (
  `persona` varchar(32) NOT NULL COMMENT 'key, [a-z0-9_]; texts are named <name>:<persona>',
  `weight` int unsigned NOT NULL DEFAULT 1 COMMENT 'relative chance a new bot rolls this persona',
  `description` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`persona`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_persona` (
  `guid` int unsigned NOT NULL COMMENT 'characters.guid',
  `persona` varchar(32) NOT NULL,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
