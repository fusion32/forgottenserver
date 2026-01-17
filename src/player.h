// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_PLAYER_H
#define FS_PLAYER_H

#include "creature.h"
#include "depotchest.h"
#include "depotlocker.h"
#include "enums.h"
#include "groups.h"
#include "guild.h"
#include "inbox.h"
#include "service_game.h"
#include "town.h"
#include "vocation.h"

class House;
struct Mount;
class NetworkMessage;
class Npc;
class Party;
class SchedulerTask;

enum skillsid_t
{
	SKILLVALUE_LEVEL = 0,
	SKILLVALUE_TRIES = 1,
	SKILLVALUE_PERCENT = 2,
};

enum fightMode_t : uint8_t
{
	FIGHTMODE_ATTACK = 1,
	FIGHTMODE_BALANCED = 2,
	FIGHTMODE_DEFENSE = 3,
};

enum pvpMode_t : uint8_t
{
	PVP_MODE_DOVE = 0,
	PVP_MODE_WHITE_HAND = 1,
	PVP_MODE_YELLOW_HAND = 2,
	PVP_MODE_RED_FIST = 3,
};

enum tradestate_t : uint8_t
{
	TRADE_NONE,
	TRADE_INITIATED,
	TRADE_ACCEPT,
	TRADE_ACKNOWLEDGE,
	TRADE_TRANSFER,
};

struct VIPEntry
{
	VIPEntry(uint32_t guid, std::string_view name, std::string_view description, uint32_t icon, bool notify) :
	    guid{guid}, name{name}, description{description}, icon{icon}, notify{notify}
	{}

	uint32_t guid;
	std::string name;
	std::string description;
	uint32_t icon;
	bool notify;
};

static constexpr int PLAYER_MAX_OPEN_CONTAINERS = 16;
struct OpenContainer
{
	Container *container = NULL;
	int firstIndex = 0;
};

static constexpr int16_t MINIMUM_SKILL_LEVEL = 10;

struct Skill
{
	uint64_t tries = 0;
	uint16_t level = MINIMUM_SKILL_LEVEL;
	uint16_t percent = 0;
};

using MuteCountMap = std::map<uint32_t, uint32_t>;

static constexpr int32_t PLAYER_MAX_SPEED = 1500;
static constexpr int32_t PLAYER_MIN_SPEED = 10;

static constexpr int32_t NOTIFY_DEPOT_BOX_RANGE = 1;

class Player final : public Creature
{
public:
	explicit Player(GameConnection_ptr connection_);
	~Player();

	using Creature::onWalk;

	// non-copyable
	Player(const Player&) = delete;
	Player& operator=(const Player&) = delete;

	Player* getPlayer() override { return this; }
	const Player* getPlayer() const override { return this; }

	Thing* getReceiver() override final { return this; }
	const Thing* getReceiver() const override final { return this; }

	void setID() final;

	static MuteCountMap muteCountMap;

	const std::string& getName() const override { return name; }
	void setName(std::string_view name) { this->name = name; }
	const std::string& getNameDescription() const override { return name; }
	std::string getDescription(int32_t lookDistance) const override;

	CreatureType_t getType() const override { return CREATURETYPE_PLAYER; }

	uint16_t getRandomMount() const;
	uint16_t getCurrentMount() const;
	void setCurrentMount(uint16_t mountId);
	bool isMounted() const { return defaultOutfit.lookMount != 0; }
	bool toggleMount(bool mount);
	bool tameMount(uint16_t mountId);
	bool untameMount(uint16_t mountId);
	bool hasMount(const Mount* mount) const;
	bool hasMounts() const;
	void dismount();

	void sendFYIBox(const std::string& message)
	{
		if (connection) {
			SendFYIBox(connection, message);
		}
	}

	void setGUID(uint32_t guid) { this->guid = guid; }
	uint32_t getGUID() const { return guid; }
	bool canSeeInvisibility() const override { return hasFlag(PlayerFlag_CanSenseInvisibility) || group->access; }

	void removeList() override;
	void addList() override;
	void kickPlayer(bool displayEffect);

	static uint64_t getExpForLevel(const uint64_t lv)
	{
		return (((lv - 6ULL) * lv + 17ULL) * lv - 12ULL) / 6ULL * 100ULL;
	}

	uint16_t getStaminaMinutes() const { return staminaMinutes; }

	bool addOfflineTrainingTries(skills_t skill, uint64_t tries);

	void addOfflineTrainingTime(int32_t addTime)
	{
		offlineTrainingTime = std::min<int32_t>(12 * 3600 * 1000, offlineTrainingTime + addTime);
	}
	void removeOfflineTrainingTime(int32_t removeTime)
	{
		offlineTrainingTime = std::max<int32_t>(0, offlineTrainingTime - removeTime);
	}
	int32_t getOfflineTrainingTime() const { return offlineTrainingTime; }

	int32_t getOfflineTrainingSkill() const { return offlineTrainingSkill; }
	void setOfflineTrainingSkill(int32_t skill) { offlineTrainingSkill = skill; }

	uint64_t getBankBalance() const { return bankBalance; }
	void setBankBalance(uint64_t balance) { bankBalance = balance; }

	Guild_ptr getGuild() const { return guild; }
	void setGuild(Guild_ptr guild);

	GuildRank_ptr getGuildRank() const { return guildRank; }
	void setGuildRank(GuildRank_ptr newGuildRank) { guildRank = newGuildRank; }

	bool isGuildMate(const Player* player) const;

	const std::string& getGuildNick() const { return guildNick; }
	void setGuildNick(std::string nick) { guildNick = nick; }

	bool isInWar(const Player* player) const;
	bool isInWarList(uint32_t guildId) const;

	void setLastWalkthroughAttempt(int64_t walkthroughAttempt) { lastWalkthroughAttempt = walkthroughAttempt; }
	void setLastWalkthroughPosition(Position walkthroughPosition) { lastWalkthroughPosition = walkthroughPosition; }

	Inbox_ptr getInbox()
	{
		if (!inbox) {
			inbox = std::make_shared<Inbox>(ITEM_INBOX);
		}
		return inbox;
	}

	StoreInbox* getStoreInbox() const { return storeInbox; }

	uint32_t getClientIcons() const;

	const GuildWarVector& getGuildWarVector() const { return guildWarVector; }

	Vocation* getVocation() const { return vocation; }

	int getTerminalType() const {
		if(connection){
			return GetTerminalType(connection);
		}else{
			return TERMINAL_NONE;
		}
	}

	int getTerminalVersion() const {
		if(connection){
			return GetTerminalVersion(connection);
		}else{
			return 0;
		}
	}

	bool hasSecureMode() const { return secureMode; }

	void setParty(Party* party) { this->party = party; }
	Party* getParty() const { return party; }
	PartyShields_t getPartyShield(const Player* player) const;
	bool isInviting(const Player* player) const;
	bool isPartner(const Player* player) const;
	void sendPlayerPartyIcons(Player* player);
	bool addPartyInvitation(Party* party);
	void removePartyInvitation(Party* party);
	void clearPartyInvitations();

	GuildEmblems_t getGuildEmblem(const Player* player) const;

	uint64_t getSpentMana() const { return manaSpent; }

	bool hasFlag(PlayerFlags value) const { return (group->flags & value) != 0; }

	BedItem* getBedItem() { return bedItem; }
	void setBedItem(BedItem* b) { bedItem = b; }

	void addBlessing(uint8_t blessing) { blessings.set(blessing); }
	void removeBlessing(uint8_t blessing) { blessings.reset(blessing); }
	bool hasBlessing(uint8_t blessing) const { return blessings.test(blessing); }

	bool isOffline() const { return (getID() == 0); }
	void disconnect()
	{
		if (connection) {
			Detach(connection);
		}
	}
	boost::asio::ip::address getIP() const;

	void addContainer(int cid, Container* container);
	void closeContainer(int cid);
	void setContainerFirstIndex(int cid, int firstIndex);
	Container* getContainerByID(int cid);
	int getContainerID(const Container* container) const;
	int getContainerFirstIndex(int cid) const;
	int findAvailableContainerID(void) const;
	const auto &getOpenContainers() const { return openContainers; }

	bool canOpenCorpse(uint32_t ownerId) const;

	void setStorageValue(uint32_t key, std::optional<int32_t> value, bool isSpawn = false) override;

	void setGroup(Group* newGroup) { group = newGroup; }
	Group* getGroup() const { return group; }

	void setInMarket(bool value) { inMarket = value; }
	bool isInMarket() const { return inMarket; }

	int32_t getIdleTime() const { return idleTime; }

	void resetIdleTime() { idleTime = 0; }

	bool isInGhostMode() const override { return ghostMode; }
	bool canSeeGhostMode(const Creature* creature) const override;
	void switchGhostMode() { ghostMode = !ghostMode; }

	uint32_t getAccount() const { return accountNumber; }
	AccountType_t getAccountType() const { return accountType; }
	uint32_t getLevel() const { return level; }
	uint8_t getLevelPercent() const { return levelPercent; }
	uint32_t getMagicLevel() const { return std::max<int32_t>(0, magLevel + varStats[STAT_MAGICPOINTS]); }
	uint32_t getSpecialMagicLevel(CombatType_t type) const
	{
		return std::max<int32_t>(0, specialMagicLevelSkill[combatTypeToIndex(type)]);
	}
	uint32_t getBaseMagicLevel() const { return magLevel; }
	uint16_t getMagicLevelPercent() const { return magLevelPercent; }
	uint8_t getSoul() const { return soul; }
	bool isAccessPlayer() const { return group->access; }
	bool isPremium() const;
	void setPremiumEnd(time_t timestamp);

	bool setVocation(uint16_t vocId);
	uint16_t getVocationId() const { return vocation->getId(); }

	PlayerSex_t getSex() const { return sex; }
	void setSex(PlayerSex_t);
	uint64_t getExperience() const { return experience; }

	time_t getLastLoginSaved() const { return lastLoginSaved; }

	time_t getLastLogout() const { return lastLogout; }

	const Position& getLoginPosition() const { return loginPosition; }
	const Position& getTemplePosition() const { return town->templePosition; }
	const Town* getTown() const { return town; }
	void setTown(const Town* town) { this->town = town; }

	void clearModalWindows();
	bool hasModalWindowOpen(uint32_t modalWindowId) const;
	void onModalWindowHandled(uint32_t modalWindowId);

	bool isPushable() const override;
	uint32_t isMuted() const;
	void addMessageBuffer();
	void removeMessageBuffer();

	bool removeItemOfType(uint16_t itemId, uint32_t amount, int32_t subType, bool ignoreEquipped = false) const;

	uint32_t getCapacity() const
	{
		if (hasFlag(PlayerFlag_CannotPickupItem)) {
			return 0;
		} else if (hasFlag(PlayerFlag_HasInfiniteCapacity)) {
			return std::numeric_limits<uint32_t>::max();
		}
		return capacity;
	}

	uint32_t getFreeCapacity() const
	{
		if (hasFlag(PlayerFlag_CannotPickupItem)) {
			return 0;
		} else if (hasFlag(PlayerFlag_HasInfiniteCapacity)) {
			return std::numeric_limits<uint32_t>::max();
		}
		return std::max<int32_t>(0, capacity - inventoryWeight);
	}

	int32_t getMaxHealth() const override { return std::max<int32_t>(1, healthMax + varStats[STAT_MAXHITPOINTS]); }
	uint32_t getMana() const { return mana; }
	uint32_t getMaxMana() const { return std::max<int32_t>(0, manaMax + varStats[STAT_MAXMANAPOINTS]); }
	uint16_t getManaShieldBar() const { return manaShieldBar; }
	void setManaShieldBar(uint16_t value) { manaShieldBar = value; }
	uint16_t getMaxManaShieldBar() const { return maxManaShieldBar; }
	void setMaxManaShieldBar(uint16_t value) { maxManaShieldBar = value; }

	Item* getInventoryItem(slots_t slot) const;

	bool isItemAbilityEnabled(slots_t slot) const { return inventoryAbilities[slot]; }
	void setItemAbility(slots_t slot, bool enabled) { inventoryAbilities[slot] = enabled; }

	void setVarSkill(skills_t skill, int32_t modifier) { varSkills[skill] += modifier; }

	void setVarSpecialSkill(SpecialSkills_t skill, int32_t modifier) { varSpecialSkills[skill] += modifier; }

	void setSpecialMagicLevelSkill(CombatType_t type, int16_t modifier)
	{
		specialMagicLevelSkill[combatTypeToIndex(type)] += modifier;
	}

	void setVarStats(stats_t stat, int32_t modifier);

	int32_t getDefaultStats(stats_t stat) const;

	void addConditionSuppressions(uint32_t conditions);
	void removeConditionSuppressions(uint32_t conditions);

	DepotChest_ptr getDepotChest(uint32_t depotId, bool autoCreate);
	DepotLocker& getDepotLocker();
	void onReceiveMail() const;
	bool isNearDepotBox() const;

	bool canSee(const Position& pos) const override;
	bool canSeeCreature(const Creature* creature) const override;

	bool canWalkthrough(const Creature* creature) const;
	bool canWalkthroughEx(const Creature* creature) const;

	RaceType_t getRace() const override { return RACE_BLOOD; }

	uint64_t getMoney() const;

	// safe-trade functions
	void setTradeState(tradestate_t state) { tradeState = state; }
	tradestate_t getTradeState() const { return tradeState; }
	Item* getTradeItem() { return tradeItem; }

	// shop functions
	void setShopOwner(Npc* owner, int32_t onBuy, int32_t onSell)
	{
		shopOwner = owner;
		purchaseCallback = onBuy;
		saleCallback = onSell;
	}

	Npc* getShopOwner(int32_t& onBuy, int32_t& onSell)
	{
		onBuy = purchaseCallback;
		onSell = saleCallback;
		return shopOwner;
	}

	const Npc* getShopOwner(int32_t& onBuy, int32_t& onSell) const
	{
		onBuy = purchaseCallback;
		onSell = saleCallback;
		return shopOwner;
	}

	// V.I.P. functions
	void notifyStatusChange(Player* loginPlayer, VipStatus_t status);
	bool removeVIP(uint32_t vipGuid);
	bool addVIP(uint32_t vipGuid, const std::string& vipName, VipStatus_t status);
	bool addVIPInternal(uint32_t vipGuid);
	bool editVIP(uint32_t vipGuid, const std::string& description, uint32_t icon, bool notify);

	// follow functions
	void setFollowCreature(Creature* creature) override;
	void goToFollowCreature() override;

	// follow events
	void onUnfollowCreature() override;

	// walk events
	void onWalk(Direction& dir) override;
	void onWalkAborted() override;
	void onWalkComplete() override;

	void stopWalk();
	void openShopWindow(Npc* npc, const std::list<ShopInfo>& shop);
	bool closeShopWindow(bool sendCloseShopWindow = true);
	bool updateSaleShopList(const Item* item);
	bool hasShopItemForSale(uint32_t itemId, uint8_t subType) const;

	void setChaseMode(bool mode);
	void setFightMode(fightMode_t mode) { fightMode = mode; }
	void setSecureMode(bool mode) { secureMode = mode; }

	// combat functions
	void setAttackedCreature(Creature* creature) override;
	void removeAttackedCreature() override;
	bool isImmune(CombatType_t type) const override;
	bool isImmune(ConditionType_t type) const override;
	bool hasShield() const;
	bool isAttackable() const override;
	static bool lastHitIsPlayer(Creature* lastHitCreature);

	void changeHealth(int32_t healthChange, bool sendHealthChange = true) override;
	void changeMana(int32_t manaChange);
	void changeSoul(int32_t soulChange);

	bool isPzLocked() const { return pzLocked; }
	BlockType_t blockHit(Creature* attacker, CombatType_t combatType, int32_t& damage, bool checkDefense = false,
	                     bool checkArmor = false, bool field = false, bool ignoreResistances = false) override;
	void doAttacking(uint32_t interval) override;
	bool hasExtraSwing() override { return lastAttack > 0 && ((OTSYS_TIME() - lastAttack) >= getAttackSpeed()); }

	uint16_t getSpecialSkill(uint8_t skill) const { return std::max<uint16_t>(0, varSpecialSkills[skill]); }
	uint16_t getSkillLevel(uint8_t skill) const
	{
		return std::max<uint16_t>(0, skills[skill].level + varSkills[skill]);
	}
	uint16_t getSpecialMagicLevelSkill(CombatType_t type) const
	{
		return std::max<int32_t>(0, specialMagicLevelSkill[combatTypeToIndex(type)]);
	}
	uint16_t getBaseSkill(uint8_t skill) const { return skills[skill].level; }
	uint16_t getSkillPercent(uint8_t skill) const { return skills[skill].percent; }

	bool getAddAttackSkill() const { return addAttackSkillPoint; }
	BlockType_t getLastAttackBlockType() const { return lastAttackBlockType; }

	Item* getWeapon(slots_t slot, bool ignoreAmmo) const;
	Item* getWeapon(bool ignoreAmmo = false) const;
	WeaponType_t getWeaponType() const;
	int32_t getWeaponSkill(const Item* item) const;
	void getShieldAndWeapon(const Item*& shield, const Item*& weapon) const;

	void drainHealth(Creature* attacker, int32_t damage) override;
	void drainMana(Creature* attacker, int32_t manaLoss);
	void addManaSpent(uint64_t amount);
	void removeManaSpent(uint64_t amount, bool notify = false);
	void addSkillAdvance(skills_t skill, uint64_t count);
	void removeSkillTries(skills_t skill, uint64_t count, bool notify = false);

	int32_t getArmor() const override;
	int32_t getDefense() const override;
	float getAttackFactor() const override;
	float getDefenseFactor() const override;

	void addInFightTicks(bool pzlock = false);

	uint64_t getGainedExperience(Creature* attacker) const override;

	// combat event functions
	void onAddCondition(ConditionType_t type) override;
	void onAddCombatCondition(ConditionType_t type) override;
	void onEndCondition(ConditionType_t type) override;
	void onCombatRemoveCondition(Condition* condition) override;
	void onAttackedCreature(Creature* target, bool addFightTicks = true) override;
	void onAttacked() override;
	void onAttackedCreatureDrainHealth(Creature* target, int32_t points) override;
	void onTargetCreatureGainHealth(Creature* target, int32_t points) override;
	bool onKilledCreature(Creature* target, bool lastHit = true) override;
	void onGainExperience(uint64_t gainExp, Creature* target) override;
	void onGainSharedExperience(uint64_t gainExp, Creature* source);
	void onAttackedCreatureBlockHit(BlockType_t blockType) override;
	void onBlockHit() override;
	void onChangeZone(ZoneType_t zone) override;
	void onAttackedCreatureChangeZone(ZoneType_t zone) override;
	void onIdleStatus() override;

	LightInfo getCreatureLight() const override;

	Skulls_t getSkull() const override;
	Skulls_t getSkullClient(const Creature* creature) const override;
	int64_t getSkullTicks() const { return skullTicks; }
	void setSkullTicks(int64_t ticks) { skullTicks = ticks; }

	bool hasAttacked(const Player* attacked) const;
	void addAttacked(const Player* attacked);
	void removeAttacked(const Player* attacked);
	void clearAttacked();
	void addUnjustifiedDead(const Player* attacked);
	void sendCreatureSkull(const Creature* creature) const
	{
		if (connection) {
			SendCreatureSkull(connection, creature);
		}
	}
	void checkSkullTicks(int64_t ticks);

	bool canWear(uint32_t lookType, uint8_t addons) const;
	bool hasOutfit(uint32_t lookType, uint8_t addons);
	void addOutfit(uint16_t lookType, uint8_t addons);
	bool removeOutfit(uint16_t lookType);
	bool removeOutfitAddon(uint16_t lookType, uint8_t addons);
	bool getOutfitAddons(const Outfit& outfit, uint8_t& addons) const;

	size_t getMaxVIPEntries() const;
	size_t getMaxDepotItems() const;

	// tile
	// send methods
	void sendAddTileItem(const Tile* tile, const Position& pos, const Item* item)
	{
		if (connection) {
			int32_t stackpos = tile->getStackposOfItem(this, item);
			if (stackpos != -1) {
				SendAddTileItem(connection, pos, stackpos, item);
			}
		}
	}
	void sendUpdateTileItem(const Tile* tile, const Position& pos, const Item* item)
	{
		if (connection) {
			int32_t stackpos = tile->getStackposOfItem(this, item);
			if (stackpos != -1) {
				SendUpdateTileItem(connection, pos, stackpos, item);
			}
		}
	}
	void sendRemoveTileThing(const Position& pos, int32_t stackpos)
	{
		if (stackpos != -1 && connection) {
			SendRemoveTileThing(connection, pos, stackpos);
		}
	}
	void sendUpdateTileCreature(const Creature* creature)
	{
		if (connection) {
			SendUpdateTileCreature(connection, creature->getPosition(),
			                               creature->getTile()->getClientIndexOfCreature(this, creature), creature);
		}
	}
	void sendRemoveTileCreature(const Creature* creature, const Position& pos, int32_t stackpos)
	{
		if (connection) {
			SendRemoveTileCreature(connection, creature, pos, stackpos);
		}
	}
	void sendUpdateTile(const Tile* tile, const Position& pos)
	{
		if (connection) {
			SendUpdateTile(connection, tile, pos);
		}
	}
	void sendUpdateCreatureIcons(const Creature* creature)
	{
		if (connection) {
			SendUpdateCreatureIcons(connection, creature);
		}
	}

	void sendChannelMessage(const std::string& author, const std::string& text, SpeakClasses type, uint16_t channel)
	{
		if (connection) {
			SendChannelMessage(connection, author, text, type, channel);
		}
	}
	void sendChannelEvent(uint16_t channelId, const std::string& playerName, ChannelEvent_t channelEvent)
	{
		if (connection) {
			SendChannelEvent(connection, channelId, playerName, channelEvent);
		}
	}
	void sendAddCreature(const Creature* creature, const Position& pos, MagicEffectClasses magicEffect = CONST_ME_NONE)
	{
		if (connection) {
			SendAddCreature(connection, creature, pos, creature->getTile()->getClientIndexOfCreature(this, creature),
			                        magicEffect);
		}
	}
	void sendCreatureMove(const Creature* creature, const Position& newPos, int32_t newStackPos, const Position& oldPos,
	                      int32_t oldStackPos, bool teleport)
	{
		if (connection) {
			SendMoveCreature(connection, creature, newPos, newStackPos, oldPos, oldStackPos, teleport);
		}
	}
	void sendCreatureTurn(const Creature* creature)
	{
		if (connection && canSeeCreature(creature)) {
			int32_t stackpos = creature->getTile()->getClientIndexOfCreature(this, creature);
			if (stackpos != -1) {
				SendCreatureTurn(connection, creature, stackpos);
			}
		}
	}
	void sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text,
	                     const Position* pos = nullptr)
	{
		if (connection) {
			SendCreatureSay(connection, creature, type, text, pos);
		}
	}
	void sendPrivateMessage(const Player* speaker, SpeakClasses type, const std::string& text)
	{
		if (connection) {
			SendPrivateMessage(connection, speaker, type, text);
		}
	}
	void sendCreatureSquare(const Creature* creature, SquareColor_t color)
	{
		if (connection) {
			SendCreatureSquare(connection, creature, color);
		}
	}
	void sendCreatureChangeOutfit(const Creature* creature, const Outfit_t& outfit)
	{
		if (connection) {
			SendCreatureOutfit(connection, creature, outfit);
		}
	}
	void sendCreatureChangeVisible(const Creature* creature, bool visible)
	{
		if (!connection) {
			return;
		}

		if (creature->getPlayer()) {
			if (visible) {
				SendCreatureOutfit(connection, creature, creature->getCurrentOutfit());
			} else {
				static Outfit_t outfit;
				SendCreatureOutfit(connection, creature, outfit);
			}
		} else if (canSeeInvisibility()) {
			SendCreatureOutfit(connection, creature, creature->getCurrentOutfit());
		} else {
			int32_t stackpos = creature->getTile()->getClientIndexOfCreature(this, creature);
			if (stackpos == -1) {
				return;
			}

			if (visible) {
				SendAddCreature(connection, creature, creature->getPosition(), stackpos);
			} else {
				SendRemoveTileCreature(connection, creature, creature->getPosition(), stackpos);
			}
		}
	}
	void sendLight()
	{
		if (connection) {
			SendCreatureLight(connection, this);
		}
	}
	void sendCreatureLight(const Creature* creature)
	{
		if (connection) {
			SendCreatureLight(connection, creature);
		}
	}
	void sendCreatureWalkthrough(const Creature* creature, bool walkthrough)
	{
		if (connection) {
			SendCreatureWalkthrough(connection, creature, walkthrough);
		}
	}
	void sendCreatureShield(const Creature* creature)
	{
		if (connection) {
			SendCreatureShield(connection, creature);
		}
	}
	void sendSpellCooldown(uint8_t spellId, uint32_t time)
	{
		if (connection) {
			SendSpellCooldown(connection, spellId, time);
		}
	}
	void sendSpellGroupCooldown(SpellGroup_t groupId, uint32_t time)
	{
		if (connection) {
			SendSpellGroupCooldown(connection, groupId, time);
		}
	}
	void sendUseItemCooldown(uint32_t time)
	{
		if (connection) {
			SendUseItemCooldown(connection, time);
		}
	}
	void sendSupplyUsed(const uint16_t clientId) const
	{
		if (connection) {
			SendSupplyUsed(connection, clientId);
		}
	}
	void sendModalWindow(const ModalWindow& modalWindow);

	// container
	void sendAddContainerItem(const Container* container, const Item* item);
	void sendUpdateContainerItem(const Container* container, int index, const Item* newItem);
	void sendRemoveContainerItem(const Container* container, int index);
	void sendContainer(uint8_t cid, const Container* container, uint16_t firstIndex)
	{
		if (connection) {
			SendContainer(connection, cid, container, firstIndex);
		}
	}

	// inventory
	void sendInventoryItem(slots_t slot, const Item* item)
	{
		if (connection) {
			SendInventoryItem(connection, slot, item);
		}
	}
	void sendItems()
	{
		if (connection) {
			SendItems(connection);
		}
	}
	void openSavedContainers();
	void sendQuiverUpdate(bool sendAll = false)
	{
		if (!sendAll) {
			// update one slot
			Thing* slotThing = getThing(CONST_SLOT_RIGHT);
			if (slotThing) {
				Item* slotItem = slotThing->getItem();
				if (slotItem && slotItem->getWeaponType() == WEAPON_QUIVER) {
					sendInventoryItem(CONST_SLOT_RIGHT, slotItem);
				}
			}
		} else {
			// update all slots
			std::vector<slots_t> slots = {CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_AMMO};
			for (auto const& slot : slots) {
				Thing* slotThing = getThing(slot);
				if (slotThing) {
					Item* slotItem = slotThing->getItem();
					if (slotItem && slotItem->getWeaponType() == WEAPON_QUIVER) {
						sendInventoryItem(slot, slotItem);
					}
				}
			}
		}
	}

	// event methods
	void onUpdateTileItem(const Tile* tile, const Position& pos, const Item* oldItem, const ItemType& oldType,
	                      const Item* newItem, const ItemType& newType) override;
	void onRemoveTileItem(const Tile* tile, const Position& pos, const ItemType& iType, const Item* item) override;

	void onCreatureAppear(Creature* creature, bool isLogin, MagicEffectClasses magicEffect) override;
	void onRemoveCreature(Creature* creature, bool isLogout) override;
	void onCreatureMove(Creature* creature, const Tile* newTile, const Position& newPos, const Tile* oldTile,
	                    const Position& oldPos, bool teleport) override;

	void onAttackedCreatureDisappear(bool isLogout) override;
	void onFollowCreatureDisappear(bool isLogout) override;

	// container
	void onAddContainerItem(const Item* item);
	void onUpdateContainerItem(const Container* container, const Item* oldItem, const Item* newItem);
	void onRemoveContainerItem(const Container* container, const Item* item);

	void onCloseContainer(const Container* container);
	void onSendContainer(const Container* container);
	void autoCloseContainers(const Container* container);

	// inventory
	void onUpdateInventoryItem(Item* oldItem, Item* newItem);
	void onRemoveInventoryItem(Item* item);

	void sendVIPEntries() const
	{
		if (connection) {
			SendVIPEntries(connection);
		}
	}
	void sendItemClasses() const
	{
		if (connection) {
			SendItemClasses(connection);
		}
	}
	void sendClientFeatures() const
	{
		if (connection) {
			SendClientFeatures(connection);
		}
	}
	void sendMapDescription() const
	{
		if (connection) {
			SendMapDescription(connection, position);
		}
	}
	void sendPendingStateEntered() const
	{
		if (connection) {
			SendPendingStateEntered(connection);
		}
	}
	void sendCancelMessage(const std::string& msg) const
	{
		if (connection) {
			SendTextMessage(connection, TextMessage(MESSAGE_STATUS_SMALL, msg));
		}
	}
	void sendCancelMessage(ReturnValue message) const;
	void sendCancelTarget() const
	{
		if (connection) {
			SendCancelTarget(connection);
		}
	}
	void sendCancelWalk() const
	{
		if (connection) {
			SendCancelWalk(connection);
		}
	}
	void sendChangeSpeed(const Creature* creature, uint32_t newSpeed) const
	{
		if (connection) {
			SendChangeSpeed(connection, creature, newSpeed);
		}
	}
	void sendCreatureHealth(const Creature* creature) const
	{
		if (connection) {
			SendCreatureHealth(connection, creature);
		}
	}
	void sendDistanceShoot(const Position& from, const Position& to, unsigned char type) const
	{
		if (connection) {
			SendDistanceShoot(connection, from, to, type);
		}
	}
	void sendHouseWindow(House* house, uint32_t listId) const;
	void sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName)
	{
		if (connection) {
			SendCreatePrivateChannel(connection, channelId, channelName);
		}
	}
	void sendClosePrivate(uint16_t channelId);
	void sendIcons() const
	{
		if (connection) {
			SendIcons(connection, getClientIcons());
		}
	}
	void sendMagicEffect(uint8_t type) const
	{
		if (connection) {
			SendMagicEffect(connection, position, type);
		}
	}
	void sendMagicEffect(const Position& pos, uint8_t type) const
	{
		if (connection) {
			SendMagicEffect(connection, pos, type);
		}
	}
	void sendPing();
	void sendPingBack() const
	{
		if (connection) {
			SendPingBack(connection);
		}
	}
	void sendStats();

	void sendExperienceTracker(int64_t rawExp, int64_t finalExp) const
	{
		if (connection) {
			SendExperienceTracker(connection, rawExp, finalExp);
		}
	}

	void sendBasicData() const
	{
		if (connection) {
			SendBasicData(connection);
		}
	}
	void sendSkills() const
	{
		if (connection) {
			SendSkills(connection);
		}
	}
	void sendTextMessage(MessageClasses mclass, const std::string& message) const
	{
		if (connection) {
			SendTextMessage(connection, TextMessage(mclass, message));
		}
	}
	void sendTextMessage(const TextMessage& message) const
	{
		if (connection) {
			SendTextMessage(connection, message);
		}
	}
	void sendReLoginWindow(uint8_t unfairFightReduction) const
	{
		if (connection) {
			SendReLoginWindow(connection, unfairFightReduction);
		}
	}
	void sendTextWindow(Item* item, uint16_t maxlen, bool canWrite) const
	{
		if (connection) {
			SendTextWindow(connection, windowTextId, item, maxlen, canWrite);
		}
	}
	void sendTextWindow(uint32_t itemId, const std::string& text) const
	{
		if (connection) {
			SendTextWindow(connection, windowTextId, itemId, text);
		}
	}
	void sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId) const
	{
		if (connection) {
			SendToChannel(connection, creature, type, text, channelId);
		}
	}
	void sendShop(Npc* npc) const
	{
		if (connection) {
			SendShop(connection, npc, shopItemList);
		}
	}
	void sendSaleItemList() const
	{
		if (connection) {
			SendSaleItemList(connection, shopItemList);
		}
	}
	void sendCloseShop() const
	{
		if (connection) {
			SendCloseShop(connection);
		}
	}
	void sendMarketEnter() const
	{
		if (connection) {
			SendMarketEnter(connection);
		}
	}
	void sendMarketLeave()
	{
		inMarket = false;
		if (connection) {
			SendMarketLeave(connection);
		}
	}
	void sendMarketBrowseItem(uint16_t itemId, const MarketOfferList& buyOffers,
	                          const MarketOfferList& sellOffers) const
	{
		if (connection) {
			SendMarketBrowseItem(connection, itemId, buyOffers, sellOffers);
		}
	}
	void sendMarketBrowseOwnOffers(const MarketOfferList& buyOffers, const MarketOfferList& sellOffers) const
	{
		if (connection) {
			SendMarketBrowseOwnOffers(connection, buyOffers, sellOffers);
		}
	}
	void sendMarketBrowseOwnHistory(const HistoryMarketOfferList& buyOffers,
	                                const HistoryMarketOfferList& sellOffers) const
	{
		if (connection) {
			SendMarketBrowseOwnHistory(connection, buyOffers, sellOffers);
		}
	}
	void sendMarketAcceptOffer(const MarketOfferEx& offer) const
	{
		if (connection) {
			SendMarketAcceptOffer(connection, offer);
		}
	}
	void sendMarketCancelOffer(const MarketOfferEx& offer) const
	{
		if (connection) {
			SendMarketCancelOffer(connection, offer);
		}
	}
	void sendTradeItemRequest(const std::string& traderName, const Item* item, bool ack) const
	{
		if (connection) {
			SendTradeItemRequest(connection, traderName, item, ack);
		}
	}
	void sendTradeClose() const
	{
		if (connection) {
			SendCloseTrade(connection);
		}
	}
	void sendChannelsDialog()
	{
		if (connection) {
			SendChannelsDialog(connection);
		}
	}
	void sendOpenPrivateChannel(const std::string& receiver)
	{
		if (connection) {
			SendOpenPrivateChannel(connection, receiver);
		}
	}
	void sendOutfitWindow()
	{
		if (connection) {
			SendOutfitWindow(connection);
		}
	}
	void sendPodiumWindow(const Item* item)
	{
		if (connection) {
			SendPodiumWindow(connection, item);
		}
	}
	void sendCloseContainer(uint8_t cid)
	{
		if (connection) {
			SendCloseContainer(connection, cid);
		}
	}

	void sendChannel(uint16_t channelId, const std::string& channelName, const UsersMap* channelUsers,
	                 const InvitedMap* invitedUsers)
	{
		if (connection) {
			SendChannel(connection, channelId, channelName, channelUsers, invitedUsers);
		}
	}
	void sendTutorial(uint8_t tutorialId)
	{
		if (connection) {
			SendTutorial(connection, tutorialId);
		}
	}
	void sendAddMarker(const Position& pos, uint8_t markType, const std::string& desc)
	{
		if (connection) {
			SendAddMarker(connection, pos, markType, desc);
		}
	}
	void sendEnterWorld()
	{
		if (connection) {
			SendEnterWorld(connection);
		}
	}
	void sendFightModes()
	{
		if (connection) {
			SendFightModes(connection);
		}
	}
	void sendNetworkMessage(const NetworkMessage& message)
	{
		if (connection) {
			WriteToOutputBuffer(connection, message);
		}
	}
	void sendCombatAnalyzer(CombatType_t type, int32_t amount, DamageAnalyzerImpactType impactType,
	                        const std::string& target)
	{
		if (connection) {
			SendCombatAnalyzer(connection, type, amount, impactType, target);
		}
	}
	void sendResourceBalance(const ResourceTypes_t resourceType, uint64_t amount)
	{
		if (connection) {
			SendResourceBalance(connection, resourceType, amount);
		}
	}

	void receivePing() { lastPong = OTSYS_TIME(); }

	void onThink(uint32_t interval) override;

	void postAddNotification(Thing* thing, const Thing* oldParent, int32_t index,
	                         ReceiverLink_t link = LINK_OWNER) override;
	void postRemoveNotification(Thing* thing, const Thing* newParent, int32_t index,
	                            ReceiverLink_t link = LINK_OWNER) override;

	void setNextAction(int64_t time)
	{
		if (time > nextAction) {
			nextAction = time;
		}
	}
	bool canDoAction() const { return nextAction <= OTSYS_TIME(); }
	uint32_t getNextActionTime() const;

	Item* getWriteItem(uint32_t& windowTextId, uint16_t& maxWriteLen);
	void setWriteItem(Item* item, uint16_t maxWriteLen = 0);

	House* getEditHouse(uint32_t& windowTextId, uint32_t& listId);
	void setEditHouse(House* house, uint32_t listId = 0);

	void learnInstantSpell(const std::string& spellName);
	void forgetInstantSpell(const std::string& spellName);
	bool hasLearnedInstantSpell(const std::string& spellName) const;

	void updateRegeneration();

	uint16_t getClientExpDisplay() const { return clientExpDisplay; }
	void setClientExpDisplay(uint16_t value) { clientExpDisplay = value; }

	uint16_t getClientStaminaBonusDisplay() const { return clientStaminaBonusDisplay; }
	void setClientStaminaBonusDisplay(uint16_t value) { clientStaminaBonusDisplay = value; }

	uint16_t getClientLowLevelBonusDisplay() const { return clientLowLevelBonusDisplay; }
	void setClientLowLevelBonusDisplay(uint16_t value) { clientLowLevelBonusDisplay = value; }

// TODO(fusion): Review this. But ultimately, having data be private just hurts
// implementation of free standing functions. It's probably not getting addressing
// anytime soon but it would be a good thing to move away from these OOP masochist
// designs. On the other hand, most sending functions could be implemented as member
// functions anyways, with the only requirement being `WriteToOutputBuffer`.
//private:
	std::forward_list<Condition*> getMuteConditions() const;

	void checkTradeState(const Item* item);
	bool hasCapacity(const Item* item, uint32_t count) const;

	void gainExperience(uint64_t gainExp, Creature* source);
	void addExperience(Creature* source, uint64_t exp, bool sendText = false);
	void removeExperience(uint64_t exp, bool sendText = false);

	void updateInventoryWeight();

	void setNextWalkActionTask(SchedulerTask* task);
	void setNextActionTask(SchedulerTask* task, bool resetIdleTime = true);

	void death(Creature* lastHitCreature) override;
	bool dropCorpse(Creature* lastHitCreature, Creature* mostDamageCreature, bool lastHitUnjustified,
	                bool mostDamageUnjustified) override;
	Item* getCorpse(Creature* lastHitCreature, Creature* mostDamageCreature) override;

	ReturnValue queryAdd(int32_t index, const Thing& thing, uint32_t count, uint32_t flags,
	                     Creature* actor = nullptr) const override;
	ReturnValue queryMaxCount(int32_t index, const Thing& thing, uint32_t count, uint32_t& maxQueryCount,
	                          uint32_t flags) const override;
	ReturnValue queryRemove(const Thing& thing, uint32_t count, uint32_t flags,
	                        Creature* actor = nullptr) const override;
	Thing* queryDestination(int32_t& index, const Thing& thing, Item** destItem, uint32_t& flags) override;

	void addThing(int32_t index, Thing* thing) override;

	void updateThing(Thing* thing, uint16_t itemId, uint32_t count) override;
	void replaceThing(uint32_t index, Thing* thing) override;

	void removeThing(Thing* thing, uint32_t count) override;

	int32_t getThingIndex(const Thing* thing) const override;
	size_t getFirstIndex() const override { return CONST_SLOT_FIRST; }
	size_t getLastIndex() const override { return CONST_SLOT_LAST + 1; }
	uint32_t getItemTypeCount(uint16_t itemId, int32_t subType = -1) const override;
	std::map<uint32_t, uint32_t>& getAllItemTypeCount(std::map<uint32_t, uint32_t>& countMap) const override;
	Thing* getThing(size_t index) const override;

	void internalAddThing(Thing* thing) override { internalAddThing(0, thing); }
	void internalAddThing(uint32_t index, Thing* thing) override;

	std::unordered_set<uint32_t> attackedSet;
	std::unordered_set<uint32_t> VIPList;

	std::array<OpenContainer, PLAYER_MAX_OPEN_CONTAINERS> openContainers;
	std::map<uint32_t, DepotChest_ptr> depotChests;

	std::map<uint16_t, uint8_t> outfits;
	std::unordered_set<uint16_t> mounts;
	GuildWarVector guildWarVector;

	std::list<ShopInfo> shopItemList;

	std::forward_list<Party*> invitePartyList;
	std::forward_list<uint32_t> modalWindows;
	std::forward_list<std::string> learnedInstantSpellList;
	std::forward_list<Condition*>
	    storedConditionList; // TODO: This variable is only temporarily used when logging in, get rid of it somehow

	std::string name;
	std::string guildNick;

	Skill skills[SKILL_LAST + 1];
	LightInfo itemsLight;
	Position loginPosition;
	Position lastWalkthroughPosition;

	time_t lastLoginSaved = 0;
	time_t lastLogout = 0;
	time_t premiumEnd = 0;

	uint64_t experience = 0;
	uint64_t manaSpent = 0;
	uint64_t lastAttack = 0;
	uint64_t bankBalance = 0;
	int64_t lastFailedFollow = 0;
	int64_t skullTicks = 0;
	int64_t lastWalkthroughAttempt = 0;
	int64_t lastToggleMount = 0;
	int64_t lastPing;
	int64_t lastPong;
	int64_t nextAction = 0;

	GameConnection_ptr connection;
	boost::asio::ip::address lastIP = {};
	BedItem* bedItem = nullptr;
	Guild_ptr guild = nullptr;
	GuildRank_ptr guildRank = nullptr;
	Group* group = nullptr;
	Inbox_ptr inbox = nullptr;
	Item* tradeItem = nullptr;
	Item* inventory[CONST_SLOT_LAST + 1] = {};
	Item* writeItem = nullptr;
	House* editHouse = nullptr;
	Npc* shopOwner = nullptr;
	Party* party = nullptr;
	Player* tradePartner = nullptr;
	SchedulerTask* walkTask = nullptr;
	const Town* town = nullptr;
	Vocation* vocation = nullptr;
	StoreInbox* storeInbox = nullptr;
	DepotLocker_ptr depotLocker = nullptr;

	uint32_t inventoryWeight = 0;
	uint32_t capacity = 40000;
	uint32_t damageImmunities = 0;
	uint32_t conditionImmunities = 0;
	uint32_t conditionSuppressions = 0;
	uint32_t level = 1;
	uint32_t magLevel = 0;
	uint32_t actionTaskEvent = 0;
	uint32_t walkTaskEvent = 0;
	uint32_t classicAttackEvent = 0;
	uint32_t MessageBufferTicks = 0;
	uint32_t accountNumber = 0;
	uint32_t guid = 0;
	uint32_t windowTextId = 0;
	uint32_t editListId = 0;
	uint32_t mana = 0;
	uint32_t manaMax = 0;
	uint16_t manaShieldBar = 0;
	uint16_t maxManaShieldBar = 0;
	int32_t varSkills[SKILL_LAST + 1] = {};
	int32_t varSpecialSkills[SPECIALSKILL_LAST + 1] = {};
	int32_t varStats[STAT_LAST + 1] = {};
	std::array<int16_t, COMBAT_COUNT> specialMagicLevelSkill = {0};
	int32_t purchaseCallback = -1;
	int32_t saleCallback = -1;
	int32_t MessageBufferCount = 0;
	int32_t bloodHitCount = 0;
	int32_t shieldBlockCount = 0;
	int32_t offlineTrainingSkill = -1;
	int32_t offlineTrainingTime = 0;
	int32_t idleTime = 0;

	uint16_t lastStatsTrainingTime = 0;
	uint16_t staminaMinutes = 2520;
	uint16_t maxWriteLen = 0;
	uint16_t clientExpDisplay = 100;
	uint16_t clientStaminaBonusDisplay = 100;
	uint16_t clientLowLevelBonusDisplay = 0;

	uint8_t soul = 0;
	std::bitset<6> blessings;
	uint8_t levelPercent = 0;
	uint16_t magLevelPercent = 0;

	PlayerSex_t sex = PLAYERSEX_FEMALE;
	BlockType_t lastAttackBlockType = BLOCK_NONE;
	tradestate_t tradeState = TRADE_NONE;
	fightMode_t fightMode = FIGHTMODE_ATTACK;
	AccountType_t accountType = ACCOUNT_TYPE_NORMAL;

	bool chaseMode = false;
	bool secureMode = false;
	bool inMarket = false;
	bool wasMounted = false;
	bool ghostMode = false;
	bool pzLocked = false;
	bool isConnecting = false;
	bool addAttackSkillPoint = false;
	bool inventoryAbilities[CONST_SLOT_LAST + 1] = {};
	bool randomizeMount = false;

	static uint32_t playerAutoID;
	static uint32_t playerIDLimit;

	void updateItemsLight(bool internal = false);
	int32_t getStepSpeed() const override
	{
		return std::max<int32_t>(PLAYER_MIN_SPEED, std::min<int32_t>(PLAYER_MAX_SPEED, getSpeed()));
	}
	void updateBaseSpeed()
	{
		if (!hasFlag(PlayerFlag_SetMaxSpeed)) {
			baseSpeed = vocation->getBaseSpeed() + (2 * (level - 1));
		} else {
			baseSpeed = PLAYER_MAX_SPEED;
		}
	}

	bool isPromoted() const;

	uint32_t getAttackSpeed() const;

	static uint16_t getBasisPointLevel(uint64_t count, uint64_t nextLevelCount);
	double getLostPercent() const;
	uint64_t getLostExperience() const override
	{
		return skillLoss ? static_cast<uint64_t>(experience * getLostPercent()) : 0;
	}
	uint32_t getDamageImmunities() const override { return damageImmunities; }
	uint32_t getConditionImmunities() const override { return conditionImmunities; }
	uint32_t getConditionSuppressions() const override { return conditionSuppressions; }
	uint16_t getLookCorpse() const override;
	void getPathSearchParams(const Creature* creature, FindPathParams& fpp) const override;

	friend class Game;
	friend class Npc;
	friend class LuaScriptInterface;
	friend class Map;
	friend class Actions;
	friend class IOLoginData;
};

#endif // FS_PLAYER_H
