#ifndef __OTSERV_SERVICE_GAME_H__
#define __OTSERV_SERVICE_GAME_H__ 1

#include "chat.h"
#include "creature.h"
#include "outputmessage.h"
#include "tasks.h"

struct TextMessage {
    MessageClasses  type = MESSAGE_STATUS_DEFAULT;
    uint16_t        channelId = 0;
    Position        position;
    std::string     text;
    struct {
        int32_t     value = 0;
        TextColor_t color = TEXTCOLOR_NONE;
    } primary, secondary;

    TextMessage() = default;
    TextMessage(MessageClasses type, std::string text)
        : type(type), text(std::move(text)) {}
};

struct GameConnection;
using GameConnection_ptr = std::shared_ptr<GameConnection>;

bool CanSeePosition(const Player *player, const Position &pos);
bool CanSeeCreature(const Player *player, const Creature *creature);
void Detach(GameConnection_ptr connection);
void WriteToOutputBuffer(const GameConnection_ptr &connection, const NetworkMessage &msg);
boost::asio::ip::address GetRemoteAddress(const GameConnection_ptr &connection);
int GetTerminalType(const GameConnection_ptr &connection);
int GetTerminalVersion(const GameConnection_ptr &connection);

void Logout(const GameConnection_ptr &connection, bool displayEffect, bool forced);
void SendOpenPrivateChannel(const GameConnection_ptr &connection, const std::string& receiver);
void SendChannelEvent(const GameConnection_ptr &connection, uint16_t channelId,
                    const std::string& playerName, ChannelEvent_t channelEvent);
void SendCreatureOutfit(const GameConnection_ptr &connection,
            const Creature* creature, const Outfit_t& outfit);
void SendCreatureLight(const GameConnection_ptr &connection, const Creature* creature);
void SendCreatureWalkthrough(const GameConnection_ptr &connection, const Creature* creature, bool walkthrough);
void SendCreatureShield(const GameConnection_ptr &connection, const Creature* creature);
void SendCreatureSkull(const GameConnection_ptr &connection, const Creature* creature);
void SendCreatureSquare(const GameConnection_ptr &connection, const Creature* creature, SquareColor_t color);
void SendTutorial(const GameConnection_ptr &connection, uint8_t tutorialId);
void SendAddMarker(const GameConnection_ptr &connection, const Position& pos,
                   uint8_t markType, const std::string& desc);
void SendReLoginWindow(const GameConnection_ptr &connection, uint8_t unfairFightReduction);
void SendStats(const GameConnection_ptr &connection);
void SendExperienceTracker(const GameConnection_ptr &connection, int64_t rawExp, int64_t finalExp);
void SendClientFeatures(const GameConnection_ptr &connection);
void SendBasicData(const GameConnection_ptr &connection);
void SendTextMessage(const GameConnection_ptr &connection, const TextMessage& message);
void SendClosePrivate(const GameConnection_ptr &connection, uint16_t channelId);
void SendCreatePrivateChannel(const GameConnection_ptr &connection, uint16_t channelId, const std::string& channelName);
void SendChannelsDialog(const GameConnection_ptr &connection);
void SendChannel(const GameConnection_ptr &connection, uint16_t channelId,
                 const std::string& channelName, const UsersMap* channelUsers,
                 const InvitedMap* invitedUsers);
void SendChannelMessage(const GameConnection_ptr &connection, const std::string& author,
                        const std::string& text, SpeakClasses type, uint16_t channel);
void SendIcons(const GameConnection_ptr &connection, uint32_t icons);
void SendContainer(const GameConnection_ptr &connection, uint8_t cid,
                   const Container* container, uint16_t firstIndex);
void SendEmptyContainer(const GameConnection_ptr &connection, uint8_t cid);
void SendShop(const GameConnection_ptr &connection, Npc* npc, const ShopInfoList& itemList);
void SendCloseShop(const GameConnection_ptr &connection);
void SendSaleItemList(const GameConnection_ptr &connection, const std::list<ShopInfo>& shop);
void SendResourceBalance(const GameConnection_ptr &connection, const ResourceTypes_t resourceType, uint64_t amount);
void SendStoreBalance(const GameConnection_ptr &connection);
void SendMarketEnter(const GameConnection_ptr &connection);
void SendMarketLeave(const GameConnection_ptr &connection);
void SendMarketBrowseItem(const GameConnection_ptr &connection, uint16_t itemId,
                          const MarketOfferList& buyOffers,
                          const MarketOfferList& sellOffers);
void SendMarketAcceptOffer(const GameConnection_ptr &connection, const MarketOfferEx& offer);
void SendMarketBrowseOwnOffers(const GameConnection_ptr &connection,
                               const MarketOfferList& buyOffers,
                               const MarketOfferList& sellOffers);
void SendMarketCancelOffer(const GameConnection_ptr &connection, const MarketOfferEx& offer);
void SendMarketBrowseOwnHistory(const GameConnection_ptr &connection,
                                const HistoryMarketOfferList& buyOffers,
                                const HistoryMarketOfferList& sellOffers);
void SendTradeItemRequest(const GameConnection_ptr &connection,
                          const std::string& traderName,
                          const Item* item, bool ack);
void SendCloseTrade(const GameConnection_ptr &connection);
void SendCloseContainer(const GameConnection_ptr &connection, uint8_t cid);
void SendCreatureTurn(const GameConnection_ptr &connection, const Creature* creature, uint32_t stackpos);
void SendCreatureSay(const GameConnection_ptr &connection, const Creature* creature,
                     SpeakClasses type, const std::string& text, const Position* pos = nullptr);
void SendToChannel(const GameConnection_ptr &connection, const Creature* creature,
                   SpeakClasses type, const std::string& text, uint16_t channelId);
void SendPrivateMessage(const GameConnection_ptr &connection, const Player* speaker,
                        SpeakClasses type, const std::string& text);
void SendCancelTarget(const GameConnection_ptr &connection);
void SendChangeSpeed(const GameConnection_ptr &connection, const Creature* creature, uint32_t speed);
void SendCancelWalk(const GameConnection_ptr &connection);
void SendSkills(const GameConnection_ptr &connection);
void SendPing(const GameConnection_ptr &connection);
void SendPingBack(const GameConnection_ptr &connection);
void SendDistanceShoot(const GameConnection_ptr &connection, const Position& from, const Position& to, uint8_t type);
void SendMagicEffect(const GameConnection_ptr &connection, const Position& pos, uint8_t type);
void SendCreatureHealth(const GameConnection_ptr &connection, const Creature* creature);
void SendFYIBox(const GameConnection_ptr &connection, const std::string& message);
void SendMapDescription(const GameConnection_ptr &connection, const Position& pos);
void SendAddTileItem(const GameConnection_ptr &connection, const Position& pos,
                     uint32_t stackpos, const Item* item);
void SendUpdateTileItem(const GameConnection_ptr &connection, const Position& pos,
                        uint32_t stackpos, const Item* item);
void SendRemoveTileThing(const GameConnection_ptr &connection, const Position& pos, uint32_t stackpos);
void SendUpdateTileCreature(const GameConnection_ptr &connection, const Position& pos, uint32_t stackpos, const Creature* creature);
void SendRemoveTileCreature(const GameConnection_ptr &connection, const Creature* creature, const Position& pos, uint32_t stackpos);
void SendUpdateTile(const GameConnection_ptr &connection, const Tile* tile, const Position& pos);
void SendUpdateCreatureIcons(const GameConnection_ptr &connection, const Creature* creature);
void SendPendingStateEntered(const GameConnection_ptr &connection);
void SendEnterWorld(const GameConnection_ptr &connection);
void SendFightModes(const GameConnection_ptr &connection);
void SendAddCreature(const GameConnection_ptr &connection, const Creature* creature,
                     const Position& pos, int32_t stackpos, MagicEffectClasses magicEffect = CONST_ME_NONE);
void SendMoveCreature(const GameConnection_ptr &connection, const Creature* creature,
                      const Position& newPos, int32_t newStackPos,
                      const Position& oldPos, int32_t oldStackPos, bool teleport);
void SendInventoryItem(const GameConnection_ptr &connection, slots_t slot, const Item* item);
void SendItems(const GameConnection_ptr &connection);
void SendAddContainerItem(const GameConnection_ptr &connection,
                          uint8_t cid, uint16_t slot, const Item* item);
void SendUpdateContainerItem(const GameConnection_ptr &connection,
                             uint8_t cid, uint16_t slot, const Item* item);
void SendRemoveContainerItem(const GameConnection_ptr &connection,
                             uint8_t cid, uint16_t slot, const Item* lastItem);
void SendTextWindow(const GameConnection_ptr &connection, uint32_t windowTextId,
                    Item* item, uint16_t maxlen, bool canWrite);
void SendTextWindow(const GameConnection_ptr &connection, uint32_t windowTextId,
                    uint32_t itemId, const std::string& text);
void SendHouseWindow(const GameConnection_ptr &connection,
                     uint32_t windowTextId, const std::string& text);
void SendCombatAnalyzer(const GameConnection_ptr &connection, CombatType_t type,
                        int32_t amount, DamageAnalyzerImpactType impactType,
                        const std::string& target);
void SendOutfitWindow(const GameConnection_ptr &connection);
void SendPodiumWindow(const GameConnection_ptr &connection, const Item* item);
void SendUpdatedVIPStatus(const GameConnection_ptr &connection, uint32_t guid, VipStatus_t newStatus);
void SendVIP(const GameConnection_ptr &connection, uint32_t guid, const std::string& name,
             const std::string& description, uint32_t icon, bool notify, VipStatus_t status);
void SendVIPEntries(const GameConnection_ptr &connection);
void SendItemClasses(const GameConnection_ptr &connection);
void SendSpellCooldown(const GameConnection_ptr &connection, uint8_t spellId, uint32_t time);
void SendSpellGroupCooldown(const GameConnection_ptr &connection, SpellGroup_t groupId, uint32_t time);
void SendUseItemCooldown(const GameConnection_ptr &connection, uint32_t time);
void SendSupplyUsed(const GameConnection_ptr &connection, const uint16_t clientId);
void SendModalWindow(const GameConnection_ptr &connection, const ModalWindow& modalWindow);

boost::asio::awaitable<void> GameService(boost::asio::ip::tcp::endpoint endpoint);

#endif //__OTSERV_SERVICE_GAME_H__
