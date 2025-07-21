#include "logger.hpp"

int main(void) {

    util::Logger<util::LogLevel::INFO> logger("test_logger");

    logger.info("This is an info message")
        .tag("key1", "value1");
    
    return 0;
}