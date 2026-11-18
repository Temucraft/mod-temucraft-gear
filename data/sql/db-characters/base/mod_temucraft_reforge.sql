-- mod-temucraft-reforging: one reforge per item (it travels with the item)
CREATE TABLE IF NOT EXISTS `mod_temucraft_reforge` (
  `item_guid` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid',
  `decrease_stat` TINYINT UNSIGNED NOT NULL COMMENT 'ITEM_MOD_* that is lowered',
  `increase_stat` TINYINT UNSIGNED NOT NULL COMMENT 'ITEM_MOD_* that is added',
  `amount` INT NOT NULL COMMENT 'how much was moved',
  PRIMARY KEY (`item_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
