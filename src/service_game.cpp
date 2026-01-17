// IMPORTANT(fusion): All functions in this file expect to be called in the game
// thread, except for the service implementation at the very bottom. This also
// has implications to which members of GameConnection can be accessed at any
// given time

#include "otpch.h"

#include "service_game.h"

#include "ban.h"
#include "base64.h"
#include "condition.h"
#include "configmanager.h"
#include "crypto.h"
#include "depotchest.h"
#include "game.h"
#include "inbox.h"
#include "iologindata.h"
#include "iomarket.h"
#include "monster.h"
#include "npc.h"
#include "outfit.h"
#include "outputmessage.h"
#include "player.h"
#include "podium.h"
#include "scheduler.h"
#include "storeinbox.h"
#include "tasks.h"

extern CreatureEvents* g_creatureEvents;
extern Chat* g_chat;
extern Game g_game;


enum SessionEndReason {
    SESSION_END_LOGOUT      = 0,
    SESSION_END_UNKNOWN     = 1, // unknown, no difference from logout
    SESSION_END_FORCECLOSE  = 2,
    SESSION_END_UNKNOWN2    = 3, // unknown, no difference from logout
};

static ClientDamageType GetClientDamageType(CombatType_t combatType){
    switch (combatType) {
        case COMBAT_PHYSICALDAMAGE:
            return CLIENT_DAMAGETYPE_PHYSICAL;
        case COMBAT_ENERGYDAMAGE:
            return CLIENT_DAMAGETYPE_ENERGY;
        case COMBAT_EARTHDAMAGE:
            return CLIENT_DAMAGETYPE_EARTH;
        case COMBAT_FIREDAMAGE:
            return CLIENT_DAMAGETYPE_FIRE;
        case COMBAT_LIFEDRAIN:
            return CLIENT_DAMAGETYPE_LIFEDRAIN;
        case COMBAT_HEALING:
            return CLIENT_DAMAGETYPE_HEALING;
        case COMBAT_DROWNDAMAGE:
            return CLIENT_DAMAGETYPE_DROWN;
        case COMBAT_ICEDAMAGE:
            return CLIENT_DAMAGETYPE_ICE;
        case COMBAT_HOLYDAMAGE:
            return CLIENT_DAMAGETYPE_HOLY;
        case COMBAT_DEATHDAMAGE:
            return CLIENT_DAMAGETYPE_DEATH;
        default:
            return CLIENT_DAMAGETYPE_UNDEFINED;
    }
}

//=============================================================================
// Preamble
//=============================================================================
namespace asio = boost::asio;
namespace chrono = std::chrono;
using asio::use_awaitable;
using asio::ip::tcp;

enum GameConnectionState {
    GAME_CONNECTION_LOGIN = 0,
    GAME_CONNECTION_OK,
    GAME_CONNECTION_CLOSE,
    GAME_CONNECTION_ABORT,
};

struct GameConnection{
    explicit GameConnection(boost::asio::ip::tcp::socket &&socket_,
                    const boost::asio::ip::tcp::endpoint &endpoint_)
    :   socket(std::move(socket_)),
        loginTimer(socket.get_executor()),
        endpoint(endpoint_)
    {
        // no-op
    }

    // NOTE(fusion): This data is synchronized by being accessed only in the
    // network thread, excluding atomics which can be shared.
    boost::asio::ip::tcp::socket            socket;
    boost::asio::steady_timer               loginTimer;
    std::atomic<GameConnectionState>        state = GAME_CONNECTION_LOGIN;
    uint32_t                                serverSequence = 0;
    uint32_t                                clientSequence = 0;
    std::array<uint32_t, 4>                 xteaKey;

    // NOTE(fusion): The output queue is shared between the network and game
    // threads so it needs to be explicitly synchronized.
    std::mutex                              outputMutex;
    OutputMessage_ptr                       outputHead;

    // NOTE(fusion): This data is not synchronized but is constant after being
    // set in the handshake phase, so it's safe to be READ from.
    const boost::asio::ip::tcp::endpoint    endpoint;
    std::string                             debugName;
    int                                     terminalType = 0;
    int                                     terminalVersion = 0;

    // NOTE(fusion): This data is synchronized by being accessed only in the game
    // thread, for the purpose of simplifying the interface with sending functions.
    //  This also means we shouldn't access these outside the game thread.
    Player                                  *player = nullptr;
    bool                                    debugAssertReceived = false;
    std::vector<uint32_t>                   knownCreatures;
};

static bool Transition(const GameConnection_ptr &connection,
            GameConnectionState from, GameConnectionState to){
    GameConnectionState expected = from;
    return connection->state.compare_exchange_strong(
            expected, to, std::memory_order_seq_cst);
}

static GameConnectionState CurrentState(const GameConnection_ptr &connection){
    return connection->state.load(std::memory_order_acquire);
}

static void ResolveLogin(const GameConnection_ptr &connection,
                         GameConnectionState state){
    if(Transition(connection, GAME_CONNECTION_LOGIN, state)){
        asio::post(connection->loginTimer.get_executor(),
                [connection]{ connection->loginTimer.cancel(); });
    }
}

//=============================================================================
// Utility
//=============================================================================
bool CanSeePosition(const Player *player, const Position &pos){
    assert(player != NULL);
    Position playerPos = player->getPosition();

    // NOTE(fusion): When you're above ground level you can't see below it.
    // NOTE(fusion): When you're below ground level, you can see up to two
    // levels above or below.
    int zOffset = playerPos.z - pos.z;
    if((playerPos.z <= 7 && pos.z > 7) || (playerPos.z >= 8 && std::abs(zOffset) > 2))
        return false;

    int minX = (playerPos.x - Map::maxClientViewportX) + zOffset;
    int minY = (playerPos.y - Map::maxClientViewportY) + zOffset;
    int maxX = (playerPos.x + Map::maxClientViewportX) + zOffset;
    int maxY = (playerPos.y + Map::maxClientViewportY) + zOffset;
    return pos.x >= minX && pos.x <= maxX
        && pos.y >= minY && pos.y <= maxY;
}

bool CanSeeCreature(const Player *player, const Creature *creature){
    assert(player != NULL);
    return creature && !creature->isRemoved() && player->canSeeCreature(creature)
            && CanSeePosition(player, creature->getPosition());
}

void Detach(GameConnection_ptr connection){
    // IMPORTANT(fusion): This function needs to take the client shared pointer
    // by value because `player->client` may be the last reference to it and may
    // become invalid after we reset it.
    Transition(connection, GAME_CONNECTION_OK, GAME_CONNECTION_CLOSE);
    if(Player *player = connection->player){
        connection->player = NULL;
        player->connection = NULL;

        // EVENT: onPlayerDetach ?
        g_game.ReleaseCreature(player);
    }
}

void WriteToOutputBuffer(const GameConnection_ptr &connection, const NetworkMessage &msg){
    constexpr int MAX_PADDING = 8;
    std::lock_guard lockGuard(connection->outputMutex);
    // TODO(fusion): Ideally we'd have the tail of the linked list stored in
    // the connection structure, but I'd assume it's just unlikely that we hit
    // more than a couple of queued messages.
    if(!connection->outputHead){
        connection->outputHead = OutputMessage::make();
    }

    OutputMessage *tail = connection->outputHead.get();
    while(tail->next){
        tail = tail->next.get();
    }

    if(!tail->canAdd(msg.getWrittenLength() + MAX_PADDING)){
        tail->next = OutputMessage::make();
        tail = tail->next.get();
    }

    assert(tail->canAdd(msg.getWrittenLength() + MAX_PADDING));
    tail->append(msg);
}

boost::asio::ip::address GetRemoteAddress(const GameConnection_ptr &connection){
    return connection->endpoint.address();
}

int GetTerminalType(const GameConnection_ptr &connection){
    return connection->terminalType;
}

int GetTerminalVersion(const GameConnection_ptr &connection){
    return connection->terminalVersion;
}

//==============================================================================
// Internal Send Helpers and Functions
//==============================================================================
static void AddOutfit(NetworkMessage& msg, const Outfit_t& outfit){
    // outfit
    msg.add<uint16_t>(outfit.lookType);
    if (outfit.lookType != 0) {
        msg.addByte(outfit.lookHead);
        msg.addByte(outfit.lookBody);
        msg.addByte(outfit.lookLegs);
        msg.addByte(outfit.lookFeet);
        msg.addByte(outfit.lookAddons);
    } else {
        msg.addItemId(outfit.lookTypeEx);
    }

    // mount
    msg.add<uint16_t>(outfit.lookMount);
    if (outfit.lookMount != 0) {
        msg.addByte(outfit.lookMountHead);
        msg.addByte(outfit.lookMountBody);
        msg.addByte(outfit.lookMountLegs);
        msg.addByte(outfit.lookMountFeet);
    }
}

static void AddCreatureIcons(NetworkMessage& msg, const Creature* creature){
    const auto& creatureIcons = creature->getIcons();
    if (const Monster* monster = creature->getMonster()) {
        const auto& monsterIcons = monster->getSpecialIcons();
        msg.addByte(creatureIcons.size() + monsterIcons.size());
        for (const auto& [iconId, level] : monsterIcons) {
            msg.addByte(iconId);
            msg.addByte(1);
            msg.add<uint16_t>(level);
        }
    } else {
        msg.addByte(creatureIcons.size());
    }

    for (const auto& [iconId, level] : creatureIcons) {
        msg.addByte(iconId);
        msg.addByte(0);
        msg.add<uint16_t>(level);
    }
}

static bool MakeCreatureKnown(const GameConnection_ptr &connection,
                        uint32_t creatureId, uint32_t *outRemoveId){
    for(uint32_t knownCreatureId: connection->knownCreatures){
        if(knownCreatureId == creatureId){
            return false;
        }
    }

    uint32_t removeId = 0;
    if(connection->knownCreatures.size() > 1300){
        for(uint32_t &knownCreatureId: connection->knownCreatures){
            Creature *creature = g_game.getCreatureByID(knownCreatureId);
            if(!creature || !CanSeeCreature(connection->player, creature)){
                removeId = knownCreatureId;
                knownCreatureId = creatureId;
                break;
            }
        }

        if(removeId == 0){
            // NOTE(fusion): With 1300 known creature slots, you'd need ~5 creatures
            // per visible tile to not find one available, which is extremely unlikely.
            // It's also not clear what should happen in this case, since we'd already
            // have a sync problem with this client regardless.
            return false;
        }
    }else{
        connection->knownCreatures.push_back(creatureId);
    }

    if(outRemoveId){
        *outRemoveId = removeId;
    }

    return true;
}

static void AddCreature(const GameConnection_ptr &connection, NetworkMessage &msg,
                        const Creature* creature, bool forceUpdate = false){
    // TODO(fusion): It's probably a good idea to clean this up sometime.
    const Player *player = connection->player;
    CreatureType_t creatureType = creature->getType();
    const Player* otherPlayer = creature->getPlayer();
    uint32_t masterId = 0;
    if (creatureType == CREATURETYPE_MONSTER) {
        const Creature* master = creature->getMaster();
        if (master && master->getPlayer()) {
            masterId = master->getID();
            if(master == player){
                creatureType = CREATURETYPE_SUMMON_OWN;
            }else{
                creatureType = CREATURETYPE_SUMMON_OTHERS;
            }
        }
    }

    uint32_t removeId;
    uint32_t creatureId = creature->getID();
    bool known = !MakeCreatureKnown(connection, creatureId, &removeId);
    if(known && forceUpdate){
        known    = false;
        removeId = creatureId;
    }

    if (known) {
        msg.add<uint16_t>(0x62);
        msg.add<uint32_t>(creature->getID());
    } else {
        msg.add<uint16_t>(0x61);
        msg.add<uint32_t>(removeId);
        msg.add<uint32_t>(creature->getID());
        msg.addByte(creature->isHealthHidden() ? CREATURETYPE_HIDDEN : creatureType);
        if (creatureType == CREATURETYPE_SUMMON_OWN) {
            msg.add<uint32_t>(masterId);
        }
        msg.addString(creature->isHealthHidden() ? "" : creature->getName());
    }

    if (creature->isHealthHidden()) {
        msg.addByte(0x00);
    } else {
        msg.addByte(std::ceil(
            (static_cast<double>(creature->getHealth()) / std::max<int32_t>(creature->getMaxHealth(), 1)) * 100));
    }

    msg.addByte(creature->getDirection());

    if (!creature->isInGhostMode() && !creature->isInvisible()) {
        const Outfit_t& outfit = creature->getCurrentOutfit();
        AddOutfit(msg, outfit);
    } else {
        static Outfit_t outfit;
        AddOutfit(msg, outfit);
    }

    LightInfo lightInfo = creature->getCreatureLight();
    msg.addByte(player->isAccessPlayer() ? 0xFF : lightInfo.level);
    msg.addByte(lightInfo.color);

    msg.add<uint16_t>(creature->getStepSpeed() / 2);

    AddCreatureIcons(msg, creature);

    msg.addByte(player->getSkullClient(creature));
    msg.addByte(player->getPartyShield(otherPlayer));

    if (!known) {
        msg.addByte(player->getGuildEmblem(otherPlayer));
    }

    // Creature type and summon emblem
    msg.addByte(creature->isHealthHidden() ? CREATURETYPE_HIDDEN : creatureType);
    if (creatureType == CREATURETYPE_SUMMON_OWN) {
        msg.add<uint32_t>(masterId);
    }

    // Player vocation info
    if (creatureType == CREATURETYPE_PLAYER) {
        msg.addByte(otherPlayer ? otherPlayer->getVocation()->getClientId() : 0x00);
    }

    if (const auto npc = creature->getNpc()) {
        msg.addByte(npc->getSpeechBubble());
    } else {
        msg.addByte(SPEECHBUBBLE_NONE);
    }

    msg.addByte(0xFF); // MARK_UNMARKED
    msg.addByte(0x00); // inspection type (bool?)

    msg.addByte(player->canWalkthroughEx(creature) ? 0x00 : 0x01);
}

static void GetTileDescription(const GameConnection_ptr &connection, NetworkMessage &msg, const Tile *tile){
    int32_t count;
    Item* ground = tile->getGround();
    if (ground) {
        msg.addItem(ground);
        count = 1;
    } else {
        count = 0;
    }

    const TileItemVector* items = tile->getItemList();
    if (items) {
        for (auto it = items->getBeginTopItem(), end = items->getEndTopItem(); it != end; ++it) {
            msg.addItem(*it);

            if (++count == MAX_STACKPOS) {
                break;
            }
        }
    }

    const CreatureVector* creatures = tile->getCreatures();
    if (creatures) {
        const Player *player = connection->player;
        for (auto it = creatures->rbegin(), end = creatures->rend(); it != end; ++it) {
            const Creature* creature = (*it);
            if (!player->canSeeCreature(creature)) {
                continue;
            }

            AddCreature(connection, msg, creature);
            ++count;
        }
    }

    if (items && count < MAX_STACKPOS) {
        for (auto it = items->getBeginDownItem(), end = items->getEndDownItem(); it != end; ++it) {
            msg.addItem(*it);

            if (++count == MAX_STACKPOS) {
                return;
            }
        }
    }
}

static void GetFloorDescription(const GameConnection_ptr &connection, NetworkMessage& msg,
        int32_t x, int32_t y, int32_t z, int32_t width, int32_t height, int32_t offset,
        int32_t& skip){
    for (int32_t nx = 0; nx < width; nx++) {
        for (int32_t ny = 0; ny < height; ny++) {
            Tile* tile = g_game.map.getTile(x + nx + offset, y + ny + offset, z);
            if (tile) {
                if (skip >= 0) {
                    msg.addByte(skip);
                    msg.addByte(0xFF);
                }

                skip = 0;
                GetTileDescription(connection, msg, tile);
            } else if (skip == 0xFE) {
                msg.addByte(0xFF);
                msg.addByte(0xFF);
                skip = -1;
            } else {
                ++skip;
            }
        }
    }
}

static void GetMapDescription(const GameConnection_ptr &connection, NetworkMessage& msg,
        int32_t x, int32_t y, int32_t z, int32_t width, int32_t height){
    int32_t skip = -1;
    int32_t startz, endz, zstep;

    if (z > 7) {
        startz = z - 2;
        endz = std::min<int32_t>(MAP_MAX_LAYERS - 1, z + 2);
        zstep = 1;
    } else {
        startz = 7;
        endz = 0;
        zstep = -1;
    }

    for (int32_t nz = startz; nz != endz + zstep; nz += zstep) {
        GetFloorDescription(connection, msg, x, y, nz, width, height, z - nz, skip);
    }

    if (skip >= 0) {
        msg.addByte(skip);
        msg.addByte(0xFF);
    }
}

static void MoveUpCreature(const GameConnection_ptr &connection, NetworkMessage& msg,
        const Creature* creature, const Position& newPos, const Position& oldPos){
    if (creature != connection->player) {
        return;
    }

    // floor change up
    msg.addByte(0xBE);

    // going to surface
    if (newPos.z == 7) {
        int32_t skip = -1;

        // floor 7 and 6 already set
        for (int i = 5; i >= 0; --i) {
            GetFloorDescription(connection, msg,
                    oldPos.x - Map::maxClientViewportX, // x
                    oldPos.y - Map::maxClientViewportY, // y
                    i,                                  // z
                    (Map::maxClientViewportX * 2) + 2,  // width
                    (Map::maxClientViewportY * 2) + 2,  // height
                    8 - i,                              // offset
                    skip);
        }
        if (skip >= 0) {
            msg.addByte(skip);
            msg.addByte(0xFF);
        }
    }
    // underground, going one floor up (still underground)
    else if (newPos.z > 7) {
        int32_t skip = -1;
        GetFloorDescription(connection, msg,
                oldPos.x - Map::maxClientViewportX, // x
                oldPos.y - Map::maxClientViewportY, // y
                oldPos.getZ() - 3,                  // z
                (Map::maxClientViewportX * 2) + 2,  // width
                (Map::maxClientViewportY * 2) + 2,  // height
                3,
                skip);

        if (skip >= 0) {
            msg.addByte(skip);
            msg.addByte(0xFF);
        }
    }

    // moving up a floor up makes us out of sync
    // west
    msg.addByte(0x68);
    GetMapDescription(connection, msg,
            oldPos.x - Map::maxClientViewportX,         // x
            oldPos.y - (Map::maxClientViewportY - 1),   // y
            newPos.z,                                   // z
            1,                                          // width
            (Map::maxClientViewportY * 2) + 2);         // height

    // north
    msg.addByte(0x65);
    GetMapDescription(connection, msg,
            oldPos.x - Map::maxClientViewportX, // x
            oldPos.y - Map::maxClientViewportY, // y
            newPos.z,                           // z
            (Map::maxClientViewportX * 2) + 2,  // width
            1);                                 // height
}

static void MoveDownCreature(const GameConnection_ptr &connection, NetworkMessage& msg,
        const Creature* creature, const Position& newPos, const Position& oldPos){
    if (creature != connection->player) {
        return;
    }

    // floor change down
    msg.addByte(0xBF);

    // going from surface to underground
    if (newPos.z == 8) {
        int32_t skip = -1;

        for (int i = 0; i < 3; ++i) {
            GetFloorDescription(connection, msg,
                    oldPos.x - Map::maxClientViewportX, // x
                    oldPos.y - Map::maxClientViewportY, // y
                    newPos.z + i,                       // z
                    (Map::maxClientViewportX * 2) + 2,  // width
                    (Map::maxClientViewportY * 2) + 2,  // height
                    -i - 1,                             // offset
                    skip);
        }
        if (skip >= 0) {
            msg.addByte(skip);
            msg.addByte(0xFF);
        }
    }
    // going further down
    else if (newPos.z > oldPos.z && newPos.z > 8 && newPos.z < 14) {
        int32_t skip = -1;
        GetFloorDescription(connection, msg,
                oldPos.x - Map::maxClientViewportX, // x
                oldPos.y - Map::maxClientViewportY, // y
                newPos.z + 2,                       // z
                (Map::maxClientViewportX * 2) + 2,  // width
                (Map::maxClientViewportY * 2) + 2,  // height
                -3,                                 // offset
                skip);

        if (skip >= 0) {
            msg.addByte(skip);
            msg.addByte(0xFF);
        }
    }

    // moving down a floor makes us out of sync
    // east
    msg.addByte(0x66);
    GetMapDescription(connection, msg,
            oldPos.x + (Map::maxClientViewportX + 1),   // x
            oldPos.y - (Map::maxClientViewportY + 1),   // y
            newPos.z,                                   // z
            1,                                          // width
            (Map::maxClientViewportY * 2) + 2);         // height

    // south
    msg.addByte(0x67);
    GetMapDescription(connection, msg,
            oldPos.x - Map::maxClientViewportX,         // x
            oldPos.y + (Map::maxClientViewportY + 1),   // y
            newPos.z,                                   // z
            (Map::maxClientViewportX * 2) + 2,          // width
            1);                                         // height
}

static void SendLoginError(const GameConnection_ptr &connection, std::string_view message){
    NetworkMessage msg;
    msg.addByte(0x14);
    msg.addString(message);
    WriteToOutputBuffer(connection, msg);
    ResolveLogin(connection, GAME_CONNECTION_CLOSE);
}

static void SendLoginWaitList(const GameConnection_ptr &connection, int waitSlot, int retrySeconds){
    NetworkMessage msg;
    msg.addByte(0x16);
    msg.addString(fmt::format("Too many players online.\n"
            "You are at place {:d} on the waiting list.", waitSlot));
    msg.addByte((uint8_t)std::min<int>(retrySeconds, UINT8_MAX));
    WriteToOutputBuffer(connection, msg);
    ResolveLogin(connection, GAME_CONNECTION_CLOSE);
}

static void SendSessionEnd(const GameConnection_ptr &connection, SessionEndReason reason){
    NetworkMessage msg;
    msg.addByte(0x18);
    msg.addByte(reason);
    WriteToOutputBuffer(connection, msg);
}

static void SendEnableExtendedOpcode(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0x32);
    msg.addByte(0x00);
    msg.add<uint16_t>(0x0000);
    WriteToOutputBuffer(connection, msg);
}

// TODO(fusion): Remove?
static void RemoveTileThing(NetworkMessage& msg, const Position& pos, uint32_t stackpos){
    if (stackpos >= MAX_STACKPOS) {
        return;
    }

    msg.addByte(0x6C);
    msg.addPosition(pos);
    msg.addByte(stackpos);
}

// TODO(fusion): Remove?
static void RemoveTileCreature(NetworkMessage& msg, const Creature* creature,
                               const Position& pos, uint32_t stackpos){
    if (stackpos < MAX_STACKPOS) {
        RemoveTileThing(msg, pos, stackpos);
        return;
    }

    msg.addByte(0x6C);
    msg.add<uint16_t>(0xFFFF);
    msg.add<uint32_t>(creature->getID());
}

//==============================================================================
// External Send Functions
//==============================================================================

void Logout(const GameConnection_ptr &connection, bool displayEffect, bool forced){
    Player *player = connection->player;
    if (!player) {
        return;
    }

    if (!player->isRemoved()) {
        if (!forced) {
            if (!player->isAccessPlayer()) {
                if (player->getTile()->hasFlag(TILESTATE_NOLOGOUT)) {
                    player->sendCancelMessage(RETURNVALUE_YOUCANNOTLOGOUTHERE);
                    return;
                }

                if (!player->getTile()->hasFlag(TILESTATE_PROTECTIONZONE) && player->hasCondition(CONDITION_INFIGHT)) {
                    player->sendCancelMessage(RETURNVALUE_YOUMAYNOTLOGOUTDURINGAFIGHT);
                    return;
                }
            }

            // scripting event - onLogout
            if (!g_creatureEvents->playerLogout(player)) {
                // Let the script handle the error message
                return;
            }
        }

        if (displayEffect && !player->isDead() && !player->isInGhostMode()) {
            g_game.addMagicEffect(player->getPosition(), CONST_ME_POFF);
        }
    }

    SendSessionEnd(connection, forced ? SESSION_END_FORCECLOSE : SESSION_END_LOGOUT);
    Detach(connection);

    g_game.removeCreature(player);
}

void SendOpenPrivateChannel(const GameConnection_ptr &connection, const std::string& receiver){
    NetworkMessage msg;
    msg.addByte(0xAD);
    msg.addString(receiver);
    WriteToOutputBuffer(connection, msg);
}

void SendChannelEvent(const GameConnection_ptr &connection, uint16_t channelId,
                    const std::string& playerName, ChannelEvent_t channelEvent){
    NetworkMessage msg;
    msg.addByte(0xF3);
    msg.add<uint16_t>(channelId);
    msg.addString(playerName);
    msg.addByte(channelEvent);
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureOutfit(const GameConnection_ptr &connection,
            const Creature* creature, const Outfit_t& outfit){
    if (!CanSeeCreature(connection->player, creature)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x8E);
    msg.add<uint32_t>(creature->getID());
    AddOutfit(msg, outfit);
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureLight(const GameConnection_ptr &connection, const Creature* creature){
    if (!CanSeeCreature(connection->player, creature)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x8D);
    msg.add<uint32_t>(creature->getID());

    auto&& [level, color] = creature->getCreatureLight();
    msg.addByte((connection->player->isAccessPlayer() ? 0xFF : level));
    msg.addByte(color);
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureWalkthrough(const GameConnection_ptr &connection, const Creature* creature, bool walkthrough){
    if (!CanSeeCreature(connection->player, creature)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x92);
    msg.add<uint32_t>(creature->getID());
    msg.addByte(walkthrough ? 0x00 : 0x01);
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureShield(const GameConnection_ptr &connection, const Creature* creature){
    if (!CanSeeCreature(connection->player, creature)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x91);
    msg.add<uint32_t>(creature->getID());
    msg.addByte(connection->player->getPartyShield(creature->getPlayer()));
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureSkull(const GameConnection_ptr &connection, const Creature* creature){
    if (g_game.getWorldType() != WORLD_TYPE_PVP) {
        return;
    }

    if (!CanSeeCreature(connection->player, creature)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x90);
    msg.add<uint32_t>(creature->getID());
    msg.addByte(connection->player->getSkullClient(creature));
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureSquare(const GameConnection_ptr &connection, const Creature* creature, SquareColor_t color){
    if (!CanSeeCreature(connection->player, creature)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x93);
    msg.add<uint32_t>(creature->getID());
    msg.addByte(0x01);
    msg.addByte(color);
    WriteToOutputBuffer(connection, msg);
}

void SendTutorial(const GameConnection_ptr &connection, uint8_t tutorialId){
    NetworkMessage msg;
    msg.addByte(0xDC);
    msg.addByte(tutorialId);
    WriteToOutputBuffer(connection, msg);
}

void SendAddMarker(const GameConnection_ptr &connection, const Position& pos,
                   uint8_t markType, const std::string& desc){
    NetworkMessage msg;
    msg.addByte(0xDD);
    msg.addByte(0x00); // unknown
    msg.addPosition(pos);
    msg.addByte(markType);
    msg.addString(desc);
    WriteToOutputBuffer(connection, msg);
}

void SendReLoginWindow(const GameConnection_ptr &connection, uint8_t unfairFightReduction){
    NetworkMessage msg;
    msg.addByte(0x28);
    msg.addByte(0x00);
    msg.addByte(unfairFightReduction);
    msg.addByte(0x00); // can use death redemption (bool)
    WriteToOutputBuffer(connection, msg);
}

void SendStats(const GameConnection_ptr &connection){
    const Player *player = connection->player;

    NetworkMessage msg;
    msg.addByte(0xA0);

    msg.add<uint32_t>(static_cast<uint32_t>(player->getHealth()));
    msg.add<uint32_t>(static_cast<uint32_t>(player->getMaxHealth()));

    msg.add<uint32_t>(player->hasFlag(PlayerFlag_HasInfiniteCapacity) ? 1000000 : player->getFreeCapacity());
    msg.add<uint64_t>(player->getExperience());

    msg.add<uint16_t>(player->getLevel());
    msg.addByte(player->getLevelPercent());

    msg.add<uint16_t>(player->getClientExpDisplay());
    msg.add<uint16_t>(player->getClientLowLevelBonusDisplay());
    msg.add<uint16_t>(0); // store exp bonus
    msg.add<uint16_t>(player->getClientStaminaBonusDisplay());

    msg.add<uint32_t>(static_cast<uint32_t>(player->getMana()));
    msg.add<uint32_t>(static_cast<uint32_t>(player->getMaxMana()));

    msg.addByte(player->getSoul());
    msg.add<uint16_t>(player->getStaminaMinutes());
    msg.add<uint16_t>(player->getBaseSpeed() / 2);

    Condition* condition = player->getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT);
    msg.add<uint16_t>(condition ? condition->getTicks() / 1000 : 0x00);

    msg.add<uint16_t>(player->getOfflineTrainingTime() / 60 / 1000);

    msg.add<uint16_t>(0); // xp boost time (seconds)
    msg.addByte(0x00);    // enables exp boost in the store

    if (ConditionManaShield* conditionManaShield =
            dynamic_cast<ConditionManaShield*>(player->getCondition(CONDITION_MANASHIELD_BREAKABLE))) {
        msg.add<uint32_t>(conditionManaShield->getManaShield());
        msg.add<uint32_t>(conditionManaShield->getMaxManaShield());
    } else {
        msg.add<uint32_t>(0); // remaining mana shield
        msg.add<uint32_t>(0); // total mana shield
    }

    WriteToOutputBuffer(connection, msg);
}

void SendExperienceTracker(const GameConnection_ptr &connection, int64_t rawExp, int64_t finalExp){
    NetworkMessage msg;
    msg.addByte(0xAF);
    msg.add<int64_t>(rawExp);
    msg.add<int64_t>(finalExp);
    WriteToOutputBuffer(connection, msg);
}

void SendClientFeatures(const GameConnection_ptr &connection){
    const Player *player = connection->player;

    NetworkMessage msg;
    msg.addByte(0x17);

    msg.add<uint32_t>(player->getID());
    msg.add<uint16_t>(50); // beat duration

    msg.addDouble(Creature::speedA, 3);
    msg.addDouble(Creature::speedB, 3);
    msg.addDouble(Creature::speedC, 3);

    // can report bugs?
    msg.addByte(player->getAccountType() >= ACCOUNT_TYPE_TUTOR ? 0x01 : 0x00);

    msg.addByte(0x00); // can change pvp framing option
    msg.addByte(0x00); // expert mode button enabled

    msg.add<uint16_t>(0x00); // store images url (string or u16 0x00)
    msg.add<uint16_t>(25);   // premium coin package size

    msg.addByte(0x00); // exiva button enabled (bool)
    msg.addByte(0x00); // Tournament button (bool)

    WriteToOutputBuffer(connection, msg);
}

void SendBasicData(const GameConnection_ptr &connection){
    const Player *player = connection->player;

    NetworkMessage msg;
    msg.addByte(0x9F);
    if (player->isPremium()) {
        msg.addByte(1);
        msg.add<uint32_t>(getBoolean(ConfigManager::FREE_PREMIUM)
                ? 0 : (uint32_t)player->premiumEnd);
    } else {
        msg.addByte(0);
        msg.add<uint32_t>(0);
    }

    msg.addByte(player->getVocation()->getClientId());
    msg.addByte(0x00); // is prey system enabled (bool)

    // unlock spells on action bar
    msg.add<uint16_t>(0xFF);
    for (uint8_t spellId = 0x00; spellId < 0xFF; spellId++) {
        msg.add<uint16_t>(spellId);
    }

    msg.addByte(player->getVocation()->getMagicShield()); // is magic shield active (bool)
    WriteToOutputBuffer(connection, msg);
}

void SendTextMessage(const GameConnection_ptr &connection, const TextMessage& message){
    NetworkMessage msg;
    msg.addByte(0xB4);
    msg.addByte(message.type);
    switch (message.type) {
        case MESSAGE_DAMAGE_DEALT:
        case MESSAGE_DAMAGE_RECEIVED:
        case MESSAGE_DAMAGE_OTHERS: {
            msg.addPosition(message.position);
            msg.add<uint32_t>(message.primary.value);
            msg.addByte(message.primary.color);
            msg.add<uint32_t>(message.secondary.value);
            msg.addByte(message.secondary.color);
            break;
        }
        case MESSAGE_HEALED:
        case MESSAGE_HEALED_OTHERS:
        case MESSAGE_EXPERIENCE:
        case MESSAGE_EXPERIENCE_OTHERS: {
            msg.addPosition(message.position);
            msg.add<uint32_t>(message.primary.value);
            msg.addByte(message.primary.color);
            break;
        }
        case MESSAGE_GUILD:
        case MESSAGE_PARTY_MANAGEMENT:
        case MESSAGE_PARTY:
            msg.add<uint16_t>(message.channelId);
            break;
        default: {
            break;
        }
    }
    msg.addString(message.text);
    WriteToOutputBuffer(connection, msg);
}

void SendClosePrivate(const GameConnection_ptr &connection, uint16_t channelId){
    NetworkMessage msg;
    msg.addByte(0xB3);
    msg.add<uint16_t>(channelId);
    WriteToOutputBuffer(connection, msg);
}

void SendCreatePrivateChannel(const GameConnection_ptr &connection, uint16_t channelId, const std::string& channelName){
    NetworkMessage msg;
    msg.addByte(0xB2);
    msg.add<uint16_t>(channelId);
    msg.addString(channelName);
    msg.add<uint16_t>(0x01);
    msg.addString(connection->player->getName());
    msg.add<uint16_t>(0x00);
    WriteToOutputBuffer(connection, msg);
}

void SendChannelsDialog(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0xAB);

    const ChannelList& list = g_chat->getChannelList(*connection->player);
    msg.addByte(list.size());
    for (ChatChannel* channel : list) {
        msg.add<uint16_t>(channel->getId());
        msg.addString(channel->getName());
    }

    WriteToOutputBuffer(connection, msg);
}

void SendChannel(const GameConnection_ptr &connection, uint16_t channelId,
                 const std::string& channelName, const UsersMap* channelUsers,
                 const InvitedMap* invitedUsers){
    NetworkMessage msg;
    msg.addByte(0xAC);

    msg.add<uint16_t>(channelId);
    msg.addString(channelName);

    if (channelUsers) {
        msg.add<uint16_t>(channelUsers->size());
        for (const auto& it : *channelUsers) {
            msg.addString(it.second->getName());
        }
    } else {
        msg.add<uint16_t>(0x00);
    }

    if (invitedUsers) {
        msg.add<uint16_t>(invitedUsers->size());
        for (const auto& it : *invitedUsers) {
            msg.addString(it.second->getName());
        }
    } else {
        msg.add<uint16_t>(0x00);
    }
    WriteToOutputBuffer(connection, msg);
}

void SendChannelMessage(const GameConnection_ptr &connection, const std::string& author,
                        const std::string& text, SpeakClasses type, uint16_t channel){
    NetworkMessage msg;
    msg.addByte(0xAA);
    msg.add<uint32_t>(0x00);
    msg.addString(author);
    msg.add<uint16_t>(0x00);
    msg.addByte(type);
    msg.add<uint16_t>(channel);
    msg.addString(text);
    WriteToOutputBuffer(connection, msg);
}

void SendIcons(const GameConnection_ptr &connection, uint32_t icons){
    NetworkMessage msg;
    msg.addByte(0xA2);
    msg.add<uint32_t>(icons);
    WriteToOutputBuffer(connection, msg);
}

void SendContainer(const GameConnection_ptr &connection, uint8_t cid,
                   const Container* container, uint16_t firstIndex){
    NetworkMessage msg;
    msg.addByte(0x6E);

    msg.addByte(cid);

    if (container->getID() == ITEM_BROWSEFIELD) {
        msg.addItem(ITEM_BAG, 1);
        msg.addString("Browse Field");
    } else {
        msg.addItem(container);
        msg.addString(container->getName());
    }

    msg.addByte(container->capacity());
    msg.addByte(container->hasContainerParent() ? 0x01 : 0x00);
    msg.addByte(0x00);                                     // show search icon (boolean)
    msg.addByte(container->isUnlocked() ? 0x01 : 0x00);    // Drag and drop
    msg.addByte(container->hasPagination() ? 0x01 : 0x00); // Pagination

    uint32_t containerSize = container->size();
    msg.add<uint16_t>(containerSize);
    msg.add<uint16_t>(firstIndex);
    if (firstIndex < containerSize) {
        int itemsToSend = std::min<int>(container->capacity(), containerSize - firstIndex);
        if(itemsToSend > UINT8_MAX){
            itemsToSend = UINT8_MAX;
        }

        msg.addByte((uint8_t)itemsToSend);
        for(int i = 0; i < itemsToSend; i += 1){
            msg.addItem(container->getItemByIndex(firstIndex + i));
        }
    } else {
        msg.addByte(0x00);
    }
    WriteToOutputBuffer(connection, msg);
}

void SendEmptyContainer(const GameConnection_ptr &connection, uint8_t cid){
    NetworkMessage msg;
    msg.addByte(0x6E);

    msg.addByte(cid);

    msg.addItem(ITEM_BAG, 1);
    msg.addString("Placeholder");

    msg.addByte(8);
    msg.addByte(0x00);
    msg.addByte(0x00);
    msg.addByte(0x01);
    msg.addByte(0x00);
    msg.add<uint16_t>(0);
    msg.add<uint16_t>(0);
    msg.addByte(0x00);
    WriteToOutputBuffer(connection, msg);
}

void SendShop(const GameConnection_ptr &connection, Npc* npc, const ShopInfoList& itemList){
    NetworkMessage msg;
    msg.addByte(0x7A);
    msg.addString(npc->getName());

    // currency displayed in trade window (currently only gold supported) if item other than gold coin is sent, the shop
    // window takes information about currency amount from player items packet (the one that updates action bars)
    msg.add<uint16_t>(Item::items[ITEM_GOLD_COIN].clientId);
    msg.addString(""); // doesn't show anywhere, could be used in otclient for currency name

    uint16_t itemsToSend = std::min<size_t>(itemList.size(), std::numeric_limits<uint16_t>::max());
    msg.add<uint16_t>(itemsToSend);

    uint16_t itemsAdded = 0;
    for (auto item = itemList.begin(); itemsAdded < itemsToSend; ++item, ++itemsAdded) {
        const ItemType& it = Item::items[item->itemId];
        msg.add<uint16_t>(it.clientId);

        if (it.isSplash() || it.isFluidContainer()) {
            msg.addByte(serverFluidToClient(item->subType));
        } else {
            msg.addByte(0x00);
        }

        msg.addString(item->realName);
        msg.add<uint32_t>(it.weight);
        msg.add<uint32_t>(std::max<uint32_t>(item->buyPrice, 0));
        msg.add<uint32_t>(std::max<uint32_t>(item->sellPrice, 0));
    }

    WriteToOutputBuffer(connection, msg);
}

void SendCloseShop(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0x7C);
    WriteToOutputBuffer(connection, msg);
}

void SendSaleItemList(const GameConnection_ptr &connection, const std::list<ShopInfo>& shop){
    const Player *player = connection->player;
    uint64_t playerBank = player->getBankBalance();
    uint64_t playerMoney = player->getMoney();
    SendResourceBalance(connection, RESOURCE_BANK_BALANCE, playerBank);
    SendResourceBalance(connection, RESOURCE_GOLD_EQUIPPED, playerMoney);

    NetworkMessage msg;
    msg.addByte(0x7B);

    std::map<uint16_t, uint32_t> saleMap;

    if (shop.size() <= 5) {
        // For very small shops it's not worth it to create the complete map
        for (const ShopInfo& shopInfo : shop) {
            if (shopInfo.sellPrice == 0) {
                continue;
            }

            int8_t subtype = -1;

            const ItemType& itemType = Item::items[shopInfo.itemId];
            if (itemType.hasSubType() && !itemType.stackable) {
                subtype = (shopInfo.subType == 0 ? -1 : shopInfo.subType);
            }

            uint32_t count = player->getItemTypeCount(shopInfo.itemId, subtype);
            if (count > 0) {
                saleMap[shopInfo.itemId] = count;
            }
        }
    } else {
        // Large shop, it's better to get a cached map of all item counts and use it We need a temporary map since the
        // finished map should only contain items available in the shop
        std::map<uint32_t, uint32_t> tempSaleMap;
        player->getAllItemTypeCount(tempSaleMap);

        // We must still check manually for the special items that require subtype matches (That is, fluids such as
        // potions etc., actually these items are very few since health potions now use their own ID)
        for (const ShopInfo& shopInfo : shop) {
            if (shopInfo.sellPrice == 0) {
                continue;
            }

            int8_t subtype = -1;

            const ItemType& itemType = Item::items[shopInfo.itemId];
            if (itemType.hasSubType() && !itemType.stackable) {
                subtype = (shopInfo.subType == 0 ? -1 : shopInfo.subType);
            }

            if (subtype != -1) {
                uint32_t count;
                if (itemType.isFluidContainer() || itemType.isSplash()) {
                    count = player->getItemTypeCount(shopInfo.itemId,
                                                     subtype); // This shop item requires extra checks
                } else {
                    count = subtype;
                }

                if (count > 0) {
                    saleMap[shopInfo.itemId] = count;
                }
            } else {
                std::map<uint32_t, uint32_t>::const_iterator findIt = tempSaleMap.find(shopInfo.itemId);
                if (findIt != tempSaleMap.end() && findIt->second > 0) {
                    saleMap[shopInfo.itemId] = findIt->second;
                }
            }
        }
    }

    uint8_t itemsToSend = std::min<size_t>(saleMap.size(), std::numeric_limits<uint8_t>::max());
    msg.addByte(itemsToSend);

    uint8_t i = 0;
    for (std::map<uint16_t, uint32_t>::const_iterator it = saleMap.begin(); i < itemsToSend; ++it, ++i) {
        msg.addItemId(it->first);
        msg.add<uint16_t>(std::min<uint16_t>(it->second, std::numeric_limits<uint16_t>::max()));
    }

    WriteToOutputBuffer(connection, msg);
}

void SendResourceBalance(const GameConnection_ptr &connection, const ResourceTypes_t resourceType, uint64_t amount){
    NetworkMessage msg;
    msg.addByte(0xEE);
    msg.addByte(resourceType);
    msg.add<uint64_t>(amount);
    WriteToOutputBuffer(connection, msg);
}

void SendStoreBalance(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0xDF);
    msg.addByte(0x01);

    // placeholder packet / to do
    msg.add<uint32_t>(0); // total store coins (transferable + non-t)
    msg.add<uint32_t>(0); // transferable store coins
    msg.add<uint32_t>(0); // reserved auction coins
    msg.add<uint32_t>(0); // tournament coins
    WriteToOutputBuffer(connection, msg);
}

void SendMarketEnter(const GameConnection_ptr &connection){
    Player *player = connection->player;

    NetworkMessage msg;
    msg.addByte(0xF6);
    msg.addByte(
        std::min<uint32_t>(tfs::iomarket::getPlayerOfferCount(player->getGUID()), std::numeric_limits<uint8_t>::max()));

    player->setInMarket(true);

    std::map<uint16_t, uint32_t> depotItems;
    std::forward_list<Container*> containerList{player->getInbox().get()};

    for (const auto& chest : player->depotChests) {
        if (!chest.second->empty()) {
            containerList.push_front(chest.second.get());
        }
    }

    do {
        Container* container = containerList.front();
        containerList.pop_front();

        for (Item* item : container->getItemList()) {
            Container* c = item->getContainer();
            if (c && !c->empty()) {
                containerList.push_front(c);
                continue;
            }

            const ItemType& itemType = Item::items[item->getID()];
            if (itemType.wareId == 0) {
                continue;
            }

            if (c && (!itemType.isContainer() || c->capacity() != itemType.maxItems)) {
                continue;
            }

            if (!item->hasMarketAttributes()) {
                continue;
            }

            depotItems[itemType.id] += Item::countByType(item, -1);
        }
    } while (!containerList.empty());

    uint16_t itemsToSend = std::min<size_t>(depotItems.size(), std::numeric_limits<uint16_t>::max());
    uint16_t i = 0;

    msg.add<uint16_t>(itemsToSend);
    for (std::map<uint16_t, uint32_t>::const_iterator it = depotItems.begin(); i < itemsToSend; ++it, ++i) {
        const ItemType& itemType = Item::items[it->first];
        msg.add<uint16_t>(itemType.wareId);
        if (itemType.classification > 0) {
            msg.addByte(0);
        }
        msg.add<uint16_t>(std::min<uint32_t>(0xFFFF, it->second));
    }
    WriteToOutputBuffer(connection, msg);

    SendResourceBalance(connection, RESOURCE_BANK_BALANCE, player->getBankBalance());
    SendResourceBalance(connection, RESOURCE_GOLD_EQUIPPED, player->getMoney());
    SendStoreBalance(connection);
}

void SendMarketLeave(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0xF7);
    WriteToOutputBuffer(connection, msg);
}

void SendMarketBrowseItem(const GameConnection_ptr &connection, uint16_t itemId,
                          const MarketOfferList& buyOffers,
                          const MarketOfferList& sellOffers){
    SendStoreBalance(connection);

    NetworkMessage msg;
    msg.addByte(0xF9);
    msg.addByte(MARKETREQUEST_ITEM);
    msg.addItemId(itemId);

    if (Item::items[itemId].classification > 0) {
        msg.addByte(0); // item tier
    }

    msg.add<uint32_t>(buyOffers.size());
    for (const MarketOffer& offer : buyOffers) {
        msg.add<uint32_t>(offer.timestamp);
        msg.add<uint16_t>(offer.counter);
        msg.add<uint16_t>(offer.amount);
        msg.add<uint64_t>(offer.price);
        msg.addString(offer.playerName);
    }

    msg.add<uint32_t>(sellOffers.size());
    for (const MarketOffer& offer : sellOffers) {
        msg.add<uint32_t>(offer.timestamp);
        msg.add<uint16_t>(offer.counter);
        msg.add<uint16_t>(offer.amount);
        msg.add<uint64_t>(offer.price);
        msg.addString(offer.playerName);
    }

    WriteToOutputBuffer(connection, msg);
}

void SendMarketAcceptOffer(const GameConnection_ptr &connection, const MarketOfferEx& offer){
    NetworkMessage msg;
    msg.addByte(0xF9);
    msg.addByte(MARKETREQUEST_ITEM);
    msg.addItemId(offer.itemId);
    if (Item::items[offer.itemId].classification > 0) {
        msg.addByte(0);
    }

    if (offer.type == MARKETACTION_BUY) {
        msg.add<uint32_t>(0x01);
        msg.add<uint32_t>(offer.timestamp);
        msg.add<uint16_t>(offer.counter);
        msg.add<uint16_t>(offer.amount);
        msg.add<uint64_t>(offer.price);
        msg.addString(offer.playerName);
        msg.add<uint32_t>(0x00);
    } else {
        msg.add<uint32_t>(0x00);
        msg.add<uint32_t>(0x01);
        msg.add<uint32_t>(offer.timestamp);
        msg.add<uint16_t>(offer.counter);
        msg.add<uint16_t>(offer.amount);
        msg.add<uint64_t>(offer.price);
        msg.addString(offer.playerName);
    }

    WriteToOutputBuffer(connection, msg);
}

void SendMarketBrowseOwnOffers(const GameConnection_ptr &connection,
                               const MarketOfferList& buyOffers,
                               const MarketOfferList& sellOffers){
    NetworkMessage msg;
    msg.addByte(0xF9);
    msg.addByte(MARKETREQUEST_OWN_OFFERS);

    msg.add<uint32_t>(buyOffers.size());
    for (const MarketOffer& offer : buyOffers) {
        msg.add<uint32_t>(offer.timestamp);
        msg.add<uint16_t>(offer.counter);
        msg.addItemId(offer.itemId);
        if (Item::items[offer.itemId].classification > 0) {
            msg.addByte(0);
        }
        msg.add<uint16_t>(offer.amount);
        msg.add<uint64_t>(offer.price);
    }

    msg.add<uint32_t>(sellOffers.size());
    for (const MarketOffer& offer : sellOffers) {
        msg.add<uint32_t>(offer.timestamp);
        msg.add<uint16_t>(offer.counter);
        msg.addItemId(offer.itemId);
        if (Item::items[offer.itemId].classification > 0) {
            msg.addByte(0);
        }
        msg.add<uint16_t>(offer.amount);
        msg.add<uint64_t>(offer.price);
    }

    WriteToOutputBuffer(connection, msg);
}

void SendMarketCancelOffer(const GameConnection_ptr &connection, const MarketOfferEx& offer){
    NetworkMessage msg;
    msg.addByte(0xF9);
    msg.addByte(MARKETREQUEST_OWN_OFFERS);

    if (offer.type == MARKETACTION_BUY) {
        msg.add<uint32_t>(0x01);
        msg.add<uint32_t>(offer.timestamp);
        msg.add<uint16_t>(offer.counter);
        msg.addItemId(offer.itemId);
        if (Item::items[offer.itemId].classification > 0) {
            msg.addByte(0);
        }
        msg.add<uint16_t>(offer.amount);
        msg.add<uint64_t>(offer.price);
        msg.add<uint32_t>(0x00);
    } else {
        msg.add<uint32_t>(0x00);
        msg.add<uint32_t>(0x01);
        msg.add<uint32_t>(offer.timestamp);
        msg.add<uint16_t>(offer.counter);
        msg.addItemId(offer.itemId);
        if (Item::items[offer.itemId].classification > 0) {
            msg.addByte(0);
        }
        msg.add<uint16_t>(offer.amount);
        msg.add<uint64_t>(offer.price);
    }

    WriteToOutputBuffer(connection, msg);
}

void SendMarketBrowseOwnHistory(const GameConnection_ptr &connection,
                                const HistoryMarketOfferList& buyOffers,
                                const HistoryMarketOfferList& sellOffers){
    uint32_t i = 0;
    std::map<uint32_t, uint16_t> counterMap;
    uint32_t buyOffersToSend =
        std::min<uint32_t>(buyOffers.size(), 810 + std::max<int32_t>(0, 810 - sellOffers.size()));
    uint32_t sellOffersToSend =
        std::min<uint32_t>(sellOffers.size(), 810 + std::max<int32_t>(0, 810 - buyOffers.size()));

    NetworkMessage msg;
    msg.addByte(0xF9);
    msg.addByte(MARKETREQUEST_OWN_HISTORY);

    msg.add<uint32_t>(buyOffersToSend);
    for (auto it = buyOffers.begin(); i < buyOffersToSend; ++it, ++i) {
        msg.add<uint32_t>(it->timestamp);
        msg.add<uint16_t>(counterMap[it->timestamp]++);
        msg.addItemId(it->itemId);
        if (Item::items[it->itemId].classification > 0) {
            msg.addByte(0);
        }
        msg.add<uint16_t>(it->amount);
        msg.add<uint64_t>(it->price);
        msg.addByte(it->state);
    }

    counterMap.clear();
    i = 0;

    msg.add<uint32_t>(sellOffersToSend);
    for (auto it = sellOffers.begin(); i < sellOffersToSend; ++it, ++i) {
        msg.add<uint32_t>(it->timestamp);
        msg.add<uint16_t>(counterMap[it->timestamp]++);
        msg.addItemId(it->itemId);
        if (Item::items[it->itemId].classification > 0) {
            msg.addByte(0);
        }
        msg.add<uint16_t>(it->amount);
        msg.add<uint64_t>(it->price);
        msg.addByte(it->state);
    }

    WriteToOutputBuffer(connection, msg);
}

void SendTradeItemRequest(const GameConnection_ptr &connection,
                          const std::string& traderName,
                          const Item* item, bool ack){
    NetworkMessage msg;

    if (ack) {
        msg.addByte(0x7D);
    } else {
        msg.addByte(0x7E);
    }

    msg.addString(traderName);

    if (const Container* tradeContainer = item->getContainer()) {
        std::list<const Container*> listContainer{tradeContainer};
        std::list<const Item*> itemList{tradeContainer};
        while (!listContainer.empty()) {
            const Container* container = listContainer.front();
            listContainer.pop_front();

            for (Item* containerItem : container->getItemList()) {
                Container* tmpContainer = containerItem->getContainer();
                if (tmpContainer) {
                    listContainer.push_back(tmpContainer);
                }
                itemList.push_back(containerItem);
            }
        }

        msg.addByte(itemList.size());
        for (const Item* listItem : itemList) {
            msg.addItem(listItem);
        }
    } else {
        msg.addByte(0x01);
        msg.addItem(item);
    }
    WriteToOutputBuffer(connection, msg);
}

void SendCloseTrade(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0x7F);
    WriteToOutputBuffer(connection, msg);
}

void SendCloseContainer(const GameConnection_ptr &connection, uint8_t cid){
    NetworkMessage msg;
    msg.addByte(0x6F);
    msg.addByte(cid);
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureTurn(const GameConnection_ptr &connection, const Creature* creature, uint32_t stackpos){
    if (!CanSeeCreature(connection->player, creature)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x6B);
    if (stackpos >= MAX_STACKPOS) {
        msg.add<uint16_t>(0xFFFF);
        msg.add<uint32_t>(creature->getID());
    } else {
        msg.addPosition(creature->getPosition());
        msg.addByte(stackpos);
    }

    msg.add<uint16_t>(0x63);
    msg.add<uint32_t>(creature->getID());
    msg.addByte(creature->getDirection());
    msg.addByte(connection->player->canWalkthroughEx(creature) ? 0x00 : 0x01);
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureSay(const GameConnection_ptr &connection, const Creature* creature,
                     SpeakClasses type, const std::string& text, const Position* pos /* = nullptr*/){
    NetworkMessage msg;
    msg.addByte(0xAA);

    static uint32_t statementId = 0;
    msg.add<uint32_t>(++statementId);

    msg.addString(creature->getName());
    msg.addByte(0x00); // "(Traded)" suffix after player name

    // Add level only for players
    if (const Player* speaker = creature->getPlayer()) {
        msg.add<uint16_t>(speaker->getLevel());
    } else {
        msg.add<uint16_t>(0x00);
    }

    msg.addByte(type);
    if (pos) {
        msg.addPosition(*pos);
    } else {
        msg.addPosition(creature->getPosition());
    }

    msg.addString(text);
    WriteToOutputBuffer(connection, msg);
}

void SendToChannel(const GameConnection_ptr &connection, const Creature* creature,
                   SpeakClasses type, const std::string& text, uint16_t channelId){
    NetworkMessage msg;
    msg.addByte(0xAA);

    static uint32_t statementId = 0;
    msg.add<uint32_t>(++statementId);
    if (!creature) {
        msg.add<uint32_t>(0x00);
        msg.addByte(0x00); // "(Traded)" suffix after player name
    } else {
        msg.addString(creature->getName());
        msg.addByte(0x00); // "(Traded)" suffix after player name

        // Add level only for players
        if (const Player* speaker = creature->getPlayer()) {
            msg.add<uint16_t>(speaker->getLevel());
        } else {
            msg.add<uint16_t>(0x00);
        }
    }

    msg.addByte(type);
    msg.add<uint16_t>(channelId);
    msg.addString(text);
    WriteToOutputBuffer(connection, msg);
}

void SendPrivateMessage(const GameConnection_ptr &connection, const Player* speaker,
                        SpeakClasses type, const std::string& text){
    NetworkMessage msg;
    msg.addByte(0xAA);
    static uint32_t statementId = 0;
    msg.add<uint32_t>(++statementId);
    if (speaker) {
        msg.addString(speaker->getName());
        msg.addByte(0x00); // "(Traded)" suffix after player name
        msg.add<uint16_t>(speaker->getLevel());
    } else {
        msg.add<uint32_t>(0x00);
        msg.addByte(0x00); // "(Traded)" suffix after player name
    }
    msg.addByte(type);
    msg.addString(text);
    WriteToOutputBuffer(connection, msg);
}

void SendCancelTarget(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0xA3);
    msg.add<uint32_t>(0x00);
    WriteToOutputBuffer(connection, msg);
}

void SendChangeSpeed(const GameConnection_ptr &connection, const Creature* creature, uint32_t speed){
    NetworkMessage msg;
    msg.addByte(0x8F);
    msg.add<uint32_t>(creature->getID());
    msg.add<uint16_t>(creature->getBaseSpeed() / 2);
    msg.add<uint16_t>(speed / 2);
    WriteToOutputBuffer(connection, msg);
}

void SendCancelWalk(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0xB5);
    msg.addByte(connection->player->getDirection());
    WriteToOutputBuffer(connection, msg);
}

void SendSkills(const GameConnection_ptr &connection){
    const Player *player = connection->player;

    NetworkMessage msg;
    msg.addByte(0xA1);
    msg.add<uint16_t>(player->getMagicLevel());
    msg.add<uint16_t>(player->getBaseMagicLevel());
    msg.add<uint16_t>(player->getBaseMagicLevel()); // base + loyalty bonus(?)
    msg.add<uint16_t>(player->getMagicLevelPercent());

    for (uint8_t i = SKILL_FIRST; i <= SKILL_LAST; ++i) {
        msg.add<uint16_t>(std::min<int32_t>(player->getSkillLevel(i), std::numeric_limits<uint16_t>::max()));
        msg.add<uint16_t>(player->getBaseSkill(i));
        msg.add<uint16_t>(player->getBaseSkill(i)); // base + loyalty bonus(?)
        msg.add<uint16_t>(player->getSkillPercent(i));
    }

    for (uint8_t i = SPECIALSKILL_FIRST; i <= SPECIALSKILL_LAST; ++i) {
        msg.add<uint16_t>(player->getSpecialSkill(i));  // base + bonus special skill
        msg.add<uint16_t>(0);                           // base special skill
    }

    msg.addByte(0); // element magic level
    // structure:
    // u8 client element id
    // u16 bonus element ml

    // fatal, dodge, momentum
    for (int i = 0; i < 3; ++i) {
        msg.add<uint16_t>(0);
        msg.add<uint16_t>(0);
    }

    // to do: bonus cap
    uint32_t capacityValue = player->hasFlag(PlayerFlag_HasInfiniteCapacity) ? 1000000 : player->getCapacity();
    msg.add<uint32_t>(capacityValue); // base + bonus capacity
    msg.add<uint32_t>(capacityValue); // base capacity
    WriteToOutputBuffer(connection, msg);
}

void SendPing(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0x1D);
    WriteToOutputBuffer(connection, msg);
}

void SendPingBack(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0x1E);
    WriteToOutputBuffer(connection, msg);
}

void SendDistanceShoot(const GameConnection_ptr &connection, const Position& from, const Position& to, uint8_t type){
    NetworkMessage msg;
    msg.addByte(0x83);
    msg.addPosition(from);
    msg.addByte(MAGIC_EFFECTS_CREATE_DISTANCEEFFECT);
    msg.addByte(type);
    msg.addByte(static_cast<uint8_t>(static_cast<int8_t>(static_cast<int32_t>(to.x) - static_cast<int32_t>(from.x))));
    msg.addByte(static_cast<uint8_t>(static_cast<int8_t>(static_cast<int32_t>(to.y) - static_cast<int32_t>(from.y))));
    msg.addByte(MAGIC_EFFECTS_END_LOOP);
    WriteToOutputBuffer(connection, msg);
}

void SendMagicEffect(const GameConnection_ptr &connection, const Position& pos, uint8_t type){
    if (!CanSeePosition(connection->player, pos)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x83);
    msg.addPosition(pos);
    msg.addByte(MAGIC_EFFECTS_CREATE_EFFECT);
    msg.addByte(type);
    msg.addByte(MAGIC_EFFECTS_END_LOOP);
    WriteToOutputBuffer(connection, msg);
}

void SendCreatureHealth(const GameConnection_ptr &connection, const Creature* creature){
    NetworkMessage msg;
    msg.addByte(0x8C);
    msg.add<uint32_t>(creature->getID());

    if (creature->isHealthHidden()) {
        msg.addByte(0x00);
    } else {
        msg.addByte(std::ceil(
            (static_cast<double>(creature->getHealth()) / std::max<int32_t>(creature->getMaxHealth(), 1)) * 100));
    }
    WriteToOutputBuffer(connection, msg);
}

void SendFYIBox(const GameConnection_ptr &connection, const std::string& message){
    NetworkMessage msg;
    msg.addByte(0x15);
    msg.addString(message);
    WriteToOutputBuffer(connection, msg);
}

void SendMapDescription(const GameConnection_ptr &connection, const Position& pos){
    NetworkMessage msg;
    msg.addByte(0x64);
    msg.addPosition(connection->player->getPosition());
    GetMapDescription(connection, msg,
            pos.x - Map::maxClientViewportX,    // x
            pos.y - Map::maxClientViewportY,    // y
            pos.z,                              // z
            (Map::maxClientViewportX * 2) + 2,  // width
            (Map::maxClientViewportY * 2) + 2); // height
    WriteToOutputBuffer(connection, msg);
}

void SendAddTileItem(const GameConnection_ptr &connection, const Position& pos,
                     uint32_t stackpos, const Item* item){
    if (!CanSeePosition(connection->player, pos)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x6A);
    msg.addPosition(pos);
    msg.addByte(stackpos);
    msg.addItem(item);
    WriteToOutputBuffer(connection, msg);
}

void SendUpdateTileItem(const GameConnection_ptr &connection, const Position& pos,
                        uint32_t stackpos, const Item* item){
    if (!CanSeePosition(connection->player, pos)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x6B);
    msg.addPosition(pos);
    msg.addByte(stackpos);
    msg.addItem(item);
    WriteToOutputBuffer(connection, msg);
}

void SendRemoveTileThing(const GameConnection_ptr &connection, const Position& pos, uint32_t stackpos){
    if (!CanSeePosition(connection->player, pos)) {
        return;
    }

    NetworkMessage msg;
    RemoveTileThing(msg, pos, stackpos);
    WriteToOutputBuffer(connection, msg);
}

void SendUpdateTileCreature(const GameConnection_ptr &connection, const Position& pos, uint32_t stackpos, const Creature* creature){
    if (!CanSeePosition(connection->player, pos)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x6B);
    msg.addPosition(pos);
    msg.addByte(stackpos);
    AddCreature(connection, msg, creature, true);
    WriteToOutputBuffer(connection, msg);
}

void SendRemoveTileCreature(const GameConnection_ptr &connection, const Creature* creature, const Position& pos, uint32_t stackpos){
    if (stackpos < MAX_STACKPOS) {
        if (!CanSeePosition(connection->player, pos)) {
            return;
        }

        NetworkMessage msg;
        RemoveTileThing(msg, pos, stackpos);
        WriteToOutputBuffer(connection, msg);
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x6C);
    msg.add<uint16_t>(0xFFFF);
    msg.add<uint32_t>(creature->getID());
    WriteToOutputBuffer(connection, msg);
}

void SendUpdateTile(const GameConnection_ptr &connection, const Tile* tile, const Position& pos){
    if (!CanSeePosition(connection->player, pos)) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x69);
    msg.addPosition(pos);

    if (tile) {
        GetTileDescription(connection, msg, tile);
        msg.addByte(0x00);
        msg.addByte(0xFF);
    } else {
        msg.addByte(0x01);
        msg.addByte(0xFF);
    }

    WriteToOutputBuffer(connection, msg);
}

void SendUpdateCreatureIcons(const GameConnection_ptr &connection, const Creature* creature){
    if (!CanSeePosition(connection->player, creature->getPosition())) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0x8B);
    msg.add<uint32_t>(creature->getID());
    msg.addByte(14); // event player icons
    AddCreatureIcons(msg, creature);
    WriteToOutputBuffer(connection, msg);
}

void SendPendingStateEntered(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0x0A);
    WriteToOutputBuffer(connection, msg);
}

void SendEnterWorld(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0x0F);
    WriteToOutputBuffer(connection, msg);
}

void SendFightModes(const GameConnection_ptr &connection){
    const Player *player = connection->player;
    NetworkMessage msg;
    msg.addByte(0xA7);
    msg.addByte(player->fightMode);
    msg.addByte(player->chaseMode);
    msg.addByte(player->secureMode);
    msg.addByte(PVP_MODE_DOVE);
    WriteToOutputBuffer(connection, msg);
}

void SendAddCreature(const GameConnection_ptr &connection, const Creature* creature,
                     const Position& pos, int32_t stackpos, MagicEffectClasses magicEffect /*= CONST_ME_NONE*/){
    assert(creature != connection->player);
    if (!CanSeePosition(connection->player, pos)) {
        return;
    }

    // stack pos is always real index now, so it can exceed the limit if stack pos exceeds the limit, we need to
    // refresh the tile instead
    // 1. this is a rare case, and is only triggered by forcing summon in a position
    // 2. since no stackpos will be send to the client about that creature, removing it must be done with its id if
    // its stackpos remains >= MAX_STACKPOS. this is done to add creatures to battle list instead of rendering on
    // screen
    if (stackpos >= MAX_STACKPOS) {
        // @todo: should we avoid this check?
        if (const Tile* tile = creature->getTile()) {
            SendUpdateTile(connection, tile, pos);
        }
    } else {
        // if stackpos is -1, the client will automatically detect it
        NetworkMessage msg;
        msg.addByte(0x6A);
        msg.addPosition(pos);
        msg.addByte(stackpos);
        AddCreature(connection, msg, creature);
        WriteToOutputBuffer(connection, msg);
    }

    if (magicEffect != CONST_ME_NONE) {
        SendMagicEffect(connection, pos, magicEffect);
    }
}

void SendMoveCreature(const GameConnection_ptr &connection, const Creature* creature,
                      const Position& newPos, int32_t newStackPos,
                      const Position& oldPos, int32_t oldStackPos, bool teleport){
    // TODO(fusion): Merge all packets into the same message, to avoid partial
    // state in case the connection auto-sends in between send calls.
    if (creature == connection->player) {
        if (teleport) {
            SendRemoveTileCreature(connection, creature, oldPos, oldStackPos);
            SendMapDescription(connection, newPos);
        } else {
            NetworkMessage msg;
            if (oldPos.z == 7 && newPos.z >= 8) {
                RemoveTileCreature(msg, creature, oldPos, oldStackPos);
            } else {
                msg.addByte(0x6D);
                if (oldStackPos < MAX_STACKPOS) {
                    msg.addPosition(oldPos);
                    msg.addByte(oldStackPos);
                } else {
                    msg.add<uint16_t>(0xFFFF);
                    msg.add<uint32_t>(creature->getID());
                }
                msg.addPosition(newPos);
            }

            if (newPos.z > oldPos.z) {
                MoveDownCreature(connection, msg, creature, newPos, oldPos);
            } else if (newPos.z < oldPos.z) {
                MoveUpCreature(connection, msg, creature, newPos, oldPos);
            }

            if (oldPos.y > newPos.y) { // north, for old x
                msg.addByte(0x65);
                GetMapDescription(connection, msg,
                        oldPos.x - Map::maxClientViewportX, // x
                        newPos.y - Map::maxClientViewportY, // y
                        newPos.z,                           // z
                        (Map::maxClientViewportX * 2) + 2,  // width
                        1);                                 // height
            } else if (oldPos.y < newPos.y) { // south, for old x
                msg.addByte(0x67);
                GetMapDescription(connection, msg,
                        oldPos.x - Map::maxClientViewportX,         // x
                        newPos.y + (Map::maxClientViewportY + 1),   // y
                        newPos.z,                                   // z
                        (Map::maxClientViewportX * 2) + 2,          // width
                        1);                                         // height
            }

            if (oldPos.x < newPos.x) { // east, [with new y]
                msg.addByte(0x66);
                GetMapDescription(connection, msg,
                        newPos.x + (Map::maxClientViewportX + 1),   // x
                        newPos.y - Map::maxClientViewportY,         // y
                        newPos.z,                                   // z
                        1,                                          // width
                        (Map::maxClientViewportY * 2) + 2);         // height
            } else if (oldPos.x > newPos.x) { // west, [with new y]
                msg.addByte(0x68);
                GetMapDescription(connection, msg,
                        newPos.x - Map::maxClientViewportX, // x
                        newPos.y - Map::maxClientViewportY, // y
                        newPos.z,                           // z
                        1,                                  // width
                        (Map::maxClientViewportY * 2) + 2); // height
            }
            WriteToOutputBuffer(connection, msg);
        }
    } else if (CanSeePosition(connection->player, oldPos)
            && CanSeePosition(connection->player, creature->getPosition())) {
        if (teleport || (oldPos.z == 7 && newPos.z >= 8)) {
            SendRemoveTileCreature(connection, creature, oldPos, oldStackPos);
            SendAddCreature(connection, creature, newPos, newStackPos);
        } else {
            NetworkMessage msg;
            msg.addByte(0x6D);
            if (oldStackPos < MAX_STACKPOS) {
                msg.addPosition(oldPos);
                msg.addByte(oldStackPos);
            } else {
                msg.add<uint16_t>(0xFFFF);
                msg.add<uint32_t>(creature->getID());
            }
            msg.addPosition(creature->getPosition());
            WriteToOutputBuffer(connection, msg);
        }
    } else if (CanSeePosition(connection->player, oldPos)) {
        SendRemoveTileCreature(connection, creature, oldPos, oldStackPos);
    } else if (CanSeePosition(connection->player, creature->getPosition())) {
        SendAddCreature(connection, creature, newPos, newStackPos);
    }
}

void SendInventoryItem(const GameConnection_ptr &connection, slots_t slot, const Item* item){
    NetworkMessage msg;
    if (item) {
        msg.addByte(0x78);
        msg.addByte(slot);
        msg.addItem(item);
    } else {
        msg.addByte(0x79);
        msg.addByte(slot);
    }
    WriteToOutputBuffer(connection, msg);
}

// to do: make it lightweight, update each time player gets/loses an item
void SendItems(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0xF5);

    // find all items carried by character (itemId, amount)
    std::map<uint32_t, uint32_t> inventory;
    connection->player->getAllItemTypeCount(inventory);

    msg.add<uint16_t>(inventory.size() + 11);
    for (uint16_t i = 1; i <= 11; i++) {
        msg.add<uint16_t>(i); // slotId
        msg.addByte(0);       // always 0
        msg.add<uint16_t>(1); // always 1
    }

    for (const auto& item : inventory) {
        msg.add<uint16_t>(Item::items[item.first].clientId); // item clientId
        msg.addByte(0);                                      // always 0
        msg.add<uint16_t>(item.second);                      // count
    }

    WriteToOutputBuffer(connection, msg);
}

void SendAddContainerItem(const GameConnection_ptr &connection,
                          uint8_t cid, uint16_t slot, const Item* item){
    NetworkMessage msg;
    msg.addByte(0x70);
    msg.addByte(cid);
    msg.add<uint16_t>(slot);
    if(item){
        msg.addItem(item);
    }else{
        msg.add<uint16_t>(0x00);
    }
    WriteToOutputBuffer(connection, msg);
}

void SendUpdateContainerItem(const GameConnection_ptr &connection,
                             uint8_t cid, uint16_t slot, const Item* item){
    NetworkMessage msg;
    msg.addByte(0x71);
    msg.addByte(cid);
    msg.add<uint16_t>(slot);
    msg.addItem(item);
    WriteToOutputBuffer(connection, msg);
}

void SendRemoveContainerItem(const GameConnection_ptr &connection,
                             uint8_t cid, uint16_t slot, const Item* lastItem){
    NetworkMessage msg;
    msg.addByte(0x72);
    msg.addByte(cid);
    msg.add<uint16_t>(slot);
    if (lastItem) {
        msg.addItem(lastItem);
    } else {
        msg.add<uint16_t>(0x00);
    }
    WriteToOutputBuffer(connection, msg);
}

void SendTextWindow(const GameConnection_ptr &connection, uint32_t windowTextId,
                    Item* item, uint16_t maxlen, bool canWrite){
    NetworkMessage msg;
    msg.addByte(0x96);
    msg.add<uint32_t>(windowTextId);
    msg.addItem(item);

    if (canWrite) {
        msg.add<uint16_t>(maxlen);
        msg.addString(item->getText());
    } else {
        const std::string& text = item->getText();
        msg.add<uint16_t>(text.size());
        msg.addString(text);
    }

    const std::string& writer = item->getWriter();
    if (!writer.empty()) {
        msg.addString(writer);
    } else {
        msg.add<uint16_t>(0x00);
    }

    msg.addByte(0x00); // "(traded)" suffix after player name (bool)

    time_t writtenDate = item->getDate();
    if (writtenDate != 0) {
        msg.addString(formatDateShort(writtenDate));
    } else {
        msg.add<uint16_t>(0x00);
    }

    WriteToOutputBuffer(connection, msg);
}

void SendTextWindow(const GameConnection_ptr &connection, uint32_t windowTextId,
                    uint32_t itemId, const std::string& text){
    NetworkMessage msg;
    msg.addByte(0x96);
    msg.add<uint32_t>(windowTextId);
    msg.addItem(itemId, 1);
    msg.add<uint16_t>(text.size());
    msg.addString(text);
    msg.add<uint16_t>(0x00); // writer name
    msg.addByte(0x00);       // "(traded)" byte
    msg.add<uint16_t>(0x00); // date
    WriteToOutputBuffer(connection, msg);
}

void SendHouseWindow(const GameConnection_ptr &connection,
                     uint32_t windowTextId, const std::string& text){
    NetworkMessage msg;
    msg.addByte(0x97);
    msg.addByte(0x00);
    msg.add<uint32_t>(windowTextId);
    msg.addString(text);
    WriteToOutputBuffer(connection, msg);
}

void SendCombatAnalyzer(const GameConnection_ptr &connection, CombatType_t type,
                        int32_t amount, DamageAnalyzerImpactType impactType,
                        const std::string& target){
    NetworkMessage msg;
    msg.addByte(0xCC);
    msg.addByte(impactType);
    msg.add<uint32_t>(amount);

    switch (impactType) {
        case RECEIVED:
            msg.addByte(GetClientDamageType(type));
            msg.addString(target);
            break;

        case DEALT:
            msg.addByte(GetClientDamageType(type));
            break;

        default:
            break;
    }
    WriteToOutputBuffer(connection, msg);
}

void SendOutfitWindow(const GameConnection_ptr &connection){
    const Player *player = connection->player;
    const auto& outfits = Outfits::getInstance().getOutfits(player->getSex());
    if (outfits.size() == 0) {
        return;
    }

    NetworkMessage msg;
    msg.addByte(0xC8);

    Outfit_t currentOutfit = player->getDefaultOutfit();

    if (currentOutfit.lookType == 0) {
        Outfit_t newOutfit;
        newOutfit.lookType = outfits.front().lookType;
        currentOutfit = newOutfit;
    }

    Mount* currentMount = g_game.mounts.getMountByID(player->getCurrentMount());
    if (currentMount) {
        currentOutfit.lookMount = currentMount->clientId;
    }

    bool mounted;
    if (player->wasMounted) {
        mounted = currentOutfit.lookMount != 0;
    } else {
        mounted = player->isMounted();
    }

    AddOutfit(msg, currentOutfit);

    // mount color bytes are required here regardless of having one
    if (currentOutfit.lookMount == 0) {
        msg.addByte(currentOutfit.lookMountHead);
        msg.addByte(currentOutfit.lookMountBody);
        msg.addByte(currentOutfit.lookMountLegs);
        msg.addByte(currentOutfit.lookMountFeet);
    }

    msg.add<uint16_t>(0); // current familiar looktype

    std::vector<ProtocolOutfit> protocolOutfits;
    if (player->isAccessPlayer()) {
        protocolOutfits.emplace_back("Gamemaster", 75, 0);
    }

    for (const Outfit& outfit : outfits) {
        uint8_t addons;
        if (!player->getOutfitAddons(outfit, addons)) {
            continue;
        }

        protocolOutfits.emplace_back(outfit.name, outfit.lookType, addons);
    }

    msg.add<uint16_t>(protocolOutfits.size());
    for (const ProtocolOutfit& outfit : protocolOutfits) {
        msg.add<uint16_t>(outfit.lookType);
        msg.addString(outfit.name);
        msg.addByte(outfit.addons);
        msg.addByte(0x00); // mode: 0x00 - available, 0x01 store (requires U32 store offerId), 0x02 golden outfit
                           // tooltip (hardcoded)
    }

    std::vector<const Mount*> mounts;
    for (const Mount& mount : g_game.mounts.getMounts()) {
        if (player->hasMount(&mount)) {
            mounts.push_back(&mount);
        }
    }

    msg.add<uint16_t>(mounts.size());
    for (const Mount* mount : mounts) {
        msg.add<uint16_t>(mount->clientId);
        msg.addString(mount->name);
        msg.addByte(0x00); // mode: 0x00 - available, 0x01 store (requires U32 store offerId)
    }

    msg.add<uint16_t>(0x00); // familiars.size()
    // size > 0
    // U16 looktype
    // String name
    // 0x00 // mode: 0x00 - available, 0x01 store (requires U32 store offerId)

    msg.addByte(0x00); // Try outfit mode (?)
    msg.addByte(mounted ? 0x01 : 0x00);
    msg.addByte(player->randomizeMount ? 0x01 : 0x00);
    WriteToOutputBuffer(connection, msg);
}

void SendPodiumWindow(const GameConnection_ptr &connection, const Item* item){
    if (!item) {
        return;
    }

    const Podium* podium = item->getPodium();
    if (!podium) {
        return;
    }

    const Tile* tile = item->getTile();
    if (!tile) {
        return;
    }

    int32_t stackpos = tile->getThingIndex(item);

    // read podium outfit
    const Player *player = connection->player;
    Outfit_t podiumOutfit = podium->getOutfit();
    Outfit_t playerOutfit = player->getDefaultOutfit();
    bool isEmpty = podiumOutfit.lookType == 0 && podiumOutfit.lookMount == 0;

    if (podiumOutfit.lookType == 0) {
        // copy player outfit
        podiumOutfit.lookType = playerOutfit.lookType;
        podiumOutfit.lookHead = playerOutfit.lookHead;
        podiumOutfit.lookBody = playerOutfit.lookBody;
        podiumOutfit.lookLegs = playerOutfit.lookLegs;
        podiumOutfit.lookFeet = playerOutfit.lookFeet;
        podiumOutfit.lookAddons = playerOutfit.lookAddons;
    }

    if (podiumOutfit.lookMount == 0) {
        // copy player mount
        podiumOutfit.lookMount = playerOutfit.lookMount;
        podiumOutfit.lookMountHead = playerOutfit.lookMountHead;
        podiumOutfit.lookMountBody = playerOutfit.lookMountBody;
        podiumOutfit.lookMountLegs = playerOutfit.lookMountLegs;
        podiumOutfit.lookMountFeet = playerOutfit.lookMountFeet;
    }

    // fetch player outfits
    const auto& outfits = Outfits::getInstance().getOutfits(player->getSex());
    if (outfits.size() == 0) {
        player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
        return;
    }

    // add GM outfit for staff members
    std::vector<ProtocolOutfit> protocolOutfits;
    if (player->isAccessPlayer()) {
        protocolOutfits.emplace_back("Gamemaster", 75, 0);
    }

    // fetch player addons info
    for (const Outfit& outfit : outfits) {
        uint8_t addons;
        if (!player->getOutfitAddons(outfit, addons)) {
            continue;
        }

        protocolOutfits.emplace_back(outfit.name, outfit.lookType, addons);
    }

    // select first outfit available when the one from podium is not unlocked
    if (!player->canWear(podiumOutfit.lookType, 0)) {
        podiumOutfit.lookType = outfits.front().lookType;
    }

    // fetch player mounts
    std::vector<const Mount*> mounts;
    for (const Mount& mount : g_game.mounts.getMounts()) {
        if (player->hasMount(&mount)) {
            mounts.push_back(&mount);
        }
    }

    // packet header
    NetworkMessage msg;
    msg.addByte(0xC8);

    // current outfit
    msg.add<uint16_t>(podiumOutfit.lookType);
    msg.addByte(podiumOutfit.lookHead);
    msg.addByte(podiumOutfit.lookBody);
    msg.addByte(podiumOutfit.lookLegs);
    msg.addByte(podiumOutfit.lookFeet);
    msg.addByte(podiumOutfit.lookAddons);

    // current mount
    msg.add<uint16_t>(podiumOutfit.lookMount);
    msg.addByte(podiumOutfit.lookMountHead);
    msg.addByte(podiumOutfit.lookMountBody);
    msg.addByte(podiumOutfit.lookMountLegs);
    msg.addByte(podiumOutfit.lookMountFeet);

    // current familiar (not used in podium mode)
    msg.add<uint16_t>(0);

    // available outfits
    msg.add<uint16_t>(protocolOutfits.size());
    for (const ProtocolOutfit& outfit : protocolOutfits) {
        msg.add<uint16_t>(outfit.lookType);
        msg.addString(outfit.name);
        msg.addByte(outfit.addons);
        msg.addByte(0x00); // mode: 0x00 - available, 0x01 store (requires U32 store offerId), 0x02 golden outfit
                           // tooltip (hardcoded)
    }

    // available mounts
    msg.add<uint16_t>(mounts.size());
    for (const Mount* mount : mounts) {
        msg.add<uint16_t>(mount->clientId);
        msg.addString(mount->name);
        msg.addByte(0x00); // mode: 0x00 - available, 0x01 store (requires U32 store offerId)
    }

    // available familiars (not used in podium mode)
    msg.add<uint16_t>(0);

    msg.addByte(0x05); // "set outfit" window mode (5 = podium)
    msg.addByte((isEmpty && playerOutfit.lookMount != 0) || podium->hasFlag(PODIUM_SHOW_MOUNT)
                    ? 0x01
                    : 0x00); // "mount" checkbox
    msg.add<uint16_t>(0);    // unknown
    msg.addPosition(item->getPosition());
    msg.add<uint16_t>(item->getClientID());
    msg.addByte(stackpos);

    msg.addByte(podium->hasFlag(PODIUM_SHOW_PLATFORM) ? 0x01 : 0x00); // is platform visible
    msg.addByte(0x01);                                                // "outfit" checkbox, ignored by the client
    msg.addByte(podium->getDirection());                              // outfit direction
    WriteToOutputBuffer(connection, msg);
}

void SendUpdatedVIPStatus(const GameConnection_ptr &connection, uint32_t guid, VipStatus_t newStatus){
    NetworkMessage msg;
    msg.addByte(0xD3);
    msg.add<uint32_t>(guid);
    msg.addByte(newStatus);
    WriteToOutputBuffer(connection, msg);
}

void SendVIP(const GameConnection_ptr &connection, uint32_t guid, const std::string& name,
             const std::string& description, uint32_t icon, bool notify, VipStatus_t status){
    NetworkMessage msg;
    msg.addByte(0xD2);
    msg.add<uint32_t>(guid);
    msg.addString(name);
    msg.addString(description);
    msg.add<uint32_t>(std::min<uint32_t>(10, icon));
    msg.addByte(notify ? 0x01 : 0x00);
    msg.addByte(status);
    msg.addByte(0x00); // vipGroups (placeholder)
    WriteToOutputBuffer(connection, msg);
}

void SendVIPEntries(const GameConnection_ptr &connection){
    const std::forward_list<VIPEntry>& vipEntries = IOLoginData::getVIPEntries(connection->player->getAccount());

    for (const VIPEntry& entry : vipEntries) {
        VipStatus_t vipStatus = VIPSTATUS_ONLINE;

        Player* vipPlayer = g_game.getPlayerByGUID(entry.guid);

        if (!vipPlayer || !connection->player->canSeeCreature(vipPlayer)) {
            vipStatus = VIPSTATUS_OFFLINE;
        }

        SendVIP(connection, entry.guid, entry.name, entry.description, entry.icon, entry.notify, vipStatus);
    }
}

void SendItemClasses(const GameConnection_ptr &connection){
    NetworkMessage msg;
    msg.addByte(0x86);

    uint8_t classSize = 4;
    uint8_t tiersSize = 10;

    // item classes
    msg.addByte(classSize);
    for (uint8_t i = 0; i < classSize; i++) {
        msg.addByte(i + 1); // class id

        // item tiers
        msg.addByte(tiersSize); // tiers size
        for (uint8_t j = 0; j < tiersSize; j++) {
            msg.addByte(j);           // tier id
            msg.add<uint64_t>(10000); // upgrade cost
        }
    }

    // unknown
    for (uint8_t i = 0; i < tiersSize + 1; i++) {
        msg.addByte(0);
    }

    WriteToOutputBuffer(connection, msg);
}

void SendSpellCooldown(const GameConnection_ptr &connection, uint8_t spellId, uint32_t time){
    NetworkMessage msg;
    msg.addByte(0xA4);
    msg.add<uint16_t>(static_cast<uint16_t>(spellId));
    msg.add<uint32_t>(time);
    WriteToOutputBuffer(connection, msg);
}

void SendSpellGroupCooldown(const GameConnection_ptr &connection, SpellGroup_t groupId, uint32_t time){
    NetworkMessage msg;
    msg.addByte(0xA5);
    msg.addByte(groupId);
    msg.add<uint32_t>(time);
    WriteToOutputBuffer(connection, msg);
}

void SendUseItemCooldown(const GameConnection_ptr &connection, uint32_t time){
    NetworkMessage msg;
    msg.addByte(0xA6);
    msg.add<uint32_t>(time);
    WriteToOutputBuffer(connection, msg);
}

void SendSupplyUsed(const GameConnection_ptr &connection, const uint16_t clientId){
    NetworkMessage msg;
    msg.addByte(0xCE);
    msg.add<uint16_t>(clientId);

    WriteToOutputBuffer(connection, msg);
}

void SendModalWindow(const GameConnection_ptr &connection, const ModalWindow& modalWindow){
    NetworkMessage msg;
    msg.addByte(0xFA);

    msg.add<uint32_t>(modalWindow.id);
    msg.addString(modalWindow.title);
    msg.addString(modalWindow.message);

    msg.addByte(modalWindow.buttons.size());
    for (const auto& it : modalWindow.buttons) {
        msg.addString(it.first);
        msg.addByte(it.second);
    }

    msg.addByte(modalWindow.choices.size());
    for (const auto& it : modalWindow.choices) {
        msg.addString(it.first);
        msg.addByte(it.second);
    }

    msg.addByte(modalWindow.defaultEscapeButton);
    msg.addByte(modalWindow.defaultEnterButton);
    msg.addByte(modalWindow.priority ? 0x01 : 0x00);

    WriteToOutputBuffer(connection, msg);
}

//==============================================================================
// Parse Helpers and Functions
//==============================================================================
static void ParseLogout(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    Logout(connection, true, false);
}

static void ParsePingBack(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerReceivePingBack(connection->player);
}

static void ParsePing(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerReceivePing(connection->player);
}

static void ParseExtendedOpcode(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t opcode = input.getByte();
    std::string buffer = input.getString();
    g_game.parsePlayerExtendedOpcode(connection->player, opcode, buffer);
}

static void ParseAutoWalk(const GameConnection_ptr &connection, NetworkMessage &input){
    int numDirections = (int)input.getByte();
    if(!input.canRead(numDirections)){
        SendCancelWalk(connection);
        return;
    }

    std::vector<Direction> path;
    path.reserve(numDirections);
    for(int i = 0; i < numDirections; i += 1){
        switch(input.getByte()){
            case 1:
                path.push_back(DIRECTION_EAST);
                break;
            case 2:
                path.push_back(DIRECTION_NORTHEAST);
                break;
            case 3:
                path.push_back(DIRECTION_NORTH);
                break;
            case 4:
                path.push_back(DIRECTION_NORTHWEST);
                break;
            case 5:
                path.push_back(DIRECTION_WEST);
                break;
            case 6:
                path.push_back(DIRECTION_SOUTHWEST);
                break;
            case 7:
                path.push_back(DIRECTION_SOUTH);
                break;
            case 8:
                path.push_back(DIRECTION_SOUTHEAST);
                break;
            default:
                break;
        }
    }

    if(!path.empty()){
        std::reverse(path.begin(), path.end());
        g_game.playerAutoWalk(connection->player, path);
    }
}

static void ParseWalk(const GameConnection_ptr &connection, NetworkMessage &input, Direction dir){
    (void)input;
    g_game.playerWalk(connection->player, dir);
}

static void ParseStopAutoWalk(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerStopAutoWalk(connection->player);
}

static void ParseTurn(const GameConnection_ptr &connection, NetworkMessage &input, Direction dir){
    (void)input;
    g_game.playerTurn(connection->player, dir);
}

static void ParseEquipObject(const GameConnection_ptr &connection, NetworkMessage &input){
    // hotkey equip (?)
    uint16_t spriteID = input.get<uint16_t>();
    input.get<uint8_t>(); // bool smartMode (?)
    g_game.playerEquipItem(connection->player, spriteID);
}

static void ParseThrow(const GameConnection_ptr &connection, NetworkMessage &input){
    Position fromPos = input.getPosition();
    uint16_t spriteId = input.get<uint16_t>();
    uint8_t fromStackpos = input.getByte();
    Position toPos = input.getPosition();
    uint8_t count = input.getByte();

    if (toPos != fromPos) {
        g_game.playerMoveThing(connection->player, fromPos, spriteId, fromStackpos, toPos, count);
    }
}

static void ParseLookInShop(const GameConnection_ptr &connection, NetworkMessage &input){
    uint16_t id = input.get<uint16_t>();
    uint8_t count = input.getByte();
    g_game.playerLookInShop(connection->player, id, count);
}

static void ParsePlayerPurchase(const GameConnection_ptr &connection, NetworkMessage &input){
    uint16_t id = input.get<uint16_t>();
    uint8_t count = input.getByte();
    uint16_t amount = input.get<uint16_t>();
    bool ignoreCap = input.getByte() != 0;
    bool inBackpacks = input.getByte() != 0;
    g_game.playerPurchaseItem(connection->player, id, count, amount, ignoreCap, inBackpacks);
}

static void ParsePlayerSale(const GameConnection_ptr &connection, NetworkMessage &input){
    uint16_t id = input.get<uint16_t>();
    uint8_t count = input.getByte();
    uint16_t amount = input.get<uint16_t>();
    bool ignoreEquipped = input.getByte() != 0;
    g_game.playerSellItem(connection->player, id, count, amount, ignoreEquipped);
}

static void ParseCloseShop(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerCloseShop(connection->player);
}

static void ParseRequestTrade(const GameConnection_ptr &connection, NetworkMessage &input){
    Position pos = input.getPosition();
    uint16_t spriteId = input.get<uint16_t>();
    uint8_t stackpos = input.getByte();
    uint32_t playerId = input.get<uint32_t>();
    g_game.playerRequestTrade(connection->player, pos, stackpos, playerId, spriteId);
}

static void ParseLookInTrade(const GameConnection_ptr &connection, NetworkMessage &input){
    bool counterOffer = (input.getByte() == 0x01);
    uint8_t index = input.getByte();
    g_game.playerLookInTrade(connection->player, counterOffer, index);
}

static void ParseAcceptTrade(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerAcceptTrade(connection->player);
}

static void ParseCloseTrade(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerCloseTrade(connection->player);
}

static void ParseUseItem(const GameConnection_ptr &connection, NetworkMessage &input){
    Position pos = input.getPosition();
    uint16_t spriteId = input.get<uint16_t>();
    uint8_t stackpos = input.getByte();
    uint8_t index = input.getByte();
    g_game.playerUseItem(connection->player, pos, stackpos, index, spriteId);
}

static void ParseUseItemEx(const GameConnection_ptr &connection, NetworkMessage &input){
    Position fromPos = input.getPosition();
    uint16_t fromSpriteId = input.get<uint16_t>();
    uint8_t fromStackPos = input.getByte();
    Position toPos = input.getPosition();
    uint16_t toSpriteId = input.get<uint16_t>();
    uint8_t toStackPos = input.getByte();
    g_game.playerUseItemEx(connection->player, fromPos, fromStackPos, fromSpriteId, toPos, toStackPos, toSpriteId);
}

static void ParseUseWithCreature(const GameConnection_ptr &connection, NetworkMessage &input){
    Position fromPos = input.getPosition();
    uint16_t spriteId = input.get<uint16_t>();
    uint8_t fromStackPos = input.getByte();
    uint32_t creatureId = input.get<uint32_t>();
    g_game.playerUseWithCreature(connection->player, fromPos, fromStackPos, creatureId, spriteId);
}

static void ParseRotateItem(const GameConnection_ptr &connection, NetworkMessage &input){
    Position pos = input.getPosition();
    uint16_t spriteId = input.get<uint16_t>();
    uint8_t stackpos = input.getByte();
    g_game.playerRotateItem(connection->player, pos, stackpos, spriteId);
}

static void ParseEditPodiumRequest(const GameConnection_ptr &connection, NetworkMessage &input){
    Position pos = input.getPosition();
    uint16_t spriteId = input.get<uint16_t>();
    uint8_t stackpos = input.getByte();
    g_game.playerRequestEditPodium(connection->player, pos, stackpos, spriteId);
}

static void ParseCloseContainer(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t cid = input.getByte();
    g_game.playerCloseContainer(connection->player, cid);
}

static void ParseUpArrowContainer(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t cid = input.getByte();
    g_game.playerMoveUpContainer(connection->player, cid);
}

static void ParseTextWindow(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t windowTextID = input.get<uint32_t>();
    std::string newText = input.getString();
    g_game.playerWriteItem(connection->player, windowTextID, newText);
}

static void ParseHouseWindow(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t doorId = input.getByte();
    uint32_t id = input.get<uint32_t>();
    std::string text = input.getString();
    g_game.playerUpdateHouseWindow(connection->player, doorId, id, text);
}

static void ParseWrapItem(const GameConnection_ptr &connection, NetworkMessage &input){
    Position pos = input.getPosition();
    uint16_t spriteId = input.get<uint16_t>();
    uint8_t stackpos = input.getByte();
    g_game.playerWrapItem(connection->player, pos, stackpos, spriteId);
}

static void ParseLookAt(const GameConnection_ptr &connection, NetworkMessage &input){
    Position pos = input.getPosition();
    input.get<uint16_t>(); // spriteId
    uint8_t stackpos = input.getByte();
    g_game.playerLookAt(connection->player, pos, stackpos);
}

static void ParseLookInBattleList(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t creatureID = input.get<uint32_t>();
    g_game.playerLookInBattleList(connection->player, creatureID);
}

static void ParseQuickLoot(const GameConnection_ptr &connection, NetworkMessage &input){
    Position pos = input.getPosition();
    uint16_t spriteId = input.get<uint16_t>();
    uint8_t stackpos = input.getByte();
    bool quickLootAllCorpses = input.getByte() != 0;
    g_game.playerQuickLoot(connection->player, pos, stackpos, spriteId, quickLootAllCorpses);
}

static void ParseSay(const GameConnection_ptr &connection, NetworkMessage &input){
    std::string receiver;
    uint16_t channelId;

    SpeakClasses type = static_cast<SpeakClasses>(input.getByte());
    switch (type) {
        case TALKTYPE_PRIVATE_TO:
        case TALKTYPE_PRIVATE_RED_TO:
            receiver = input.getString();
            channelId = 0;
            break;

        case TALKTYPE_CHANNEL_Y:
        case TALKTYPE_CHANNEL_R1:
            channelId = input.get<uint16_t>();
            break;

        default:
            channelId = 0;
            break;
    }

    std::string text = input.getString();
    if (text.length() > 255) {
        return;
    }

    g_game.playerSay(connection->player, channelId, type, receiver, text);
}

static void ParseRequestChannels(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerRequestChannels(connection->player);
}

static void ParseOpenChannel(const GameConnection_ptr &connection, NetworkMessage &input){
    uint16_t channelID = input.get<uint16_t>();
    g_game.playerOpenChannel(connection->player, channelID);
}

static void ParseCloseChannel(const GameConnection_ptr &connection, NetworkMessage &input){
    uint16_t channelID = input.get<uint16_t>();
    g_game.playerCloseChannel(connection->player, channelID);
}

static void ParseOpenPrivateChannel(const GameConnection_ptr &connection, NetworkMessage &input){
    std::string receiver = input.getString();
    g_game.playerOpenPrivateChannel(connection->player, receiver);
}

static void ParseCloseNpcChannel(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerCloseNpcChannel(connection->player);
}

static void ParseFightModes(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t rawFightMode = input.getByte();  // 1 - offensive, 2 - balanced, 3 - defensive
    uint8_t rawChaseMode = input.getByte();  // 0 - stand while fighting, 1 - chase opponent
    uint8_t rawSecureMode = input.getByte(); // 0 - can't attack unmarked, 1 - can attack unmarked
    // uint8_t rawPvpMode = input.getByte(); // pvp mode introduced in 10.0

    fightMode_t fightMode;
    if (rawFightMode == 1) {
        fightMode = FIGHTMODE_ATTACK;
    } else if (rawFightMode == 2) {
        fightMode = FIGHTMODE_BALANCED;
    } else {
        fightMode = FIGHTMODE_DEFENSE;
    }

    g_game.playerSetFightModes(connection->player, fightMode, rawChaseMode != 0, rawSecureMode != 0);
}

static void ParseAttack(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t creatureID = input.get<uint32_t>();
    input.get<uint32_t>(); // target seq
    g_game.playerSetAttackedCreature(connection->player, creatureID);

}

static void ParseFollow(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t creatureID = input.get<uint32_t>();
    input.get<uint32_t>(); // target seq
    g_game.playerFollowCreature(connection->player, creatureID);
}

static void ParseInviteToParty(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t targetID = input.get<uint32_t>();
    g_game.playerInviteToParty(connection->player, targetID);
}

static void ParseJoinParty(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t targetID = input.get<uint32_t>();
    g_game.playerJoinParty(connection->player, targetID);
}

static void ParseRevokePartyInvite(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t targetID = input.get<uint32_t>();
    g_game.playerRevokePartyInvitation(connection->player, targetID);
}

static void ParsePassPartyLeadership(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t targetID = input.get<uint32_t>();
    g_game.playerPassPartyLeadership(connection->player, targetID);
}

static void ParseLeaveParty(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerLeaveParty(connection->player);
}

static void ParseEnableSharedPartyExperience(const GameConnection_ptr &connection, NetworkMessage &input){
    bool sharedExpActive = input.getByte() == 1;
    g_game.playerEnableSharedPartyExperience(connection->player, sharedExpActive);
}

static void ParseCreatePrivateChannel(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerCreatePrivateChannel(connection->player);
}

static void ParseChannelInvite(const GameConnection_ptr &connection, NetworkMessage &input){
    std::string name = input.getString();
    g_game.playerChannelInvite(connection->player, name);
}

static void ParseChannelExclude(const GameConnection_ptr &connection, NetworkMessage &input){
    std::string name = input.getString();
    g_game.playerChannelExclude(connection->player, name);
}

static void ParseCancelAttackAndFollow(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerCancelAttackAndFollow(connection->player);
}

static void ParseUpdateContainer(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t cid = input.getByte();
    g_game.playerUpdateContainer(connection->player, cid);
}

static void ParseBrowseField(const GameConnection_ptr &connection, NetworkMessage &input){
    Position pos = input.getPosition();
    g_game.playerBrowseField(connection->player, pos);
}

static void ParseSeekInContainer(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t containerId = input.getByte();
    uint16_t index = input.get<uint16_t>();
    g_game.playerSeekInContainer(connection->player, containerId, index);
}

static void ParseRequestOutfit(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerRequestOutfit(connection->player);
}

static void ParseSetOutfit(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t outfitType = input.getByte();

    Outfit_t newOutfit;
    newOutfit.lookType = input.get<uint16_t>();
    newOutfit.lookHead = input.getByte();
    newOutfit.lookBody = input.getByte();
    newOutfit.lookLegs = input.getByte();
    newOutfit.lookFeet = input.getByte();
    newOutfit.lookAddons = input.getByte();

    // Set outfit window
    if (outfitType == 0) {
        newOutfit.lookMount = input.get<uint16_t>();
        if (newOutfit.lookMount != 0) {
            newOutfit.lookMountHead = input.getByte();
            newOutfit.lookMountBody = input.getByte();
            newOutfit.lookMountLegs = input.getByte();
            newOutfit.lookMountFeet = input.getByte();
        } else {
            input.get<uint32_t>(); // ??

            // prevent mount color settings from resetting
            const Outfit_t& currentOutfit = connection->player->getCurrentOutfit();
            newOutfit.lookMountHead = currentOutfit.lookMountHead;
            newOutfit.lookMountBody = currentOutfit.lookMountBody;
            newOutfit.lookMountLegs = currentOutfit.lookMountLegs;
            newOutfit.lookMountFeet = currentOutfit.lookMountFeet;
        }

        input.get<uint16_t>(); // familiar looktype
        bool randomizeMount = input.getByte() == 0x01;
        g_game.playerChangeOutfit(connection->player, newOutfit, randomizeMount);

        // Store "try outfit" window
    } else if (outfitType == 1) {
        newOutfit.lookMount = 0;
        // mount colors or store offerId (needs testing)
        newOutfit.lookMountHead = input.getByte();
        newOutfit.lookMountBody = input.getByte();
        newOutfit.lookMountLegs = input.getByte();
        newOutfit.lookMountFeet = input.getByte();
        // player->? (open store?)

        // Podium interaction
    } else if (outfitType == 2) {
        Position pos = input.getPosition();
        uint16_t spriteId = input.get<uint16_t>();
        uint8_t stackpos = input.getByte();
        newOutfit.lookMount = input.get<uint16_t>();
        newOutfit.lookMountHead = input.getByte();
        newOutfit.lookMountBody = input.getByte();
        newOutfit.lookMountLegs = input.getByte();
        newOutfit.lookMountFeet = input.getByte();
        Direction direction = static_cast<Direction>(input.getByte());
        bool podiumVisible = input.getByte() == 1;

        // apply to podium
        g_game.playerEditPodium(connection->player, newOutfit, pos, stackpos, spriteId, podiumVisible, direction);
    }
}

static void ParseAddVip(const GameConnection_ptr &connection, NetworkMessage &input){
    std::string name = input.getString();
    g_game.playerRequestAddVip(connection->player, name);
}

static void ParseRemoveVip(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t guid = input.get<uint32_t>();
    g_game.playerRequestRemoveVip(connection->player, guid);
}

static void ParseEditVip(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t guid = input.get<uint32_t>();
    std::string description = input.getString();
    uint32_t icon = std::min<uint32_t>(10, input.get<uint32_t>()); // 10 is max icon in 9.63
    bool notify = input.getByte() != 0;
    g_game.playerRequestEditVip(connection->player, guid, description, icon, notify);
}

static void ParseDebugAssert(const GameConnection_ptr &connection, NetworkMessage &input){
    if (connection->debugAssertReceived) {
        return;
    }

    connection->debugAssertReceived = true;
    std::string assertLine = input.getString();
    std::string date = input.getString();
    std::string description = input.getString();
    std::string comment = input.getString();
    g_game.playerDebugAssert(connection->player, assertLine, date, description, comment);
}

static void ParseRuleViolationReport(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t reportType = input.getByte();
    uint8_t reportReason = input.getByte();
    std::string targetName = input.getString();
    std::string comment = input.getString();
    std::string translation;
    if (reportType == REPORT_TYPE_NAME) {
        translation = input.getString();
    } else if (reportType == REPORT_TYPE_STATEMENT) {
        translation = input.getString();
        input.get<uint32_t>(); // statement id, used to get whatever player have said, we don't log that.
    }

    g_game.playerReportRuleViolation(connection->player, targetName, reportType, reportReason, comment, translation);
}

static void ParseMarketLeave(const GameConnection_ptr &connection, NetworkMessage &input){
    (void)input;
    g_game.playerLeaveMarket(connection->player);
}

static void ParseMarketBrowse(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t browseId = input.get<uint8_t>();
    if (browseId == MARKETREQUEST_OWN_OFFERS) {
        g_game.playerBrowseMarketOwnOffers(connection->player);
    } else if (browseId == MARKETREQUEST_OWN_HISTORY) {
        g_game.playerBrowseMarketOwnHistory(connection->player);
    } else {
        uint16_t spriteID = input.get<uint16_t>();
        g_game.playerBrowseMarket(connection->player, spriteID);
    }

}

static void ParseMarketCreateOffer(const GameConnection_ptr &connection, NetworkMessage &input){
    uint8_t type = input.getByte();
    uint16_t spriteId = input.get<uint16_t>();

    const ItemType& it = Item::items.getItemIdByClientId(spriteId);
    if (it.id == 0 || it.wareId == 0) {
        return;
    } else if (it.classification > 0) {
        input.getByte(); // item tier
    }

    uint16_t amount = input.get<uint16_t>();
    uint64_t price = input.get<uint64_t>();
    bool anonymous = (input.getByte() != 0);
    g_game.playerCreateMarketOffer(connection->player, type, spriteId, amount, price, anonymous);
    SendStoreBalance(connection);
}

static void ParseMarketCancelOffer(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t timestamp = input.get<uint32_t>();
    uint16_t counter = input.get<uint16_t>();
    g_game.playerCancelMarketOffer(connection->player, timestamp, counter);
    SendStoreBalance(connection);
}

static void ParseMarketAcceptOffer(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t timestamp = input.get<uint32_t>();
    uint16_t counter = input.get<uint16_t>();
    uint16_t amount = input.get<uint16_t>();
    g_game.playerAcceptMarketOffer(connection->player, timestamp, counter, amount);
}

static void ParseModalWindowAnswer(const GameConnection_ptr &connection, NetworkMessage &input){
    uint32_t id = input.get<uint32_t>();
    uint8_t button = input.getByte();
    uint8_t choice = input.getByte();
    g_game.playerAnswerModalWindow(connection->player, id, button, choice);
}

static void ParseUnhandledCommand(const GameConnection_ptr &connection,
                                  uint8_t command, NetworkMessage &input){
    g_game.playerUnhandledCommand(connection->player, command, input);
}

static void ParsePacket(const GameConnection_ptr &connection, const std::vector<uint8_t> &data) {
    // NOTE(fusion): Probably do it some other way, to avoid the extra copy?
    NetworkMessage input;
    input.addBytes(data.data(), (size_t)data.size());
    if (!input.canRead(1) || input.isOverrun() || g_game.getGameState() == GAME_STATE_SHUTDOWN){
        return;
    }

    // TODO(fusion): Process multiple commands? I'm not even sure the client will
    // send multiple commands on a single packet but it would require us to properly
    // consume all command data before going to the next, which I'm not sure we're
    // doing currently.
    //while(input.getRemainingLength() > 0){
        uint8_t command = input.getByte();

        Player *player = connection->player;
        if(!player || player->isDead() || player->isRemoved()){
            if(!player || command == 0x0F){
                Detach(connection);
            }else if(command == 0x14){
                Logout(connection, true, false);
            }
            return;
        }

        switch (command) {
            case 0x14: ParseLogout(connection, input); break;
            case 0x1D: ParsePingBack(connection, input); break;
            case 0x1E: ParsePing(connection, input); break;
            // case 0x2A: break; // bestiary tracker
            // case 0x2C: break; // team finder (leader)
            // case 0x2D: break; // team finder (member)
            // case 0x28: break; // stash withdraw
            case 0x32: ParseExtendedOpcode(connection, input); break;
            case 0x64: ParseAutoWalk(connection, input); break;
            case 0x65: ParseWalk(connection, input, DIRECTION_NORTH); break;
            case 0x66: ParseWalk(connection, input, DIRECTION_EAST); break;
            case 0x67: ParseWalk(connection, input, DIRECTION_SOUTH); break;
            case 0x68: ParseWalk(connection, input, DIRECTION_WEST); break;
            case 0x69: ParseStopAutoWalk(connection, input); break;
            case 0x6A: ParseWalk(connection, input, DIRECTION_NORTHEAST); break;
            case 0x6B: ParseWalk(connection, input, DIRECTION_SOUTHEAST); break;
            case 0x6C: ParseWalk(connection, input, DIRECTION_SOUTHWEST); break;
            case 0x6D: ParseWalk(connection, input, DIRECTION_NORTHWEST); break;
            case 0x6F: ParseTurn(connection, input, DIRECTION_NORTH); break;
            case 0x70: ParseTurn(connection, input, DIRECTION_EAST); break;
            case 0x71: ParseTurn(connection, input, DIRECTION_SOUTH); break;
            case 0x72: ParseTurn(connection, input, DIRECTION_WEST); break;
            case 0x77: ParseEquipObject(connection, input); break;
            case 0x78: ParseThrow(connection, input); break;
            case 0x79: ParseLookInShop(connection, input); break;
            case 0x7A: ParsePlayerPurchase(connection, input); break;
            case 0x7B: ParsePlayerSale(connection, input); break;
            case 0x7C: ParseCloseShop(connection, input); break;
            case 0x7D: ParseRequestTrade(connection, input); break;
            case 0x7E: ParseLookInTrade(connection, input); break;
            case 0x7F: ParseAcceptTrade(connection, input); break;
            case 0x80: ParseCloseTrade(connection, input); break;
            case 0x82: ParseUseItem(connection, input); break;
            case 0x83: ParseUseItemEx(connection, input); break;
            case 0x84: ParseUseWithCreature(connection, input); break;
            case 0x85: ParseRotateItem(connection, input); break;
            case 0x86: ParseEditPodiumRequest(connection, input); break;
            case 0x87: ParseCloseContainer(connection, input); break;
            case 0x88: ParseUpArrowContainer(connection, input); break;
            case 0x89: ParseTextWindow(connection, input); break;
            case 0x8A: ParseHouseWindow(connection, input); break;
            case 0x8B: ParseWrapItem(connection, input); break;
            case 0x8C: ParseLookAt(connection, input); break;
            case 0x8D: ParseLookInBattleList(connection, input); break;
            case 0x8E: break; // join aggression
            case 0x8F: ParseQuickLoot(connection, input); break;
            // case 0x90: break; // loot container
            // case 0x91: break; // update loot whitelist
            // case 0x92: break; // request locker items
            case 0x96: ParseSay(connection, input); break;
            case 0x97: ParseRequestChannels(connection, input); break;
            case 0x98: ParseOpenChannel(connection, input); break;
            case 0x99: ParseCloseChannel(connection, input); break;
            case 0x9A: ParseOpenPrivateChannel(connection, input); break;
            case 0x9E: ParseCloseNpcChannel(connection, input); break;
            case 0xA0: ParseFightModes(connection, input); break;
            case 0xA1: ParseAttack(connection, input); break;
            case 0xA2: ParseFollow(connection, input); break;
            case 0xA3: ParseInviteToParty(connection, input); break;
            case 0xA4: ParseJoinParty(connection, input); break;
            case 0xA5: ParseRevokePartyInvite(connection, input); break;
            case 0xA6: ParsePassPartyLeadership(connection, input); break;
            case 0xA7: ParseLeaveParty(connection, input); break;
            case 0xA8: ParseEnableSharedPartyExperience(connection, input); break;
            case 0xAA: ParseCreatePrivateChannel(connection, input); break;
            case 0xAB: ParseChannelInvite(connection, input); break;
            case 0xAC: ParseChannelExclude(connection, input); break;
            // case 0xB1: break; // request highscores
            case 0xBE: ParseCancelAttackAndFollow(connection, input); break;
            // case 0xC7: break; // request tournament leaderboard
            case 0xC9: break; // update tile
            case 0xCA: ParseUpdateContainer(connection, input); break;
            case 0xCB: ParseBrowseField(connection, input); break;
            case 0xCC: ParseSeekInContainer(connection, input); break;
            // case 0xCD: break; // request inspect window
            case 0xD2: ParseRequestOutfit(connection, input); break;
            case 0xD3: ParseSetOutfit(connection, input); break;
            // case 0xD5: break; // apply imbuement
            // case 0xD6: break; // clear imbuement
            // case 0xD7: break; // close imbuing window
            case 0xDC: ParseAddVip(connection, input); break;
            case 0xDD: ParseRemoveVip(connection, input); break;
            case 0xDE: ParseEditVip(connection, input); break;
            // case 0xDF: break; // premium shop (?)
            // case 0xE0: break; // premium shop (?)
            // case 0xE4: break; // buy charm rune
            // case 0xE5: break; // request character info (cyclopedia)
            // case 0xE6: break; // parse bug report
            case 0xE7: break; // thank you
            case 0xE8: ParseDebugAssert(connection, input); break;
            // case 0xEF: break; // request store coins transfer
            case 0xF2: ParseRuleViolationReport(connection, input); break;
            case 0xF3: break; // get object info
            case 0xF4: ParseMarketLeave(connection, input); break;
            case 0xF5: ParseMarketBrowse(connection, input); break;
            case 0xF6: ParseMarketCreateOffer(connection, input); break;
            case 0xF7: ParseMarketCancelOffer(connection, input); break;
            case 0xF8: ParseMarketAcceptOffer(connection, input); break;
            case 0xF9: ParseModalWindowAnswer(connection, input); break;
            // case 0xFA: break; // store window open
            // case 0xFB: break; // store window click
            // case 0xFC: break; // store window buy
            // case 0xFD: break; // store window history 1
            // case 0xFE: break; // store window history 2
            default:
                ParseUnhandledCommand(connection, command, input); break;
                break;
        }

        if (input.isOverrun()) {
            // log error/warning?
            Detach(connection);
        }
    //}
}

//=============================================================================
// Login
//=============================================================================
static int GetWaitSlot(const Player *player, int *outRetrySeconds){
    using std::chrono::steady_clock;
    struct WaitSlot {
        steady_clock::time_point    timeout;
        uint32_t                    playerGuid;
        bool                        premium;
    };
    static std::deque<WaitSlot> waitList;

    if (player->hasFlag(PlayerFlag_CanAlwaysLogin) || player->getAccountType() >= ACCOUNT_TYPE_GAMEMASTER) {
        return 0;
    }

    int numPlayers = (int)g_game.getPlayersOnline();
    int maxPlayers = getNumber(ConfigManager::MAX_PLAYERS);
    int freeSlots  = maxPlayers - numPlayers;
    if (maxPlayers == 0 || (waitList.empty() && freeSlots > 0)) {
        return 0;
    }

    // NOTE(fusion): Remove timed out entries at the front of the list.
    auto now = steady_clock::now();
    auto it = waitList.begin();
    while(it != waitList.end() && it->timeout <= now){
        it = waitList.erase(it);
    }

    // NOTE(fusion): Count players up until we find the player's entry
    // or reach the end of the list.
    int freeAccount = 0;
    int premiumAccount = 0;
    uint32_t playerGuid = player->getGUID();
    while(it != waitList.end()){
        if(it->timeout <= now){
            continue;
        }

        if(it->playerGuid == playerGuid){
            break;
        }

        if(it->premium){
            premiumAccount += 1;
        }else{
            freeAccount += 1;
        }
    }

    int waitSlot = premiumAccount + 1;
    if(!player->isPremium()){
        waitSlot += freeAccount;
    }

    int retrySeconds = ((waitSlot / 5) + 1) * 5;
    if(retrySeconds > 60){
        retrySeconds = 60;
    }

    if(outRetrySeconds){
        *outRetrySeconds = retrySeconds;
    }

    if(waitSlot <= freeSlots){
        if(it != waitList.end()){
            waitList.erase(it);
        }
        return 0;
    }else{
        auto timeout = now + std::chrono::seconds(retrySeconds + 15);
        if(it != waitList.end()){
            it->timeout = timeout;
        }else{
            waitList.push_back({timeout, playerGuid, player->isPremium()});
        }
        return waitSlot;
    }
}

static void PerformLogin(GameConnection_ptr connection, bool isGamemaster,
        std::string_view sessionToken, std::string_view characterName){
    // IMPORTANT(fusion): We're doing database access inline which is not optimal
    // and will block the running thread. It's not like we were doing anything
    // different before. This has always been a problem. There may be workarounds
    // but they involve offloading database access to another thread but requires
    // more synchronization and it doesn't help that Lua scripts also have multiple
    // inline database queries.

    (void)isGamemaster;

    if(sessionToken.empty() || characterName.empty()){
        SendLoginError(connection, "Malformed session data.");
        return;
    }

    if(connection->terminalVersion < CLIENT_VERSION_MIN
            || connection->terminalVersion > CLIENT_VERSION_MAX){
        SendLoginError(connection, fmt::format(
                "Only clients with protocol {:s} allowed!",
                CLIENT_VERSION_STR));
        return;
    }

    if (g_game.getGameState() == GAME_STATE_STARTUP) {
        SendLoginError(connection, "Gameworld is starting up. Please wait.");
        return;
    }

    if (g_game.getGameState() == GAME_STATE_MAINTAIN) {
        SendLoginError(connection, "Gameworld is under maintenance. Please re-connect in a while.");
        return;
    }

    if(const auto &banInfo = IOBan::getIpBanInfo(connection->endpoint.address())){
        SendLoginError(connection, fmt::format(
                "Your IP has been banned until {:s} by {:s}.\n\n""Reason specified:\n{:s}",
                formatDateShort(banInfo->expiresAt), banInfo->bannedBy, banInfo->reason));
        return;
    }

    SessionData sessionData = {};
    if(!IOLoginData::loadSession(sessionToken, characterName, &sessionData)
            || sessionData.accountId == 0){
        SendLoginError(connection, "Account name or password is not correct.");
        return;
    }

    if(sessionData.address != connection->endpoint.address()){
        SendLoginError(connection, "Your game session is already locked to a"
                                    " different IP. Please log in again.");
        return;
    }

    Player* foundPlayer = g_game.getPlayerByGUID(sessionData.characterId);
    if (!foundPlayer || getBoolean(ConfigManager::ALLOW_CLONES)) {
        Player *player = new Player(connection);
        player->incrementReferenceCounter();
        player->setID();
        player->setGUID(sessionData.characterId);
        connection->player = player;

        if (!IOLoginData::preloadPlayer(player)) {
            SendLoginError(connection, "Your character could not be loaded.");
            return;
        }

        if (IOBan::isPlayerNamelocked(player->getGUID())) {
            SendLoginError(connection, "Your character has been namelocked.");
            return;
        }

        if (g_game.getGameState() == GAME_STATE_CLOSING
                && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
            SendLoginError(connection, "The game is just going down.\nPlease try again later.");
            return;
        }

        if (g_game.getGameState() == GAME_STATE_CLOSED
                && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
            SendLoginError(connection, "Server is currently closed.\nPlease try again later.");
            return;
        }

        if (getBoolean(ConfigManager::ONE_PLAYER_PER_ACCOUNT)
                && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER
                && g_game.getPlayerByAccount(player->getAccount())) {
            SendLoginError(connection, "You may only login with one character\nof your account at the same time.");
            return;
        }

        if (!player->hasFlag(PlayerFlag_CannotBeBanned)) {
            if (const auto &banInfo = IOBan::getAccountBanInfo(sessionData.accountId)) {
                if (banInfo->expiresAt > 0) {
                    SendLoginError(connection, fmt::format(
                            "Your account has been banned until {:s} by {:s}.\n\nReason specified:\n{:s}",
                            formatDateShort(banInfo->expiresAt), banInfo->bannedBy, banInfo->reason));
                } else {
                    SendLoginError(connection, fmt::format(
                            "Your account has been permanently banned by {:s}.\n\nReason specified:\n{:s}",
                            banInfo->bannedBy, banInfo->reason));
                }
                return;
            }
        }

        int retrySeconds;
        if(int waitSlot = GetWaitSlot(player, &retrySeconds)){
            SendLoginWaitList(connection, waitSlot, retrySeconds);
            return;
        }

        if (!IOLoginData::loadPlayerById(player, player->getGUID())) {
            SendLoginError(connection, "Your character could not be loaded.");
            return;
        }

        if (!g_game.placeCreature(player, player->getLoginPosition(), false, false, CONST_ME_TELEPORT)) {
            if (!g_game.placeCreature(player, player->getTemplePosition(), false, true, CONST_ME_TELEPORT)) {
                SendLoginError(connection, "Temple position is wrong. Contact the administrator.");
                return;
            }
        }

        if (connection->terminalType >= TERMINAL_OTCLIENT_LINUX) {
            SendEnableExtendedOpcode(connection);
            player->registerCreatureEvent("ExtendedOpcode");
        }

        player->lastIP = player->getIP();
        player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
        ResolveLogin(connection, GAME_CONNECTION_OK);
    }
#if 0
    else {
        if (eventConnect != 0 || !getBoolean(ConfigManager::REPLACE_KICK_ON_LOGIN)) {
            // Already trying to connect
            disconnectClient("You are already logged in.");
            return;
        }

        if (foundPlayer->client) {
            foundPlayer->disconnect();
            foundPlayer->isConnecting = true;

            eventConnect = g_scheduler.addEvent(
                createSchedulerTask(1000, [=, self = shared_from_this(), playerID = foundPlayer->getID()]() {
                    self->connect(playerID, terminalType);
                }));
        } else {
            connect(foundPlayer->getID(), terminalType);
        }
    }
#endif
}

#if 0
void GameConnection::connect(uint32_t playerId, TerminalType terminalType)
{
    // dispatcher thread
    eventConnect = 0;

    Player* foundPlayer = g_game.getPlayerByID(playerId);
    if (!foundPlayer || foundPlayer->client) {
        disconnectClient("You are already logged in.");
        return;
    }

    if (isConnectionExpired()) {
        // GameConnection::release() has been called at this point and the Connection object no longer exists, so we
        // return to prevent leakage of the Player.
        return;
    }

    player = foundPlayer;
    player->incrementReferenceCounter();

    g_chat->removeUserFromAllChannels(*player);
    player->clearModalWindows();
    player->isConnecting = false;

    player->client = shared_from_this();
    player->onCreatureAppear(player, false, CONST_ME_NONE);
    player->lastIP = player->getIP();
    player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
    player->resetIdleTime();
    acceptPackets = true;

    g_creatureEvents->playerReconnect(player);
}
#endif

//==============================================================================
// Service Implementation
//==============================================================================
// IMPORTANT(fusion): All these functions expect to be running on the network
// thread.

static void Close(const GameConnection_ptr &connection){
    boost::system::error_code ec;
    if(connection->socket.is_open()){
        connection->socket.shutdown(tcp::socket::shutdown_both, ec);
        connection->socket.close(ec);
        g_dispatcher.addTask(
            [connection] mutable {
                Detach(std::move(connection));
            });
    }
}

static void Abort(const GameConnection_ptr &connection){
    boost::system::error_code ec;
    if(connection->socket.is_open()){
        connection->socket.close(ec);
        g_dispatcher.addTask(
            [connection] mutable {
                Detach(std::move(connection));
            });
    }
}

static asio::awaitable<bool> ReadGamePacket(const GameConnection_ptr &connection,
                                            NetworkMessage &input,
                                            bool encryptionEnabled = true){
    co_await asio::async_read(connection->socket,
            asio::buffer(input.buffer, 2), use_awaitable);
    int numXteaBlocks = ((uint16_t)input.buffer[0]) | ((uint16_t)input.buffer[1] << 8);
    int packetLen = 4 + numXteaBlocks * 8;
    if(numXteaBlocks == 0 || packetLen > (int)input.buffer.size()){
        co_return false;
    }

    co_await asio::async_read(connection->socket,
            asio::buffer(input.buffer, packetLen), use_awaitable);

    // TODO(fusion): I think the 2 high bits of the sequence are used for another
    // purpose. The previous version would set the high bit for compressed packets
    // but we still need to figure out what's going on with the other bit.
    input.rdpos = 0;
    input.wrpos = packetLen;
    uint32_t sequence = input.get<uint32_t>();
    if(sequence != connection->clientSequence){
        co_return false;
    }

    if(encryptionEnabled){
        if(!XteaDecrypt(connection->xteaKey,
                input.getRemainingBuffer(),
                input.getRemainingLength())){
            co_return false;
        }
    }

    int padding = input.getByte();
    if(!input.discardPadding(padding)){
        co_return false;
    }

    // TODO(fusion): Maybe inflate? Check sequence high bits.

    connection->clientSequence += 1;
    co_return true;
}

static asio::awaitable<bool> WriteGamePacket(const GameConnection_ptr &connection,
                                             const OutputMessage_ptr &output,
                                             bool encryptionEnabled = true){
    // TODO(fusion): Maybe deflate? Set sequence high bits.

    // TODO(fusion): Probably just compute the amount of padding and call `CryptoRand` once.
    int padding = 0;
    while((output->getOutputLength() + 1) % 8 != 0){
        output->addByte(CryptoRandByte());
        padding += 1;
    }
    output->addHeader<uint8_t>(padding);

    int numXteaBlocks = output->getOutputLength() / 8;
    if(output->isOverrun()
            || numXteaBlocks <= 0
            || numXteaBlocks > UINT16_MAX){
        co_return false;
    }

    if(encryptionEnabled){
        if(!XteaEncrypt(connection->xteaKey,
                output->getOutputBuffer(),
                output->getOutputLength())){
            co_return false;
        }
    }

    output->addHeader<uint32_t>(connection->serverSequence++);
    output->addHeader<uint16_t>(numXteaBlocks);
    co_await asio::async_write(connection->socket,
            asio::buffer(output->getOutputBuffer(), output->getOutputLength()),
            use_awaitable);
    co_return true;
}

static asio::awaitable<void> GameReader(GameConnection_ptr connection){
    constexpr chrono::duration READ_TIMEOUT = chrono::seconds(15);
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    NetworkMessage input;
    try{
        while(CurrentState(connection) == GAME_CONNECTION_OK){
            timer.expires_after(READ_TIMEOUT);
            timer.async_wait(
                [&](boost::system::error_code ec){
                    if(!ec){
                        // TODO(fusion): Maybe use a cancellation signal instead to
                        // let the cancellation bubble up, rather than the read failing
                        // due to a socket close?
                        Abort(connection);
                    }
                });

            if(bool ok = co_await ReadGamePacket(connection, input); !ok){
                Abort(connection);
                co_return;
            }

            timer.cancel();

            {
                // NOTE(fusion): Dispatch the whole message to the game thread to
                // be parsed. This will cause an extra allocation, but it's not
                // like there are no allocations all around anyways.
                uint8_t *start = input.getRemainingBuffer();
                uint8_t *end   = start + input.getRemainingLength();
                if(start != end){
                    g_dispatcher.addTask(
                        [connection, data = std::vector(start, end)]{
                            ParsePacket(connection, data);
                        });
                }
            }
        }
    }catch(const boost::system::system_error &e){
        if(e.code() == asio::error::eof){
            Close(connection);
        }else{
            std::cout << "GameReader: " << e.what() << std::endl;
            Abort(connection);
        }
    }
}

static asio::awaitable<void> GameWriter(GameConnection_ptr connection){
    constexpr chrono::duration WRITE_TIMEOUT = chrono::seconds(15);
    constexpr chrono::duration AUTO_SEND_INTERVAL = chrono::milliseconds(10);
    auto executor = co_await asio::this_coro::executor;
    try {
        asio::steady_timer timer(executor);
        while(true){
            // TODO(fusion): Process CLOSE and ABORT?
            GameConnectionState state = CurrentState(connection);
            //switch(){}

            if(state != GAME_CONNECTION_OK && state != GAME_CONNECTION_CLOSE){
                break;
            }

            OutputMessage_ptr output;
            {
                std::lock_guard lockGuard(connection->outputMutex);
                if(connection->outputHead){
                    output = std::move(connection->outputHead);
                    connection->outputHead = std::move(output->next);
                }
            }

            if(!output){
                boost::system::error_code ec;
                timer.expires_after(AUTO_SEND_INTERVAL);
                co_await timer.async_wait(asio::redirect_error(use_awaitable, ec));
                continue;
            }

            timer.expires_after(WRITE_TIMEOUT);
            timer.async_wait(
                [&](boost::system::error_code ec){
                    if(!ec){
                        // TODO(fusion): Similar to `GameReader`.
                        Abort(connection);
                    }
                });

            if(bool ok = co_await WriteGamePacket(connection, output); !ok){
                Abort(connection);
                co_return;
            }

            timer.cancel();
        }
    }catch(const boost::system::system_error &e){
        std::cout << "GameWriter: " << e.what() << std::endl;
        Abort(connection);
    }
}

static asio::awaitable<void> GameHandshake(GameConnection_ptr connection){
    constexpr chrono::duration LOGIN_TIMEOUT = chrono::seconds(5);
    auto executor = co_await asio::this_coro::executor;

    try{
        // TODO(fusion): Review how we timeout operations?
        connection->loginTimer.expires_after(LOGIN_TIMEOUT);
        connection->loginTimer.async_wait(
            [&](boost::system::error_code ec){
                if(!ec){
                    Abort(connection);
                }
            });

        // SERVER <- CLIENT (WORLDNAME)
        {
            // NOTE(fusion): Make sure the client only sends the world name followed
            // by a line feed as its first message.
            std::string worldName;
            co_await asio::async_read_until(connection->socket,
                    asio::dynamic_buffer(worldName), '\n', use_awaitable);
            if(worldName.back() != '\n'){
                Abort(connection);
                co_return;
            }

            // NOTE(fusion): Make sure the world name matches.
            worldName.pop_back();
            if(worldName != getString(ConfigManager::SERVER_NAME)){
                Abort(connection);
                co_return;
            }
        }

        // SERVER -> CLIENT (CHALLENGE)
        // TODO(fusion): This is not a race condition because we only change the
        // initial time at startup, but we could turn it into a freestanding
        // function, probably in tools.cpp?
        uint32_t challengeUptime = (uint32_t)(g_game.getUptimeSeconds());
        uint8_t  challengeRandom = CryptoRandByte();
        {
            uint8_t buffer[] = {
                0x01, 0x00,             // NUM XTEA BLOCKS
                0x00, 0x00, 0x00, 0x00, // SEQUENCE
                0x01,                   // PADDING
                0x1F,                   // CHALLENGE ID
                0x00, 0x00, 0x00, 0x00, // WORLD UPTIME SECONDS
                0x00,                   // RANDOM BYTE
                0x00,                   // PADDING BYTE
            };

            memcpy(&buffer[2], &connection->serverSequence, 4);
            memcpy(&buffer[8], &challengeUptime, 4);
            buffer[12] = challengeRandom;
            buffer[13] = CryptoRandByte();
            co_await asio::async_write(connection->socket,
                    asio::buffer(buffer, sizeof(buffer)),
                    use_awaitable);
            connection->serverSequence += 1;
        }

        // SERVER <- CLIENT (LOGIN)
        {
            NetworkMessage input;
            if(bool ok = co_await ReadGamePacket(connection, input, false); !ok){
                Abort(connection);
                co_return;
            }

            if(input.getRemainingLength() != 252){
                Abort(connection);
                co_return;
            }

            if(input.getByte() != 0x0A){
                Abort(connection);
                co_return;
            }

            connection->terminalType    = input.get<uint16_t>();
            connection->terminalVersion = input.get<uint16_t>();
            input.get<uint32_t>();  // terminal version 32?
            input.getString();      // version string
            input.getString();      // hex string => client/assets checksum?
            input.getByte();        // ?

            if(!RsaDecrypt(input.getRemainingBuffer(), input.getRemainingLength())
                    || input.getByte() != 0){
                Abort(connection);
                co_return;
            }

            connection->xteaKey[0] = input.get<uint32_t>();
            connection->xteaKey[1] = input.get<uint32_t>();
            connection->xteaKey[2] = input.get<uint32_t>();
            connection->xteaKey[3] = input.get<uint32_t>();

            bool isGamemaster         = input.getByte();
            std::string sessionToken  = tfs::base64::decode(input.getString());
            std::string characterName = input.getString();

            // NOTE(fusion): I think this is more of a consistency check than a challenge?
            if(input.get<uint32_t>() != challengeUptime
                    || input.getByte() != challengeRandom
                    || input.isOverrun()){
                Abort(connection);
                co_return;
            }

            g_dispatcher.addTask(
                [connection, isGamemaster,
                        sessionToken = std::move(sessionToken),
                        characterName = std::move(characterName)]{
                    PerformLogin(connection, isGamemaster, sessionToken, characterName);
                });
        }

        // TODO(fusion): If there is no error code, then the timer ran out and
        // the connection was already aborted by the async_wait at the beginning.
        // We might want to review how we're doing it, so we only care whether
        // the current state is GAME_CONNECTION_OK or not.
        boost::system::error_code ec;
        co_await connection->loginTimer.async_wait(
                asio::redirect_error(use_awaitable, ec));
        if(!ec){
            //
        }

        GameConnectionState currentState = connection->state.load(std::memory_order_acquire);
        if(currentState == GAME_CONNECTION_LOGIN){
            Abort(connection);
            co_return;
        }

        if(currentState == GAME_CONNECTION_OK){
            asio::co_spawn(executor,
                    GameReader(connection),
                    asio::detached);
        }

        if(currentState == GAME_CONNECTION_OK || currentState == GAME_CONNECTION_CLOSE){
            asio::co_spawn(executor,
                    GameWriter(connection),
                    asio::detached);
        }
    }catch(const boost::system::system_error &e){
        std::cout << "GameHandshake: " << e.what() << std::endl;
        Abort(connection);
    }
}

// TODO(fusion): Pass RSA key as param? Probably just load some global?
asio::awaitable<void> GameService(tcp::endpoint endpoint){
    // TODO(fusion): Same as `StatusService`.
    auto executor = co_await asio::this_coro::executor;
    try{
        tcp::acceptor acceptor(executor);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.set_option(asio::ip::tcp::no_delay(true));
        if(endpoint.address().is_v6()){
            acceptor.set_option(asio::ip::v6_only(false));
        }
        acceptor.bind(endpoint);
        acceptor.listen(1024);

        std::cout << ">> Game service listening on " << endpoint << std::endl;
        while(true){
            // NOTE(fusion): Each connection will have two coroutines + timers
            // running after the handshake which means that on a multi-threaded
            // setting, we could have unsynchronized access to the connection
            // object. By using a strand, we can effectivelly crank up the number
            // of threads, although I can only see it being beneficial if the
            // I/O thread is reaching 100% usage, which is probably never the
            // case (?).
            //auto strand = asio::make_strand(executor);
            //tcp::endpoint peer_endpoint;
            //tcp::socket socket = co_await acceptor.async_accept(strand, peer_endpoint, use_awaitable);

            tcp::endpoint peer_endpoint;
            tcp::socket socket = co_await acceptor.async_accept(peer_endpoint, use_awaitable);
            auto connection = std::make_shared<GameConnection>(std::move(socket), peer_endpoint);
            asio::co_spawn(executor,
                    GameHandshake(std::move(connection)),
                    asio::detached);
        }
    }catch(const std::exception &e){
        std::cout << ">> Game service error: " << e.what() << std::endl;
        throw;
    }
}

