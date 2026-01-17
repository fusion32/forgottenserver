// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_OUTPUTMESSAGE_H
#define FS_OUTPUTMESSAGE_H

#include "networkmessage.h"
#include "tools.h"

class OutputMessage;
struct OutputMessageDeleter {
	void operator()(OutputMessage*) const;
};

using OutputMessage_ptr = std::unique_ptr<OutputMessage, OutputMessageDeleter>;
class OutputMessage : public NetworkMessage
{
public:
	OutputMessage_ptr next;
	int               start;

	OutputMessage() { reset(); }

	void reset(){
		// NOTE(fusion): We need to leave some room for packet headers and the
		// largest header is the one that goes with the Tibia packet. It should
		// have the layout below, which roughly explains why we have 8 bytes of
		// room for headers:
		//
		//	PLAINTEXT:
		//		0 .. 2 => Packet Size
		//		2 .. 6 => Checksum or Sequence Number
		//	ENCRYPTED:
		//		6 .. 8 => Payload Size
		//		8 ..   => Payload + Padding
		//
		start = 8;
		rdpos = start;
		wrpos = start;
	}

	uint8_t *getOutputBuffer() { return &buffer[start]; }
	const uint8_t *getOutputBuffer() const { return &buffer[start]; }
	int getOutputLength() const {
		assert(wrpos >= start);
		if(!isOverrun()){
			return wrpos - start;
		}else{
			return 0;
		}
	}

	template <typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>>
	void addHeader(T value)
	{
		assert(start >= (int)sizeof(T));
		start -= sizeof(T);
		std::memcpy(&buffer[start], &value, sizeof(T));
	}

	void append(const NetworkMessage& msg)
	{
		if(!msg.isOverrun()){
			addBytes(msg.buffer.data(), msg.wrpos);
		}
	}

	static OutputMessage_ptr make(void);
};

#endif // FS_OUTPUTMESSAGE_H
