#include "ScriptMgr.h"
#include "Player.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "Guild.h"
#include "SpellAuraEffects.h"
#include "Chat.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "GuildMgr.h"
#include "Define.h"
#include "GossipDef.h"
#include "DataMap.h"
#include "GameObject.h"
#include "Transport.h"
#include "Maps/MapMgr.h"
#include "WorldSession.h"
#include "DatabaseEnv.h"
#include "guildhouse.h"
#include <array>
#include <unordered_map>

namespace
{
    // id -> text per LocaleConstant, preloaded once so UI strings don't hit the
    // database on the world thread for every message.
    std::unordered_map<uint32, std::array<std::string, TOTAL_LOCALES>> _guildHouseLocaleTexts;
} // namespace

void LoadGuildHouseLocales()
{
    _guildHouseLocaleTexts.clear();

    QueryResult result = WorldDatabase.Query("SELECT `Id`, `Locale`, `Text` FROM `mod_guildhouse_locale`");
    if (!result)
    {
        LOG_WARN("modules", "GUILDHOUSE: `mod_guildhouse_locale` is empty or missing; localized texts unavailable.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 id = fields[0].Get<uint32>();
        LocaleConstant locale = GetLocaleByName(fields[1].Get<std::string>());
        _guildHouseLocaleTexts[id][locale] = fields[2].Get<std::string>();
        ++count;
    } while (result->NextRow());

    LOG_INFO("modules", "GUILDHOUSE: Loaded {} localized text entries.", count);
}

std::string GetGuildHouseLocaleText(uint32 id, Player* player)
{
    if (!player || !player->GetSession())
        return {};

    auto it = _guildHouseLocaleTexts.find(id);
    if (it == _guildHouseLocaleTexts.end())
        return {};

    LocaleConstant locale = player->GetSession()->GetSessionDbLocaleIndex();
    if (locale < TOTAL_LOCALES && !it->second[locale].empty())
        return it->second[locale];

    return it->second[LOCALE_enUS]; // fall back to English
}

class GuildData : public DataMap::Base
{
public:
    GuildData() {}

    GuildData(uint32 phase, float posX, float posY, float posZ, float ori)
    : phase(phase), posX(posX), posY(posY), posZ(posZ), ori(ori) {}

    uint32 phase = PHASEMASK_NORMAL;
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    float ori = 0.0f;
    float radius = 100.0f;

    // Map where the guild house is located.
    uint32 map = 0;

    // True while the player has explicitly entered their guild house
    // through the guild-house teleport mechanism.
    bool inGuildHouse = false;
};

struct GuildHouseLocation
{
    uint32 id = 0;
    std::string name;
    uint32 map = 0;
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    float ori = 0.0f;
    float radius = 100.0f;
};



class GuildHelper : public GuildScript
{

public:
    GuildHelper() : GuildScript("GuildHelper") {}

    void OnCreate(Guild* /*guild*/, Player* leader, const std::string& /*name*/)
    {
        ChatHandler(leader->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_YOU_NOW_OWN_A_GUILD, leader).c_str());
    }

    uint32 GetGuildPhase(Guild* guild)
    {
        return guild->GetId() + 10;
    }

    void OnDisband(Guild* guild)
    {

        if (RemoveGuildHouse(guild))
        {
            LOG_INFO("modules", "GUILDHOUSE: Deleting Guild House data due to disbanding of guild...");
        }
        else
        {
            LOG_INFO("modules", "GUILDHOUSE: Error deleting Guild House data during disbanding of guild!!");
        }
    }

    bool RemoveGuildHouse(Guild* guild)
    {
        uint32 guildPhase = GetGuildPhase(guild);

        uint32 houseMap = 0;

        // Retrieve the map before deleting the guild_house record.
        QueryResult houseResult = CharacterDatabase.Query(
            "SELECT `map` FROM `guild_house` WHERE `guild` = {}",
            guild->GetId());

        if (houseResult)
            houseMap = houseResult->Fetch()[0].Get<uint32>();

        QueryResult CreatureResult = WorldDatabase.Query(
            "SELECT `guid` FROM `creature` "
            "WHERE `map` = {} AND `phaseMask` = {}",
            houseMap,
            guildPhase);

        QueryResult GameobjResult = WorldDatabase.Query(
            "SELECT `guid` FROM `gameobject` "
            "WHERE `map` = {} AND `phaseMask` = {}",
            houseMap,
            guildPhase);

        Map* map = sMapMgr->FindMap(houseMap, 0);

        if (map)
        {
            // Remove creatures belonging to this guild phase.
            if (CreatureResult)
            {
                do
                {
                    Field* fields = CreatureResult->Fetch();
                    uint32 lowguid = fields[0].Get<uint32>();

                    if (CreatureData const* cr_data =
                        sObjectMgr->GetCreatureData(lowguid))
                    {
                        if (Creature* creature =
                            map->GetCreature(
                                ObjectGuid::Create<HighGuid::Unit>(
                                    cr_data->id,
                                    lowguid)))
                        {
                            creature->CombatStop();
                            creature->DeleteFromDB();
                            creature->AddObjectToRemoveList();
                        }
                    }
                } while (CreatureResult->NextRow());
            }

            // Remove gameobjects belonging to this guild phase.
            if (GameobjResult)
            {
                do
                {
                    Field* fields = GameobjResult->Fetch();
                    uint32 lowguid = fields[0].Get<uint32>();

                    if (GameObjectData const* go_data =
                        sObjectMgr->GetGameObjectData(lowguid))
                    {
                        if (GameObject* gobject =
                            map->GetGameObject(
                                ObjectGuid::Create<HighGuid::GameObject>(
                                    go_data->id,
                                    lowguid)))
                        {
                            gobject->SetRespawnTime(0);
                            gobject->Delete();
                            gobject->DeleteFromDB();
                            gobject->CleanupsBeforeDelete();
                        }
                    }
                } while (GameobjResult->NextRow());
            }
        }

        CharacterDatabase.Query(
            "DELETE FROM `guild_house` WHERE `guild` = {}",
            guild->GetId());

        return true;
    }

};

class GuildHouseSeller : public CreatureScript
{
public:
    GuildHouseSeller() : CreatureScript("GuildHouseSeller") {}

    static constexpr uint32 ACTION_BUY_LOCATION = 1000;
    static constexpr uint32 ACTION_LOCATION_PAGE = 2000;
    static constexpr uint32 LOCATIONS_PER_PAGE = 8;

    struct GuildHouseSellerAI : public ScriptedAI
    {
        GuildHouseSellerAI(Creature* creature) : ScriptedAI(creature) {}

        void UpdateAI(uint32 /*diff*/) override
        {
            me->SetFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new GuildHouseSellerAI(creature);
    }

    static uint32 GetGuildPhase(Player* player)
    {
        return player->GetGuildId() + 10;
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!player->GetGuild())
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "%s",
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_NOT_IN_GUILD,
                    player).c_str());

            CloseGossipMenuFor(player);
            return false;
        }

        QueryResult hasHouse = CharacterDatabase.Query(
            "SELECT `id` FROM `guild_house` WHERE `guild` = {}",
            player->GetGuildId());

        if (hasHouse)
        {
            AddGossipItemFor(
                player,
                GOSSIP_ICON_TABARD,
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_GOSSIP_TELEPORT_TO_HOUSE,
                    player),
                    GOSSIP_SENDER_MAIN,
                    1);

            Guild* guild =
            sGuildMgr->GetGuildById(player->GetGuildId());

            if (guild)
            {
                Guild::Member const* member =
                guild->GetMember(player->GetGUID());

                if (member &&
                    member->IsRankNotLower(
                        sConfigMgr->GetOption<int32>(
                            "GuildHouseSellRank",
                            0)))
                {
                    AddGossipItemFor(
                        player,
                        GOSSIP_ICON_TABARD,
                        GetGuildHouseLocaleText(
                            GUILDHOUSE_TEXT_GOSSIP_SELL_HOUSE,
                            player),
                            GOSSIP_SENDER_MAIN,
                            3,
                            GetGuildHouseLocaleText(
                                GUILDHOUSE_TEXT_GOSSIP_SELL_HOUSE_CONFIRM,
                                player),
                                0,
                                false);
                }
            }
        }
        else if (player->GetGuild()->GetLeaderGUID() == player->GetGUID())
        {
            AddGossipItemFor(
                player,
                GOSSIP_ICON_TABARD,
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_GOSSIP_BUY_HOUSE,
                    player),
                    GOSSIP_SENDER_MAIN,
                    2);
        }

        AddGossipItemFor(
            player,
            GOSSIP_ICON_CHAT,
            GetGuildHouseLocaleText(
                GUILDHOUSE_TEXT_GOSSIP_CLOSE,
                player),
                GOSSIP_SENDER_MAIN,
                5);

        SendGossipMenuFor(
            player,
            DEFAULT_GOSSIP_MESSAGE,
            creature->GetGUID());

        return true;
    }

    bool OnGossipSelect(
        Player* player,
        Creature* creature,
        uint32 /*sender*/,
        uint32 action) override
        {
            if (action == 5)
            {
                CloseGossipMenuFor(player);
                return true;
            }

            if (action == 1)
            {
                TeleportGuildHouse(
                    player->GetGuild(),
                                   player,
                                   creature);

                return true;
            }

            if (action == 2)
            {
                ShowGuildHouseLocations(
                    player,
                    creature,
                    0);

                return true;
            }

            if (action == 3)
            {
                QueryResult hasHouse = CharacterDatabase.Query(
                    "SELECT `id` FROM `guild_house` "
                    "WHERE `guild` = {}",
                    player->GetGuildId());

                if (!hasHouse)
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "%s",
                        GetGuildHouseLocaleText(
                            GUILDHOUSE_TEXT_GUILD_HAS_NO_HOUSE,
                            player).c_str());

                    CloseGossipMenuFor(player);
                    return false;
                }

                if (RemoveGuildHouse(player))
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "%s",
                        GetGuildHouseLocaleText(
                            GUILDHOUSE_TEXT_HOUSE_SOLD_SUCCESS,
                            player).c_str());

                    player->GetGuild()->BroadcastToGuild(
                        player->GetSession(),
                                                         false,
                                                         GetGuildHouseLocaleText(
                                                             GUILDHOUSE_TEXT_BROADCAST_HOUSE_SOLD,
                                                             player).c_str(),
                                                         LANG_UNIVERSAL);

                    player->ModifyMoney(
                        sConfigMgr->GetOption<int32>(
                            "CostGuildHouse",
                            10000000) / 2);

                    LOG_INFO(
                        "modules",
                        "GUILDHOUSE: Successfully returned money "
                        "and sold Guild House");

                    CloseGossipMenuFor(player);
                }
                else
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "%s",
                        GetGuildHouseLocaleText(
                            GUILDHOUSE_TEXT_HOUSE_SOLD_ERROR,
                            player).c_str());

                    CloseGossipMenuFor(player);
                }

                return true;
            }

            if (action >= ACTION_BUY_LOCATION &&
                action < ACTION_BUY_LOCATION + 1000)
            {
                uint32 locationId =
                action - ACTION_BUY_LOCATION;

                BuyGuildHouseLocation(
                    player,
                    creature,
                    locationId);

                return true;
            }

            if (action >= ACTION_LOCATION_PAGE &&
                action < ACTION_LOCATION_PAGE + 1000)
            {
                uint32 page =
                action - ACTION_LOCATION_PAGE;

                ShowGuildHouseLocations(
                    player,
                    creature,
                    page);

                return true;
            }

            return true;
        }

private:

    void ShowGuildHouseLocations(
        Player* player,
        Creature* creature,
        uint32 page)
    {
        ClearGossipMenuFor(player);

        uint32 offset =
        page * LOCATIONS_PER_PAGE;

        QueryResult result = WorldDatabase.Query(
            "SELECT `id`, `name` "
            "FROM `guild_house_locations` "
            "WHERE `enabled` = 1 "
            "ORDER BY `id` "
            "LIMIT {} OFFSET {}",
            LOCATIONS_PER_PAGE,
            offset);

        if (!result)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "No guild house locations are currently available.");

            CloseGossipMenuFor(player);
            return;
        }

        do
        {
            Field* fields = result->Fetch();

            uint32 locationId =
            fields[0].Get<uint32>();

            std::string name =
            fields[1].Get<std::string>();

            AddGossipItemFor(
                player,
                GOSSIP_ICON_MONEY_BAG,
                name,
                GOSSIP_SENDER_MAIN,
                ACTION_BUY_LOCATION + locationId,
                "Purchase this guild house location?",
                sConfigMgr->GetOption<int32>(
                    "CostGuildHouse",
                    10000000),
                    false);

        } while (result->NextRow());

        if (page > 0)
        {
            AddGossipItemFor(
                player,
                GOSSIP_ICON_CHAT,
                "Previous page",
                GOSSIP_SENDER_MAIN,
                ACTION_LOCATION_PAGE + page - 1);
        }

        QueryResult nextPage = WorldDatabase.Query(
            "SELECT `id` "
            "FROM `guild_house_locations` "
            "WHERE `enabled` = 1 "
            "ORDER BY `id` "
            "LIMIT 1 OFFSET {}",
            (page + 1) * LOCATIONS_PER_PAGE);

        if (nextPage)
        {
            AddGossipItemFor(
                player,
                GOSSIP_ICON_CHAT,
                "Next page",
                GOSSIP_SENDER_MAIN,
                ACTION_LOCATION_PAGE + page + 1);
        }

        AddGossipItemFor(
            player,
            GOSSIP_ICON_CHAT,
            GetGuildHouseLocaleText(
                GUILDHOUSE_TEXT_GOSSIP_CLOSE,
                player),
                GOSSIP_SENDER_MAIN,
                5);

        SendGossipMenuFor(
            player,
            DEFAULT_GOSSIP_MESSAGE,
            creature->GetGUID());
    }

    void BuyGuildHouseLocation(
        Player* player,
        Creature* creature,
        uint32 locationId)
    {
        Guild* guild = player->GetGuild();

        if (!guild)
        {
            CloseGossipMenuFor(player);
            return;
        }

        if (guild->GetLeaderGUID() != player->GetGUID())
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Only the guild leader can purchase a guild house.");

            CloseGossipMenuFor(player);
            return;
        }

        QueryResult existing = CharacterDatabase.Query(
            "SELECT `id` "
            "FROM `guild_house` "
            "WHERE `guild` = {}",
            guild->GetId());

        if (existing)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "%s",
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_GUILD_ALREADY_HAS_HOUSE,
                    player).c_str());

            CloseGossipMenuFor(player);
            return;
        }

        QueryResult location = WorldDatabase.Query(
            "SELECT `name`, `map`, `positionX`, `positionY`, "
            "`positionZ`, `orientation`, `radius` "
            "FROM `guild_house_locations` "
            "WHERE `id` = {} "
            "AND `enabled` = 1",
            locationId);

        if (!location)
        {
            LOG_ERROR(
                "modules",
                "GUILDHOUSE: Guild {} attempted to purchase "
                "invalid location {}",
                guild->GetId(),
                      locationId);

            ChatHandler(player->GetSession()).PSendSysMessage(
                "That guild house location is no longer available.");

            CloseGossipMenuFor(player);
            return;
        }

        Field* fields = location->Fetch();

        std::string locationName =
        fields[0].Get<std::string>();

        uint32 map =
        fields[1].Get<uint32>();

        float posX =
        fields[2].Get<float>();

        float posY =
        fields[3].Get<float>();

        float posZ =
        fields[4].Get<float>();

        float ori =
        fields[5].Get<float>();

        float radius =
        fields[6].Get<float>();

        int32 cost =
        sConfigMgr->GetOption<int32>(
            "CostGuildHouse",
            10000000);

        if (player->GetMoney() < cost)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You do not have enough gold to purchase a guild house.");

            CloseGossipMenuFor(player);
            return;
        }

        uint32 phase =
        GetGuildPhase(player);

        CharacterDatabase.Query(
            "INSERT INTO `guild_house` "
            "(`id`, `guild`, `phase`, `map`, `positionX`, "
            "`positionY`, `positionZ`, `orientation`) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {})",
                                locationId,
                                guild->GetId(),
                                phase,
                                map,
                                posX,
                                posY,
                                posZ,
                                ori);

        player->ModifyMoney(-cost);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "%s",
            GetGuildHouseLocaleText(
                GUILDHOUSE_TEXT_HOUSE_PURCHASED_SUCCESS,
                player).c_str());

        guild->BroadcastToGuild(
            player->GetSession(),
                                false,
                                GetGuildHouseLocaleText(
                                    GUILDHOUSE_TEXT_BROADCAST_HOUSE_PURCHASED,
                                    player).c_str(),
                                LANG_UNIVERSAL);

        guild->BroadcastToGuild(
            player->GetSession(),
                                false,
                                GetGuildHouseLocaleText(
                                    GUILDHOUSE_TEXT_BROADCAST_USE_TELEPORT,
                                    player).c_str(),
                                LANG_UNIVERSAL);

        LOG_INFO(
            "modules",
            "GUILDHOUSE: GuildId {} purchased "
            "guild house location {} ({})",
                 guild->GetId(),
                 locationId,
                 locationName);

        CloseGossipMenuFor(player);
    }

    bool RemoveGuildHouse(Player* player)
    {
        uint32 guildPhase =
        GetGuildPhase(player);

        uint32 houseMap = 0;

        QueryResult house =
        CharacterDatabase.Query(
            "SELECT `map` "
            "FROM `guild_house` "
            "WHERE `guild` = {}",
            player->GetGuildId());

        if (house)
            houseMap = house->Fetch()[0].Get<uint32>();

        QueryResult creatures =
        WorldDatabase.Query(
            "SELECT `guid` FROM `creature` "
            "WHERE `map` = {} "
            "AND `phaseMask` = {}",
            houseMap,
            guildPhase);

        QueryResult gameobjects =
        WorldDatabase.Query(
            "SELECT `guid` FROM `gameobject` "
            "WHERE `map` = {} "
            "AND `phaseMask` = {}",
            houseMap,
            guildPhase);

        Map* map =
        sMapMgr->FindMap(houseMap, 0);

        if (map)
        {
            if (creatures)
            {
                do
                {
                    Field* fields =
                    creatures->Fetch();

                    uint32 lowguid =
                    fields[0].Get<uint32>();

                    if (CreatureData const* data =
                        sObjectMgr->GetCreatureData(lowguid))
                    {
                        if (Creature* creature =
                            map->GetCreature(
                                ObjectGuid::Create<HighGuid::Unit>(
                                    data->id,
                                    lowguid)))
                        {
                            creature->CombatStop();
                            creature->DeleteFromDB();
                            creature->AddObjectToRemoveList();
                        }
                    }
                } while (creatures->NextRow());
            }

            if (gameobjects)
            {
                do
                {
                    Field* fields =
                    gameobjects->Fetch();

                    uint32 lowguid =
                    fields[0].Get<uint32>();

                    if (GameObjectData const* data =
                        sObjectMgr->GetGameObjectData(lowguid))
                    {
                        if (GameObject* object =
                            map->GetGameObject(
                                ObjectGuid::Create<HighGuid::GameObject>(
                                    data->id,
                                    lowguid)))
                        {
                            object->SetRespawnTime(0);
                            object->Delete();
                            object->DeleteFromDB();
                            object->CleanupsBeforeDelete();
                        }
                    }
                } while (gameobjects->NextRow());
            }
        }

        CharacterDatabase.Query(
            "DELETE FROM `guild_house` "
            "WHERE `guild` = {}",
            player->GetGuildId());

        GuildData* guildData =
        player->CustomData.GetDefault<GuildData>("phase");

        guildData->inGuildHouse = false;
        guildData->phase = PHASEMASK_NORMAL;

        player->SetPhaseMask(
            guildData->phase,
            true);

        return true;
    }

    void SpawnStarterPortal(Player* player)
    {
        QueryResult house =
        CharacterDatabase.Query(
            "SELECT `map`, `phase`, "
            "`positionX`, `positionY`, "
            "`positionZ`, `orientation` "
            "FROM `guild_house` "
            "WHERE `guild` = {}",
            player->GetGuildId());

        if (!house)
            return;

        Field* fields =
        house->Fetch();

        uint32 mapId =
        fields[0].Get<uint32>();

        uint32 phase =
        fields[1].Get<uint32>();

        float posX =
        fields[2].Get<float>();

        float posY =
        fields[3].Get<float>();

        float posZ =
        fields[4].Get<float>();

        float ori =
        fields[5].Get<float>();

        Map* map =
        sMapMgr->FindMap(mapId, 0);

        if (!map)
            return;

        uint32 entry =
        player->GetTeamId() == TEAM_ALLIANCE
        ? GetGameObjectEntry(0)
        : GetGameObjectEntry(4);

        if (!entry)
            return;

        const GameObjectTemplate* objectInfo =
        sObjectMgr->GetGameObjectTemplate(entry);

        if (!objectInfo)
            return;

        GameObject* object =
        sObjectMgr->IsGameObjectStaticTransport(
            objectInfo->entry)
        ? static_cast<GameObject*>(new StaticTransport())
        : static_cast<GameObject*>(new GameObject());

        ObjectGuid::LowType guidLow =
        map->GenerateLowGuid<HighGuid::GameObject>();

        if (!object->Create(
            guidLow,
            objectInfo->entry,
            map,
            phase,
            posX,
            posY,
            posZ,
            ori,
            G3D::Quat(),
                            0,
                            GO_STATE_READY))
        {
            delete object;
            return;
        }

        object->SaveToDB(
            map->GetId(),
                         (1 << map->GetSpawnMode()),
                         phase);

        guidLow =
        object->GetSpawnId();

        delete object;

        object =
        sObjectMgr->IsGameObjectStaticTransport(
            objectInfo->entry)
        ? static_cast<GameObject*>(new StaticTransport())
        : static_cast<GameObject*>(new GameObject());

        if (!object->LoadGameObjectFromDB(
            guidLow,
            map,
            true))
        {
            delete object;
            return;
        }

        sObjectMgr->AddGameobjectToGrid(
            guidLow,
            sObjectMgr->GetGameObjectData(guidLow));
    }

    bool SpawnButlerNPC(Player* player)
    {
        uint32 entry = GetCreatureEntry(1);

        QueryResult house = CharacterDatabase.Query(
            "SELECT `id`, `map`, `phase` "
            "FROM `guild_house` "
            "WHERE `guild` = {}",
            player->GetGuildId());

        if (!house)
            return false;


        Field* houseFields = house->Fetch();

        uint32 locationId = houseFields[0].Get<uint32>();
        uint32 mapId = houseFields[1].Get<uint32>();
        uint32 phase = houseFields[2].Get<uint32>();

        QueryResult spawn = WorldDatabase.Query(
            "SELECT `posX`, `posY`, `posZ`, `orientation` "
            "FROM `guild_house_spawns` "
            "WHERE `locationid` = {} "
            "AND `entry` = {}",
            locationId,
            entry);

        if (!spawn)
        {
            LOG_ERROR(
                "modules",
                "GUILDHOUSE: No Butler spawn configured for guild house location ID {}",
                locationId);
            return false;

        }

        Field* spawnFields = spawn->Fetch();

        float posX = spawnFields[0].Get<float>();
        float posY = spawnFields[1].Get<float>();
        float posZ = spawnFields[2].Get<float>();
        float ori = spawnFields[3].Get<float>();

        Map* map = sMapMgr->FindMap(mapId, 0);

        if (!map)
        {
            LOG_ERROR(
                "modules",
                "GUILDHOUSE: Could not load map {} for guild house location ID {}",
                mapId,
                locationId);
            return false;

        }

        Creature* creature = new Creature();

        if (!creature->Create(
            map->GenerateLowGuid<HighGuid::Unit>(),
                              map,
                              phase,
                              entry,
                              0,
                              posX,
                              posY,
                              posZ,
                              ori))
        {
            delete creature;
            return false;

        }

        creature->SaveToDB(
            map->GetId(),
                           (1 << map->GetSpawnMode()),
                           phase);

        uint32 lowguid = creature->GetSpawnId();

        creature->CleanupsBeforeDelete();
        delete creature;

        creature = new Creature();

        if (!creature->LoadCreatureFromDB(lowguid, map))
        {
            delete creature;
            return false;

        }

        sObjectMgr->AddCreatureToGrid(
            lowguid,
            sObjectMgr->GetCreatureData(lowguid));

        LOG_INFO(
            "modules",
            "GUILDHOUSE: Spawned Butler for guild {} at location ID {}",
            player->GetGuildId(),
                 locationId);

        return true;

    }


    void TeleportGuildHouse(
        Guild* guild,
        Player* player,
        Creature* creature)
    {
        QueryResult result =
        CharacterDatabase.Query(
            "SELECT `phase`, `map`, "
            "`positionX`, `positionY`, "
            "`positionZ`, `orientation` "
            "FROM `guild_house` "
            "WHERE `guild` = {}",
            guild->GetId());

        if (!result)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "%s",
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_GUILD_HAS_NO_HOUSE,
                    player).c_str());

            CloseGossipMenuFor(player);
            return;
        }

        Field* fields =
        result->Fetch();

        GuildData* guildData =
        player->CustomData.GetDefault<GuildData>("phase");

        guildData->phase =
        fields[0].Get<uint32>();

        guildData->map =
        fields[1].Get<uint32>();

        guildData->posX =
        fields[2].Get<float>();

        guildData->posY =
        fields[3].Get<float>();

        guildData->posZ =
        fields[4].Get<float>();

        guildData->ori =
        fields[5].Get<float>();

        // Mark the player as deliberately entering the guild house.
        guildData->inGuildHouse = true;

        // Set the phase before teleporting so the player arrives
        // in the guild-specific copy rather than the public phase.
        player->SetPhaseMask(
            guildData->phase,
            true);

        player->TeleportTo(
            guildData->map,
            guildData->posX,
            guildData->posY,
            guildData->posZ,
            guildData->ori);

        CloseGossipMenuFor(player);
    }
};


class GuildHousePlayerScript : public PlayerScript
{
public:
    GuildHousePlayerScript()
    : PlayerScript("GuildHousePlayerScript")
    {
    }

    void OnPlayerLogin(Player* player)
    {
        GuildData* guildData =
        player->CustomData.GetDefault<GuildData>("phase");

        guildData->inGuildHouse = false;

        CheckPlayer(player);
    }

    void OnPlayerUpdateZone(
        Player* player,
        uint32 /*newZone*/,
        uint32 /*newArea*/)
    {
        GuildData* guildData =
        player->CustomData.GetDefault<GuildData>("phase");

        /*
         * Do not automatically assign a guild phase merely because
         * the player walked into the physical location.
         *
         * The guild phase is an explicit "entered guild house"
         * state established by TeleportGuildHouse() or the .gh
         * teleport command.
         */
        if (!guildData->inGuildHouse)
        {
            player->SetPhaseMask(
                GetNormalPhase(player),
                                 true);
        }
    }

    bool OnPlayerBeforeTeleport(
        Player* player,
        uint32 /*mapid*/,
        float /*x*/,
        float /*y*/,
        float /*z*/,
        float /*orientation*/,
        uint32 /*options*/,
        Unit* /*target*/)
    {
        GuildData* guildData =
        player->CustomData.GetDefault<GuildData>("phase");

        /*
         * A teleport away from the house is the equivalent of
         * leaving the instance. Restore the player's normal phase
         * before the destination is loaded.
         */
        if (guildData->inGuildHouse)
        {
            guildData->inGuildHouse = false;

            player->SetPhaseMask(
                GetNormalPhase(player),
                                 true);
        }

        return true;
    }

    uint32 GetNormalPhase(Player* player) const
    {
        if (player->IsGameMaster())
            return PHASEMASK_ANYWHERE;

        uint32 phase =
        player->GetPhaseByAuras();

        if (!phase)
            return PHASEMASK_NORMAL;

        return phase;
    }

    void CheckPlayer(Player* player)
    {
        GuildData* guildData =
        player->CustomData.GetDefault<GuildData>("phase");

        /*
         * A player should never receive a guild-house phase merely
         * because they happen to be standing at the coordinates.
         *
         * Only the explicit guild-house teleport establishes the
         * private phase.
         */
        if (!guildData->inGuildHouse)
        {
            player->SetPhaseMask(
                GetNormalPhase(player),
                                 true);

            return;
        }

        QueryResult result =
        CharacterDatabase.Query(
            "SELECT `phase`, `map`, "
            "`positionX`, `positionY`, "
            "`positionZ`, `orientation`, "
            "`id` "
            "FROM `guild_house` "
            "WHERE `guild` = {}",
            player->GetGuildId());


        if (!result || !player->GetGuild())
        {
            guildData->inGuildHouse = false;

            player->SetPhaseMask(
                GetNormalPhase(player),
                                 true);

            return;
        }

        Field* fields =
        result->Fetch();

        guildData->phase =
        fields[0].Get<uint32>();

        guildData->map =
        fields[1].Get<uint32>();

        guildData->posX =
        fields[2].Get<float>();

        guildData->posY =
        fields[3].Get<float>();

        guildData->posZ =
        fields[4].Get<float>();

        guildData->ori =
        fields[5].Get<float>();

        QueryResult locationResult =
        WorldDatabase.Query(
            "SELECT `radius` "
            "FROM `guild_house_locations` "
            "WHERE `id` = {}",
            fields[6].Get<uint32>());

        if (locationResult)
        {
            guildData->radius =
            locationResult->Fetch()[0].Get<float>();
        }
        else
        {
            guildData->radius = 100.0f;
        }


        player->SetPhaseMask(
            guildData->phase,
            true);
    }

    void teleportToDefault(Player* player)
    {
        if (player->GetTeamId() == TEAM_ALLIANCE)
        {
            player->TeleportTo(
                0,
                -8833.379883f,
                628.627991f,
                94.006599f,
                1.0f);
        }
        else
        {
            player->TeleportTo(
                1,
                1486.048340f,
                -4415.140625f,
                24.187496f,
                0.13f);
        }
    }
};


using namespace Acore::ChatCommands;

class GuildHouseCommand : public CommandScript
{
public:
    GuildHouseCommand() : CommandScript("GuildHouseCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable GuildHouseCommandTable =
        {
            {"teleport", HandleGuildHouseTeleCommand, SEC_PLAYER, Console::Yes},
            {"butler", HandleSpawnButlerCommand, SEC_PLAYER, Console::Yes},
        };

        static ChatCommandTable GuildHouseCommandBaseTable =
        {
            {"guildhouse", GuildHouseCommandTable},
            {"gh", GuildHouseCommandTable}
        };

        return GuildHouseCommandBaseTable;
    }

    static uint32 GetGuildPhase(Player* player)
    {
        return player->GetGuildId() + 10;
    }

    static bool HandleSpawnButlerCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Map* map = player->GetMap();

        if (!player->GetGuild() || (player->GetGuild()->GetLeaderGUID() != player->GetGUID()))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_NEED_GUILDMASTER, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        GuildData* guildData =
        player->CustomData.GetDefault<GuildData>("phase");

        if (!guildData->inGuildHouse)
        {
            handler->SendSysMessage(
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_CMD_NEED_IN_GUILDHOUSE,
                    player).c_str());

            handler->SetSentErrorMessage(true);
            return false;
        }


        if (player->FindNearestCreature(GetCreatureEntry(1), VISIBLE_RANGE, true))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_BUTLER_ALREADY_EXISTS, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        float posX = guildData->posX;
        float posY = guildData->posY;
        float posZ = guildData->posZ;
        float ori = guildData->ori;

        Creature* creature = new Creature();

        if (!creature->Create(
            map->GenerateLowGuid<HighGuid::Unit>(),
                              map,
                              GetGuildPhase(player),
                              GetCreatureEntry(1),
                              0,
                              posX,
                              posY,
                              posZ,
                              ori))
        {
            handler->SendSysMessage(
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_CMD_BUTLER_ALREADY_EXISTS,
                    player).c_str());

            handler->SetSentErrorMessage(true);
            delete creature;
            return false;
        }

        creature->SaveToDB(
            player->GetMapId(),
                           (1 << player->GetMap()->GetSpawnMode()),
                           GetGuildPhase(player));

        uint32 lowguid = creature->GetSpawnId();

        creature->CleanupsBeforeDelete();
        delete creature;

        creature = new Creature();

        if (!creature->LoadCreatureFromDB(
            lowguid,
            player->GetMap()))
        {
            handler->SendSysMessage(
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_CMD_BUTLER_ADD_ERROR,
                    player).c_str());

            handler->SetSentErrorMessage(true);
            delete creature;
            return false;
        }

        sObjectMgr->AddCreatureToGrid(
            lowguid,
            sObjectMgr->GetCreatureData(lowguid));

        return true;


    }
    static bool HandleGuildHouseTeleCommand(ChatHandler* handler)
    {
        Player* player =
        handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->IsInCombat())
        {
            handler->SendSysMessage(
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_CMD_IN_COMBAT,
                    player).c_str());

            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!player->GetGuild())
        {
            handler->SendSysMessage(
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_NOT_IN_GUILD,
                    player).c_str());

            handler->SetSentErrorMessage(true);
            return false;
        }

        QueryResult result =
        CharacterDatabase.Query(
            "SELECT `phase`, `map`, "
            "`positionX`, `positionY`, "
            "`positionZ`, `orientation` "
            "FROM `guild_house` "
            "WHERE `guild` = {}",
            player->GetGuildId());

        if (!result)
        {
            handler->SendSysMessage(
                GetGuildHouseLocaleText(
                    GUILDHOUSE_TEXT_GUILD_HAS_NO_HOUSE,
                    player).c_str());

            handler->SetSentErrorMessage(true);
            return false;
        }

        Field* fields =
        result->Fetch();

        GuildData* guildData =
        player->CustomData.GetDefault<GuildData>("phase");

        guildData->phase =
        fields[0].Get<uint32>();

        guildData->map =
        fields[1].Get<uint32>();

        guildData->posX =
        fields[2].Get<float>();

        guildData->posY =
        fields[3].Get<float>();

        guildData->posZ =
        fields[4].Get<float>();

        guildData->ori =
        fields[5].Get<float>();

        /*
         * Enter the guild-house phase before teleporting.
         * This is the key behavior inherited from the author's v2
         * implementation.
         */
        guildData->inGuildHouse = true;

        player->SetPhaseMask(
            guildData->phase,
            true);

        player->TeleportTo(
            guildData->map,
            guildData->posX,
            guildData->posY,
            guildData->posZ,
            guildData->ori);

        return true;
    }

};

class GuildHouseGlobal : public GlobalScript
{
public:
    GuildHouseGlobal() : GlobalScript("GuildHouseGlobal") {}

    void OnBeforeWorldObjectSetPhaseMask(WorldObject const* worldObject, uint32 & /*oldPhaseMask*/, uint32 & /*newPhaseMask*/, bool &useCombinedPhases, bool & /*update*/) override
    {
        if (worldObject->GetZoneId() == 876)
            useCombinedPhases = false;
        else
            useCombinedPhases = true;
    }
};

class GuildHouseWorld : public WorldScript
{
public:
    GuildHouseWorld() : WorldScript("GuildHouseWorld") {}

    void OnStartup() override
    {
        LoadGuildHouseLocales();
    }
};

void AddGuildHouseScripts()
{
    new GuildHelper();
    new GuildHouseSeller();
    new GuildHousePlayerScript();
    new GuildHouseCommand();
    new GuildHouseGlobal();
    new GuildHouseWorld();
}
