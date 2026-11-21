-- mod-temucraft-gear: its commands (all under .temucraft) as RBAC permissions 102450-102499. Every command is admin only by
-- default: linked to the administrator roles 192 and 1014. To give one to players, link it to 195 and 1010 here (or
-- grant it per account with .rbac account grant <account> <id>). "Temucraft: use ..." permissions are not
-- commands: they stay linked to the player roles so the items keep working.
DELETE FROM `rbac_linked_permissions` WHERE `linkedId` BETWEEN 102450 AND 102499;
DELETE FROM `rbac_permissions` WHERE `id` BETWEEN 102450 AND 102499;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES
(102450, 'Command: temucraft upgrades'),
(102451, 'Command: temucraft reforge');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(192, 102450),
(1014, 102450),
(192, 102451),
(1014, 102451);
