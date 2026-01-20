#include "otpch.h"
#include "common.h"

struct tm GetLocalTime(time_t t){
    struct tm result;
#if COMPILER_MSVC
    localtime_s(&result, &t);
#else
    localtime_r(&t, &result);
#endif
    return result;
}

struct tm GetGMTime(time_t t){
    struct tm result;
#if COMPILER_MSVC
    gmtime_s(&result, &t);
#else
    gmtime_r(&t, &result);
#endif
    return result;
}

void PrintBuffer(std::string_view name, const uint8_t *data, int len){
    constexpr int bytesPerLine = 16;
    int lines = (len + bytesPerLine - 1) / bytesPerLine;
    fmt::print("{} (len={}, lines={}):\n", name, len, lines);
    for(int i = 0; i < lines; i += 1){
        int offset = bytesPerLine * i;
        int count  = bytesPerLine;
        if((offset + count) > len){
            count = len - offset;
        }

        fmt::print("{:8d} | ", offset);
        for(int j = 0; j < bytesPerLine; j += 1){
            if(j > 0) fmt::print(" ");
            if(j < count){
                fmt::print("{:02X}", data[offset+j]);
            }else{
                fmt::print("  ");
            }
        }

        fmt::print(" | ");

        for(int j = 0; j < count; j += 1){
            if(isprint(data[offset+j])){
                fmt::print("{:c}", data[offset+j]);
            }else{
                fmt::print(".");
            }
        }
        fmt::print("\n");
    }
}


