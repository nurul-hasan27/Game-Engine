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

        // Phase 5 integration: the application measures every frame it runs, so
        // the timing abstraction is genuinely wired into the main loop rather
        // than constructed and forgotten.
        if (application.time().frameCount() < kFramesToRun)
        {
            std::cerr << "Smoke test failed: only " << application.time().frameCount()
                      << " frame(s) were timed, expected at least " << kFramesToRun << '\n';
            return EXIT_FAILURE;
        }

        // The runtime exposes the world and the systems as accessors, so main()
        // and a future example can populate the engine without reaching inside.
        if (application.entityManager().aliveEntityCount() != 0 || application.systemManager().systemCount() != 0)
        {
            std::cerr << "Smoke test failed: a fresh application should start with an empty world and no systems\n";
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
