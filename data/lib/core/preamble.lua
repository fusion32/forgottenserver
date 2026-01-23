math.randomseed(os.time())

function print(...)
    local message = ""
    for i, v in ipairs{...} do
        if i > 1 then
            message = message .. "    "
        end
        message = message .. tostring(v)
    end

    logInfo(message)
end

function pinfo(fmt, ...)
    local message = string.format(fmt, ...)
    logInfo(message)
end

function pwarn(fmt, ...)
    local message = string.format(fmt, ...)
    local callerInfo = debug.getinfo(2, "nSl")
    logWarn(callerInfo.name, callerInfo.source, callerInfo.currentline, message)
end

function perror(fmt, ...)
    local message = string.format(fmt, ...)
    local callerInfo = debug.getinfo(2, "nSl")
    logError(callerInfo.name, callerInfo.source, callerInfo.currentline, message)
end

function ptrace(fmt, ...)
    local message = string.format(fmt, ...)
    logError("", "", 1, debug.traceback(message))
end

