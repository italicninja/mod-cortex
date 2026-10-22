UPDATE `updates_include`
SET `path` = '$/data/sql/cortex/updates'
WHERE `state` = 'RELEASED';

UPDATE `updates_include`
SET `path` = '$/data/sql/cortex/custom'
WHERE `state` = 'CUSTOM';

UPDATE `updates_include`
SET `path` = '$/data/sql/cortex/archive'
WHERE `state` = 'ARCHIVED';
