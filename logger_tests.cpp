#include "logger.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>

using namespace util;
using namespace std::chrono_literals;

// Test utilities
class TestTimer {
    std::chrono::steady_clock::time_point start_;
public:
    TestTimer() : start_(std::chrono::steady_clock::now()) {}
    auto elapsed() const {
        return std::chrono::steady_clock::now() - start_;
    }
};

class FileLogSink {
    std::string filename_;
    std::ofstream file_;
    std::mutex mutex_;
    
public:
    explicit FileLogSink(std::string filename) 
        : filename_(std::move(filename)) {
        std::filesystem::remove(filename_);
        file_.open(filename_, std::ios::app);
        if (!file_) {
            throw std::runtime_error("Cannot open log file: " + filename_);
        }
    }
    
    ~FileLogSink() {
        if (file_.is_open()) {
            file_.close();
        }
        std::filesystem::remove(filename_);
    }
    
    LogSink get_sink() {
        return [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex_);
            file_ << line << '\n' << std::flush;
        };
    }
    
    std::vector<std::string> read_lines() {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.flush();
        
        std::ifstream reader(filename_);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(reader, line)) {
            lines.push_back(line);
        }
        return lines;
    }
    
    size_t count_lines() {
        return read_lines().size();
    }
};

class OutputCapture {
    std::vector<std::string> messages_;
    std::mutex mutex_;
    
public:
    LogSink get_sink() {
        return [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back(line);
        };
    }
    
    std::vector<std::string> get_messages() {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
    }
    
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_.size();
    }
};

// ========== UPDATED EXISTING TESTS ==========

void test_high_volume_logging() {
    std::cout << "Testing high volume logging (100,000 messages)..." << std::endl;
    
    TestTimer timer;
    FileLogSink file_sink("high_volume.log");
    Logger<LogLevel::INFO> logger("volume");
    logger.add_sink(file_sink.get_sink(), FormatType::TEXT);
    
    constexpr int MESSAGE_COUNT = 100000;
    
    for (int i = 0; i < MESSAGE_COUNT; ++i) {
        logger.info("Message number {} with some data {}", i, "test_data_" + std::to_string(i));
    }
    
    // Wait for all messages to be processed
    std::this_thread::sleep_for(1000ms);
    logger.shutdown();
    
    auto lines = file_sink.read_lines();
    assert(lines.size() == MESSAGE_COUNT);
    
    // Verify ordering (messages should be in sequence)
    for (size_t i = 0; i < std::min(lines.size(), size_t(1000)); ++i) { // Check first 1000 for performance
        std::string expected = "Message number " + std::to_string(i);
        assert(lines[i].find(expected) != std::string::npos);
    }
    
    auto elapsed = timer.elapsed();
    std::cout << "  Processed " << MESSAGE_COUNT << " messages in " 
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() 
              << "ms" << std::endl;
}

void test_concurrent_logging() {
    std::cout << "Testing concurrent logging (8 threads, 5000 messages each)..." << std::endl;
    
    FileLogSink file_sink("concurrent.log");
    Logger<LogLevel::INFO> logger("concurrent");
    logger.add_sink(file_sink.get_sink(), FormatType::TEXT);
    
    constexpr int THREAD_COUNT = 8;
    constexpr int MESSAGES_PER_THREAD = 5000;
    
    std::vector<std::future<void>> futures;
    std::atomic<int> completed_threads{0};
    
    TestTimer timer;
    
    for (int thread_id = 0; thread_id < THREAD_COUNT; ++thread_id) {
        futures.push_back(std::async(std::launch::async, [&, thread_id]() {
            for (int msg = 0; msg < MESSAGES_PER_THREAD; ++msg) {
                logger.info("Thread {} message {}", thread_id, msg)
                      .tag("thread_id", std::to_string(thread_id))
                      .tag("message_id", std::to_string(msg));
            }
            completed_threads.fetch_add(1);
        }));
    }
    
    // Wait for all threads to complete
    for (auto& future : futures) {
        future.get();
    }
    
    // Wait for async processing
    std::this_thread::sleep_for(200ms);
    logger.shutdown();
    
    auto lines = file_sink.read_lines();
    assert(lines.size() == THREAD_COUNT * MESSAGES_PER_THREAD);
    
    auto elapsed = timer.elapsed();
    std::cout << "  " << THREAD_COUNT << " threads completed " 
              << (THREAD_COUNT * MESSAGES_PER_THREAD) << " messages in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() 
              << "ms" << std::endl;
}

void test_error_resilience() {
    std::cout << "Testing error resilience (failing sinks)..." << std::endl;
    
    std::vector<std::string> successful_logs;
    int failure_count = 0;
    std::mutex success_mutex;
    
    Logger<LogLevel::INFO> logger("resilient");
    
    // Add a good sink
    logger.add_sink([&successful_logs, &success_mutex](const std::string& line) {
        std::lock_guard<std::mutex> lock(success_mutex);
        successful_logs.push_back(line);
    }, FormatType::TEXT);
    
    // Add a bad sink that throws exceptions
    logger.add_sink([&failure_count](const std::string&) {
        failure_count++;
        throw std::runtime_error("Simulated sink failure");
    }, FormatType::JSON);
    
    // Add another good sink
    logger.add_sink([&successful_logs, &success_mutex](const std::string& line) {
        std::lock_guard<std::mutex> lock(success_mutex);
        successful_logs.push_back("backup: " + line);
    }, FormatType::CSV);
    
    // Log some messages
    for (int i = 0; i < 10; ++i) {
        logger.info("Test message {}", i);
    }
    
    std::this_thread::sleep_for(50ms);
    logger.shutdown();
    
    // Both good sinks should have received all messages
    assert(successful_logs.size() == 20); // 2 sinks × 10 messages
    assert(failure_count == 10); // Bad sink was called for all messages
    
    std::cout << "  Logger remained stable despite " << failure_count << " sink failures" << std::endl;
}

void test_json_special_characters() {
    std::cout << "Testing JSON escaping with special characters..." << std::endl;
    
    std::string captured;
    Logger<LogLevel::INFO> logger("json_escape");
    logger.add_sink([&captured](const std::string& line) {
        captured = line;
    }, FormatType::JSON);
    
    // Test various special characters
    std::string test_message = "Special chars: \"quotes\" \\backslash\\ \n\r\t control \b\f";
    logger.info("{}", test_message)
          .tag("special", "value with \"quotes\" and \\backslashes\\")
          .tag("control", "\n\r\t\b\f");
    
    std::this_thread::sleep_for(10ms);
    logger.shutdown();
    
    assert(!captured.empty());
    
    // Verify proper JSON escaping
    assert(captured.find("\\\"quotes\\\"") != std::string::npos);
    assert(captured.find("\\\\backslash\\\\") != std::string::npos);
    assert(captured.find("\\n") != std::string::npos);
    assert(captured.find("\\r") != std::string::npos);
    assert(captured.find("\\t") != std::string::npos);
    assert(captured.find("\\b") != std::string::npos);
    assert(captured.find("\\f") != std::string::npos);
    
    std::cout << "  JSON properly escaped special characters" << std::endl;
}

void test_very_long_messages() {
    std::cout << "Testing very long log messages..." << std::endl;
    
    FileLogSink file_sink("long_messages.log");
    Logger<LogLevel::INFO> logger("long_msg");
    logger.add_sink(file_sink.get_sink(), FormatType::TEXT);
    
    // Create a very long message (1MB)
    std::string long_data(1024 * 1024, 'A');
    std::string very_long_message = "Start-" + long_data + "-End";
    
    logger.info("Long message test: {}", very_long_message);
    
    std::this_thread::sleep_for(100ms);
    logger.shutdown();
    
    auto lines = file_sink.read_lines();
    assert(lines.size() == 1);
    assert(lines[0].find("Start-") != std::string::npos);
    assert(lines[0].find("-End") != std::string::npos);
    assert(lines[0].size() > 1024 * 1024); // Should be larger than 1MB
    
    std::cout << "  Successfully handled message of size: " << lines[0].size() << " bytes" << std::endl;
}

void test_rapid_level_changes() {
    std::cout << "Testing rapid level changes..." << std::endl;
    
    std::atomic<int> message_count{0};
    Logger<LogLevel::INFO> logger("level_change");
    logger.add_sink([&message_count](const std::string&) {
        message_count.fetch_add(1);
    }, FormatType::TEXT);
    
    // Rapidly change levels while logging
    std::thread level_changer([&logger]() {
        for (int i = 0; i < 100; ++i) {
            logger.set_level(static_cast<LogLevel>(i % 6)); // Cycle through all levels
            std::this_thread::sleep_for(1ms);
        }
    });
    
    std::thread message_sender([&logger]() {
        for (int i = 0; i < 1000; ++i) {
            logger.trace("Trace {}", i);
            logger.debug("Debug {}", i);
            logger.info("Info {}", i);
            logger.warn("Warn {}", i);
            logger.error("Error {}", i);
            logger.fatal("Fatal {}", i);
        }
    });
    
    level_changer.join();
    message_sender.join();
    
    std::this_thread::sleep_for(50ms);
    logger.shutdown();
    
    // Should have received some messages (exact count depends on timing)
    assert(message_count.load() > 0);
    assert(message_count.load() <= 6000); // Max possible if all levels allowed
    
    std::cout << "  Processed " << message_count.load() << " messages during level changes" << std::endl;
}

void test_shutdown_behavior() {
    std::cout << "Testing shutdown behavior..." << std::endl;
    
    FileLogSink file_sink("shutdown_test.log");
    
    {
        Logger<LogLevel::INFO> logger("shutdown");
        logger.add_sink(file_sink.get_sink(), FormatType::TEXT);
        
        // Queue many messages
        for (int i = 0; i < 1000; ++i) {
            logger.info("Shutdown test message {}", i);
        }
        
        // Logger destructor should wait for all messages to be processed
    }
    
    // Verify all messages were written before shutdown
    auto lines = file_sink.read_lines();
    assert(lines.size() == 1000);
    
    // Verify last message exists
    assert(lines.back().find("Shutdown test message 999") != std::string::npos);
    
    std::cout << "  All " << lines.size() << " messages processed before shutdown" << std::endl;
}

void test_compile_time_performance() {
    std::cout << "Testing compile-time filtering performance..." << std::endl;
    
    std::atomic<int> processed_messages{0};
    
    // Logger that filters out TRACE and DEBUG at compile time
    Logger<LogLevel::INFO> logger("compile_perf");
    logger.add_sink([&processed_messages](const std::string&) {
        processed_messages.fetch_add(1);
    }, FormatType::TEXT);
    
    TestTimer timer;
    
    // These should be no-ops at compile time
    for (int i = 0; i < 100000; ++i) {
        logger.trace("This should be optimized away {}", i);
        logger.debug("This should also be optimized away {}", i);
    }
    
    // These should be processed
    for (int i = 0; i < 1000; ++i) {
        logger.info("This should be processed {}", i);
    }
    
    auto compile_filtered_time = timer.elapsed();
    
    std::this_thread::sleep_for(50ms);
    logger.shutdown();
    
    // Only INFO messages should have been processed
    assert(processed_messages.load() == 1000);
    
    std::cout << "  Compile-time filtering completed in " 
              << std::chrono::duration_cast<std::chrono::microseconds>(compile_filtered_time).count() 
              << " microseconds" << std::endl;
    std::cout << "  " << processed_messages.load() << " messages processed (200,000 filtered out)" << std::endl;
}

void test_scoped_tags() {
    std::cout << "Testing scoped tags..." << std::endl;
    
    OutputCapture capture;  // Use OutputCapture instead of vector
    Logger<LogLevel::INFO> logger("scoped");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    // Static tags first
    logger.tag("service", "auth").tag("version", "1.0");
    
    // IMPORTANT: Set pattern to include tags
    logger.set_pattern("{timestamp} [{level}] [{logger}] {message}{tags}");
    
    // Global message should have static tags
    logger.info("Global message");
    
    {
        auto scoped = logger.with_scope({{"request_id", "12345"}, {"user", "alice"}});
        logger.info("Scoped message 1");
        
        {
            auto nested_scoped = logger.with_scope({{"operation", "login"}});
            logger.info("Nested scoped message");
        }
        
        logger.info("Scoped message 2");
    }
    
    logger.info("Back to global");
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 5);
    
    // Debug output
    for (size_t i = 0; i < messages.size(); ++i) {
        std::cout << "  Message " << i << ": " << messages[i] << std::endl;
    }
    
    // Global message should have static tags
    assert(messages[0].find("service=auth") != std::string::npos);
    assert(messages[0].find("version=1.0") != std::string::npos);
    
    // Scoped messages should have static + scoped tags
    assert(messages[1].find("request_id=12345") != std::string::npos);
    assert(messages[1].find("user=alice") != std::string::npos);
    assert(messages[1].find("service=auth") != std::string::npos);
    assert(messages[1].find("version=1.0") != std::string::npos);
    
    // Nested should have all tags
    assert(messages[2].find("operation=login") != std::string::npos);
    assert(messages[2].find("request_id=12345") != std::string::npos);
    assert(messages[2].find("user=alice") != std::string::npos);
    assert(messages[2].find("service=auth") != std::string::npos);
    
    // After nested scope destruction, should still have first scope tags
    assert(messages[3].find("request_id=12345") != std::string::npos);
    assert(messages[3].find("user=alice") != std::string::npos);
    assert(messages[3].find("service=auth") != std::string::npos);
    // Should NOT have nested operation tag
    assert(messages[3].find("operation=login") == std::string::npos);
    
    // After all scopes destroyed, should only have static tags
    assert(messages[4].find("request_id=12345") == std::string::npos);
    assert(messages[4].find("user=alice") == std::string::npos);
    assert(messages[4].find("operation=login") == std::string::npos);
    assert(messages[4].find("service=auth") != std::string::npos);
    assert(messages[4].find("version=1.0") != std::string::npos);
    
    std::cout << "  Scoped tags working correctly" << std::endl;
}

void test_global_scope() {
    std::cout << "Testing global scope tags..." << std::endl;
    
    OutputCapture capture;  // Use OutputCapture
    Logger<LogLevel::INFO> logger("global_scope");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    // Set pattern to include tags
    logger.set_pattern("{timestamp} [{level}] [{logger}] {message}{tags}");
    
    // Set global scope
    logger.set_global_scope({{"environment", "production"}, {"datacenter", "us-east-1"}});
    
    // Add static tags
    logger.tag("service", "api");
    
    logger.info("Test message");
    
    // Add more global scope
    logger.add_global_scope_tag("node_id", "node-001");
    
    logger.info("Another test message");
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 2);
    
    // Debug output
    for (size_t i = 0; i < messages.size(); ++i) {
        std::cout << "  Message " << i << ": " << messages[i] << std::endl;
    }
    
    // Both messages should have global scope and static tags
    for (const auto& msg : messages) {
        assert(msg.find("environment=production") != std::string::npos);
        assert(msg.find("datacenter=us-east-1") != std::string::npos);
        assert(msg.find("service=api") != std::string::npos);
    }
    
    // Second message should also have the new global tag
    assert(messages[1].find("node_id=node-001") != std::string::npos);
    
    std::cout << "  Global scope tags working correctly" << std::endl;
}

void test_multiple_format_sinks() {
    std::cout << "Testing multiple format sinks..." << std::endl;
    
    std::string text_output, json_output, csv_output;
    
    Logger<LogLevel::INFO> logger("multi_format");
    logger.set_pattern("{timestamp} [{level}] [{logger}] {message}{tags}");

    logger.add_sink([&text_output](const std::string& line) {
        text_output = line;
    }, FormatType::TEXT);
    
    logger.add_sink([&json_output](const std::string& line) {
        json_output = line;
    }, FormatType::JSON);
    
    logger.add_sink([&csv_output](const std::string& line) {
        csv_output = line;
    }, FormatType::CSV);
    
    logger.info("Multi-format test").tag("key", "value");
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    // Verify each format
    assert(text_output.find("[multi_format]") != std::string::npos);
    assert(text_output.find("key=value") != std::string::npos);
    
    assert(json_output.find("\"timestamp\":") != std::string::npos);
    assert(json_output.find("\"key\":\"value\"") != std::string::npos);
    
    assert(csv_output.find("INFO,multi_format") != std::string::npos);
    assert(csv_output.find("key=value") != std::string::npos);
    
    std::cout << "  TEXT: " << text_output.substr(0, 50) << "..." << std::endl;
    std::cout << "  JSON: " << json_output.substr(0, 50) << "..." << std::endl;
    std::cout << "  CSV:  " << csv_output.substr(0, 50) << "..." << std::endl;
}

void test_csv_formatting() {
    std::cout << "Testing CSV formatting..." << std::endl;
    
    std::vector<std::string> csv_lines;
    Logger<LogLevel::INFO> logger("csv_test");
    logger.add_sink([&csv_lines](const std::string& line) {
        csv_lines.push_back(line);
    }, FormatType::CSV);
    
    logger.info("CSV test message").tag("user", "alice").tag("action", "login");
    logger.warn("CSV warning with \"quotes\"").tag("code", "WARN_001");
    logger.error("CSV error\nwith newlines").tag("severity", "high");
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    assert(csv_lines.size() == 3);
    
    // Verify CSV structure
    for (const auto& line : csv_lines) {
        // Should have timestamp,level,logger,message,tags format
        auto comma_count = std::count(line.begin(), line.end(), ',');
        assert(comma_count >= 4); // At least 4 commas for 5 fields
        
        assert(line.find("csv_test") != std::string::npos);
    }
    
    // Check specific formatting
    assert(csv_lines[0].find("INFO,csv_test") != std::string::npos);
    assert(csv_lines[0].find("user=alice") != std::string::npos);
    
    assert(csv_lines[1].find("WARN,csv_test") != std::string::npos);
    assert(csv_lines[1].find("\"CSV warning with \"\"quotes\"\"\"") != std::string::npos); // CSV quote escaping
    
    std::cout << "  CSV formatting working correctly" << std::endl;
}

void test_sampling() {
    std::cout << "Testing sampling..." << std::endl;
    
    std::atomic<int> message_count{0};
    Logger<LogLevel::INFO> logger("sampling");
    logger.add_sink([&message_count](const std::string&) {
        message_count.fetch_add(1);
    }, FormatType::TEXT);
    
    // Test random sampling
    logger.set_sampling(SamplingConfig(SamplingType::RANDOM, 0.1, LogLevel::INFO));
    
    for (int i = 0; i < 1000; ++i) {
        logger.info("Sampled message {}", i);
    }
    
    std::this_thread::sleep_for(50ms);
    int sampled_count = message_count.load();
    
    // Should be approximately 10% (allow some variance)
    assert(sampled_count > 50 && sampled_count < 200);
    
    message_count.store(0);
    
    // Test every-nth sampling - Fixed: Use explicit uint32_t cast
    logger.set_sampling(SamplingConfig(SamplingType::EVERY_NTH, static_cast<uint32_t>(10), LogLevel::INFO));
    
    for (int i = 0; i < 100; ++i) {
        logger.info("Every 10th message {}", i);
    }
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    // Should be exactly 10 messages (every 10th)
    assert(message_count.load() == 10);
    
    std::cout << "  Random sampling: ~" << sampled_count << " out of 1000" << std::endl;
    std::cout << "  Every-nth sampling: " << message_count.load() << " out of 100" << std::endl;
}

void test_per_message_sampling() {
    std::cout << "Testing per-message sampling..." << std::endl;
    
    std::atomic<int> normal_count{0};
    std::atomic<int> sampled_count{0};
    std::atomic<int> every_nth_count{0};
    
    Logger<LogLevel::INFO> logger("per_msg_sampling");
    logger.add_sink([&](const std::string& line) {
        if (line.find("Normal message") != std::string::npos) {
            normal_count.fetch_add(1);
        } else if (line.find("Sampled message") != std::string::npos) {
            sampled_count.fetch_add(1);
        } else if (line.find("Every nth message") != std::string::npos) {
            every_nth_count.fetch_add(1);
        }
    }, FormatType::TEXT);
    
    // Normal messages (no sampling)
    for (int i = 0; i < 50; ++i) {
        logger.info("Normal message {}", i);
    }
    
    // Random sampled messages (50% sampling)
    for (int i = 0; i < 100; ++i) {
        logger.info("Sampled message {}", i).sample(0.5);
    }
    
    // Every-nth sampled messages - Fixed: Use explicit uint32_t cast
    for (int i = 0; i < 50; ++i) {
        logger.info("Every nth message {}", i).every_nth(static_cast<uint32_t>(5));
    }
    
    std::this_thread::sleep_for(100ms);
    logger.shutdown();
    
    assert(normal_count.load() == 50);  // All normal messages should appear
    assert(sampled_count.load() > 30 && sampled_count.load() < 70);  // ~50% of sampled
    assert(every_nth_count.load() == 10); // Every 5th message out of 50
    
    std::cout << "  Normal messages: " << normal_count.load() << " out of 50" << std::endl;
    std::cout << "  Random sampled: " << sampled_count.load() << " out of 100" << std::endl;
    std::cout << "  Every-nth sampled: " << every_nth_count.load() << " out of 50" << std::endl;
}

void test_sampling_level_control() {
    std::cout << "Testing sampling level control..." << std::endl;
    
    std::atomic<int> info_count{0};
    std::atomic<int> error_count{0};
    
    Logger<LogLevel::INFO> logger("level_sampling");
    logger.add_sink([&](const std::string& line) {
        if (line.find("INFO") != std::string::npos) {
            info_count.fetch_add(1);
        } else if (line.find("ERROR") != std::string::npos) {
            error_count.fetch_add(1);
        }
    }, FormatType::TEXT);
    
    // Set sampling to apply only to ERROR level and above
    logger.set_sampling(SamplingConfig(SamplingType::RANDOM, 0.1, LogLevel::ERROR));
    
    // Send INFO messages (should not be sampled)
    for (int i = 0; i < 100; ++i) {
        logger.info("Info message {}", i);
    }
    
    // Send ERROR messages (should be sampled)
    for (int i = 0; i < 100; ++i) {
        logger.error("Error message {}", i);
    }
    
    std::this_thread::sleep_for(50ms);
    logger.shutdown();
    
    assert(info_count.load() == 100); // All INFO messages should pass (below sampling level)
    assert(error_count.load() > 5 && error_count.load() < 25); // ~10% of ERROR messages
    
    std::cout << "  INFO messages (no sampling): " << info_count.load() << " out of 100" << std::endl;
    std::cout << "  ERROR messages (sampled): " << error_count.load() << " out of 100" << std::endl;
}

void test_json_override_with_formats() {
    std::cout << "Testing JSON override with different sink formats..." << std::endl;
    
    std::string text_sink_output, json_sink_output, csv_sink_output;
    
    Logger<LogLevel::INFO> logger("json_override");
    logger.add_sink([&text_sink_output](const std::string& line) {
        text_sink_output = line;
    }, FormatType::TEXT);
    
    logger.add_sink([&json_sink_output](const std::string& line) {
        json_sink_output = line;
    }, FormatType::JSON);
    
    logger.add_sink([&csv_sink_output](const std::string& line) {
        csv_sink_output = line;
    }, FormatType::CSV);
    
    // Message with JSON override should be JSON in all sinks
    logger.info("Override test").json();
    
    std::this_thread::sleep_for(10ms);
    logger.shutdown();
    
    // All sinks should receive JSON because of .json() override
    assert(text_sink_output.find("\"timestamp\":") != std::string::npos);
    assert(json_sink_output.find("\"timestamp\":") != std::string::npos);
    assert(csv_sink_output.find("\"timestamp\":") != std::string::npos);
    
    std::cout << "  JSON override respected in all sinks" << std::endl;
}

void test_per_message_json_formatting() {
    std::cout << "Testing per-message JSON formatting..." << std::endl;
    
    std::vector<std::string> messages;
    Logger<LogLevel::INFO> logger("mixed_format");
    
    logger.add_sink([&messages](const std::string& line) {
        messages.push_back(line);
    }, FormatType::TEXT);
    
    // Mix of text and JSON messages (logger is in text mode by default)
    logger.info("Regular text message");                             // Should be text
    logger.info("Force JSON message").json();                       // Should be JSON
    logger.info("Another text message");                            // Should be text  
    logger.info("JSON with tags").tag("key", "value").json();       // Should be JSON
    logger.info("Text with tags").tag("key", "value");              // Should be text
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    assert(messages.size() == 5);
    
    // Check message formats
    assert(messages[0].find("[mixed_format]") != std::string::npos);     // Text format
    assert(messages[0].find("\"timestamp\":") == std::string::npos);     // Not JSON
    
    assert(messages[1].find("\"timestamp\":") != std::string::npos);     // JSON format  
    assert(messages[1].find("[mixed_format]") == std::string::npos);     // Not text format
    assert(messages[1].find("\"message\":\"Force JSON message\"") != std::string::npos);
    
    assert(messages[2].find("[mixed_format]") != std::string::npos);     // Text format
    assert(messages[2].find("\"timestamp\":") == std::string::npos);     // Not JSON
    
    assert(messages[3].find("\"timestamp\":") != std::string::npos);     // JSON format
    assert(messages[3].find("\"key\":\"value\"") != std::string::npos);  // JSON tags
    
    assert(messages[4].find("[mixed_format]") != std::string::npos);     // Text format
    assert(messages[4].find("key=value") != std::string::npos);          // Text tags
    
    std::cout << "  Per-message JSON formatting working correctly" << std::endl;
}

void test_tag_priority_simple() {
    std::cout << "Testing tag priority step by step..." << std::endl;
    
    OutputCapture capture;  // Use OutputCapture
    Logger<LogLevel::INFO> logger("priority");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    // Set pattern to include tags
    logger.set_pattern("{timestamp} [{level}] [{logger}] {message}{tags}");
    
    // Test 1: Only global scope
    logger.set_global_scope({{"source", "global"}});
    logger.info("Test 1");
    
    // Test 2: Global + static
    logger.tag("source", "static");
    logger.info("Test 2");
    
    // Test 3: Global + static + scoped
    {
        auto scoped = logger.with_scope({{"source", "scoped"}});
        logger.info("Test 3");
        
        // Test 4: Global + static + scoped + per-message
        logger.info("Test 4").tag("source", "per-message");
    }
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 4);
    
    // Debug output
    for (size_t i = 0; i < messages.size(); ++i) {
        std::cout << "  Message " << i << ": " << messages[i] << std::endl;
    }
    
    // Verify each level of priority
    assert(messages[0].find("source=global") != std::string::npos);      // Only global
    assert(messages[1].find("source=static") != std::string::npos);      // Static overrides global
    assert(messages[2].find("source=scoped") != std::string::npos);      // Scoped overrides static
    assert(messages[3].find("source=per-message") != std::string::npos); // Per-message overrides scoped
    
    std::cout << "  All priority levels working correctly" << std::endl;
}

void test_complex_tag_hierarchy() {
    std::cout << "Testing complex tag hierarchy..." << std::endl;
    
    std::vector<std::string> messages;
    Logger<LogLevel::INFO> logger("hierarchy");
    logger.add_sink([&messages](const std::string& line) {
        messages.push_back(line);
    }, FormatType::TEXT);
    
    // Global scope (lowest priority)
    logger.set_global_scope({{"env", "prod"}, {"region", "us-east"}, {"version", "global-1.0"}});
    
    // Static tags (medium-low priority)  
    logger.tag("service", "api").tag("version", "static-2.0").tag("component", "logger");
    
    {
        // Scoped tags (medium-high priority)
        auto scoped = logger.with_scope({
            {"request_id", "req123"}, 
            {"version", "scoped-3.0"},
            {"user", "alice"}
        });
        
        // Per-message tags (highest priority) - should override everything
        logger.info("Hierarchy test")
              .tag("message_id", "msg456")
              .tag("version", "message-4.0")  // This should win
              .tag("action", "test");
    }
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    assert(messages.size() == 1);
    const auto& msg = messages[0];
    
    std::cout << "  Message: " << msg << std::endl;
    
    // Verify tag hierarchy (per-message should have highest priority)
    assert(msg.find("env=prod") != std::string::npos);                      // Global scope
    assert(msg.find("region=us-east") != std::string::npos);                // Global scope  
    assert(msg.find("service=api") != std::string::npos);                   // Static tag
    assert(msg.find("component=logger") != std::string::npos);              // Static tag
    assert(msg.find("request_id=req123") != std::string::npos);             // Scoped tag
    assert(msg.find("user=alice") != std::string::npos);                    // Scoped tag
    assert(msg.find("message_id=msg456") != std::string::npos);             // Per-message tag
    assert(msg.find("action=test") != std::string::npos);                   // Per-message tag
    
    // Most importantly: per-message version should win
    assert(msg.find("version=message-4.0") != std::string::npos);           // Should be final override
    
    // Should NOT contain overridden values
    assert(msg.find("version=global-1.0") == std::string::npos);
    assert(msg.find("version=static-2.0") == std::string::npos);
    assert(msg.find("version=scoped-3.0") == std::string::npos);
    
    std::cout << "  Tag hierarchy working correctly: per-message > scoped > static > global" << std::endl;
}

void test_performance_impact() {
    std::cout << "Testing performance impact of advanced features..." << std::endl;
    
    std::atomic<int> message_count{0};
    Logger<LogLevel::INFO> logger("performance");
    
    logger.add_sink([&message_count](const std::string&) {
        message_count.fetch_add(1);
    }, FormatType::TEXT);
    
    // Set up complex configuration
    logger.set_global_scope({{"env", "test"}, {"service", "perf_test"}});
    logger.tag("version", "1.0").tag("component", "logger");
    logger.set_sampling(SamplingConfig(SamplingType::RANDOM, 0.8)); // 80% sampling
    
    auto start = std::chrono::steady_clock::now();
    
    {
        auto scoped = logger.with_scope({{"test_run", "performance"}});
        
        // Generate many messages with mixed features
        for (int i = 0; i < 10000; ++i) {
            if (i % 4 == 0) {
                logger.info("Text message {}", i).tag("type", "text");
            } else if (i % 4 == 1) {
                logger.info("JSON message {}", i).tag("type", "json").json();
            } else if (i % 4 == 2) {
                logger.info("Sampled message {}", i).tag("type", "sampled").sample(0.5);
            } else {
                logger.info("Complex message {}", i)
                      .tag("type", "complex")
                      .tag("iteration", std::to_string(i))
                      .every_nth(static_cast<uint32_t>(10));
            }
        }
    }
    
    std::this_thread::sleep_for(200ms);
    logger.shutdown();
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should have processed most messages (accounting for sampling)
    assert(message_count.load() > 5000); // At least 50% should get through
    assert(message_count.load() < 10000); // But not all due to sampling
    
    std::cout << "  Processed " << message_count.load() 
              << " messages with advanced features in " << duration.count() << "ms" << std::endl;
}

void test_structured_fields_basic() {
    std::cout << "Testing basic structured fields..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("fields_test");
    logger.add_sink(capture.get_sink(), FormatType::JSON);
    
    logger.info("User action")
          .field("user_id", 12345)
          .field("success", true)
          .field("response_time", 1.23)
          .field("message_count", static_cast<int64_t>(999999999))
          .tag("action", "login");
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 1);
    
    const auto& json = messages[0];
    std::cout << "  JSON Output: " << json.substr(0, 100) << "..." << std::endl;
    
    // Verify typed fields in JSON
    assert(json.find("\"user_id\":12345") != std::string::npos);          // int (no quotes)
    assert(json.find("\"success\":true") != std::string::npos);           // bool (no quotes)  
    assert(json.find("\"response_time\":1.23") != std::string::npos);     // double (no quotes)
    assert(json.find("\"message_count\":999999999") != std::string::npos); // int64_t (no quotes)
    assert(json.find("\"action\":\"login\"") != std::string::npos);       // string tag (quotes)
    
    std::cout << "  Structured fields correctly typed in JSON" << std::endl;
}

void test_structured_fields_text_format() {
    std::cout << "Testing structured fields in TEXT format..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("fields_text");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    logger.info("Mixed data")
          .field("count", 42)
          .field("active", true)
          .field("ratio", 0.75)
          .tag("category", "test");
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 1);
    
    const auto& text = messages[0];
    std::cout << "  TEXT Output: " << text << std::endl;
    
    // In text format, everything becomes string
    assert(text.find("count=42") != std::string::npos);
    assert(text.find("active=true") != std::string::npos);
    assert(text.find("ratio=0.75") != std::string::npos);
    assert(text.find("category=test") != std::string::npos);
    
    std::cout << "  Structured fields converted to strings in TEXT format" << std::endl;
}

void test_complex_json_fields() {
    std::cout << "Testing complex JSON fields..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("complex_json");
    logger.add_sink(capture.get_sink(), FormatType::JSON);
    
    // JSON string as a field value
    std::string user_object = R"({"id": 123, "name": "Alice", "roles": ["admin", "user"]})";
    std::string metadata = R"({"version": "1.0", "features": ["auth", "logging"]})";
    
    logger.info("Complex data logged")
          .field("user_object", user_object)
          .field("metadata", metadata)
          .field("processing_time", 1.456)
          .field("items_processed", 1000)
          .tag("operation", "bulk_update");
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 1);
    
    const auto& json = messages[0];
    std::cout << "  Complex JSON: " << json.substr(0, 150) << "..." << std::endl;
    
    // JSON strings are properly escaped
    assert(json.find("\"user_object\":") != std::string::npos);
    assert(json.find("\"processing_time\":1.456") != std::string::npos);
    assert(json.find("\"items_processed\":1000") != std::string::npos);
    
    std::cout << "  Complex JSON fields handled correctly" << std::endl;
}

// ========== RAII PERFORMANCE PROFILER TESTS ==========

void test_basic_profiling() {
    std::cout << "Testing basic RAII profiling..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("profiler_test");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    {
        auto timer = logger.profile("test_operation");
        std::this_thread::sleep_for(50ms);  // Simulate work
    } // Auto-logs when timer destructs
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 1);
    
    const auto& log = messages[0];
    std::cout << "  Profile Output: " << log << std::endl;
    
    assert(log.find("Operation 'test_operation' completed in") != std::string::npos);
    assert(log.find("ms") != std::string::npos);
    assert(log.find("profiler=auto") != std::string::npos);
    assert(log.find("duration_ms=") != std::string::npos);
    
    std::cout << "  Basic profiling working correctly" << std::endl;
}

void test_profiling_with_chaining() {
    std::cout << "Testing RAII profiling with chaining..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("profiler_chain");
    logger.add_sink(capture.get_sink(), FormatType::JSON);
    
    {
        auto timer = logger.profile("database_query", LogLevel::WARN)
                           .tag("table", "users")
                           .tag("query_type", "SELECT")
                           .field("expected_rows", 1000)
                           .field("timeout_ms", 5000);
        
        std::this_thread::sleep_for(75ms);  // Simulate database work
    } // Auto-logs with all accumulated tags/fields
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 1);
    
    const auto& json = messages[0];
    std::cout << "  Chained Profile JSON: " << json.substr(0, 100) << "..." << std::endl;
    
    assert(json.find("\"level\":\"WARN\"") != std::string::npos);         // Custom level
    assert(json.find("\"table\":\"users\"") != std::string::npos);       // Chained tag
    assert(json.find("\"query_type\":\"SELECT\"") != std::string::npos); // Chained tag
    assert(json.find("\"expected_rows\":1000") != std::string::npos);     // Chained field
    assert(json.find("\"timeout_ms\":5000") != std::string::npos);        // Chained field
    assert(json.find("\"operation_name\":\"database_query\"") != std::string::npos); // Auto field
    assert(json.find("\"profiler\":\"auto\"") != std::string::npos);      // Auto tag
    
    std::cout << "  Chained profiling working correctly" << std::endl;
}

void test_nested_profiling() {
    std::cout << "Testing nested RAII profiling..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("nested_profile");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    {
        auto outer_timer = logger.profile("outer_operation");
        std::this_thread::sleep_for(30ms);
        
        {
            auto inner_timer = logger.profile("inner_operation", LogLevel::DEBUG);
            std::this_thread::sleep_for(20ms);
        } // Inner completes first
        
        std::this_thread::sleep_for(10ms);
    } // Outer completes second
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 2);
    
    // First message should be inner (completed first)
    assert(messages[0].find("inner_operation") != std::string::npos);
    // Second message should be outer (completed second)  
    assert(messages[1].find("outer_operation") != std::string::npos);
    
    std::cout << "  Nested profiling working correctly" << std::endl;
}

// ========== EXCEPTION INTEGRATION TESTS ==========

void test_exception_integration_basic() {
    std::cout << "Testing basic exception integration..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("exception_test");
    logger.add_sink(capture.get_sink(), FormatType::JSON);
    
    try {
        throw std::runtime_error("Test exception message");
    } catch (const std::exception& e) {
        logger.error("Operation failed")
              .exception(e)
              .tag("operation", "test_op")
              .field("retry_count", 3);
    }
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 1);
    
    const auto& json = messages[0];
    std::cout << "  Exception JSON: " << json.substr(0, 100) << "..." << std::endl;
    
    assert(json.find("\"exception_message\":\"Test exception message\"") != std::string::npos);
    assert(json.find("\"exception_type\":") != std::string::npos);
    assert(json.find("\"has_exception\":\"true\"") != std::string::npos);
    assert(json.find("\"operation\":\"test_op\"") != std::string::npos);
    assert(json.find("\"retry_count\":3") != std::string::npos);
    
    std::cout << "  Exception integration working correctly" << std::endl;
}

void test_exception_edge_cases() {
    std::cout << "Testing exception edge cases..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("exception_edge");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    // Test nullptr exception
    std::exception* null_ex = nullptr;
    logger.warn("Null exception test").exception(null_ex);
    
    // Test explicit nullptr
    logger.warn("Explicit null test").exception(nullptr);
    
    // Test exception without JSON (should work in TEXT format)
    try {
        throw std::invalid_argument("Invalid input");
    } catch (const std::exception& e) {
        logger.info("Exception in text format").exception(e);
    }
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 3);
    
    // Check null pointer handling
    assert(messages[0].find("exception=nullptr") != std::string::npos);
    assert(messages[0].find("exception_status=null_pointer") != std::string::npos);
    
    assert(messages[1].find("exception=nullptr") != std::string::npos);
    assert(messages[1].find("exception_status=explicit_null") != std::string::npos);
    
    // Check text format exception
    assert(messages[2].find("exception_message=Invalid input") != std::string::npos);
    assert(messages[2].find("has_exception=true") != std::string::npos);
    
    std::cout << "  Exception edge cases handled safely" << std::endl;
}

// ========== PATTERN FORMATTING TESTS ==========

void test_custom_patterns() {
    std::cout << "Testing custom pattern formatting..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("pattern_test");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    // FIXED: Use simple placeholders (format specifiers not yet implemented)
    logger.set_pattern("[{timestamp}] {level} | {logger} | {message}{tags}");
    
    logger.info("Custom pattern test")
          .tag("user", "alice")
          .field("count", 42);
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 1);
    
    const auto& formatted = messages[0];
    std::cout << "  Custom Pattern: " << formatted << std::endl;
    
    // Should contain our custom format elements
    assert(formatted.find("INFO") != std::string::npos);
    assert(formatted.find("pattern_test") != std::string::npos);
    assert(formatted.find("Custom pattern test") != std::string::npos);
    assert(formatted.find("user=alice") != std::string::npos);
    assert(formatted.find("count=42") != std::string::npos);
    // Should use our custom separators
    assert(formatted.find(" | ") != std::string::npos);
    
    std::cout << "  Custom pattern applied correctly" << std::endl;
}

void test_pattern_with_different_elements() {
    std::cout << "Testing pattern with different elements..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("element_test");
    logger.add_sink(capture.get_sink(), FormatType::TEXT);
    
    // Minimal pattern
    logger.set_pattern("{level}: {message}{tags}");
    
    logger.warn("Simple message")
          .field("importance", 5)
          .tag("category", "test");
    
    std::this_thread::sleep_for(20ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() == 1);
    
    const auto& simple = messages[0];
    std::cout << "  Simple Pattern: " << simple << std::endl;
    
    assert(simple.find("WARN: Simple message") != std::string::npos);
    assert(simple.find("importance=5") != std::string::npos);
    assert(simple.find("category=test") != std::string::npos);
    
    std::cout << "  Pattern elements working correctly" << std::endl;
}

// ========== INTEGRATION TESTS ==========

void test_all_features_together() {
    std::cout << "Testing all advanced features together..." << std::endl;
    
    OutputCapture capture;
    Logger<LogLevel::INFO> logger("integration_test");
    logger.add_sink(capture.get_sink(), FormatType::JSON);
    
    // Set up complex scenario
    logger.set_global_scope({{"service", "user_service"}, {"version", "2.0"}});
    logger.tag("environment", "production");
    logger.set_pattern("{timestamp} [{level}] {message}{tags}");
    
    {
        auto scoped = logger.with_scope({{"request_id", "req_123"}});
        
        {
            auto timer = logger.profile("user_update_operation", LogLevel::INFO)
                               .tag("operation_type", "update")
                               .field("user_id", 12345);
            
            try {
                // Simulate some work
                std::this_thread::sleep_for(30ms);
                
                // Simulate an error
                throw std::runtime_error("Database connection timeout");
                
            } catch (const std::exception& e) {
                logger.error("Operation encountered error")
                      .exception(e)
                      .field("elapsed_before_error", 30)
                      .field("retry_possible", true)
                      .tag("error_type", "database")
                      .sample(1.0)  // Ensure this error is logged
                      .json();
            }
            
            std::this_thread::sleep_for(20ms);
        } // ProfileTimer destructs here, logs completion
    }
    
    std::this_thread::sleep_for(50ms);
    logger.shutdown();
    
    auto messages = capture.get_messages();
    assert(messages.size() >= 2); // At least error message + profile completion
    
    // Find error message
    std::string error_msg;
    for (const auto& msg : messages) {
        if (msg.find("Operation encountered error") != std::string::npos) {
            error_msg = msg;
            break;
        }
    }
    assert(!error_msg.empty());
    
    std::cout << "  Error Message: " << error_msg.substr(0, 100) << "..." << std::endl;
    
    // Verify all features working together
    assert(error_msg.find("\"service\":\"user_service\"") != std::string::npos);     // Global scope
    assert(error_msg.find("\"environment\":\"production\"") != std::string::npos);  // Static tag
    assert(error_msg.find("\"request_id\":\"req_123\"") != std::string::npos);      // Scoped tag
    assert(error_msg.find("\"error_type\":\"database\"") != std::string::npos);     // Per-message tag
    assert(error_msg.find("\"elapsed_before_error\":30") != std::string::npos);     // Typed field
    assert(error_msg.find("\"retry_possible\":true") != std::string::npos);         // Boolean field
    assert(error_msg.find("\"exception_message\":") != std::string::npos);          // Exception integration
    assert(error_msg.find("\"has_exception\":\"true\"") != std::string::npos);     // Exception tag
    
    std::cout << "  All features integrated successfully!" << std::endl;
}

// ========== PERFORMANCE TESTS ==========

void test_advanced_features_performance() {
    std::cout << "Testing performance with advanced features..." << std::endl;
    
    std::atomic<int> message_count{0};
    Logger<LogLevel::INFO> logger("perf_test");
    logger.add_sink([&message_count](const std::string&) {
        message_count.fetch_add(1);
    }, FormatType::JSON);
    
    auto start = std::chrono::steady_clock::now();
    
    // High-volume logging with all features
    for (int i = 0; i < 10000; ++i) {
        if (i % 4 == 0) {
            logger.info("Performance test {}", i)
                  .field("iteration", i)
                  .field("batch_size", 1000)
                  .field("success_rate", 0.95)
                  .tag("test_type", "performance");
        } else if (i % 4 == 1) {
            // With profiling (but very short duration)
            auto timer = logger.profile("micro_operation")
                               .field("op_id", i);
            // Micro-operation simulation
        } else if (i % 4 == 2) {
            // With exception (simulated)
            logger.warn("Simulated warning {}", i)
                  .field("warning_code", 1001 + i % 100)
                  .tag("severity", "medium");
        } else {
            // Complex fields
            logger.debug("Complex data {}", i)
                  .field("data_size", i * 1024)
                  .field("compressed", true)
                  .field("compression_ratio", 0.67)
                  .sample(0.1); // Only log 10% of these
        }
    }
    
    std::this_thread::sleep_for(200ms);
    logger.shutdown();
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    assert(message_count.load() > 5000); // Should have processed most messages
    
    std::cout << "  Processed " << message_count.load() 
              << " messages with advanced features in " << duration.count() << "ms" << std::endl;
    std::cout << "  Average: " << (duration.count() * 1000.0 / message_count.load()) 
              << " microseconds per message" << std::endl;
}

// ========== MAIN TEST RUNNER ==========

int main() {
    std::vector<std::pair<std::string, std::function<void()>>> all_tests = {
        // Updated existing tests
        {"High Volume Logging", test_high_volume_logging},
        {"Concurrent Logging", test_concurrent_logging},
        {"Error Resilience", test_error_resilience},
        {"JSON Special Characters", test_json_special_characters},
        {"Very Long Messages", test_very_long_messages},
        {"Rapid Level Changes", test_rapid_level_changes},
        {"Shutdown Behavior", test_shutdown_behavior},
        {"Compile-time Performance", test_compile_time_performance},
        
        // New modern feature tests
        {"Scoped Tags", test_scoped_tags},
        {"Global Scope", test_global_scope},
        {"Multiple Format Sinks", test_multiple_format_sinks},
        {"CSV Formatting", test_csv_formatting},
        {"Sampling", test_sampling},
        {"Per-Message Sampling", test_per_message_sampling},
        {"Sampling Level Control", test_sampling_level_control},
        {"JSON Override with Formats", test_json_override_with_formats},
        {"Per-Message JSON Formatting", test_per_message_json_formatting},
        {"Simple Tag Priority", test_tag_priority_simple},
        {"Complex Tag Hierarchy", test_complex_tag_hierarchy},
        {"Performance Impact", test_performance_impact},

        // Structured Fields Tests
        {"Structured Fields Basic", test_structured_fields_basic},
        {"Structured Fields Text Format", test_structured_fields_text_format},
        {"Complex JSON Fields", test_complex_json_fields},

        // RAII Profiler Tests  
        {"Basic Profiling", test_basic_profiling},
        {"Profiling with Chaining", test_profiling_with_chaining},
        {"Nested Profiling", test_nested_profiling},
        
        // Exception Integration Tests
        {"Exception Integration Basic", test_exception_integration_basic},
        {"Exception Edge Cases", test_exception_edge_cases},
        
        // Pattern Formatting Tests
        {"Custom Patterns", test_custom_patterns},
        {"Pattern Elements", test_pattern_with_different_elements},
        
        // Integration Tests
        {"All Features Together", test_all_features_together},
        {"Advanced Features Performance", test_advanced_features_performance}
    };
    
    int passed = 0;
    TestTimer total_timer;
    
    std::cout << "=== Modern Logger Complete Test Suite ===" << std::endl;
    std::cout << "Running " << all_tests.size() << " tests...\n" << std::endl;
    
    for (auto& [name, test_func] : all_tests) {
        try {
            test_func();
            ++passed;
            std::cout << "✓ PASSED" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "✗ FAILED: " << e.what() << std::endl;
        } catch (...) {
            std::cout << "✗ FAILED: Unknown exception" << std::endl;
        }
        std::cout << std::endl;
    }
    
    auto total_time = total_timer.elapsed();
    
    std::cout << "=== Final Results ===" << std::endl;
    std::cout << "Passed: " << passed << "/" << all_tests.size() << std::endl;
    std::cout << "Total time: " 
              << std::chrono::duration_cast<std::chrono::milliseconds>(total_time).count() 
              << "ms" << std::endl;
    
    if (passed == static_cast<int>(all_tests.size())) {
        std::cout << "\n🎉 ALL TESTS PASSED! Logger is working perfectly!" << std::endl;
    }
    
    return (passed == static_cast<int>(all_tests.size())) ? 0 : 1;
}
