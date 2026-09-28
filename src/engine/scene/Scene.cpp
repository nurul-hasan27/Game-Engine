#include "engine/scene/Scene.hpp"

#include "engine/scene/MenuScene.hpp"
#include "engine/scene/PlayScene.hpp"

#include <memory>
#include <string>

namespace engine::scene
{

std::unique_ptr<Scene> makeScene(const SceneId id, const SceneContext& context)
{
    // A switch, deliberately, and not a registry.
    //
    // A registry would be a global table mapping ids to factories, which is a
    // singleton with extra steps and would make "which scenes exist" a question
    // answered by a data structure rather than by reading a function. A switch is
    // the whole list, it is checked by the compiler when a case is added, and
    // there is nothing to initialise and nothing that can be half-registered.
    switch (id)
    {
        case SceneId::Menu:
            return std::make_unique<MenuScene>(context);

        case SceneId::Play:
            return std::make_unique<PlayScene>(context);
    }

    // Unreachable for a value of the enum, and not silently ignored if one ever
    // appears. A scene id nobody can build is a bug, and returning an empty scene
    // would turn it into a blank screen instead.
    throw std::invalid_argument{"makeScene called with an unknown scene id " + std::to_string(static_cast<int>(id))};
}

} // namespace engine::scene
