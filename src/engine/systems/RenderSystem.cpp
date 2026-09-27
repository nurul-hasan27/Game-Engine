#include "engine/systems/RenderSystem.hpp"

#include "engine/components/Rectangle.hpp"
#include "engine/components/Texture.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"

namespace engine::systems
{

void RenderSystem::update(engine::ecs::EntityManager& entities, input::Input& input, const float deltaSeconds)
{
    // Drawing is not time dependent and does not read the keyboard. Both
    // parameters are accepted only so every system shares one signature.
    static_cast<void>(input);
    static_cast<void>(deltaSeconds);

    // ---- Query 1: rectangles -------------------------------------------------
    //
    // Unchanged, and first. Every rectangle is submitted before any texture, so
    // textures end up drawn on top of rectangles.
    //
    // That ordering is a consequence of appending a second query rather than a
    // decision about layers. There is no z-order, no layer field and no sorting
    // anywhere in the engine, and inventing one here would mean a scheme with no
    // second user. Within each query the order is ECS iteration order, that is
    // creation order, exactly as it was before textures existed. The rectangle
    // query being unchanged also means its output is byte-for-byte what it always
    // was, so nothing that already renders moves.
    for (auto&& [entity, transform, rectangle] :
         entities.query<components::Transform, components::Rectangle>())
    {
        static_cast<void>(entity);
        // The one line where world space becomes screen space. The entity's
        // Transform is read, never written: it stays a world position however
        // the camera is configured.
        m_renderer->drawRectangle(rectangle.size, rectangle.color, graphics::toRenderTransform(transform, *m_camera));
    }

    // ---- Query 2: textures ---------------------------------------------------
    //
    // The same shape as the first: read the Transform, convert through the same
    // pure function, submit. The only difference is that a name has to be
    // resolved to an image first.
    //
    // A name that is not declared throws, and is deliberately not caught. An
    // entity that silently draws nothing is a bug that surfaces much later, as a
    // missing sprite with nothing pointing at the cause.
    for (auto&& [entity, transform, texture] :
         entities.query<components::Transform, components::Texture>())
    {
        static_cast<void>(entity);

        // Same conversion as the rectangle above, so a texture and a rectangle
        // with the same Transform land identically, zoom included. A name that is
        // not declared throws from the lookup, and is deliberately not caught: an
        // entity that silently draws nothing is a bug that surfaces much later,
        // as a missing sprite with nothing pointing at the cause.
        m_renderer->drawTexture(m_assets.texture(texture.assetName),
                                 graphics::toRenderTransform(transform, *m_camera));
    }
}

} // namespace engine::systems
