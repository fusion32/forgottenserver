#ifndef __OTSERV_HTTP_H__
#define __OTSERV_HTTP_H__ 1

boost::asio::awaitable<void> HttpService(boost::asio::ip::tcp::endpoint endpoint);

#endif //__OTSERV_HTTP_H__
