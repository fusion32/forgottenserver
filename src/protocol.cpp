// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "protocol.h"

#include "outputmessage.h"
#include "rsa.h"
#include "xtea.h"

namespace {

bool XTEA_encrypt(OutputMessage& msg, const xtea::round_keys& key)
{
	int xteaLen = msg.getOutputLength();
	while(xteaLen % 8 != 0){
		msg.addByte((uint8_t)rand());
		xteaLen += 1;
	}

	if(msg.isOverrun()){
		return false;
	}

	xtea::encrypt(msg.getOutputBuffer(), xteaLen, key);
	return true;
}

bool XTEA_decrypt(NetworkMessage& msg, const xtea::round_keys& key)
{
	if(msg.isOverrun()){
		return false;
	}

	int xteaLen = msg.getRemainingLength();
	if(xteaLen % 8 != 0){
		return false;
	}

	xtea::decrypt(msg.getRemainingBuffer(), xteaLen, key);
	int payloadLen = msg.get<uint16_t>();
	int padding = xteaLen - (payloadLen + 2);
	if(padding < 0){
		return false;
	}

	msg.discardPadding(padding);
	return true;
}

} // namespace

Protocol::Protocol(Connection_ptr connection) : connection(connection)
{
	if (deflateInit2(&zstream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		std::cout << "ZLIB initialization error: " << (zstream.msg ? zstream.msg : "unknown") << std::endl;
	}
}

Protocol::~Protocol()
{
	const auto zlibEndResult = deflateEnd(&zstream);
	if (zlibEndResult == Z_DATA_ERROR) {
		std::cout << "ZLIB discarded pending output or unprocessed input while cleaning up stream state" << std::endl;
	} else if (zlibEndResult == Z_STREAM_ERROR) {
		std::cout << "ZLIB encountered an error while cleaning up stream state" << std::endl;
	}
}

bool Protocol::wrapPacket(const OutputMessage_ptr& msg)
{
	if(msg->isOverrun()){
		return false;
	}

	if (!rawMessages) {
		// NOTE(fusion): Each `addHeader` will change the output buffer length.
		if(encryptionEnabled){
			uint32_t checksum = 0;
			if(checksumMode == CHECKSUM_ADLER){
				checksum = adlerChecksum(msg->getOutputBuffer(), msg->getOutputLength());
			}else if(checksumMode == CHECKSUM_SEQUENCE){
				checksum = getNextSequenceId();
				if(msg->getOutputLength() >= 128 && deflateMessage(*msg)){
					checksum |= 0x80000000;
				}
			}

			msg->addHeader<uint16_t>(msg->getOutputLength());
			if(!XTEA_encrypt(*msg, key)){
				return false;
			}

			msg->addHeader<uint32_t>(checksum);
		}
		msg->addHeader<uint16_t>(msg->getOutputLength());
	}

	return true;
}

void Protocol::onRecvMessage(NetworkMessage& msg)
{
	if(encryptionEnabled){
		if(checksumMode != CHECKSUM_DISABLED){
			// TODO(fusion): Actually verify checksum/sequence?
			msg.get<uint32_t>();
		}

		if(!XTEA_decrypt(msg, key)){
			std::cout << "Protocol::onRecvMessage: failed to decrypt XTEA message..." << std::endl;
			return;
		}
	}

	parsePacket(msg);
}

OutputMessage_ptr Protocol::getOutputBuffer(int32_t size)
{
	// dispatcher thread
	if (!outputBuffer) {
		outputBuffer = tfs::net::make_output_message();
	} else if (!outputBuffer->canAdd(size)) {
		send(outputBuffer);
		outputBuffer = tfs::net::make_output_message();
	}
	return outputBuffer;
}

bool Protocol::RSA_decrypt(NetworkMessage& msg)
{
	if (msg.getRemainingLength() < RSA_BUFFER_LENGTH) {
		return false;
	}

	tfs::rsa::decrypt(msg.getRemainingBuffer(), RSA_BUFFER_LENGTH);
	return msg.getByte() == 0;
}

bool Protocol::deflateMessage(OutputMessage& msg)
{
	uint8_t *outputBuffer = msg.getOutputBuffer();
	int uncompressedSize  = msg.getOutputLength();
	if(uncompressedSize <= 0){
		std::cout << "Protocol::deflateMessage: trying to compress empty message..." << std::endl;
		return false;
	}

	std::array<uint8_t, NETWORKMESSAGE_MAXSIZE> buffer;
	zstream.next_in = outputBuffer;
	zstream.avail_in = uncompressedSize;
	zstream.next_out = buffer.data();
	zstream.avail_out = buffer.size();

	int ret = deflate(&zstream, Z_FINISH);
	if (ret != Z_STREAM_END) {
		// NOTE(fusion): We can only get Z_OK here if the supplied buffer was
		// too small.
		if(ret != Z_OK){
			std::cout << "Protocol::deflateMessage: failed to compress message: "
						<< (zstream.msg ? zstream.msg : "unknown error")
						<< std::endl;
		}
		return false;
	}

	int compressedSize = zstream.total_out;
	deflateReset(&zstream);

	// NOTE(fusion): It is very unlikely but compressed data may end up being
	// larger if the uncompressed data has a lot of entropy (e.g. random or
	// already compressed data).
	if(compressedSize >= uncompressedSize){
		return false;
	}

	std::memcpy(outputBuffer, buffer.data(), compressedSize);
	msg.wrpos -= (uncompressedSize - compressedSize);
	return true;
}

Connection::Address Protocol::getIP() const
{
	if (auto connection = getConnection()) {
		return connection->getIP();
	}

	return {};
}
