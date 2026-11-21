/*
 * mod-temucraft-item-upgrade: Mists-style item upgrades. The Upgrade Vendor raises an equipped item by one upgrade
 * level at a time (Temucraft.ItemUpgrade.MaxLevel levels); every level adds Temucraft.ItemUpgrade.PercentPerLevel
 * percent to the item's stats, armor and weapon damage. Each level costs an item (Temucraft.ItemUpgrade.CostItem /
 * CostCount; 0 = none) and gold. Upgrades stay on the item when it is traded or mailed.
 *
 * How the stats change: every item stat passes through OnPlayerApplyItemModsBefore (scaled there), weapon damage
 * through OnPlayerApplyWeaponDamage, and the extra armor is applied once per item next to the item's own bonuses
 * (OnPlayerCustomScalingStatValueBefore), with a per-slot flag so it is never applied twice. The owner gets a
 * private tooltip with the upgraded numbers. If mod-temucraft-reforging is installed, its reforge is shown in the
 * same tooltip (read from its table, no build dependency).
 *
 * Text comes from module_string (module "mod-temucraft-item-upgrade"), with English built in as a fallback.
 */
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <array>
#include <cmath>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* MODULE = "mod-temucraft-item-upgrade";

    enum Str : uint32 { S_ITEM = 1, S_NOTHING, S_CONFIRM, S_DONE, S_MAXED, S_NO_COST, S_CHANGED, S_TOOLTIP, S_DISABLED, S_STATUS, S_NONE, S_REFORGED };
    std::unordered_map<uint32, char const*> const FALLBACK =
    {
        { S_ITEM,     "{} (upgrade {}/{})" },
        { S_NOTHING,  "None of your equipped items can be upgraded." },
        { S_CONFIRM,  "Upgrade {} to level {}/{} for {}?" },
        { S_DONE,     "{} is now upgrade level {}/{}." },
        { S_MAXED,    "{} is fully upgraded." },
        { S_NO_COST,  "You need {} to upgrade this item." },
        { S_CHANGED,  "The item changed; please choose again." },
        { S_TOOLTIP,  "Upgrade Level: {}/{}" },
        { S_DISABLED, "Item upgrades are turned off." },
        { S_STATUS,   "{}: upgrade {}/{}" },
        { S_NONE,     "None of your equipped items is upgraded." },
        { S_REFORGED, "Reforged: -{} {}, +{} {}" },
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
    std::string Text(Player* player, uint32 id, Args&&... args) { return Acore::StringFormat(Text(player, id), std::forward<Args>(args)...); }

    struct Settings
    {
        bool enable = true;
        uint32 maxLevel = 2;
        float percent = 4.0f;            // per level
        uint32 costItem = 0, costCount = 0, costGold = 100000;
        uint32 minQuality = ITEM_QUALITY_RARE, minItemLevel = 200;
        uint32 itemLevelPerLevel = 4;
        uint32 creature = 608801, text = 608801;
    } cfg;

    std::unordered_map<ObjectGuid::LowType, uint8> upgrades;                                 // item guid -> level
    std::unordered_map<ObjectGuid, std::array<int32, EQUIPMENT_SLOT_END>> armorAdded;        // player -> armor added per slot (0 = none)
    bool reforgeTable = false;

    float Factor(uint8 level) { return 1.0f + level * cfg.percent / 100.0f; }

    uint8 LevelOf(Item* item)
    {
        if (!item) return 0;
        auto it = upgrades.find(item->GetGUID().GetCounter());
        return it != upgrades.end() ? it->second : 0;
    }
    uint8 LevelAt(Player* player, uint8 slot) { return slot < EQUIPMENT_SLOT_END ? LevelOf(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) : 0; }

    bool CanUpgrade(Item* item)
    {
        if (!item) return false;
        ItemTemplate const* p = item->GetTemplate();
        return (p->Class == ITEM_CLASS_WEAPON || p->Class == ITEM_CLASS_ARMOR) && p->Quality >= cfg.minQuality && p->Quality <= ITEM_QUALITY_EPIC &&
               p->ItemLevel >= cfg.minItemLevel && !p->ScalingStatDistribution;
    }

    std::string Cost(Player* player)
    {
        std::string out;
        if (cfg.costItem && cfg.costCount)
            if (ItemTemplate const* t = sObjectMgr->GetItemTemplate(cfg.costItem))
                out = std::to_string(cfg.costCount) + "x " + t->Name1;
        if (cfg.costGold)
        {
            if (!out.empty()) out += " + ";
            out += std::to_string(cfg.costGold / 10000) + "g";
        }
        (void)player;
        return out.empty() ? "nothing" : out;
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

    char const* StatName(uint32 stat)
    {
        switch (stat)
        {
            case ITEM_MOD_SPIRIT: return "Spirit"; case ITEM_MOD_DEFENSE_SKILL_RATING: return "Defense Rating";
            case ITEM_MOD_DODGE_RATING: return "Dodge Rating"; case ITEM_MOD_PARRY_RATING: return "Parry Rating";
            case ITEM_MOD_HIT_RATING: return "Hit Rating"; case ITEM_MOD_CRIT_RATING: return "Critical Strike Rating";
            case ITEM_MOD_HASTE_RATING: return "Haste Rating"; case ITEM_MOD_EXPERTISE_RATING: return "Expertise Rating";
            case ITEM_MOD_ARMOR_PENETRATION_RATING: return "Armor Penetration Rating"; default: return "?";
        }
    }

    void SendTooltip(Player* player, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        ItemTemplate copy = *proto;
        std::string description = proto->Description;
        uint8 level = LevelOf(item);
        float f = Factor(level);
        if (level)
        {
            for (uint8 i = 0; i < copy.StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
                copy.ItemStat[i].ItemStatValue = int32(std::lround(copy.ItemStat[i].ItemStatValue * f));
            copy.Armor = uint32(std::lround(copy.Armor * f));
            for (auto& dmg : copy.Damage) { dmg.DamageMin *= f; dmg.DamageMax *= f; }
            copy.ItemLevel += level * cfg.itemLevelPerLevel;
            description = Text(player, S_TOOLTIP, level, cfg.maxLevel);
        }
        if (reforgeTable)    // mod-temucraft-reforging is installed: show its reforge in the same tooltip
            if (QueryResult r = CharacterDatabase.Query("SELECT decrease_stat, increase_stat, amount FROM mod_temucraft_reforge WHERE item_guid = {}", item->GetGUID().GetCounter()))
            {
                uint32 dec = r->Fetch()[0].Get<uint32>(), inc = r->Fetch()[1].Get<uint32>(); int32 amount = r->Fetch()[2].Get<int32>();
                bool added = false;
                for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
                {
                    if (i < copy.StatsCount && copy.ItemStat[i].ItemStatType == dec) copy.ItemStat[i].ItemStatValue -= amount;
                    else if (!added && i >= copy.StatsCount) { copy.ItemStat[i].ItemStatType = inc; copy.ItemStat[i].ItemStatValue = amount; added = true; }
                }
                if (added) ++copy.StatsCount;
                std::string line = Text(player, S_REFORGED, amount, StatName(dec), amount, StatName(inc));
                description = description.empty() ? line : description + " - " + line;
            }
        SendItemQuery(player, copy, description);
    }
}

class TemucraftItemUpgrade_Player : public PlayerScript
{
public:
    TemucraftItemUpgrade_Player() : PlayerScript("TemucraftItemUpgrade_Player", {
        PLAYERHOOK_ON_LOAD_FROM_DB, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_EQUIP, PLAYERHOOK_ON_APPLY_ITEM_MODS_BEFORE,
        PLAYERHOOK_ON_APPLY_WEAPON_DAMAGE, PLAYERHOOK_ON_CUSTOM_SCALING_STAT_VALUE_BEFORE }) { }

    // before the inventory loads, so the items' bonuses are applied already upgraded
    void OnPlayerLoadFromDB(Player* player) override
    {
        QueryResult result = CharacterDatabase.Query("SELECT u.item_guid, u.level FROM mod_temucraft_item_upgrade u JOIN item_instance i ON i.guid = u.item_guid "
                                                     "WHERE i.owner_guid = {}", player->GetGUID().GetCounter());
        if (!result) return;
        do upgrades[result->Fetch()[0].Get<uint32>()] = result->Fetch()[1].Get<uint8>(); while (result->NextRow());
    }

    void OnPlayerLogin(Player* player) override
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (LevelOf(item)) SendTooltip(player, item);
    }

    void OnPlayerLogout(Player* player) override { armorAdded.erase(player->GetGUID()); }

    void OnPlayerEquip(Player* player, Item* item, uint8, uint8, bool) override { if (LevelOf(item)) SendTooltip(player, item); }

    void OnPlayerApplyItemModsBefore(Player* player, uint8 slot, bool, uint8, uint32, int32& val) override
    {
        if (!cfg.enable) return;
        if (uint8 level = LevelAt(player, slot))
            val = int32(std::lround(val * Factor(level)));
    }

    void OnPlayerApplyWeaponDamage(Player* player, uint8 slot, ItemTemplate const*, float& minDamage, float& maxDamage, uint8) override
    {
        if (!cfg.enable) return;
        if (uint8 level = LevelAt(player, slot)) { minDamage *= Factor(level); maxDamage *= Factor(level); }
    }

    // once per apply/remove of an item's bonuses: the extra armor
    void OnPlayerCustomScalingStatValueBefore(Player* player, ItemTemplate const* proto, uint8 slot, bool apply, uint32&) override
    {
        if (slot >= EQUIPMENT_SLOT_END) return;
        int32& added = armorAdded[player->GetGUID()][slot];
        if (apply && !added && cfg.enable)
        {
            if (uint8 level = LevelAt(player, slot))
            {
                added = int32(std::lround(proto->Armor * (Factor(level) - 1.0f)));
                if (added) player->HandleStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, float(added), true);
            }
        }
        else if (!apply && added)
        {
            player->HandleStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, float(added), false);
            added = 0;
        }
    }
};

namespace
{
    enum : uint32 { ACT_ITEM = 100, ACT_UPGRADE = 200 };
    std::unordered_map<ObjectGuid, ObjectGuid::LowType> chosen;

    void ShowItems(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        bool any = false;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!CanUpgrade(item)) continue;
            any = true;
            uint8 level = LevelOf(item);
            std::string label = Text(player, S_ITEM, item->GetTemplate()->Name1, level, cfg.maxLevel);
            if (level >= cfg.maxLevel)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, GOSSIP_SENDER_MAIN, ACT_ITEM + slot);
            else
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label, GOSSIP_SENDER_MAIN, ACT_UPGRADE + slot,
                                 Text(player, S_CONFIRM, item->GetTemplate()->Name1, level + 1, cfg.maxLevel, Cost(player)), cfg.costGold, false);
        }
        if (!any) ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_NOTHING));
        SendGossipMenuFor(player, cfg.text, creature->GetGUID());
    }

    void DoUpgrade(Player* player, Creature* creature, uint8 slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!CanUpgrade(item)) { ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_CHANGED)); ShowItems(player, creature); return; }
        uint8 level = LevelOf(item);
        if (level >= cfg.maxLevel) { ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_MAXED, item->GetTemplate()->Name1)); ShowItems(player, creature); return; }
        if (!player->HasEnoughMoney(cfg.costGold) || (cfg.costItem && cfg.costCount && !player->HasItemCount(cfg.costItem, cfg.costCount)))
        {
            ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_NO_COST, Cost(player))); CloseGossipMenuFor(player); return;
        }
        if (cfg.costGold) player->ModifyMoney(-int32(cfg.costGold));
        if (cfg.costItem && cfg.costCount) player->DestroyItemCount(cfg.costItem, cfg.costCount, true);
        ObjectGuid::LowType low = item->GetGUID().GetCounter();
        player->_ApplyItemMods(item, slot, false);
        upgrades[low] = level + 1;
        player->_ApplyItemMods(item, slot, true);
        CharacterDatabase.Execute("REPLACE INTO mod_temucraft_item_upgrade (item_guid, level) VALUES ({}, {})", low, level + 1);
        SendTooltip(player, item);
        ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_DONE, item->GetTemplate()->Name1, level + 1, cfg.maxLevel));
        ShowItems(player, creature);
    }
}

class TemucraftItemUpgrade_Creature : public AllCreatureScript
{
public:
    TemucraftItemUpgrade_Creature() : AllCreatureScript("TemucraftItemUpgrade_Creature") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (creature->GetEntry() != cfg.creature) return false;
        if (!cfg.enable) { ChatHandler(player->GetSession()).SendSysMessage(Text(player, S_DISABLED)); return true; }
        ShowItems(player, creature);
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32, uint32 action) override
    {
        if (creature->GetEntry() != cfg.creature) return false;
        if (action >= ACT_UPGRADE && action < uint32(ACT_UPGRADE) + uint32(EQUIPMENT_SLOT_END)) DoUpgrade(player, creature, uint8(action - ACT_UPGRADE));
        else ShowItems(player, creature);
        return true;
    }
};

class TemucraftItemUpgrade_Command : public CommandScript
{
public:
    TemucraftItemUpgrade_Command() : CommandScript("TemucraftItemUpgrade_Command") { }
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table = { { "upgrades", HandleUpgrades, SEC_PLAYER, Console::No } };
        return table;
    }
    // .upgrades: upgrade levels of your equipped items
    static bool HandleUpgrades(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer(); bool any = false;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (uint8 level = LevelOf(item)) { handler->SendSysMessage(Text(player, S_STATUS, item->GetTemplate()->Name1, level, cfg.maxLevel)); any = true; }
        if (!any) handler->SendSysMessage(Text(player, S_NONE));
        return true;
    }
};

class TemucraftItemUpgrade_World : public WorldScript
{
public:
    TemucraftItemUpgrade_World() : WorldScript("TemucraftItemUpgrade_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool) override
    {
        cfg.enable = sConfigMgr->GetOption<bool>("Temucraft.ItemUpgrade.Enable", true);
        cfg.maxLevel = std::min<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.MaxLevel", 2), 10);
        cfg.percent = sConfigMgr->GetOption<float>("Temucraft.ItemUpgrade.PercentPerLevel", 4.0f);
        cfg.costItem = sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.CostItem", 0);
        cfg.costCount = sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.CostCount", 0);
        cfg.costGold = sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.CostGold", 100000);
        cfg.minQuality = sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.MinQuality", ITEM_QUALITY_RARE);
        cfg.minItemLevel = sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.MinItemLevel", 200);
        cfg.itemLevelPerLevel = sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.ItemLevelPerLevel", 4);
        cfg.creature = sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.Creature", 608801);
        cfg.text = sConfigMgr->GetOption<uint32>("Temucraft.ItemUpgrade.Text", 608801);
    }

    void OnStartup() override
    {
        reforgeTable = bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_temucraft_reforge'"));
    }
};

void AddTemucraftItemUpgradeScripts()
{
    new TemucraftItemUpgrade_Player();
    new TemucraftItemUpgrade_Creature();
    new TemucraftItemUpgrade_Command();
    new TemucraftItemUpgrade_World();
}
