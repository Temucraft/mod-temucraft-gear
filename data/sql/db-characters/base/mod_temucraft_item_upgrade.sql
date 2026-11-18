-- mod-temucraft-item-upgrade: upgrade level per item (it travels with the item)
CREATE TABLE IF NOT EXISTS `mod_temucraft_item_upgrade` (
  `item_guid` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid',
  `level` TINYINT UNSIGNED NOT NULL,
  PRIMARY KEY (`item_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
