// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "connection.h"

#include "configmanager.h"
#include "outputmessage.h"
#include "protocol.h"
#include "server.h"
#include "tasks.h"

Connection_ptr ConnectionManager::createConnection(boost::asio::io_context& io_context,
                                                   ConstServicePort_ptr servicePort)
{
	std::lock_guard<std::mutex> lockClass(connectionManagerLock);

	auto connection = std::make_shared<Connection>(io_context, servicePort);
	connections.insert(connection);
	return connection;
}

void ConnectionManager::releaseConnection(const Connection_ptr& connection)
{
	std::lock_guard<std::mutex> lockClass(connectionManagerLock);

	connections.erase(connection);
}

void ConnectionManager::closeAll()
{
	std::lock_guard<std::mutex> lockClass(connectionManagerLock);

	for (const auto& connection : connections) {
		try {
			boost::system::error_code error;
			connection->socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
			connection->socket.close(error);
		} catch (boost::system::system_error&) {
		}
	}
	connections.clear();
}

// Connection

Connection::Connection(boost::asio::io_context& io_context, ConstServicePort_ptr service_port) :
    readTimer(io_context),
    writeTimer(io_context),
    service_port(std::move(service_port)),
    socket(io_context),
    timeConnected(time(nullptr))
{}

void Connection::close(bool force)
{
	// any thread
	ConnectionManager::getInstance().releaseConnection(shared_from_this());

	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	connectionState = CONNECTION_STATE_DISCONNECTED;

	if (protocol) {
		g_dispatcher.addTask([protocol = protocol]() { protocol->release(); });
	}

	if (messageQueue.empty() || force) {
		closeSocket();
	} else {
		// will be closed by the destructor or onWriteOperation
	}
}

void Connection::closeSocket()
{
	if (socket.is_open()) {
		try {
			readTimer.cancel();
			writeTimer.cancel();
			boost::system::error_code error;
			socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
			socket.close(error);
		} catch (boost::system::system_error& e) {
			std::cout << "[Network error - Connection::closeSocket] " << e.what() << std::endl;
		}
	}
}

Connection::~Connection() { closeSocket(); }

void Connection::accept(Protocol_ptr protocol)
{
	this->protocol = protocol;
	g_dispatcher.addTask([=]() { protocol->onConnect(); });
	connectionState = CONNECTION_STATE_GAMEWORLD_AUTH;
	accept();
}

void Connection::accept()
{
	if (connectionState == CONNECTION_STATE_PENDING) {
		connectionState = CONNECTION_STATE_REQUEST_CHARLIST;
	}

	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);

	boost::system::error_code error;
	if (auto endpoint = socket.remote_endpoint(error); !error) {
		remoteAddress = endpoint.address();
	}

	try {
		readTimer.expires_after(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		readTimer.async_wait(
		    [thisPtr = std::weak_ptr<Connection>(shared_from_this())](const boost::system::error_code& error) {
			    Connection::handleTimeout(thisPtr, error);
		    });

		int bufferLength = 2;
		if(!receivedLastChar && receivedName && connectionState == CONNECTION_STATE_GAMEWORLD_AUTH){
			bufferLength = 1;
		}

		boost::asio::async_read(
		    socket, boost::asio::buffer(msg.buffer.data(), bufferLength),
		    [thisPtr = shared_from_this()](const boost::system::error_code& error, auto /*bytes_transferred*/) {
			    thisPtr->parseHeader(error);
		    });
	} catch (boost::system::system_error& e) {
		std::cout << "[Network error - Connection::accept] " << e.what() << std::endl;
		close(FORCE_CLOSE);
	}
}

void Connection::parseHeader(const boost::system::error_code& error)
{
	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	readTimer.cancel();

	if (error) {
		close(FORCE_CLOSE);
		return;
	} else if (connectionState == CONNECTION_STATE_DISCONNECTED) {
		return;
	}

	uint32_t timePassed = std::max<uint32_t>(1, (time(nullptr) - timeConnected) + 1);
	if ((++packetsSent / timePassed) > static_cast<uint32_t>(getNumber(ConfigManager::MAX_PACKETS_PER_SECOND))) {
		std::cout << getIP() << " disconnected for exceeding packet per second limit." << std::endl;
		close();
		return;
	}

	if (!receivedLastChar && connectionState == CONNECTION_STATE_GAMEWORLD_AUTH) {
		// TODO(fusion): This is probably not a good heuristic. If the first game
		// packet surpases 255 bytes (which would only require a 2048-bit RSA key)
		// then this suddenly starts to fail. Unless the client always begins with
		// the world name, which seems to be the case nowadays so...
		if (!receivedName && msg.buffer[1] == 0x00) {
			receivedLastChar = true;
		} else {
			if (!receivedName) {
				receivedName = true;

				accept();
				return;
			}

			if (msg.buffer[0] == 0x0A) {
				receivedLastChar = true;
			}

			accept();
			return;
		}
	}

	if (receivedLastChar && connectionState == CONNECTION_STATE_GAMEWORLD_AUTH) {
		connectionState = CONNECTION_STATE_GAME;
	}

	if (timePassed > 2) {
		timeConnected = time(nullptr);
		packetsSent = 0;
	}

	// TODO(fusion): It seems that with the latest protocol, this value is turned
	// into the number of XTEA blocks so we'd need to multiply by 8 and add the
	// usual unencrypted header bytes for the checksum or sequence number.
	//		=> packetLen = 4 + 8 * numBlocks.
	int packetLen = ((uint16_t)msg.buffer[0] << 0) | ((uint16_t)msg.buffer[1] << 8);
	if (packetLen <= 0 || packetLen > (int)msg.buffer.size()){
		close(FORCE_CLOSE);
		return;
	}

	try {
		readTimer.expires_after(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		readTimer.async_wait(
		    [thisPtr = std::weak_ptr<Connection>(shared_from_this())](const boost::system::error_code& error) {
			    Connection::handleTimeout(thisPtr, error);
		    });

		// Read packet content
		msg.rdpos = 0;
		msg.wrpos = packetLen;
		boost::asio::async_read(
		    socket, boost::asio::buffer(msg.buffer.data(), packetLen),
		    [thisPtr = shared_from_this()](const boost::system::error_code& error, auto /*bytes_transferred*/) {
			    thisPtr->parsePacket(error);
		    });
	} catch (boost::system::system_error& e) {
		std::cout << "[Network error - Connection::parseHeader] " << e.what() << std::endl;
		close(FORCE_CLOSE);
	}
}

void Connection::parsePacket(const boost::system::error_code& error)
{
	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	readTimer.cancel();

	if (error) {
		close(FORCE_CLOSE);
		return;
	} else if (connectionState == CONNECTION_STATE_DISCONNECTED) {
		return;
	}

	if (!receivedFirst) {
		receivedFirst = true;
		if(!protocol){
			protocol = service_port->make_protocol(msg, shared_from_this());
			if (!protocol) {
				close(FORCE_CLOSE);
				return;
			}
		}
		protocol->onRecvFirstMessage(msg);
	} else {
		protocol->onRecvMessage(msg); // Send the packet to the current protocol
	}

	try {
		readTimer.expires_after(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		readTimer.async_wait(
		    [thisPtr = std::weak_ptr<Connection>(shared_from_this())](const boost::system::error_code& error) {
			    Connection::handleTimeout(thisPtr, error);
		    });

		// Wait to the next packet
		boost::asio::async_read(
		    socket, boost::asio::buffer(msg.buffer.data(), 2),
		    [thisPtr = shared_from_this()](const boost::system::error_code& error, auto /*bytes_transferred*/) {
			    thisPtr->parseHeader(error);
		    });
	} catch (boost::system::system_error& e) {
		std::cout << "[Network error - Connection::parsePacket] " << e.what() << std::endl;
		close(FORCE_CLOSE);
	}
}

void Connection::send(const OutputMessage_ptr& msg)
{
	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	if (connectionState == CONNECTION_STATE_DISCONNECTED) {
		return;
	}

	bool noPendingWrite = messageQueue.empty();
	messageQueue.emplace_back(msg);
	if (noPendingWrite) {
		try {
			boost::asio::post(socket.get_executor(),
			                  [thisPtr = shared_from_this(), msg] { thisPtr->internalSend(msg); });
		} catch (const boost::system::system_error& e) {
			std::cout << "[Network error - Connection::send] " << e.what() << std::endl;
			messageQueue.clear();
			close(FORCE_CLOSE);
		}
	}
}

void Connection::internalSend(const OutputMessage_ptr& msg)
{
	if(!protocol->wrapPacket(msg)){
		std::cout << "Connection::internalSend: failed to wrap packet to "<< remoteAddress << std::endl;
		close(FORCE_CLOSE);
		return;
	}

	try {
		writeTimer.expires_after(std::chrono::seconds(CONNECTION_WRITE_TIMEOUT));
		writeTimer.async_wait(
		    [thisPtr = std::weak_ptr<Connection>(shared_from_this())](const boost::system::error_code& error) {
			    Connection::handleTimeout(thisPtr, error);
		    });

		boost::asio::async_write(
		    socket, boost::asio::buffer(msg->getOutputBuffer(), msg->getOutputLength()),
		    [thisPtr = shared_from_this()](const boost::system::error_code& error, auto /*bytes_transferred*/) {
			    thisPtr->onWriteOperation(error);
		    });
	} catch (boost::system::system_error& e) {
		std::cout << "[Network error - Connection::internalSend] " << e.what() << std::endl;
		close(FORCE_CLOSE);
	}
}

void Connection::onWriteOperation(const boost::system::error_code& error)
{
	std::lock_guard<std::recursive_mutex> lockClass(connectionLock);
	writeTimer.cancel();
	messageQueue.pop_front();

	if (error) {
		messageQueue.clear();
		close(FORCE_CLOSE);
		return;
	}

	if (!messageQueue.empty()) {
		internalSend(messageQueue.front());
	} else if (connectionState == CONNECTION_STATE_DISCONNECTED) {
		closeSocket();
	}
}

void Connection::handleTimeout(ConnectionWeak_ptr connectionWeak, const boost::system::error_code& error)
{
	if (error == boost::asio::error::operation_aborted) {
		// The timer has been cancelled manually
		return;
	}

	if (auto connection = connectionWeak.lock()) {
		connection->close(FORCE_CLOSE);
	}
}
