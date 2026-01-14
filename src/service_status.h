#ifndef __OTSERV_STATUS_H__
#define __OTSERV_STATUS_H__ 1

void SetStatusString(std::string_view s);
size_t GetStatusString(uint8_t *buffer, size_t bufferCapacity);

boost::asio::awaitable<void> StatusService(boost::asio::ip::tcp::endpoint endpoint,
                            std::chrono::steady_clock::duration minRequestInterval);

#endif //__OTSERV_STATUS_H__

