// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "networkmessage.h"

#include "container.h"
#include "podium.h"

#include <boost/locale.hpp>

Position NetworkMessage::getPosition()
{
	Position pos;
	pos.x = get<uint16_t>();
	pos.y = get<uint16_t>();
	pos.z = getByte();
	return pos;
}

void NetworkMessage::addPosition(const Position& pos)
{
	add<uint16_t>(pos.x);
	add<uint16_t>(pos.y);
	addByte(pos.z);
}

std::string NetworkMessage::getString(int stringLen /* = 0*/)
{
	if (stringLen <= 0) {
		stringLen = get<uint16_t>();
	}

	std::string result;
	if (canRead(stringLen)) {
		// NOTE(fusion): Same as `NetworkMessage::addString`.
		std::string_view latin1{(const char*)(&buffer[rdpos]), (size_t)stringLen};
		result = boost::locale::conv::to_utf<char>(
				latin1.data(), latin1.data() + latin1.size(),
				"ISO-8859-1", boost::locale::conv::skip);
	}
	rdpos += stringLen;
	return result;
}

void NetworkMessage::addString(std::string_view s)
{
	// NOTE(fusion): Using `s.begin()` and `s.end()` fails on MSVC because for
	// whatever reason those iterators are not `const char*`, which causes the
	// overload resolution for `from_utf` to fail. The absolute peak of C++
	// design.
	std::string latin1 = boost::locale::conv::from_utf<char>(
			s.data(), s.data() + s.size(),
			"ISO-8859-1", boost::locale::conv::skip);

	int stringLen = (int)latin1.size();
	if(canAdd(stringLen)){
		add<uint16_t>(stringLen);
		std::memcpy(&buffer[wrpos], latin1.data(), stringLen);
	}

	wrpos += stringLen;
}

void NetworkMessage::addBytes(const uint8_t* bytes, int size)
{
	if (canAdd(size)) {
		std::memcpy(&buffer[wrpos], bytes, size);
	}
	wrpos += size;
}

void NetworkMessage::addDouble(double value, uint8_t precision /* = 2*/)
{
	addByte(precision);
	add<uint32_t>(value * std::pow(10.0f, precision) + INT32_MAX);
}

void NetworkMessage::addItem(uint16_t id, uint8_t count)
{
	const ItemType& it = Item::items[id];

	add<uint16_t>(it.clientId);

	if (it.stackable) {
		addByte(count);
	} else if (it.isSplash() || it.isFluidContainer()) {
		addByte(fluidMap[count & 7]);
	} else if (it.isContainer()) {
		addByte(0x00); // assigned loot container icon
		addByte(0x00); // quiver ammo count
	} else if (it.classification > 0) {
		addByte(0x00); // item tier (0-10)
	} else if (it.showClientCharges) {
		add<uint32_t>(it.charges);
		addByte(0x00); // boolean (is brand new)
	} else if (it.showClientDuration) {
		add<uint32_t>(it.decayTimeMin);
		addByte(0x00); // boolean (is brand new)
	}

	if (it.isPodium()) {
		add<uint16_t>(0); // looktype
		add<uint16_t>(0); // lookmount
		addByte(2);       // direction
		addByte(0x01);    // is visible (bool)
	}
}

void NetworkMessage::addItem(const Item* item)
{
	const ItemType& it = Item::items[item->getID()];

	add<uint16_t>(it.clientId);

	if (it.stackable) {
		addByte(std::min<uint16_t>(0xFF, item->getItemCount()));
	} else if (it.isSplash() || it.isFluidContainer()) {
		addByte(fluidMap[item->getFluidType() & 7]);
	} else if (it.classification > 0) {
		addByte(0x00); // item tier (0-10)
	}

	if (it.showClientCharges) {
		add<uint32_t>(item->getCharges());
		addByte(0); // boolean (is brand new)
	} else if (it.showClientDuration) {
		add<uint32_t>(item->getDuration() / 1000);
		addByte(0); // boolean (is brand new)
	}

	if (it.isContainer()) {
		addByte(0x00); // assigned loot container icon
		// quiver ammo count
		const Container* container = item->getContainer();
		if (container && it.weaponType == WEAPON_QUIVER) {
			addByte(0x01);
			add<uint32_t>(container->getAmmoCount());
		} else {
			addByte(0x00);
		}
	}

	// display outfit on the podium
	if (it.isPodium()) {
		const Podium* podium = item->getPodium();
		const Outfit_t& outfit = podium->getOutfit();

		// add outfit
		if (podium->hasFlag(PODIUM_SHOW_OUTFIT)) {
			add<uint16_t>(outfit.lookType);
			if (outfit.lookType != 0) {
				addByte(outfit.lookHead);
				addByte(outfit.lookBody);
				addByte(outfit.lookLegs);
				addByte(outfit.lookFeet);
				addByte(outfit.lookAddons);
			}
		} else {
			add<uint16_t>(0);
		}

		// add mount
		if (podium->hasFlag(PODIUM_SHOW_MOUNT)) {
			add<uint16_t>(outfit.lookMount);
			if (outfit.lookMount != 0) {
				addByte(outfit.lookMountHead);
				addByte(outfit.lookMountBody);
				addByte(outfit.lookMountLegs);
				addByte(outfit.lookMountFeet);
			}
		} else {
			add<uint16_t>(0);
		}

		addByte(podium->getDirection());
		addByte(podium->hasFlag(PODIUM_SHOW_PLATFORM) ? 0x01 : 0x00);
	}
}

void NetworkMessage::addItemId(uint16_t itemId) {
	add<uint16_t>(Item::items[itemId].clientId);
}

void NetworkMessage::dump(std::string_view name) const {
	int len = getWrittenLength();
	fmt::print("NetworkMessage ({}, rdpos={}, len={}):", name, rdpos, len);
	for(int i = 0; i < len; i += 1){
		if(i % 16 == 0){
			fmt::print("\n");
		}else{
			fmt::print(" ");
		}
		fmt::print("{:02X}", buffer[i]);
	}
	fmt::print("\n");
}

