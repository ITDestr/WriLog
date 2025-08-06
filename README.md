
# WriLog

WriLog is a logging library for C++20.  It is designed for structured logging without dependencies.  WriLog supports asynchronous processing, compile‑time level filtering, structured fields, JSON/CSV/Text output formats and profiling.  
It offers a flexible tagging system with global, static, scoped and per‑message tags and supports sampling to reduce log volume under load.

## Features

- **Asynchronous logging** – messages are queued and processed in a background thread so that the application threads are not blocked while writing logs.
- **Structured logging** – attach arbitrary key/value pairs as typed fields or tags to each log entry.  Fields preserve numeric and boolean types in JSON, while tags are always strings.
- **Flexible output formats** – built‑in sinks support.  You can add your own sinks (for example, write to files, sockets or in‑memory buffers) and specify the output format per sink.
- **RAII profiling** – measure the duration of a scoped operation and automatically log it on destruction, with support for additional fields and tags.
- **Sampling** – built‑in random and deterministic sampling allows you to reduce log volume by dropping a fraction of messages, either globally or per log call.
- **Custom patterns** – define your own text formatting pattern using placeholders such as `{timestamp}`, `{level}`, `{logger}`, `{message}` and `{tags}`.
- **Tag hierarchy** – combine global scope tags, static tags, scoped tags and per‑message tags with well‑defined priority (per‑message > scoped > static > global).  Scoped tags follow RAII semantics and automatically apply to all log calls within a scope.
- **Thread‑safe** – all internal data structures are protected by mutexes or atomic operations.  Multiple threads may concurrently log without contention for sinks.

## Requirements

WriLog requires a C++20 compiler with support for the `<format>` header and `<chrono>` time facilities.  It has no third‑party dependencies.  A typical build command using GCC or Clang looks like:

```sh
g++ -std=c++20 -pthread -I path/to/wrilog
```

The provided `Makefile` compiles the test suite into an executable called `logger_tests` and a simple example called `main`. 

## Quick start

Here are some minimal example that demonstrates the basic usage of WriLog:

```cpp
#include "logger.hpp"

int main() {
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
        auto timer = logger.profile("database_query", util::LogLevel::INFO)
                            .tag("table", "users")
                            .field("expected_rows", 1000);

        
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(1337));
    }

    // Shutdown the logger to flush queued messages before exiting
    logger.shutdown();
    return 0;
}
```

Compile and run this program to see messages printed to the standard error stream.  The first call emits a human‑readable text line.  The second call uses `.json()` to override the sink’s format and outputs a single JSON object.  The final RAII profiling block logs the elapsed time with automatically added fields such as `duration_ms` and `operation_name`.

## Configuration

### Log levels

Log levels are defined by the `LogLevel` enum: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR` and `FATAL`.  When instantiating a `Logger` you provide a compile‑time minimum level (default is `INFO`).  Messages below this level are compiled out entirely.  At run time you can call `set_level()` to raise or lower the threshold:

```cpp
util::Logger<util::LogLevel::INFO> logger("module");
logger.set_level(util::LogLevel::DEBUG); // allow DEBUG and above
```

### Sinks and formats

WriLog writes each log record to one or more **sinks**.  A sink is a simple `std::function<void(const std::string&)>` which receives the formatted message.  You can add sinks via `add_sink()`, specifying the desired format:

```cpp
std::ofstream file("app.log");
logger.add_sink([&file](const std::string& line) {
    file << line << '\n';
}, util::FormatType::TEXT);

logger.add_sink([](const std::string& json) {
    send_to_remote(json);
}, util::FormatType::JSON);
```

Available formats are:

| Format      | Description                                               |
|-------------|-----------------------------------------------------------|
| `TEXT`      | Human‑readable text respecting the configured pattern.    |
| `JSON`      | Structured JSON object preserving numeric types.          |
| `CSV`       | Comma‑separated values: timestamp, level, logger, message and combined fields/tags. |

### Patterns

The **pattern** controls how text messages are formatted.  It is a string containing placeholders which are replaced at run time.  The default pattern is:

```
[{timestamp}] {color}{level}{color_reset} [{logger}] {message}{tags}
```

Placeholders include:

- `{timestamp}` – local date/time in `YYYY‑MM‑DD HH:MM:SS` format.
- `{level}` – log level name.
- `{color}` / `{color_reset}` – ANSI colour codes; remove these if you do not want coloured output.
- `{logger}` – the name passed to the `Logger` constructor.
- `{message}` – the formatted message.
- `{tags}` – a space‑prefixed list of `key=value` pairs derived from fields and tags, enclosed in braces.

You can call `set_pattern()` to customise the pattern:

```cpp
logger.set_pattern("{level}: {message}{tags}");
```

### Tags and fields

*Tags* are untyped key/value pairs (both strings) attached to a log record.  *Fields* are typed values (int, float, double, bool, string or `string_view`) that retain their type when output in JSON format.  Tags and fields are added fluently on the `LogEntryBuilder` returned by log calls:

```cpp
logger.info("User {} logged in", username)
      .tag("user", username)
      .field("user_id", 42)
      .field("success", true);
```

#### Tag hierarchy

WriLog merges tags from several sources in a defined order:

1. **Global scope tags** – set once on the logger with `set_global_scope()` or `add_global_scope_tag()`.  They apply to all log records.
2. **Static tags** – set with `tag()` on the logger itself; override global scope.
3. **Scoped tags** – created via `with_scope()`; apply to all log calls within a lexical scope and override static tags.
4. **Per‑message tags** – added on the log call; override all other tags.

### Sampling

Sampling allows you to drop a percentage of log records.  Sampling can be configured globally on the logger or per message.  Supported strategies:

- **Random sampling**: log only a random fraction of records.  Example: log roughly 10% of `INFO` and above messages:

  ```cpp
  logger.set_sampling(util::SamplingConfig(util::SamplingType::RANDOM, 0.1, util::LogLevel::INFO));
  ```

- **Every‑nth sampling**: log every `N`‑th message.  Example: log every 50th debug message:

  ```cpp
  logger.debug("Debug message").every_nth(50);
  ```

You can also apply sampling to individual log calls using `.sample(rate)` or `.every_nth(n)` on the builder.  The `min_level` parameter determines the minimum level at which sampling applies; messages below that level bypass sampling.

### RAII Profiling

Profiling is implemented through a RAII class returned by `profile()`.  The timer captures the start time on construction and logs the elapsed time on destruction.  You can chain tags and fields on the timer just like on a log call.  Profiling messages include automatically added fields:

| Field            | Description                                 |
|------------------|---------------------------------------------|
| `duration_ms`    | Duration of the scoped block in milliseconds |
| `operation_name` | String passed to `profile()`                 |

An example of profiling a database query:

```cpp
{
    auto timer = logger.profile("database_query")
                       .tag("table", "users")
                       .field("expected_rows", 5000);
    // perform query
}
```

## Running the test suite

The `logger_tests.cpp` file contains an extensive suite of unit tests covering high‑volume logging, concurrency, sampling, tag hierarchy, JSON formatting, structured fields, RAII profiling, exception integration and performance.  To build and run the tests:

```sh
g++ -std=c++20 -pthread -O2 logger_tests.cpp -o logger_tests
./logger_tests
```

When executed, the test runner will shuffle and run all tests, reporting pass/fail status and timings.  The tests should all pass on a standards‑conformant C++20 compiler.

## Roadmap and potential improvements

While WriLog already provides a useful feature set, there are several areas that could be explored to further enhance its capabilities:

* **Format specifiers in patterns** – support width, alignment and truncation in pattern placeholders using the `std::format` syntax.
* **Custom time formatting** – allow specifying time zones or high‑resolution timestamps (microseconds).
* **Log rotation** – built‑in support for rotating files based on size or time and optional compression of old log files.
* **Lock‑free queues or batching** – reduce contention and improve throughput by using lock‑free data structures or batching multiple log records per write.
* **More field types** – extend `FieldValue` to support custom types via `std::any` or `std::variant` expansions, with user‑supplied converters.
* **Automatic context capture** – macros that automatically capture source file, line number and function name without requiring manual tags.
* **Pluggable sampling strategies** – implement adaptive sampling that changes rates based on load or error conditions.
* **CMake integration** – provide a `CMakeLists.txt` and install targets to ease integration into existing projects.

Contributions are welcome!  Feel free to open issues or pull requests to discuss enhancements or report bugs.

## License

This project is distributed under the [MIT License](LICENSE).  See the `LICENSE` file for details.