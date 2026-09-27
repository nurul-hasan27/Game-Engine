#pragma once

#include "engine/ecs/System.hpp"
#include "engine/graphics/Camera.hpp"

#include <string_view>

namespace engine::ecs
{
class EntityManager;
}

namespace engine::input
{
class Input;
}

namespace engine::systems
{

/// Points the camera at an entity's world position.
///
/// ### The rule this system exists to protect
///
/// **The camera moves. The entity does not.** This system reads the target's
/// `Transform::position` and writes the camera's position. It never writes the
/// `Transform`. A camera implemented by pushing entities around to fake a
/// scrolling view would break physics, because collision would then be computed
/// in screen space and would disagree with the moment the camera moved.
///
/// ### Finding the target
///
/// The target is identified by **tag**, and the tag is looked up **fresh every
/// frame** with `EntityManager::getEntities()`.
///
/// That is deliberate and it is the safest available option. Phase 3 documents
/// that an `Entity&` is borrowed from the manager and is invalidated by
/// `EntityManager::update()`, which erases and move-assigns entities. Caching an
/// `Entity&` in a system would therefore be a latent dangling reference that
/// happens to work until something is destroyed. Re-resolving by tag each frame
/// costs a short string comparison over the entity set and cannot dangle, because
/// the view and the reference it yields both live only inside this call.
///
/// The alternative, a persistent handle, would mean changing the ECS, and this
/// phase should not do that.
///
/// ### Missing or dead target
///
/// If no live entity carries the tag, the camera **keeps the position it had**
/// and the frame continues. That is not an error path to be avoided, it is the
/// ordinary case for a target that has not spawned yet or has just been
/// destroyed, and freezing the view is the only sane thing to do: snapping the
/// camera to the origin would make the world jump for no reason.
///
/// ### Ordering
///
/// Must be registered **after** `MovementSystem` and `PhysicsSystem`, and before
/// rendering. The camera should follow where the target ended up this frame, not
/// where it was at the start of it.
///
/// `input` and `deltaSeconds` are accepted and ignored. Following a target needs
/// neither the keyboard nor a duration: the target's position is already
/// wherever physics put it.
///
/// There is no smoothing, no interpolation, no damping, no spring and no shake.
/// The camera is placed exactly on the target. See [docs/camera.md](../../docs/camera.md).
class CameraSystem final : public engine::ecs::System
{
public:
    /// @param camera The camera to drive. Referenced, not owned: `Application`
    ///        owns it, and a system must not own the thing it renders.
    /// @param targetTag Tag of the entity to follow. The tag must outlive this
    ///        system. An empty tag matches every entity, so an empty tag would
    ///        follow the first one created rather than nothing.
    CameraSystem(graphics::Camera& camera, std::string_view targetTag) noexcept
        : m_camera{&camera}, m_targetTag{targetTag}
    {
    }

    void update(engine::ecs::EntityManager& entities, input::Input& input, float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "CameraSystem"; }

    /// The tag being followed. Diagnostics and tests only.
    [[nodiscard]] std::string_view targetTag() const noexcept { return m_targetTag; }

private:
    graphics::Camera* m_camera = nullptr;
    std::string_view m_targetTag;
};

} // namespace engine::systems
