#pragma once

#include "engine/debug/DebugRenderState.hpp"
#include "engine/ecs/System.hpp"
#include "engine/graphics/Renderer.hpp"

namespace engine::ecs
{
class EntityManager;
}

namespace engine::graphics
{
class Camera;
}

namespace engine::systems
{

/// Draws the two overlays that are pictures of the world rather than part of it: the
/// collider boxes and the level grid.
///
/// ### Why this is a system and not a scene
///
/// It shares [engine::ecs::System]'s interface - it is handed the world, this
/// frame's actions and the frame duration - so it is driven, owned and tested
/// exactly like every other behaviour in the engine. But like
/// [engine::systems::RenderSystem] it is deliberately **not** registered in the
/// [engine::ecs::SystemManager]: it draws, so it belongs to the render pass rather
/// than to the simulation, and a render pass cannot live inside a simulation list.
/// See [docs/rendering.md](../../docs/rendering.md) §4 for why that is true of the
/// render pass as a whole.
///
/// ### What it draws
///
/// | Flag | What appears |
/// | ---- | ------------ |
/// | `showBoundingBoxes` | one rectangle per [engine::components::Collider], at the collider's own size and the transform's own position |
/// | `showGrid` | the 64-pixel [engine::level::LevelGrid] cell boundaries across the visible world |
///
/// Both are world space, submitted through the **same**
/// [toRenderTransform](engine/graphics/RenderTransform.hpp) every other draw uses,
/// so both move with the camera and scale with its zoom without either knowing a
/// camera exists. There is no second coordinate system and no second origin: the
/// grid's lines are the cell edges the level's own coordinates already name.
///
/// ### What it deliberately does not do
///
/// - **It does not change collision.** Nothing here writes a component, and no
///   gameplay system can see this class. A world with the grid showing collides
///   exactly as it does with it hidden, which is what "purely visual" has to mean
///   if it is to mean anything.
/// - **It does not duplicate the collider's geometry.** It reads
///   [engine::components::Collider::size] rather than naming a size, so a 70x70
///   pipe and a 40x60 player each get their own box and an entity with no collider
///   gets nothing. A hard-coded `64` here would be a second, disagreeing copy of
///   the geometry physics is using.
/// - **It does not draw a box for an entity whose collider has been removed.** The
///   query requires a [engine::components::Collider], so a brick that has exploded
///   - and has had its collider taken away, which is what makes it no longer solid -
///   loses its box in the same frame it stops being solid, with no bookkeeping.
/// - **It names no game concept.** "Bounding box" and "grid" are the two words, and
///   neither is a player, a bullet or a level. A `drawPlayerBoundingBox()` here
///   would be the first step towards a renderer that knows what a Mega Man is.
/// - **It reads no input.** The flags arrive as configuration, so there is no path
///   by which a debug overlay can become a control scheme.
class DebugRenderSystem final : public engine::ecs::System
{
public:
    /// @param renderer The graphics boundary. Borrowed; the window outlives it.
    /// @param camera The view, **const**, for the reason
    ///        [engine::systems::RenderSystem] holds it const: a debug overlay that
    ///        could move the camera would be a very hard bug to see, because it
    ///        would look like a game that pans on its own.
    /// @param state The flags this pass reads. **Borrowed and not owned**, so the
    ///        scene's single state object is what both renderers agree on - there
    ///        is one answer to "are the overlays on this frame", not two copies that
    ///        could disagree. The state must outlive this system.
    DebugRenderSystem(graphics::Renderer& renderer, const graphics::Camera& camera,
                      const debug::DebugRenderState& state) noexcept
        : m_renderer{&renderer}, m_camera{&camera}, m_state{state}
    {
    }

    /// Draws whichever overlays the state asks for, and nothing else.
    void update(engine::ecs::EntityManager& entities, const input::ActionState& actions, float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "DebugRenderSystem"; }

    /// The state this system reads, for tests and diagnostics.
    [[nodiscard]] const debug::DebugRenderState& state() const noexcept { return m_state; }

private:
    /// One rectangle per collider, at the collider's own size.
    void drawBoundingBoxes(engine::ecs::EntityManager& entities);

    /// The cell boundaries across the visible part of the world.
    void drawGrid();

    graphics::Renderer* m_renderer = nullptr;
    const graphics::Camera* m_camera = nullptr;

    /// A reference and not a nullable pointer, for the reason
    /// [engine::systems::RenderSystem] gives for the asset manager: there is no valid
    /// empty state here. The flags are a construction requirement - a debug renderer
    /// that had none would not know whether to draw anything, and every answer to
    /// that is wrong somewhere. A pointer would be a member that could only be null
    /// by mistake, with a null check at every use for a case the type should have
    /// made impossible.
    const debug::DebugRenderState& m_state;
};

} // namespace engine::systems