#include "engine/systems/RenderSystem.hpp"

#include "engine/components/Animation.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Texture.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"

#include <optional>

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
        // An entity that also animates is drawn by query 3 instead, as an
        // animation frame. Skipping it here is what makes it drawn exactly once;
        // see the note on that query for why the animation wins rather than the
        // plain texture.
        if (entity.hasComponent<components::Animation>())
        {
            continue;
        }

        // Same conversion as the rectangle above, so a texture and a rectangle
        // with the same Transform land identically, zoom included. A name that is
        // not declared throws from the lookup, and is deliberately not caught: an
        // entity that silently draws nothing is a bug that surfaces much later,
        // as a missing sprite with nothing pointing at the cause.
        //
        // No source region: a plain texture is drawn whole, exactly as it always
        // was. An entity that animates is drawn by the third query below instead,
        // which names a region.
        m_renderer->drawTexture(m_assets.texture(texture.assetName),
                                 graphics::toRenderTransform(transform, *m_camera), std::nullopt);
    }

    // ---- Query 3: animation frames -----------------------------------------
    //
    // Appended third, so animated entities are drawn after both plain textures
    // and rectangles, and therefore on top of them. That follows from being a
    // later query rather than from a decision about layers: there is still no
    // z-order, no layer field and no sorting anywhere in the engine.
    //
    // ### An animation wins over a plain texture
    //
    // An entity may hold both a `components::Texture` and a
    // `components::Animation`. Query 2 skips it, so it is drawn exactly once, by
    // this query, as an animation frame. Drawing it twice would put a whole sprite
    // sheet on screen behind a sprite of it, which is never what anything wants.
    //
    // The alternative - letting the plain texture win - would make adding an
    // animation component silently do nothing to an entity that already had a
    // texture, which is the more surprising of the two failures. Skipping here
    // rather than branching inside `components::Texture` keeps the two components
    // unaware of each other, and keeps the rule in one place: this query.
    for (auto&& [entity, transform, animation] : entities.query<components::Transform, components::Animation>())
    {
        static_cast<void>(entity);

        // Two steps, and deliberately so. The animation says which image and how
        // it is divided; the texture it names is a separate asset, looked up by
        // name. Resolving the texture once per frame per entity keeps the
        // alternative - an animation carrying a texture handle - out of the
        // component, where a copy of the artwork would exist for every entity.
        const assets::Animation& definition = m_assets.animation(animation.assetName);
        const assets::Texture& texture = m_assets.texture(definition.textureName());

        // An ended animation is on its final frame, which is the frame the course
        // wants seen before the entity goes. An empty asset - one naming an
        // animation that somehow has no frames - yields an empty region, which
        // draws nothing rather than the whole sheet.
        const IntRect source = definition.frameRect(animation.currentFrame);

        // Same conversion as the other two queries, so an animated sprite, a plain
        // sprite and a rectangle with the same Transform land identically, zoom
        // included.
        m_renderer->drawTexture(texture, graphics::toRenderTransform(transform, *m_camera), source);
    }
}

} // namespace engine::systems
