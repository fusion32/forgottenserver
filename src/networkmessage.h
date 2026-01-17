// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_NETWORKMESSAGE_H
#define FS_NETWORKMESSAGE_H

#include "const.h"

class Item;
struct Position;

class NetworkMessage;
using NetworkMessage_ptr = std::unique_ptr<NetworkMessage>;

class NetworkMessage
{
public:
	int rdpos;
	int wrpos;
	std::array<uint8_t, NETWORKMESSAGE_MAXSIZE> buffer;

	NetworkMessage(void) {
		rdpos = 0;
		wrpos = 0;
	}

	NetworkMessage(const NetworkMessage &other){
		rdpos = 0;
		wrpos = 0;
		if(!other.isOverrun()){
			wrpos = other.getRemainingLength();
			memcpy(buffer.data(),
				other.getRemainingBuffer(),
				other.getRemainingLength());
		}
	}

	bool canRead(int n) const { return (rdpos + n) <= wrpos; }
	bool canAdd(int n) const { return (wrpos + n) <= (int)buffer.size(); }
	bool isOverrun() const { return rdpos > wrpos || wrpos > (int)buffer.size(); }

	uint8_t *getRemainingBuffer() {
		if(!isOverrun()){
			return &buffer[rdpos];
		}else{
			return &buffer[0];
		}
	}

	const uint8_t *getRemainingBuffer() const {
		if(!isOverrun()){
			return &buffer[rdpos];
		}else{
			return &buffer[0];
		}
	}

	int getRemainingLength() const {
		if(!isOverrun()){
			return wrpos - rdpos;
		}else{
			return 0;
		}
	}

	int getWrittenLength() const {
		if(!isOverrun()){
			return wrpos;
		}else{
			return 0;
		}
	}

	bool discardPadding(int padding) {
		if(padding > getRemainingLength()){
			return false;
		}

		wrpos -= padding;
		return true;
	}

	uint8_t peekByte(int offset = 0)
	{
		uint8_t result = 0;
		if (canRead(offset + 1)) {
			result = buffer[offset + rdpos];
		}
		return result;
	}

	template <typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>>
	T peek(int offset = 0)
	{
		T result = {};
		if (canRead(offset + sizeof(T))) {
			std::memcpy(&result, &buffer[rdpos + offset], sizeof(T));
		}
		return result;
	}

	uint8_t getByte()
	{
		uint8_t result = 0;
		if (canRead(1)) {
			result = buffer[rdpos];
		}
		rdpos += 1;
		return result;
	}

	template <typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>>
	T get()
	{
		T result = {};
		if (canRead(sizeof(T))) {
			std::memcpy(&result, &buffer[rdpos], sizeof(T));
		}
		rdpos += sizeof(T);
		return result;
	}

	void addByte(uint8_t value)
	{
		if(canAdd(1)){
			buffer[wrpos] = value;
		}
		wrpos += 1;
	}

	template <typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>>
	void add(T value)
	{
		if(canAdd(sizeof(T))){
			std::memcpy(&buffer[wrpos], &value, sizeof(T));
		}
		wrpos += sizeof(T);
	}

	Position getPosition();
	void addPosition(const Position& pos);
	std::string getString(int stringLen = 0);
	void addString(std::string_view value);
	void addBytes(const uint8_t* bytes, int size);
	void addDouble(double value, uint8_t precision = 2);
	void addItem(uint16_t id, uint8_t count);
	void addItem(const Item* item);
	void addItemId(uint16_t itemId);

	void dump(std::string_view name) const;
};

#endif // FS_NETWORKMESSAGE_H
