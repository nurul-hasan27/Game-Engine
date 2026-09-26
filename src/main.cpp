#include "engine/Application.hpp"

#include <charconv>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace
{

/// Reads the optional `--frames <count>` argument.
///
/// This exists so the automated smoke test can run the main loop a fixed number
/// of times and shut down on its own, rather than waiting for a human to close
/// the window. Without the flag the application runs until the window closes.
std::optional<std::size_t> parseFrameLimit(const int argc, char* const argv[])
{
    constexpr std::string_view kFrameLimitFlag = "--frames";

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};

        if (argument != kFrameLimitFlag)
        {
            continue;
        }

        if (index + 1 >= argc)
        {
            throw std::invalid_argument{"--frames requires a value"};
        }

        const std::string_view value{argv[++index]};
        std::size_t frameCount = 0;
        const std::from_chars_result result =
            std::from_chars(value.data(), value.data() + value.size(), frameCount);

        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || frameCount == 0)
        {
            throw std::invalid_argument{"--frames expects a positive integer"};
        }

        return frameCount;
    }

    return std::nullopt;
}

} // namespace

int main(const int argc, char* const argv[])
{
    try
    {
        const std::optional<std::size_t> frameLimit = parseFrameLimit(argc, argv);

        engine::Application application;
        return application.run(frameLimit);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
