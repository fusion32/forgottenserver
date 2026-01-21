// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_OTPCH_H
#define FS_OTPCH_H

// NOTE(fusion): These defines need to come before system/library headers or they
// will have no effect.

#ifndef __FUNCTION__
#   define __FUNCTION__ __func__
#endif

#ifndef _CRT_SECURE_NO_WARNINGS
#   define _CRT_SECURE_NO_WARNINGS 1
#endif

#ifndef _USE_MATH_DEFINES
#   define _USE_MATH_DEFINES 1
#endif

#ifdef _WIN32
#   ifndef NOMINMAX
#       define NOMINMAX 1
#   endif
#   define WIN32_LEAN_AND_MEAN 1
#   ifdef _MSC_VER
#       ifdef NDEBUG
#           define _SECURE_SCL 0
#           define HAS_ITERATOR_DEBUGGING 0
#       endif
#       pragma warning(disable : 4127) // conditional expression is constant
#       pragma warning(disable : 4244) // 'argument' : conversion from 'type1' to 'type2', possible loss of data
#       pragma warning(disable : 4250) // 'class1' : inherits 'class2::member' via dominance
#       pragma warning(disable : 4267) // 'var' : conversion from 'size_t' to 'type', possible loss of data
#       pragma warning(disable : 4319) // '~': zero extending 'unsigned int' to 'lua_Number' of greater size
#       pragma warning(disable : 4351) // new behavior: elements of array will be default initialized
#       pragma warning(disable : 4458) // declaration hides class member
#   endif
#   ifndef _WIN32_WINNT
#       define _WIN32_WINNT 0x0602 // Windows 7
#   endif
#endif

#define BOOST_ASIO_NO_DEPRECATED 1
//#define OPENSSL_NO_DEPRECATED 1 // probably use EVP API if this is important?

// System headers required in headers should be included here.
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/variant.hpp>
#include <cassert>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <forward_list>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <mysql/mysql.h>
#include <optional>
#include <pugixml.hpp>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <valarray>
#include <variant>
#include <vector>

#if __has_include("luajit/lua.hpp")
#include <luajit/lua.hpp>
#else
#include <lua.hpp>
#endif

// NOTE(fusion): Common utility, definitions and macros.
#include "common.h"

#endif // FS_OTPCH_H
