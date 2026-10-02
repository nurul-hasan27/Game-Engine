#include "engine/systems/DebugRenderSystem.hpp"

#include "engine/Color.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/level/LevelGrid.hpp"

#include <cmath>

namespace engine::systems
{

namespace
{

using engine::Vec2;

/// The colour a collider box is drawn in.
///
/// Deliberately not a colour that appears in the committed artwork. The whole
/// point of the overlay is to be unmistakable, and a box in a palette colour would
/// be mistaken for a sprite on a busy frame.
///
/// Opaque rather than translucent, because [engine::graphics::SfmlRenderer] sets no
/// blend mode: an alpha channel here would be silently ignored and the box would be
/// a wall of solid colour anyway. Drawing it opaque is honest about what appears on
/// screen, and it is why the box is a *diagnostic* view rather than something to
/// leave on - `C` twice puts the game back.
constexpr Color kBoundingBoxColor{1.0F, 0.0F, 1.0F, 1.0F};

/// The colour a grid line is drawn in. Dimmer than the boxes, so a box stays the
/// thing the eye goes to.
constexpr Color kGridLineColor{0.30F, 0.45F, 0.65F, 1.0F};

/// How thick a grid line is, in **world** pixels.
///
/// One world pixel, and deliberately not one screen pixel. A world-space thickness
/// means the grid is the same thing at every zoom, drawn through the same
/// transform as everything else: at zoom 2 the lines are two pixels apart *and*
/// two pixels wide, which is what a grid the level is authored on looks like
/// magnified. Making it screen-constant would need the system to undo the camera
/// before choosing a size, which is a second conversion rule in the one file whose
/// job is to have none.
constexpr float kGridLineThickness = 1.0F;

/// How many grid lines one axis will draw before it stops.
///
/// The grid is a picture of the *visible* world, so the count is a function of the
/// viewport and the zoom - and `graphics::Camera::setZoom` clamps only the lower
/// bound, at [kMinimumZoom](engine/graphics/Camera.hpp). At the clamp a 1280x720
/// view spans twelve million world pixels, which is two hundred thousand cell
/// boundaries and a frame that never finishes.
///
/// So the loops stop. It is a bound rather than a failure: at any zoom a human would
/// actually use the limit is never reached (zoom 0.05 needs 256 lines per axis at
/// 1280 pixels wide), and beyond it the grid shows the first lines it drew rather
/// than hanging the game. Recorded as a limitation here and in
/// [docs/debug.md](../../docs/debug.md) rather than left to be discovered.
constexpr int kMaxGridLinesPerAxis = 256;

/// Submits one world-space rectangle through the engine's single world-to-screen
/// conversion.
///
/// The temporary [engine::components::Transform] is the point rather than an
/// inconvenience: the debug overlay goes through
/// [toRenderTransform](engine/graphics/RenderTransform.hpp) exactly as a sprite
/// does, so it inherits the camera and the zoom for free and there is no second
/// place in the engine where a world position becomes a screen position.
void submitWorldBox(graphics::Renderer& renderer, const graphics::Camera& camera, const Color color,
                    const Vec2 centre, const Vec2 size)
{
    renderer.drawRectangle(size, color,
                           graphics::toRenderTransform(components::Transform{centre, Vec2{0.0F, 0.0F},
                                                                              Vec2{1.0F, 1.0F}, 0.0F},
                                                       camera));
}

} // namespace

void DebugRenderSystem::update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                               const float deltaSeconds)
{
    // Neither parameter is read. An overlay is a picture of a world that already
    // exists, so it does not read the keyboard and it does not integrate over time;
    // both are accepted only so every system in the engine shares one signature.
    static_cast<void>(actions);
    static_cast<void>(deltaSeconds);

    // Bounding boxes first, then the grid, which is the order they are drawn in
    // because [engine::scene::PlayScene] runs this pass *after* the ordinary render
    // pass: an overlay that went underneath the sprites would be invisible, which is
    // the one thing a debugging overlay cannot be. Within this pass the grid goes
    // on top of the boxes because it is the coarser of the two, and a hairline
    // under a box is a hairline nobody sees.
    if (m_state.showBoundingBoxes)
    {
        drawBoundingBoxes(entities);
    }

    if (m_state.showGrid)
    {
        drawGrid();
    }
}

void DebugRenderSystem::drawBoundingBoxes(engine::ecs::EntityManager& entities)
{
    // The query is the whole of the rule: a [engine::components::Collider] and a
    // [engine::components::Transform]. No collider means no box, and that is not a
    // special case - it is the same rule physics already uses when it decides what
    // can be collided with.
    //
    // It is what makes the overlay track the game's own geometry with no bookkeeping
    // at all. A brick that has exploded has had its collider removed, so it is out
    // of this query on the same frame it stops being solid and its box disappears
    // with it. A player, a bullet, a pipe and a coin all appear exactly when and
    // where their collider says.
    for (auto&& [entity, transform, collider] : entities.query<components::Transform, components::Collider>())
    {
        static_cast<void>(entity);

        // `collider.size`, not a size this file chose. A 70x70 pipe gets a 70x70
        // box, a 40x60 player gets a 40x60 box, and a debug box cannot disagree
        // with what physics collided with because it is reading the same number.
        m_renderer->drawRectangle(collider.size, kBoundingBoxColor, graphics::toRenderTransform(transform, *m_camera));
    }
}

void DebugRenderSystem::drawGrid()
{
    const graphics::Camera& camera = *m_camera;

    // The two corners of the *world* the view currently covers. `screenToWorld` is
    // the exact inverse of the mapping every draw goes through, so the rectangle
    // this produces is precisely the part of the world that can be on screen - and
    // at any zoom, because the inverse divides by it.
    const Vec2 topLeft = camera.screenToWorld(Vec2{0.0F, 0.0F});
    const Vec2 bottomRight = camera.screenToWorld(camera.viewport());

    // The course's cell size, read from the level grid rather than written down
    // again here. This is what makes the overlay *the level's* grid and not a second
    // grid that happens to be nearby: [engine::level::LevelGrid] is the one
    // definition of a cell, and a level's `Tile 5 3` means the cell whose left edge
    // is at world x 320 and whose bottom edge is one grid step below the top of the
    // world.
    const float cell = level::LevelGrid::kCellSize;

    // Each line reaches one cell past the visible rectangle at both ends. A line
    // that falls exactly on the edge of the view would otherwise be a hairline of
    // nothing, and a grid with a gap in it is worse than one line too many.
    const Vec2 span{bottomRight.x - topLeft.x + (cell * 2.0F), bottomRight.y - topLeft.y + (cell * 2.0F)};
    const Vec2 centre{(topLeft.x + bottomRight.x) * 0.5F, (topLeft.y + bottomRight.y) * 0.5F};

    // Snapped down to a cell boundary, so the first line drawn is the cell edge at or
    // before the view begins rather than an arbitrary offset from it. This is the
    // only origin the grid has, and it is the level's.
    int drawn = 0;
    for (float x = std::floor(topLeft.x / cell) * cell; x <= bottomRight.x && drawn < kMaxGridLinesPerAxis;
         x += cell, ++drawn)
    {
        submitWorldBox(*m_renderer, camera, kGridLineColor, Vec2{x, centre.y}, Vec2{kGridLineThickness, span.y});
    }

    drawn = 0;
    for (float y = std::floor(topLeft.y / cell) * cell; y <= bottomRight.y && drawn < kMaxGridLinesPerAxis;
         y += cell, ++drawn)
    {
        submitWorldBox(*m_renderer, camera, kGridLineColor, Vec2{centre.x, y}, Vec2{span.x, kGridLineThickness});
    }
}

} // namespace engine::systems