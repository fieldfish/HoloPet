#pragma once
/**
 * logger.hpp — 轻量级日志 (V1 简化版)
 *
 * 级别: INFO / WARN / ERROR
 * 格式: [LEVEL] [Module] message
 */

#include <iostream>
#include <sstream>
#include <mutex>

namespace holopet {

enum class LogLevel { Info = 0, Warn = 1, Error = 2 };

class Logger {
public:
    static Logger& instance() {
        static Logger log;
        return log;
    }

    void setLevel(LogLevel lv) { level_ = lv; }

    struct Stream {
        Stream(Logger& log, LogLevel lv, const char* mod, const char* tag)
            : logger(log), level(lv), module(mod), tag_(tag) {}
        ~Stream() { logger.write(level, module, tag_, ss.str()); }

        template<typename T>
        Stream& operator<<(const T& val) { ss << val; return *this; }

        Logger& logger;
        LogLevel level;
        const char* module;
        const char* tag_;
        std::ostringstream ss;
    };

    Stream info(const char* mod)  { return {*this, LogLevel::Info,  mod, "INFO" }; }
    Stream warn(const char* mod)  { return {*this, LogLevel::Warn,  mod, "WARN" }; }
    Stream error(const char* mod) { return {*this, LogLevel::Error, mod, "ERROR"}; }

private:
    LogLevel level_ = LogLevel::Info;
    std::mutex mtx_;

    Logger() = default;

    void write(LogLevel lv, const char* mod, const char* tag, const std::string& msg) {
        if (lv < level_) return;
        std::lock_guard lock(mtx_);
        std::cout << "[" << tag << "] [" << mod << "] " << msg << '\n';
        std::cout.flush();   // V6: 管道/重定向下即时可见 (含被终止场景)
    }
};

#define LOG_INFO(mod)  holopet::Logger::instance().info(mod)
#define LOG_WARN(mod)  holopet::Logger::instance().warn(mod)
#define LOG_ERROR(mod) holopet::Logger::instance().error(mod)

} // namespace holopet
