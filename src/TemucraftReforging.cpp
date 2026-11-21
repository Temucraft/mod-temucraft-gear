/*
 * mod-temucraft-reforging: Cataclysm-style reforging for 3.3.5. Talk to the Reforger, pick one of your equipped
 * items, pick one of its secondary stats and the stat to move it into: Temucraft.Reforging.Percent of the first stat
 * (40% by default) is taken away and added as the second. One reforge per item; it stays on the item when it is
 * traded or mailed, and can be undone at the Reforger.
 *
 * How the stats change: AzerothCore asks scripts about every item stat when an item's bonuses are applied or
 * removed (OnPlayerApplyItemModsBefore), so the reduced stat is lowered there. The new stat isn't on the item at
 * all, so it is applied once per item next to the item's own bonuses (OnPlayerCustomScalingStatValueBefore runs
 * once at the start of every apply/remove), with a per-slot flag so it can never be applied twice. The client
 * learns the new numbers through a private item tooltip sent only to that player.
 *
 * If mod-temucraft-item-upgrade is installed, its upgrade level is shown in the same tooltip (read from its table,
 * no build dependency).
 *
 * Text comes from module_string (module "mod-temucraft-reforging"), with English built in as a fallback.
 */
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <array>
#include <cmath>
#include <functional>
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* MODULE = "mod-temucraft-reforging";

    // module_string ids, with the English text used when the row is missing
    enum Str : uint32
    {
        S_PICK_ITEM = 1, S_NOTHING, S_RESTORE, S_PICK_SOURCE, S_PICK_TARGET, S_CONFIRM, S_DONE, S_RESTORED,
        S_NO_MONEY, S_BACK, S_CHANGED, S_TOOLTIP, S_DISABLED, S_STATUS_NONE, S_STATUS_LINE
    };
    std::unordered_map<uint32, char const*> const FALLBACK =
    {
        { S_PICK_ITEM,    "Which item should I reforge?" },
        { S_NOTHING,      "None of your equipped items can be reforged." },
        { S_RESTORE,      "Undo the reforge on {}" },
        { S_PICK_SOURCE,  "Reduce {} ({} -> {})" },
        { S_PICK_TARGET,  "Add {} {}" },
        { S_CONFIRM,      "Reforge {}: -{} {}, +{} {}?" },
        { S_DONE,         "{} reforged: -{} {}, +{} {}." },
        { S_RESTORED,     "{} is back to its original stats." },
        { S_NO_MONEY,     "Reforging costs {}." },
        { S_BACK,         "Back" },
        { S_CHANGED,      "The item changed; please choose again." },
        { S_TOOLTIP,      "Reforged: -{} {}, +{} {}" },
        { S_DISABLED,     "Reforging is turned off." },
        { S_STATUS_NONE,  "None of your equipped items is reforged." },
        { S_STATUS_LINE,  "{}: -{} {}, +{} {}" },
    };

    std::string Text(Player* player, uint32 id)
    {
        if (sObjectMgr->GetModuleString(MODULE, id))
            if (std::string const* s = player->GetSession()->GetModuleString(MODULE, id))
                return *s;
        auto it = FALLBACK.find(id);
        return it != FALLBACK.end() ? it->second : "";
    }
    template<typename... Args>
    std::string Text(Player* player, uint32 id, Args&&... args)
    {
        return Acore::StringFormat(Text(player, id), std::forward<Args>(args)...);
    }

    struct Settings
    {
        bool enable = true;
        uint32 percent = 40;
        uint32 cost = 50000;            // copper
        uint32 minQuality = ITEM_QUALITY_UNCOMMON;
        uint32 minItemLevel = 0;
        uint32 creature = 608800, text = 608800;
    } cfg;

    // stats that can be reduced and added (secondary stats, as in Cataclysm, in 3.3.5 terms)
    std::vector<uint32> const REFORGE_STATS = { ITEM_MOD_SPIRIT, ITEM_MOD_DEFENSE_SKILL_RATING, ITEM_MOD_DODGE_RATING, ITEM_MOD_PARRY_RATING,
        ITEM_MOD_HIT_RATING, ITEM_MOD_CRIT_RATING, ITEM_MOD_HASTE_RATING, ITEM_MOD_EXPERTISE_RATING, ITEM_MOD_ARMOR_PENETRATION_RATING };

    char const* StatName(uint32 stat)
    {
        switch (stat)
        {
            case ITEM_MOD_SPIRIT: return "Spirit";
            case ITEM_MOD_DEFENSE_SKILL_RATING: return "Defense Rating";
            case ITEM_MOD_DODGE_RATING: return "Dodge Rating";
            case ITEM_MOD_PARRY_RATING: return "Parry Rating";
            case ITEM_MOD_HIT_RATING: return "Hit Rating";
            case ITEM_MOD_CRIT_RATING: return "Critical Strike Rating";
            case ITEM_MOD_HASTE_RATING: return "Haste Rating";
            case ITEM_MOD_EXPERTISE_RATING: return "Expertise Rating";
            case ITEM_MOD_ARMOR_PENETRATION_RATING: return "Armor Penetration Rating";
            default: return "?";
        }
    }

    struct Reforge { uint32 decrease = 0; uint32 increase = 0; int32 amount = 0; };
    bool upgradeTable = false;
    std::string upgradeLine;
    std::unordered_map<ObjectGuid::LowType, Reforge> reforges;              // item guid -> reforge
    std::unordered_map<ObjectGuid, std::array<bool, EQUIPMENT_SLOT_END>> applied;   // player -> slot has the added stat

    bool IsReforgeStat(uint32 stat) { return std::find(REFORGE_STATS.begin(), REFORGE_STATS.end(), stat) != REFORGE_STATS.end(); }

    int32 ItemStatValue(ItemTemplate const* proto, uint32 stat)
    {
        for (uint8 i = 0; i < proto->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
            if (proto->ItemStat[i].ItemStatType == stat)
                return proto->ItemStat[i].ItemStatValue;
        return 0;
    }

    bool CanReforge(Item* item)
    {
        if (!item) return false;
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Quality < cfg.minQuality || proto->Quality > ITEM_QUALITY_EPIC || proto->ItemLevel < cfg.minItemLevel)
            return false;
        if (proto->ScalingStatDistribution)          // heirlooms scale their stats themselves
            return false;
        for (uint32 stat : REFORGE_STATS)
            if (ItemStatValue(proto, stat) > 0)
                return true;
        return false;
    }

    void ApplyAddedStat(Player* player, uint32 stat, int32 value, bool apply)
    {
        switch (stat)
        {
            case ITEM_MOD_SPIRIT:
                player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT, BASE_VALUE, float(value), apply);
                player->UpdateStatBuffMod(STAT_SPIRIT);
                break;
            case ITEM_MOD_DEFENSE_SKILL_RATING: player->ApplyRatingMod(CR_DEFENSE_SKILL, value, apply); break;
            case ITEM_MOD_DODGE_RATING: player->ApplyRatingMod(CR_DODGE, value, apply); break;
            case ITEM_MOD_PARRY_RATING: player->ApplyRatingMod(CR_PARRY, value, apply); break;
            case ITEM_MOD_HIT_RATING:
                player->ApplyRatingMod(CR_HIT_MELEE, value, apply); player->ApplyRatingMod(CR_HIT_RANGED, value, apply);
                player->ApplyRatingMod(CR_HIT_SPELL, value, apply); break;
            case ITEM_MOD_CRIT_RATING:
                player->ApplyRatingMod(CR_CRIT_MELEE, value, apply); player->ApplyRatingMod(CR_CRIT_RANGED, value, apply);
                player->ApplyRatingMod(CR_CRIT_SPELL, value, apply); break;
            case ITEM_MOD_HASTE_RATING:
                player->ApplyRatingMod(CR_HASTE_MELEE, value, apply); player->ApplyRatingMod(CR_HASTE_RANGED, value, apply);
                player->ApplyRatingMod(CR_HASTE_SPELL, value, apply); break;
            case ITEM_MOD_EXPERTISE_RATING: player->ApplyRatingMod(CR_EXPERTISE, value, apply); break;
            case ITEM_MOD_ARMOR_PENETRATION_RATING: player->ApplyRatingMod(CR_ARMOR_PENETRATION, value, apply); break;
            default: break;
        }
    }

    // SMSG_ITEM_QUERY_SINGLE_RESPONSE for a changed copy of an item template (same layout as WorldSession's handler)
    void SendItemQuery(Player* player, ItemTemplate const& p, std::string const& description)
    {
        std::string name = p.Name1;
        int loc = player->GetSession()->GetSessionDbLocaleIndex();
        if (loc >= 0)
            if (ItemLocale const* il = sObjectMgr->GetItemLocale(p.ItemId))
                ObjectMgr::GetLocaleString(il->Name, loc, name);
        WorldPacket d(SMSG_ITEM_QUERY_SINGLE_RESPONSE, 600);
        d << p.ItemId << p.Class << p.SubClass << p.SoundOverrideSubclass << name << uint8(0) << uint8(0) << uint8(0);
        d << p.DisplayInfoID << p.Quality << p.Flags << p.Flags2 << p.BuyPrice << p.SellPrice << p.InventoryType << p.AllowableClass;
        d << p.AllowableRace << p.ItemLevel << p.RequiredLevel << p.RequiredSkill << p.RequiredSkillRank << p.RequiredSpell;
        d << p.RequiredHonorRank << p.RequiredCityRank << p.RequiredReputationFaction << p.RequiredReputationRank;
        d << int32(p.MaxCount) << int32(p.Stackable) << p.ContainerSlots << p.StatsCount;
        for (uint32 i = 0; i < p.StatsCount; ++i)
            d << p.ItemStat[i].ItemStatType << p.ItemStat[i].ItemStatValue;
        d << p.ScalingStatDistribution << p.ScalingStatValue;
        for (int i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
            d << p.Damage[i].DamageMin << p.Damage[i].DamageMax << p.Damage[i].DamageType;
        d << p.Armor << p.HolyRes << p.FireRes << p.NatureRes << p.FrostRes << p.ShadowRes << p.ArcaneRes;
        d << p.Delay << p.AmmoType << p.RangedModRange;
        for (int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
        {
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(p.Spells[s].SpellId);
            if (spell)
            {
                bool db = p.Spells[s].SpellCooldown >= 0 || p.Spells[s].SpellCategoryCooldown >= 0;
                d << p.Spells[s].SpellId << p.Spells[s].SpellTrigger << int32(p.Spells[s].SpellCharges);
                if (db) d << uint32(p.Spells[s].SpellCooldown) << uint32(p.Spells[s].SpellCategory) << uint32(p.Spells[s].SpellCategoryCooldown);
                else d << uint32(spell->RecoveryTime) << uint32(spell->GetCategory()) << uint32(spell->CategoryRecoveryTime);
            }
            else
                d << uint32(0) << uint32(0) << uint32(0) << uint32(-1) << uint32(0) << uint32(-1);
        }
        d << p.Bonding << description << p.PageText << p.LanguageID << p.PageMaterial << p.StartQuest << p.LockID << int32(p.Material);
        d << p.Sheath << p.RandomProperty << p.RandomSuffix << p.Block << p.ItemSet << p.MaxDurability << p.Area << p.Map;
        d << p.BagFamily << p.TotemCategory;
        for (int s = 0; s < MAX_ITEM_PROTO_SOCKETS; ++s)
            d << p.Socket[s].Color << p.Socket[s].Content;
        d << p.socketBonus << p.GemProperties << p.RequiredDisenchantSkill << p.ArmorDamageModifier << p.Duration;
        d << p.ItemLimitCategory << p.HolidayId;
        player->GetSession()->SendPacket(&d);
    }

    // A private copy of the item's tooltip with the reforged numbers, sent only to this player.
    void SendTooltip(Player* player, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        ItemTemplate copy = *proto;
        std::string description = proto->Description;
        if (upgradeTable)    // mod-temucraft-item-upgrade is installed: show its upgrade level in the same tooltip
            if (QueryResult u = CharacterDatabase.Query("SELECT level FROM mod_temucraft_item_upgrade WHERE item_guid = {}", item->GetGUID().GetCounter()))
            {
                uint32 level = u->Fetch()[0].Get<uint32>();
                float f = 1.0f + level * sConfigMgr->GetOption<float>("Temucraft.ItemUpgrade.PercentPerLevel", 4.0f) / 100.0f;
                for (uint8 i = 0; i < copy.StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
                    copy.ItemStat[i].ItemStatValue = int32(std::lround(copy.ItemStat[i].ItemStatValue * f));
                copy.Armor = uint32(std::lround(copy.Armor * f));
                for (auto& dmg : copy.Damage) { dmg.DamageMin *= f; dmg.DamageMax *= f; }
                copy.ItemLevel += level * sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.ItemLevelPerLevel", 4);
                upgradeLine = Acore::StringFormat("Upgrade Level: {}/{}", level, sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.MaxLevel", 2));
            }
        auto it = reforges.find(item->GetGUID().GetCounter());
        if (it != reforges.end())
        {
            Reforge const& r = it->second;
            bool added = false;
            for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
            {
                if (copy.ItemStat[i].ItemStatType == r.decrease && i < copy.StatsCount)
                    copy.ItemStat[i].ItemStatValue -= r.amount;
                else if (!added && i >= copy.StatsCount)
                {
                    copy.ItemStat[i].ItemStatType = r.increase; copy.ItemStat[i].ItemStatValue = r.amount;
                    added = true;
                }
            }
            if (added) ++copy.StatsCount;
            description = Text(player, S_TOOLTIP, r.amount, StatName(r.decrease), r.amount, StatName(r.increase));
        }
        if (!upgradeLine.empty()) description = upgradeLine + (description.empty() ? "" : " - " + description);
        upgradeLine.clear();
        SendItemQuery(player, copy, description);
    }

    void LoadPlayer(Player* player)
    {
        QueryResult result = CharacterDatabase.Query("SELECT r.item_guid, r.decrease_stat, r.increase_stat, r.amount FROM mod_temucraft_reforge r "
                                                     "JOIN item_instance i ON i.guid = r.item_guid WHERE i.owner_guid = {}", player->GetGUID().GetCounter());
        if (!result) return;
        do
        {
            Field* f = result->Fetch();
            reforges[f[0].Get<uint32>()] = { f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<int32>() };
        } while (result->NextRow());
    }

    Reforge const* Find(Player* player, uint8 slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item) return nullptr;
        auto it = reforges.find(item->GetGUID().GetCounter());
        return it != reforges.end() ? &it->second : nullptr;
    }

    std::string ItemName(Item* item) { return item->GetTemplate()->Name1; }
}

class TemucraftReforging_Player : public PlayerScript
{
public:
    TemucraftReforging_Player() : PlayerScript("TemucraftReforging_Player", {
        PLAYERHOOK_ON_LOAD_FROM_DB, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_APPLY_ITEM_MODS_BEFORE, PLAYERHOOK_ON_CUSTOM_SCALING_STAT_VALUE_BEFORE,
        PLAYERHOOK_ON_EQUIP }) { }

    // runs before the inventory is loaded, so the items' bonuses are applied with the reforges already known
    void OnPlayerLoadFromDB(Player* player) override { LoadPlayer(player); }

    void OnPlayerLogin(Player* player) override
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (reforges.count(item->GetGUID().GetCounter()))
                    SendTooltip(player, item);
    }

    void OnPlayerLogout(Player* player) override { applied.erase(player->GetGUID()); }

    void OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        if (reforges.count(item->GetGUID().GetCounter()))
            SendTooltip(player, item);
    }

    void OnPlayerApplyItemModsBefore(Player* player, uint8 slot, bool /*apply*/, uint8 /*statIndex*/, uint32 statType, int32& val) override
    {
        if (!cfg.enable || slot >= EQUIPMENT_SLOT_END) return;
        if (Reforge const* r = Find(player, slot))
            if (r->decrease == statType)
                val -= r->amount;
    }

    // runs once at the start of every apply/remove of an item's bonuses (and of its weapon damage)
    void OnPlayerCustomScalingStatValueBefore(Player* player, ItemTemplate const* /*proto*/, uint8 slot, bool apply, uint32& /*value*/) override
    {
        if (slot >= EQUIPMENT_SLOT_END) return;
        auto& flags = applied[player->GetGUID()];
        Reforge const* r = cfg.enable ? Find(player, slot) : nullptr;
        if (apply && r && !flags[slot])
        {
            ApplyAddedStat(player, r->increase, r->amount, true);
            flags[slot] = true;
            pending[player->GetGUID()][slot] = *r;
        }
        else if (!apply && flags[slot])
        {
            Reforge const& was = pending[player->GetGUID()][slot];   // what was added, even if the reforge changed since
            ApplyAddedStat(player, was.increase, was.amount, false);
            flags[slot] = false;
        }
    }

    static std::unordered_map<ObjectGuid, std::array<Reforge, EQUIPMENT_SLOT_END>> pending;
};
std::unordered_map<ObjectGuid, std::array<Reforge, EQUIPMENT_SLOT_END>> TemucraftReforging_Player::pending;

namespace
{
    // gossip actions: slot*1000 + step data
    enum : uint32 { ACT_ITEMS = 1, ACT_ITEM = 100, ACT_SOURCE = 10000, ACT_TARGET = 20000, ACT_RESTORE = 30000, ACT_CONFIRM = 40000 };
    struct Choice { uint8 slot = 0; uint32 source = 0; uint32 target = 0; ObjectGuid::LowType item = 0; };
    std::unordered_map<ObjectGuid, Choice> choice;

    std::string Money(uint32 copper)
    {
        uint32 g = copper / 10000, s = (copper % 10000) / 100, c = copper % 100;
        std::string out;
        if (g) out += std::to_string(g) + "g ";
        if (s || g) out += std::to_string(s) + "s ";
        out += std::to_string(c) + "c";
        return out;
    }

    void ShowItems(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        bool any = false;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!CanReforge(item)) continue;
            any = true;
            std::string label = ItemName(item);
            if (Reforge const* r = Find(player, slot))
                label += " (" + Text(player, S_STATUS_LINE, "", r->amount, StatName(r->decrease), r->amount, StatName(r->increase)).substr(2) + ")";
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label, GOSSIP_SENDER_MAIN, ACT_ITEM + slot);
        }
        if (!any)
            ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_NOTHING));
        SendGossipMenuFor(player, cfg.text, creature->GetGUID());
    }

    void ShowSources(Player* player, Creature* creature, uint8 slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!CanReforge(item)) { ShowItems(player, creature); return; }
        ClearGossipMenuFor(player);
        choice[player->GetGUID()] = { slot, 0, 0, item->GetGUID().GetCounter() };
        ItemTemplate const* proto = item->GetTemplate();
        if (Find(player, slot))
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, Text(player, S_RESTORE, ItemName(item)), GOSSIP_SENDER_MAIN, ACT_RESTORE + slot);
        else
            for (uint32 stat : REFORGE_STATS)
                if (int32 v = ItemStatValue(proto, stat); v > 0)
                {
                    int32 amount = v * int32(cfg.percent) / 100;
                    if (amount > 0)
                        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, Text(player, S_PICK_SOURCE, StatName(stat), v, v - amount), GOSSIP_SENDER_MAIN, ACT_SOURCE + stat);
                }
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Text(player, S_BACK), GOSSIP_SENDER_MAIN, ACT_ITEMS);
        SendGossipMenuFor(player, cfg.text, creature->GetGUID());
    }

    void ShowTargets(Player* player, Creature* creature, uint32 source)
    {
        Choice& c = choice[player->GetGUID()];
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, c.slot);
        if (!item || item->GetGUID().GetCounter() != c.item) { ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_CHANGED)); ShowItems(player, creature); return; }
        c.source = source;
        ItemTemplate const* proto = item->GetTemplate();
        int32 amount = ItemStatValue(proto, source) * int32(cfg.percent) / 100;
        ClearGossipMenuFor(player);
        for (uint32 stat : REFORGE_STATS)
            if (stat != source && ItemStatValue(proto, stat) == 0)
                AddGossipItemFor(player, GOSSIP_ICON_TRAINER, Text(player, S_PICK_TARGET, amount, StatName(stat)), GOSSIP_SENDER_MAIN, ACT_TARGET + stat,
                                 Text(player, S_CONFIRM, ItemName(item), amount, StatName(source), amount, StatName(stat)), cfg.cost, false);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Text(player, S_BACK), GOSSIP_SENDER_MAIN, ACT_ITEM + c.slot);
        SendGossipMenuFor(player, cfg.text, creature->GetGUID());
    }

    void Reapply(Player* player, Item* item, uint8 slot, std::function<void()> change)
    {
        player->_ApplyItemMods(item, slot, false);
        change();
        player->_ApplyItemMods(item, slot, true);
        SendTooltip(player, item);
    }

    void DoReforge(Player* player, Creature* creature, uint32 target)
    {
        Choice c = choice[player->GetGUID()];
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, c.slot);
        if (!item || item->GetGUID().GetCounter() != c.item || !CanReforge(item) || !IsReforgeStat(c.source) || !IsReforgeStat(target) ||
            Find(player, c.slot) || ItemStatValue(item->GetTemplate(), target) != 0)
        {
            ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_CHANGED)); ShowItems(player, creature); return;
        }
        if (!player->HasEnoughMoney(cfg.cost))
        {
            ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_NO_MONEY, Money(cfg.cost))); CloseGossipMenuFor(player); return;
        }
        int32 amount = ItemStatValue(item->GetTemplate(), c.source) * int32(cfg.percent) / 100;
        player->ModifyMoney(-int32(cfg.cost));
        Reapply(player, item, c.slot, [&]() { reforges[c.item] = { c.source, target, amount }; });
        CharacterDatabase.Execute("REPLACE INTO mod_temucraft_reforge (item_guid, decrease_stat, increase_stat, amount) VALUES ({}, {}, {}, {})",
                                  c.item, c.source, target, amount);
        ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_DONE, ItemName(item), amount, StatName(c.source), amount, StatName(target)));
        ShowItems(player, creature);
    }

    void DoRestore(Player* player, Creature* creature, uint8 slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !Find(player, slot)) { ShowItems(player, creature); return; }
        ObjectGuid::LowType low = item->GetGUID().GetCounter();
        Reapply(player, item, slot, [&]() { reforges.erase(low); });
        CharacterDatabase.Execute("DELETE FROM mod_temucraft_reforge WHERE item_guid = {}", low);
        ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_RESTORED, ItemName(item)));
        ShowItems(player, creature);
    }
}

class TemucraftReforging_Creature : public AllCreatureScript
{
public:
    TemucraftReforging_Creature() : AllCreatureScript("TemucraftReforging_Creature") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (creature->GetEntry() != cfg.creature) return false;
        if (!cfg.enable) { ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_DISABLED)); return true; }
        ShowItems(player, creature);
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (creature->GetEntry() != cfg.creature) return false;
        if (action == ACT_ITEMS) ShowItems(player, creature);
        else if (action >= ACT_ITEM && action < uint32(ACT_ITEM) + uint32(EQUIPMENT_SLOT_END)) ShowSources(player, creature, uint8(action - ACT_ITEM));
        else if (action >= ACT_SOURCE && action < ACT_TARGET) ShowTargets(player, creature, action - ACT_SOURCE);
        else if (action >= ACT_TARGET && action < ACT_RESTORE) DoReforge(player, creature, action - ACT_TARGET);
        else if (action >= ACT_RESTORE && action < ACT_CONFIRM) DoRestore(player, creature, uint8(action - ACT_RESTORE));
        else CloseGossipMenuFor(player);
        return true;
    }
};

class TemucraftReforging_Command : public CommandScript
{
public:
    TemucraftReforging_Command() : CommandScript("TemucraftReforging_Command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table = { { "reforge", HandleReforge, SEC_PLAYER, Console::No } };
        return table;
    }

    // .reforge: what is reforged on your equipped items
    static bool HandleReforge(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        bool any = false;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Reforge const* r = Find(player, slot))
            {
                Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                handler->SendSysMessage(Text(player, S_STATUS_LINE, ItemName(item), r->amount, StatName(r->decrease), r->amount, StatName(r->increase)));
                any = true;
            }
        if (!any) handler->SendSysMessage(Text(player, S_STATUS_NONE));
        return true;
    }
};

class TemucraftReforging_World : public WorldScript
{
public:
    TemucraftReforging_World() : WorldScript("TemucraftReforging_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        upgradeTable = bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_temucraft_item_upgrade'"));
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        cfg.enable = sConfigMgr->GetOption<bool>("Temucraft.Reforging.Enable", true);
        cfg.percent = std::min<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.Reforging.Percent", 40), 100);
        cfg.cost = sConfigMgr->GetOption<uint32>("Temucraft.Reforging.Cost", 50000);
        cfg.minQuality = sConfigMgr->GetOption<uint32>("Temucraft.Reforging.MinQuality", ITEM_QUALITY_UNCOMMON);
        cfg.minItemLevel = sConfigMgr->GetOption<uint32>("Temucraft.Reforging.MinItemLevel", 0);
        cfg.creature = sConfigMgr->GetOption<uint32>("Temucraft.Reforging.Creature", 608800);
        cfg.text = sConfigMgr->GetOption<uint32>("Temucraft.Reforging.Text", 608800);
    }
};

void AddTemucraftReforgingScripts()
{
    new TemucraftReforging_Player();
    new TemucraftReforging_Creature();
    new TemucraftReforging_Command();
    new TemucraftReforging_World();
}
