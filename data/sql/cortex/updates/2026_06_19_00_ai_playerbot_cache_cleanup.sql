-- Flush all caches after RandomItemMgr changes. Tables will be repopulated with
-- the correct entries on the next start.
TRUNCATE TABLE `cortex_equip_cache`;
TRUNCATE TABLE `cortex_rnditem_cache`;
TRUNCATE TABLE `cortex_rarity_cache`;
