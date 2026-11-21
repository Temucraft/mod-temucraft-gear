-- mod-temucraft-reforging: the Reforger, its greeting and the module's texts
SET @Reforger := 608800;
DELETE FROM `creature_template` WHERE `entry` = @Reforger;
INSERT INTO `creature_template` (`entry`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`,
  `speed_walk`, `speed_run`, `rank`, `unit_class`, `unit_flags`, `type`, `type_flags`, `AIName`, `MovementType`, `HealthModifier`, `RegenHealth`,
  `flags_extra`, `ScriptName`) VALUES
(@Reforger, 'Arcanist Veyra', 'Reforger', '', 0, 80, 80, 2, 35, 1, 1, 1.14286, 0, 1, 768, 7, 0, '', 0, 1, 1, 2, '');
DELETE FROM `creature_template_model` WHERE `CreatureID` = @Reforger;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT @Reforger, 0, `CreatureDisplayID`, 1, 1, 0 FROM `creature_template_model` WHERE `CreatureID` = 28693 AND `Idx` = 0;

DELETE FROM `npc_text` WHERE `ID` = @Reforger;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `Probability0`) VALUES
(@Reforger, 'Every piece of gear has a little more to give than its maker intended. Show me what you are wearing and I will move its strengths where you need them.', '', 1);

DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-reforging';
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
('mod-temucraft-reforging', 1, 'Which item should I reforge?'),
('mod-temucraft-reforging', 2, 'None of your equipped items can be reforged.'),
('mod-temucraft-reforging', 3, 'Undo the reforge on {}'),
('mod-temucraft-reforging', 4, 'Reduce {} ({} -> {})'),
('mod-temucraft-reforging', 5, 'Add {} {}'),
('mod-temucraft-reforging', 6, 'Reforge {}: -{} {}, +{} {}?'),
('mod-temucraft-reforging', 7, '{} reforged: -{} {}, +{} {}.'),
('mod-temucraft-reforging', 8, '{} is back to its original stats.'),
('mod-temucraft-reforging', 9, 'Reforging costs {}.'),
('mod-temucraft-reforging', 10, 'Back'),
('mod-temucraft-reforging', 11, 'The item changed; please choose again.'),
('mod-temucraft-reforging', 12, 'Reforged: -{} {}, +{} {}'),
('mod-temucraft-reforging', 13, 'Reforging is turned off.'),
('mod-temucraft-reforging', 14, 'None of your equipped items is reforged.'),
('mod-temucraft-reforging', 15, '{}: -{} {}, +{} {}');
-- Translations go in `module_string_locale` (`module`, `id`, `locale`, `string`), e.g. ('mod-temucraft-reforging', 10, 'frFR', 'Retour').
