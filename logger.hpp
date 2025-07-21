#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>
#include <typeinfo>
#include <iostream>
#include <cassert>

namespace util {

// Log levels with explicit numeric values for easy comparison
enum class LogLevel : int {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5
};

// Format types for sinks
enum class FormatType {
    TEXT,
    JSON,
    CSV
};

// Sampling strategies
enum class SamplingType {
    NONE,
    RANDOM,     // Sample X% randomly
    EVERY_NTH   // Sample every Nth message
};

// Level metadata for formatting
struct LevelMetadata {
    std::string_view name;
    std::string_view color_code;
    int priority;
};

inline const LevelMetadata& get_level_metadata(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::TRACE: {
            static const LevelMetadata metadata{"TRACE", "\x1b[90m", 0};
            return metadata;
        }
        case LogLevel::DEBUG: {
            static const LevelMetadata metadata{"DEBUG", "\x1b[36m", 1};
            return metadata;
        }
        case LogLevel::INFO: {
            static const LevelMetadata metadata{"INFO", "\x1b[32m", 2};
            return metadata;
        }
        case LogLevel::WARN: {
            static const LevelMetadata metadata{"WARN", "\x1b[33m", 3};
            return metadata;
        }
        case LogLevel::ERROR: {
            static const LevelMetadata metadata{"ERROR", "\x1b[31m", 4};
            return metadata;
        }
        case LogLevel::FATAL: {
            static const LevelMetadata metadata{"FATAL", "\x1b[35m", 5};
            return metadata;
        }
    }
}

inline constexpr std::string_view color_reset() noexcept { 
    return "\x1b[0m"; 
}

// Sampling configuration
struct SamplingConfig {
    SamplingType type = SamplingType::NONE;
    double rate = 1.0;           // For random sampling (0.0-1.0)
    uint32_t every_nth = 1;      // For every-nth sampling
    LogLevel min_level = LogLevel::TRACE;  // Apply only to this level and above
    
    SamplingConfig() = default;
    SamplingConfig(SamplingType t, double r, LogLevel lvl = LogLevel::TRACE) 
        : type(t), rate(r), min_level(lvl) {}
    SamplingConfig(SamplingType t, uint32_t n, LogLevel lvl = LogLevel::TRACE) 
        : type(t), every_nth(n), min_level(lvl) {}
};

// Typed field value that can hold different types
using FieldValue = std::variant<
    int, int64_t, uint32_t, uint64_t,
    float, double, 
    bool, 
    std::string, 
    std::string_view
>;

// Helper to convert FieldValue to string for TEXT format
inline std::string field_value_to_string(const FieldValue& value) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return std::string(v);
        } else if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_arithmetic_v<T>) {
            return std::to_string(v);
        } else {
            return "unknown_type";
        }
    }, value);
}

// Helper to convert FieldValue to JSON string
inline std::string field_value_to_json(const FieldValue& value) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return "\"" + v + "\"";  // TODO: Add proper JSON escaping
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return "\"" + std::string(v) + "\"";  // TODO: Add proper JSON escaping
        } else if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_arithmetic_v<T>) {
            return std::to_string(v);
        } else {
            return "\"unknown_type\"";
        }
    }, value);
}

// Log record containing all information for one log event
struct LogRecord {
    LogLevel level;
    std::variant<std::string, std::string_view> message; // Zero-copy optimization
    std::map<std::string, std::string> tags;
    std::map<std::string, FieldValue> fields;  // NEW: Typed fields
    std::chrono::system_clock::time_point timestamp;
    std::thread::id thread_id;
    bool use_json = false;
    SamplingConfig sampling;
    
    LogRecord(LogLevel lvl, std::string msg) 
        : level(lvl)
        , message(std::move(msg))
        , timestamp(std::chrono::system_clock::now())
        , thread_id(std::this_thread::get_id())
    {}
    
    LogRecord(LogLevel lvl, std::string_view msg) 
        : level(lvl)
        , message(msg)
        , timestamp(std::chrono::system_clock::now())
        , thread_id(std::this_thread::get_id())
    {}
    
    // Get message as string (promotes string_view to string if needed)
    std::string get_message_string() const {
        return std::visit([](const auto& msg) -> std::string {
            return std::string(msg);
        }, message);
    }
    
    // TODO: Add LogRecord pooling for better performance
    // TODO: Add pattern-based formatting support
};

using LogSink = std::function<void(const std::string&)>;

// Sink with format specification
struct FormattedSink {
    std::function<void(const std::string&)> sink_fn;
    FormatType format_type;
    
    FormattedSink(std::function<void(const std::string&)> fn, FormatType fmt)
        : sink_fn(std::move(fn)), format_type(fmt) {}
};

// Forward declaration for template
template<LogLevel CompileTimeMinLevel>
class Logger;

template<LogLevel CompileTimeMinLevel>
class LogEntryBuilder;

// RAII Performance Profiler
template<LogLevel CompileTimeMinLevel>
class ProfileTimer {
public:
    ProfileTimer(Logger<CompileTimeMinLevel>* logger, LogLevel level, std::string operation)
        : logger_(logger)
        , level_(level)
        , operation_(std::move(operation))
        , start_(std::chrono::steady_clock::now())
    {}
    
    // Support chaining like normal log entries - FIXED: return rvalue reference for chaining
    ProfileTimer&& tag(const std::string& key, const std::string& value) && {
        tags_[key] = value;
        return std::move(*this);
    }
    
    template<typename T>
    ProfileTimer&& field(const std::string& key, T&& value) && {
        fields_[key] = FieldValue(std::forward<T>(value));
        return std::move(*this);
    }
    
    // Lvalue versions for when ProfileTimer is stored in a variable
    ProfileTimer& tag(const std::string& key, const std::string& value) & {
        tags_[key] = value;
        return *this;
    }
    
    template<typename T>
    ProfileTimer& field(const std::string& key, T&& value) & {
        fields_[key] = FieldValue(std::forward<T>(value));
        return *this;
    }
    
    ~ProfileTimer() {
        if (logger_ == nullptr) return;
        
        auto duration = std::chrono::steady_clock::now() - start_;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
        
        // Create log entry with profiling info
        auto entry = logger_->create_log_entry_direct(level_, 
            "Operation '{}' completed in {}ms", operation_, ms.count());
        
        // Apply accumulated tags and fields
        for (const auto& [k, v] : tags_) {
            entry.tag(k, v);
        }
        for (const auto& [k, v] : fields_) {
            entry.field(k, v);
        }
        
        // Add automatic profiling fields
        entry.field("duration_ms", static_cast<int64_t>(ms.count()))
             .field("operation_name", operation_)
             .tag("profiler", "auto");
    }
    
    // Non-copyable, movable
    ProfileTimer(const ProfileTimer&) = delete;
    ProfileTimer& operator=(const ProfileTimer&) = delete;
    ProfileTimer(ProfileTimer&& other) noexcept
        : logger_(other.logger_)
        , level_(other.level_)
        , operation_(std::move(other.operation_))
        , start_(other.start_)
        , tags_(std::move(other.tags_))
        , fields_(std::move(other.fields_))
    {
        other.logger_ = nullptr;  // Prevent double logging
    }
    ProfileTimer& operator=(ProfileTimer&&) = delete;

private:
    Logger<CompileTimeMinLevel>* logger_;
    LogLevel level_;
    std::string operation_;
    std::chrono::steady_clock::time_point start_;
    std::map<std::string, std::string> tags_;
    std::map<std::string, FieldValue> fields_;
};

// Scoped tag manager (RAII)
template<LogLevel CompileTimeMinLevel>
class ScopedTags {
public:
    ScopedTags(Logger<CompileTimeMinLevel>* logger, std::map<std::string, std::string> tags)
        : logger_(logger), tags_(std::move(tags)) {
        logger_->push_scoped_tags(tags_);
    }
    
    ~ScopedTags() {
        if (logger_) {
            logger_->pop_scoped_tags(tags_);
        }
    }
    
    // Non-copyable, movable
    ScopedTags(const ScopedTags&) = delete;
    ScopedTags& operator=(const ScopedTags&) = delete;
    ScopedTags(ScopedTags&& other) noexcept 
        : logger_(other.logger_), tags_(std::move(other.tags_)) {
        other.logger_ = nullptr;
    }
    ScopedTags& operator=(ScopedTags&&) = delete;

private:
    Logger<CompileTimeMinLevel>* logger_;
    std::map<std::string, std::string> tags_;
};

// Builder pattern for constructing log entries with optional tags and fields
template<LogLevel CompileTimeMinLevel>
class LogEntryBuilder {
public:
    LogEntryBuilder(Logger<CompileTimeMinLevel>* logger, LogLevel level, std::string message) 
        : logger_(logger)
        , record_(level, std::move(message))
    {}
    
    // Add a string tag to this log entry
    LogEntryBuilder& tag(const std::string& key, const std::string& value) {
        record_.tags.emplace(key, value);
        return *this;
    }
    
    // Add a typed field to this log entry
    template<typename T>
    LogEntryBuilder& field(const std::string& key, T&& value) {
        record_.fields.emplace(key, FieldValue(std::forward<T>(value)));
        return *this;
    }
    
    // SIMPLIFIED: Exception integration - separate overloads without SFINAE
    LogEntryBuilder& exception(const std::exception& e) {
        try {
            return field("exception_type", std::string(typeid(e).name()))
                   .field("exception_message", std::string(e.what()))
                   .tag("has_exception", "true");
        } catch (...) {
            return field("exception_message", std::string("Exception details unavailable"))
                   .tag("exception_status", "serialization_failed");
        }
    }
    
    LogEntryBuilder& exception(const std::exception* e) {
        if (e != nullptr) {
            return exception(*e);
        }
        return field("exception", std::string("nullptr"))
               .tag("exception_status", "null_pointer");
    }
    
    LogEntryBuilder& exception(std::nullptr_t) {
        return field("exception", std::string("nullptr"))
               .tag("exception_status", "explicit_null");
    }
    
    // For derived exception types
    template<typename ExceptionType>
    LogEntryBuilder& exception(const ExceptionType& e, 
                              std::enable_if_t<std::is_base_of_v<std::exception, ExceptionType>>* = nullptr) {
        return exception(static_cast<const std::exception&>(e));
    }
    
    // Force this message to be formatted as JSON
    LogEntryBuilder& json() {
        record_.use_json = true;
        return *this;
    }
    
    // Add random sampling to this message
    LogEntryBuilder& sample(double rate, LogLevel min_level = LogLevel::TRACE) {
        record_.sampling = SamplingConfig(SamplingType::RANDOM, rate, min_level);
        return *this;
    }
    
    // Add every-nth sampling to this message
    LogEntryBuilder& every_nth(uint32_t n, LogLevel min_level = LogLevel::TRACE) {
        record_.sampling = SamplingConfig(SamplingType::EVERY_NTH, n, min_level);
        return *this;
    }
    
    // Destructor automatically emits the log record
    ~LogEntryBuilder() {
        if (logger_ != nullptr) {
            logger_->enqueue_log_record(std::move(record_));
        }
    }
    
    // Non-copyable, movable
    LogEntryBuilder(const LogEntryBuilder&) = delete;
    LogEntryBuilder& operator=(const LogEntryBuilder&) = delete;
    LogEntryBuilder(LogEntryBuilder&&) = default;
    LogEntryBuilder& operator=(LogEntryBuilder&&) = default;

private:
    Logger<CompileTimeMinLevel>* logger_;
    LogRecord record_;
};

// Dummy builder for compile-time filtered out log levels
class DummyLogEntryBuilder {
public:
    template<typename... Args>
    DummyLogEntryBuilder& tag(Args&&...) noexcept { return *this; }
    template<typename... Args>
    DummyLogEntryBuilder& field(Args&&...) noexcept { return *this; }
    template<typename... Args>
    DummyLogEntryBuilder& exception(Args&&...) noexcept { return *this; }
    DummyLogEntryBuilder& json() noexcept { return *this; }
    DummyLogEntryBuilder& sample(double, LogLevel = LogLevel::TRACE) noexcept { return *this; }
    DummyLogEntryBuilder& every_nth(uint32_t, LogLevel = LogLevel::TRACE) noexcept { return *this; }
};

// Main Logger class with async processing and compile-time level filtering
template<LogLevel CompileTimeMinLevel = LogLevel::INFO>
class Logger {
public:
    using ScopedTagsType = ScopedTags<CompileTimeMinLevel>;
    using ProfileTimerType = ProfileTimer<CompileTimeMinLevel>;
    
    explicit Logger(std::string name) 
        : name_(std::move(name))
        , runtime_min_level_(CompileTimeMinLevel)
        , shutdown_requested_(false)
        , sampling_counter_(0)
        , rng_(std::random_device{}())
        , pattern_("[{timestamp}] {color}{level}{color_reset} [{logger}] {message}{tags}") // Default pattern
    {
        // Start background worker thread
        worker_thread_ = std::thread([this] { process_log_queue(); });
        
        // Add default console sink
        add_default_sink();
    }
    
    ~Logger() {
        shutdown();
    }
    
    // Non-copyable, movable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = default;
    Logger& operator=(Logger&&) = default;

    // Configuration methods (fluent interface)
    Logger& set_level(LogLevel level) {
        runtime_min_level_.store(level, std::memory_order_relaxed);
        return *this;
    }
    
    // Pattern-based formatting
    Logger& set_pattern(const std::string& pattern) {
        std::lock_guard<std::mutex> lock(pattern_mutex_);
        pattern_ = pattern;
        return *this;
    }
    
    // Static tags applied to all messages
    Logger& tag(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(static_tags_mutex_);
        static_tags_[key] = value;
        return *this;
    }
    
    // Global scope tags (persistent across all log calls)
    Logger& set_global_scope(std::map<std::string, std::string> global_scope) {
        std::lock_guard<std::mutex> lock(static_tags_mutex_);
        global_scope_ = std::move(global_scope);
        return *this;
    }
    
    Logger& add_global_scope_tag(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(static_tags_mutex_);
        global_scope_[key] = value;
        return *this;
    }
    
    // Create scoped tags (RAII)
    ScopedTagsType with_scope(std::map<std::string, std::string> tags) {
        return ScopedTagsType(this, std::move(tags));
    }
    
    // Create performance profiler (RAII)
    ProfileTimerType profile(const std::string& operation, LogLevel level = LogLevel::INFO) {
        return ProfileTimerType(this, level, operation);
    }
    
    // Sink management with format specification
    Logger& add_sink(LogSink sink, FormatType format = FormatType::TEXT) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        sinks_.emplace_back(std::move(sink), format);
        return *this;
    }
    
    // Global sampling configuration
    Logger& set_sampling(SamplingConfig config) {
        global_sampling_ = config;
        return *this;
    }
    
    // TODO: Add content-based filtering
    // TODO: Add adaptive sampling based on load
    
    // Internal methods for scoped tags
    void push_scoped_tags(const std::map<std::string, std::string>& tags) {
        std::lock_guard<std::mutex> lock(scoped_tags_mutex_);
        for (const auto& [key, value] : tags) {
            scoped_tags_stack_.push_back({key, value});
        }
    }
    
    void pop_scoped_tags(const std::map<std::string, std::string>& tags) {
        std::lock_guard<std::mutex> lock(scoped_tags_mutex_);
        for (auto it = scoped_tags_stack_.rbegin(); it != scoped_tags_stack_.rend();) {
            bool found = false;
            for (const auto& [key, value] : tags) {
                if (it->first == key && it->second == value) {
                    it = std::make_reverse_iterator(scoped_tags_stack_.erase(std::next(it).base()));
                    found = true;
                    break;
                }
            }
            if (!found) ++it;
        }
    }
    
    // Internal method for ProfileTimer
    LogEntryBuilder<CompileTimeMinLevel> create_log_entry_direct(LogLevel level, const std::string& message) {
        return LogEntryBuilder<CompileTimeMinLevel>(this, level, message);
    }
    
    template<typename... Args>
    LogEntryBuilder<CompileTimeMinLevel> create_log_entry_direct(LogLevel level, std::string_view fmt, Args&&... args) {
        std::string formatted_message;
        try {
            formatted_message = std::vformat(fmt, std::make_format_args(args...));
        } catch (const std::exception&) {
            formatted_message = std::string(fmt);
        }
        return LogEntryBuilder<CompileTimeMinLevel>(this, level, std::move(formatted_message));
    }
    
    // Internal method called by LogEntryBuilder destructor
    void enqueue_log_record(LogRecord record) {
        // Check sampling first
        if (!should_sample(record)) {
            return;
        }
        
        // Add all types of tags to the record in correct priority order
        // Priority: per-message (highest) > scoped > static > global (lowest)
        std::map<std::string, std::string> final_tags;
        
        {
            std::lock_guard<std::mutex> lock(static_tags_mutex_);
            
            // Start with global scope tags (lowest priority)
            for (const auto& [key, value] : global_scope_) {
                final_tags[key] = value;
            }
            
            // Add static tags (overwrite global if same key)
            for (const auto& [key, value] : static_tags_) {
                final_tags[key] = value;
            }
        }
        
        // Add scoped tags (overwrite static/global if same key)
        {
            std::lock_guard<std::mutex> lock(scoped_tags_mutex_);
            for (const auto& [key, value] : scoped_tags_stack_) {
                final_tags[key] = value;
            }
        }
        
        // Finally, add per-message tags (highest priority - overwrite everything)
        for (const auto& [key, value] : record.tags) {
            final_tags[key] = value;
        }
        
        // Update record with final tag hierarchy
        record.tags = std::move(final_tags);
        
        // Enqueue for async processing
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            log_queue_.push(std::move(record));
        }
        queue_condition_.notify_one();
    }
    
    // Logging methods with compile-time level filtering
    template<typename... Args>
    auto trace(std::string_view fmt, Args&&... args) {
        if constexpr (static_cast<int>(LogLevel::TRACE) < static_cast<int>(CompileTimeMinLevel)) {
            return DummyLogEntryBuilder{};
        } else {
            return create_log_entry(LogLevel::TRACE, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    auto debug(std::string_view fmt, Args&&... args) {
        if constexpr (static_cast<int>(LogLevel::DEBUG) < static_cast<int>(CompileTimeMinLevel)) {
            return DummyLogEntryBuilder{};
        } else {
            return create_log_entry(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    auto info(std::string_view fmt, Args&&... args) {
        if constexpr (static_cast<int>(LogLevel::INFO) < static_cast<int>(CompileTimeMinLevel)) {
            return DummyLogEntryBuilder{};
        } else {
            return create_log_entry(LogLevel::INFO, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    auto warn(std::string_view fmt, Args&&... args) {
        if constexpr (static_cast<int>(LogLevel::WARN) < static_cast<int>(CompileTimeMinLevel)) {
            return DummyLogEntryBuilder{};
        } else {
            return create_log_entry(LogLevel::WARN, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    auto error(std::string_view fmt, Args&&... args) {
        if constexpr (static_cast<int>(LogLevel::ERROR) < static_cast<int>(CompileTimeMinLevel)) {
            return DummyLogEntryBuilder{};
        } else {
            return create_log_entry(LogLevel::ERROR, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    auto fatal(std::string_view fmt, Args&&... args) {
        if constexpr (static_cast<int>(LogLevel::FATAL) < static_cast<int>(CompileTimeMinLevel)) {
            return DummyLogEntryBuilder{};
        } else {
            return create_log_entry(LogLevel::FATAL, fmt, std::forward<Args>(args)...);
        }
    }
    
    // Manual shutdown (called automatically by destructor)
    void shutdown() {
        if (!shutdown_requested_.exchange(true)) {
            queue_condition_.notify_all();
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }
    }

private:
    std::string name_;
    std::atomic<LogLevel> runtime_min_level_;
    std::atomic<bool> shutdown_requested_;
    
    // Pattern formatting
    std::string pattern_;
    mutable std::mutex pattern_mutex_;
    
    // Global scope tags (applied to all messages)
    std::map<std::string, std::string> global_scope_;
    
    // Static tags applied to all log records
    std::map<std::string, std::string> static_tags_;
    mutable std::mutex static_tags_mutex_;
    
    // Scoped tags stack (RAII-managed)
    std::vector<std::pair<std::string, std::string>> scoped_tags_stack_;
    mutable std::mutex scoped_tags_mutex_;
    
    // Output sinks with format specifications
    std::vector<FormattedSink> sinks_;
    mutable std::mutex sinks_mutex_;
    
    // Sampling state
    SamplingConfig global_sampling_;
    std::atomic<uint32_t> sampling_counter_;
    mutable std::mt19937 rng_;
    mutable std::mutex sampling_mutex_;
    
    // Async processing queue
    // TODO: Replace with lock-free queue for better performance
    std::queue<LogRecord> log_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::thread worker_thread_;
    
    template<typename... Args>
    LogEntryBuilder<CompileTimeMinLevel> create_log_entry(LogLevel level, std::string_view fmt, Args&&... args) {
        // Check runtime level filter
        if (static_cast<int>(level) < static_cast<int>(runtime_min_level_.load(std::memory_order_relaxed))) {
            return LogEntryBuilder<CompileTimeMinLevel>{nullptr, level, std::string()};
        }
        
        // Format message
        std::string formatted_message;
        try {
            formatted_message = std::vformat(fmt, std::make_format_args(args...));
        } catch (const std::exception&) {
            // Fallback to raw format string if formatting fails
            formatted_message = std::string(fmt);
        }
        
        return LogEntryBuilder<CompileTimeMinLevel>{this, level, std::move(formatted_message)};
    }
    
    bool should_sample(const LogRecord& record) {
        // Check per-message sampling first
        const auto& sampling = (record.sampling.type != SamplingType::NONE) 
                              ? record.sampling 
                              : global_sampling_;
        
        if (sampling.type == SamplingType::NONE) {
            return true;
        }
        
        // Only apply sampling to specified level and above
        if (static_cast<int>(record.level) < static_cast<int>(sampling.min_level)) {
            return true;
        }
        
        std::lock_guard<std::mutex> lock(sampling_mutex_);
        
        switch (sampling.type) {
            case SamplingType::RANDOM: {
                std::uniform_real_distribution<double> dist(0.0, 1.0);
                return dist(rng_) < sampling.rate;
            }
            case SamplingType::EVERY_NTH: {
                uint32_t counter = sampling_counter_.fetch_add(1, std::memory_order_relaxed);
                return (counter % sampling.every_nth) == 0;
            }
            default:
                return true;
        }
    }
    
    void process_log_queue() {
        // TODO: Add batch processing for better performance
        while (true) {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            
            // Wait for records or shutdown
            queue_condition_.wait(lock, [this] {
                return shutdown_requested_.load() || !log_queue_.empty();
            });
            
            // Process all available records
            while (!log_queue_.empty()) {
                LogRecord record = std::move(log_queue_.front());
                log_queue_.pop();
                lock.unlock();
                
                // Format and output the record to all sinks
                output_to_sinks(record);
                
                lock.lock();
            }
            
            // Exit if shutdown was requested and queue is empty
            if (shutdown_requested_.load()) {
                break;
            }
        }
    }
    
    void output_to_sinks(const LogRecord& record) {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        for (const auto& formatted_sink : sinks_) {
            try {
                std::string formatted = format_record_for_sink(record, formatted_sink.format_type);
                formatted_sink.sink_fn(formatted);
            } catch (...) {
                // Silently ignore sink errors to prevent logging failures
                // from crashing the application
            }
        }
    }
    
    std::string format_record_for_sink(const LogRecord& record, FormatType format_type) {
        // Per-message JSON override takes precedence
        if (record.use_json) {
            return format_json(record);
        }
        
        switch (format_type) {
            case FormatType::JSON:
                return format_json(record);
            case FormatType::CSV:
                return format_csv(record);
            case FormatType::TEXT:
            default:
                return format_text(record);
        }
    }
    
    // Pattern-based text formatting
    std::string format_text(const LogRecord& record) {
        std::lock_guard<std::mutex> lock(pattern_mutex_);
        return apply_pattern(record, pattern_);
    }
    
    std::string apply_pattern(const LogRecord& record, const std::string& pattern) {
        const auto& metadata = get_level_metadata(record.level);
        
        // Format timestamp
        auto time_t_val = std::chrono::system_clock::to_time_t(record.timestamp);
        std::tm tm_val{};
        localtime_r(&time_t_val, &tm_val);
        
        char timestamp_buffer[32] = {0};
        std::strftime(timestamp_buffer, sizeof(timestamp_buffer), "%Y-%m-%d %H:%M:%S", &tm_val);
        
        std::string result = pattern;
        
        // Replace pattern placeholders
        replace_all(result, "{timestamp}", timestamp_buffer);
        replace_all(result, "{level}", std::string(metadata.name));
        replace_all(result, "{color}", std::string(metadata.color_code));        // ADDED: Color support
        replace_all(result, "{color_reset}", std::string(color_reset()));        // ADDED: Color reset
        replace_all(result, "{logger}", name_);
        replace_all(result, "{message}", record.get_message_string());
        
        // Add fields and tags
        std::string fields_tags;
        if (!record.fields.empty() || !record.tags.empty()) {
            fields_tags += " {";
            bool first = true;
            
            // Add fields first
            for (const auto& [key, value] : record.fields) {
                if (!first) fields_tags += ", ";
                fields_tags += key + "=" + field_value_to_string(value);
                first = false;
            }
            
            // Add tags
            for (const auto& [key, value] : record.tags) {
                if (!first) fields_tags += ", ";
                fields_tags += key + "=" + value;
                first = false;
            }
            
            fields_tags += "}";
        }
        
        replace_all(result, "{tags}", fields_tags);
        
        return result;
    }
    
    void replace_all(std::string& str, const std::string& from, const std::string& to) {
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    }
    
    std::string format_json(const LogRecord& record) {
        const auto& metadata = get_level_metadata(record.level);
        
        // Format timestamp in ISO 8601
        auto time_t_val = std::chrono::system_clock::to_time_t(record.timestamp);
        std::tm tm_val{};
        gmtime_r(&time_t_val, &tm_val);
        
        // Initialize buffer to zero to avoid garbage data
        char timestamp_buffer[32] = {0};
        std::strftime(timestamp_buffer, sizeof(timestamp_buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
        std::string timestamp_str(timestamp_buffer);
        
        std::string result = std::format(R"({{"timestamp":"{}","level":"{}","logger":"{}","message":"{}",)",
            timestamp_str,
            metadata.name,
            escape_json_string(name_),
            escape_json_string(record.get_message_string()));
        
        // Add fields (typed)
        result += R"("fields":{)";
        bool first_field = true;
        for (const auto& [key, value] : record.fields) {
            if (!first_field) result += ",";
            result += std::format(R"("{}":{})", 
                escape_json_string(key), 
                field_value_to_json(value));
            first_field = false;
        }
        result += "},";
        
        // Add tags (always strings)
        result += R"("tags":{)";
        bool first_tag = true;
        for (const auto& [key, value] : record.tags) {
            if (!first_tag) result += ",";
            result += std::format(R"("{}":"{}")", 
                escape_json_string(key), 
                escape_json_string(value));
            first_tag = false;
        }
        
        result += "}}";
        return result;
    }
    
    std::string format_csv(const LogRecord& record) {
        const auto& metadata = get_level_metadata(record.level);
        
        // Format timestamp
        auto time_t_val = std::chrono::system_clock::to_time_t(record.timestamp);
        std::tm tm_val{};
        localtime_r(&time_t_val, &tm_val);
        
        char timestamp_buffer[32] = {0};
        std::strftime(timestamp_buffer, sizeof(timestamp_buffer), "%Y-%m-%d %H:%M:%S", &tm_val);
        
        // CSV format: timestamp,level,logger,message,fields_and_tags
        std::string result = std::format("{},{},{},\"{}\",\"",
            timestamp_buffer,
            metadata.name,
            name_,
            escape_csv_string(record.get_message_string()));
        
        // Add fields and tags as combined CSV field
        bool first = true;
        for (const auto& [key, value] : record.fields) {
            if (!first) result += ",";
            result += key + "=" + field_value_to_string(value);
            first = false;
        }
        for (const auto& [key, value] : record.tags) {
            if (!first) result += ",";
            result += key + "=" + value;
            first = false;
        }
        result += "\"";
        
        return result;
    }
    
    std::string escape_json_string(const std::string& input) {
        std::string result;
        result.reserve(input.size() * 2); // Worst case estimate
        
        for (char c : input) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b";  break;
                case '\f': result += "\\f";  break;
                case '\n': result += "\\n";  break;
                case '\r': result += "\\r";  break;
                case '\t': result += "\\t";  break;
                default:
                    if (c >= 0 && c < 0x20) {
                        result += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                    } else {
                        result += c;
                    }
                    break;
            }
        }
        
        return result;
    }
    
    std::string escape_csv_string(const std::string& input) {
        std::string result;
        result.reserve(input.size() * 2);
        
        for (char c : input) {
            if (c == '"') {
                result += "\"\"";  // Escape quotes in CSV
            } else {
                result += c;
            }
        }
        
        return result;
    }
    
    void add_default_sink() {
        add_sink([](const std::string& record) {
            std::clog << record << '\n';
        }, FormatType::TEXT);
    }
};

} // namespace util
