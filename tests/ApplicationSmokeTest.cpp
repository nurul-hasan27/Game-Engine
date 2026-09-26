#include "engine/Application.hpp"

#include <cstddef>
#include <exception>
#include <iostream>

namespace
{

/// The application must open its window, run a few frames of the main loop and
/// shut down cleanly. Anything less than that means Phase 1 is broken.
constexpr std::size_t kFramesToRun = 3;

} // namespace

int main()
{
    try
    {
        engine::Application application;

        if (application.run(kFramesToRun) != EXIT_SUCCESS)
        {
            std::cerr << "Smoke test failed: Application::run did not succeed\n";
            return EXIT_FAILURE;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Smoke test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
