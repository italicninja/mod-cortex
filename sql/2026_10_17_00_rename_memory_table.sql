-- Cortex was called the player brain until 2026-10-17: its memory table moves from playerbots_brain_memory to
-- playerbots_cortex_memory, data kept. Database: acore_playerbots. Safe to run twice: it renames only when the old
-- table exists and the new one doesn't, and otherwise does nothing (if both exist it says so and leaves them).
-- Run with the worldserver stopped (it writes the table at shutdown and reads it as bots log in).
SET @old_exists := (SELECT COUNT(*) FROM information_schema.TABLES
                    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'playerbots_brain_memory');
SET @new_exists := (SELECT COUNT(*) FROM information_schema.TABLES
                    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'playerbots_cortex_memory');
SET @stmt := IF(@old_exists = 1 AND @new_exists = 0,
                'RENAME TABLE `playerbots_brain_memory` TO `playerbots_cortex_memory`',
                'DO 0');
PREPARE rename_memory FROM @stmt;
EXECUTE rename_memory;
DEALLOCATE PREPARE rename_memory;
SELECT CASE
         WHEN @old_exists = 1 AND @new_exists = 0 THEN 'renamed playerbots_brain_memory to playerbots_cortex_memory'
         WHEN @old_exists = 1 AND @new_exists = 1 THEN 'both tables exist: nothing renamed, check by hand'
         WHEN @new_exists = 1 THEN 'already migrated'
         ELSE 'no memory table yet: apply playerbots_cortex_memory.sql'
       END AS result;
