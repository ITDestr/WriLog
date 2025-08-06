#include "logger.hpp"

int main(void) {
    // Create a logger with compile‑time minimum level INFO
    util::Logger<util::LogLevel::INFO> logger("app");

    // Log a simple message with tags
    logger.info("User {} logged in", "alice").tag("user_id", "42");

    // Add typed fields and switch to JSON format for this message only
    logger.warn("Disk space low on {}", "/dev/sda1")
          .field("free_gb", 1.2)
          .field("threshold_gb", 5.0)
          .json();

    // Profile an operation and automatically log its duration on destruction
    {
        auto timer = logger.profile("database_query", util::LogLevel::WARN)
                            .tag("table", "users")
                            .field("expected_rows", 1000);
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(1337));
    }

    {
        auto outer_scope = logger.with_scope({{"request_id", "req_123"}, {"user", "bob"}});
        logger.info("Scoped message with tags")
              .field("action", std::string("login"))
              .tag("status", "success");

        {
            auto inner_scope = logger.with_scope({{"operation", "fetch_data"}, {"request_id", "req_456"}});

            auto nested_timer = logger.profile("nested_operation", util::LogLevel::DEBUG)
                                    .tag("operation", "fetch_data")
                                    .field("data_size", 2048);
            // Simulate nested operation
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        logger.info("Back to outer scope")
              .field("outer_action", std::string("complete"))
              .tag("outer_status", "done");
    }

    // Final shutdown to ensure all messages are processed

    logger.info("Application shutdown")
          .tag("shutdown_reason", "normal");

    logger.shutdown();
    return 0;
}