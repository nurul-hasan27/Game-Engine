#include "engine/systems/RenderSystem.hpp"

#include "engine/components/Animation.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Text.hpp"
#include "engine/components/Texture.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"

#include <optional>

namespace engine::systems
{

namespace
{

/// The colour text is drawn in when the component does not say.
///
/// A named constant rather than a literal buried in the draw call, so that the
/// question "what colour is text?" has one answer in the engine instead of one per
/// call site. [engine::kWhite](engine/Color.hpp) is right because it is the
/// graphics library's own default for a text fill, so an implementation that did
/// nothing at all would produce the same picture.
constexpr Color kDefaultTextColor{1.0F, 1.0F, 1.0F, 1.0F};

} // namespace

void RenderSystem::update(engine::ecs::EntityManager& entities, const input::ActionState& actions, const float deltaSeconds)
{
    // Drawing is not time dependent and does not read the keyboard. Both
    // parameters are accepted only so every system shares one signature.
    static_cast<void>(actions);
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

    // ---- Query 4: text ------------------------------------------------------
    //
    // Appended fourth, so strings are drawn after rectangles, plain textures and
    // animation frames, and therefore on top of them. That is the same consequence
    // of being a later query that query 3 is, and for the same reason: there is no
    // z-order, no layer field and no sorting anywhere in this engine. A label that
    // has to sit behind a sprite is a question about layers, and answering it would
    // mean building a scheme with exactly one user.
    //
    // ### A string does not skip an entity that also has something else to draw
    //
    // Query 2 skips an entity that animates, and the reason is specific: a
    // `Texture` and an `Animation` on one entity would draw **the same artwork
    // twice**, once whole and once as a frame, and skipping is what makes it drawn
    // exactly once. Text and a `Rectangle` or an `Animation` draw *different* things
    // - a label and a box behind it, or a label on a sprite - and drawing both is
    // exactly what was asked for. So there is no skip rule here, and none is needed:
    // the query order above already decides which of them is on top.
    for (auto&& [entity, transform, text] : entities.query<components::Transform, components::Text>())
    {
        static_cast<void>(entity);

        // The font is resolved by name, exactly as a texture and an animation are.
        // A name that is not declared throws from the lookup and is deliberately not
        // caught, for the reason the other two queries give: a label that silently
        // draws nothing is a bug that surfaces much later as missing text with
        // nothing pointing at the cause.
        //
        // `components::Text` carries no colour, which is the same decision
        // `Texture` and `Animation` make - they draw from an asset rather than being
        // a coloured primitive. The renderer still takes one, the way
        // `drawRectangle` does, so the default lives here as a named constant
        // rather than being buried in the graphics layer. Nothing has asked for
        // coloured text, and the course does not ask for it.
        m_renderer->drawText(m_assets.font(text.fontAssetName), text.content, text.characterSize, kDefaultTextColor,
                             graphics::toRenderTransform(transform, *m_camera));
    }
}

} // namespace engine::systems
