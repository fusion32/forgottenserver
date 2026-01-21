#ifndef __OTSERV_COMMON_H__
#define __OTSERV_COMMON_H__ 1

#define STATUS_SERVER_NAME          "The Forgotten Server"
#define STATUS_SERVER_VERSION       "1.7"
#define STATUS_SERVER_DEVELOPERS    "The Forgotten Server Team"
#define CLIENT_VERSION_STR          "15.20"
#define CLIENT_VERSION_MIN          1520
#define CLIENT_VERSION_MAX          1521
#define AUTHENTICATOR_DIGITS        6
#define AUTHENTICATOR_PERIOD        30

#if defined(_WIN32)
#   define OS_WINDOWS 1
#elif defined(__linux__) || defined(__gnu_linux__)
#   define OS_LINUX 1
#else
#   error "Unknown operating system."
#endif

#if defined(_MSC_VER)
#   define COMPILER_MSVC 1
#elif defined(__GNUC__)
#   define COMPILER_GCC 1
#elif defined(__clang__)
#   define COMPILER_CLANG 1
#else
#   error "Unknown compiler."
#endif

// TODO(fusion): Add other settings if running the server on anything other than
// x86 becomes a thing.
#if defined(__amd64__) || defined(_M_X64)
#   define ARCH_X64 1
#   define ARCH_NAME "x64"
#elif defined(__i386__) || defined(_M_IX86) || defined(_X86_)
#   define ARCH_X86 1
#   define ARCH_NAME "x86"
#else
#   error "Unknown arch."
#endif

#define NARRAY(arr) (int)(sizeof(arr)/sizeof(arr[0]))

struct tm GetLocalTime(time_t t);
struct tm GetGMTime(time_t t);
void PrintBuffer(std::string_view name, const uint8_t *data, int len);

// Custom Formatters
//==============================================================================
template<> struct fmt::formatter<boost::asio::ip::address> : fmt::ostream_formatter {};
template<> struct fmt::formatter<boost::asio::ip::tcp::endpoint> : fmt::ostream_formatter {};

// Logging
//==============================================================================
#define LOG(...) \
    LogAdd({}, "INFO", __VA_ARGS__)

#define LOG_WARN(...) \
    LogAddVerbose((fg(fmt::color::gold) | fmt::emphasis::bold),\
            "WARN", __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_ERR(...) \
    LogAddVerbose((fg(fmt::color::crimson) | fmt::emphasis::bold), \
            "ERROR", __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)

// NOTE(fusion): Spliting log output to a secondary file doesn't make sense in
// a server context where you're in control of how executables are run and can
// easily redirect output to an external file. For that reason all log entries
// should be written to `stdout` only.
struct FileInserter{
private:
    FILE *fp;

public:
    // NOTE(fusion): Make sure the file is locked while inserting, so we don't
    // end up with a letter soup when multiple threads are logging to the same
    // file. Also, flush at the end to prevent losing information in case of
    // unexpected program termination.

#if OS_WINDOWS
    FileInserter(FILE *fp_) : fp(fp_) {
        if(fp){
            _lock_file(fp);
        }
    }

    ~FileInserter(void){
        if(fp){
            _fflush_nolock(fp);
            _unlock_file(fp);
        }
    }

    FileInserter &operator=(char c){
        if(fp){
            _putc_nolock(c, fp);
        }
        return *this;
    }
#else
    FileInserter(FILE *fp_) : fp(fp_) {
        if(fp){
            flockfile(fp);
        }
    }

    ~FileInserter(void){
        if(fp){
            fflush_unlocked(fp);
            funlockfile(fp);
        }
    }

    FileInserter &operator=(char c){
        if(fp){
            putc_unlocked(c, fp);
        }
        return *this;
    }
#endif

    FileInserter &operator*(void) { return *this; }
    FileInserter &operator++(void) { return *this; }
    FileInserter &operator++(int) { return *this; }
};

// NOTE(fusion): Enable enums to be formated as their underlying type.
template<typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
auto format_as(T t) {
    return fmt::underlying(t);
}

template<typename ...Args>
inline void LogAdd(fmt::text_style style, const char *prefix,
            fmt::format_string<Args...> fmt, Args &&...args) {
    FileInserter output(stdout);
    fmt::format_to(output, style, "[{}] [{}] ", GetLocalTime(time(NULL)), prefix);
    fmt::format_to(output, style, fmt, std::forward<Args>(args)...);
    fmt::format_to(output, style, "\n");
}

template<typename ...Args>
inline void LogAddVerbose(fmt::text_style style, const char *prefix,
            const char *function, const char *file, int line,
            fmt::format_string<Args...> fmt, Args &&...args) {
    FileInserter output(stdout);
    fmt::format_to(output, style, "[{}] [{}] {}:{}: {}: ",
            GetLocalTime(time(NULL)), prefix, file, line, function);
    fmt::format_to(output, style, fmt, std::forward<Args>(args)...);
    fmt::format_to(output, style, "\n");
}

#endif //__OTSERV_COMMON_H__

