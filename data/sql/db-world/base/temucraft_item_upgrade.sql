-- mod-temucraft-item-upgrade: the Upgrade Vendor, its greeting and the module's texts
DELETE FROM `creature_template` WHERE `entry` = 608801;
INSERT INTO `creature_template` (`entry`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`,
  `speed_walk`, `speed_run`, `rank`, `unit_class`, `unit_flags`, `type`, `type_flags`, `AIName`, `MovementType`, `HealthModifier`, `RegenHealth`,
  `flags_extra`, `ScriptName`) VALUES
(608801, 'Smith Torvald', 'Item Upgrades', '', 0, 80, 80, 2, 35, 1, 1, 1.14286, 0, 1, 768, 7, 0, '', 0, 1, 1, 2, '');
DELETE FROM `creature_template_model` WHERE `CreatureID` = 608801;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 608801, 0, `CreatureDisplayID`, 1, 1, 0 FROM `creature_template_model` WHERE `CreatureID` = 28714 AND `Idx` = 0;

DELETE FROM `npc_text` WHERE `ID` = 608801;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `Probability0`) VALUES
(608801, 'Good steel can always be made better. Bring me what you wear and the means to pay, and I will hammer more out of it.', '', 1);

DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-item-upgrade';
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
('mod-temucraft-item-upgrade', 1, '{} (upgrade {}/{})'),
('mod-temucraft-item-upgrade', 2, 'None of your equipped items can be upgraded.'),
('mod-temucraft-item-upgrade', 3, 'Upgrade {} to level {}/{} for {}?'),
('mod-temucraft-item-upgrade', 4, '{} is now upgrade level {}/{}.'),
('mod-temucraft-item-upgrade', 5, '{} is fully upgraded.'),
('mod-temucraft-item-upgrade', 6, 'You need {} to upgrade this item.'),
('mod-temucraft-item-upgrade', 7, 'The item changed; please choose again.'),
('mod-temucraft-item-upgrade', 8, 'Upgrade Level: {}/{}'),
('mod-temucraft-item-upgrade', 9, 'Item upgrades are turned off.'),
('mod-temucraft-item-upgrade', 10, '{}: upgrade {}/{}'),
('mod-temucraft-item-upgrade', 11, 'None of your equipped items is upgraded.'),
('mod-temucraft-item-upgrade', 12, 'Reforged: -{} {}, +{} {}');
-- Translations go in `module_string_locale` (`module`, `id`, `locale`, `string`).
