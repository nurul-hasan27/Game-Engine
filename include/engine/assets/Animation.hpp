#pragma once

#include "engine/math/IntRect.hpp"

#include <cstdint>
#include <string>

namespace engine::assets
{

/// A loaded, immutable **definition** of an animation: which texture to slice,
/// into how many frames, and how fast to play them.
///
/// ### What an animation is
///
/// The course defines an animation as several frames stored inside one texture,
/// displayed one at a time by changing which part of the texture is drawn. That
/// makes an animation a *description of a region of an image*, and that is all
/// this type holds:
///
/// ```text
/// Texture   mario_run     library/images/megaman/megaRun.png
/// Animation mario_running mario_run 3 8
/// ```
///
/// resolves to "the image called `mario_run`, cut into 3 equal columns, advancing
/// every 8 game frames".
///
/// ### What it is not: playback state
///
/// There is deliberately **no** current frame, no tick counter and no `repeat`
/// flag here, and their absence is the single most important thing about this
/// design.
///
/// A loaded asset is shared. Two entities playing `mario_running` share one
/// `Animation` object, exactly as they share one `Texture`. If the current frame
/// lived here, they would share a frame index, so two entities would be locked
/// into playing in lockstep and there would be no way to make them differ except
/// by copying the whole asset per entity - which is what the course's reference
/// implementation does, storing an `sf::Sprite` inside every entity's animation
/// component. That is the pattern this engine's components-as-data rule exists to
/// avoid.
///
/// So playback state lives in [engine::components::Animation], one per entity,
/// and this type stays immutable. There are no setters, and the accessors return
/// `const&` or by value.
///
/// ### `repeat` is not here, and is not in the configuration either
///
/// The course's asset-file format is `Animation <name> <texture> <frames>
/// <speed>` and has no repeat field, and the course is explicit that repeat is a
/// property of the *entity*, not of the artwork: a brick's explosion animation
/// plays once, and the identical animation attached to a coin spins forever. The
/// format therefore cannot express it, this asset cannot hold it, and it lives
/// with the playback state in the component.
///
/// ### Frame layout
///
/// Frames are a single horizontal row of equal width filling the full height,
/// which is the layout the course describes and the only one the four-field
/// format can express. There is no `columns` field, so a grid layout is not
/// representable, and adding one would be a documented deviation from the
/// assignment.
///
/// `frameWidth` and `frameHeight` are **resolved at load time** from the real
/// image rather than computed by each caller, and the loader refuses any frame
/// count that does not divide the texture's width exactly. That check is the
/// reason this type can offer `frameRect()` as a total function: a caller cannot
/// get a frame rectangle for an animation whose frames do not line up, because
/// such an animation never loads.
///
/// Storing the resolved size also satisfies the course's requirement that a
/// tile's collision box is the size of its animation - `getSize()` in the
/// reference - without any caller having to reach into the image to find out how
/// big a frame is.
///
/// ### Copyable, because it owns nothing
///
/// This is plain data: a name and four integers. It is copyable, unlike
/// [Texture](Texture.hpp) and [Font](Font.hpp), because those own a platform
/// resource and a copy would either duplicate it or share it behind two owners'
/// backs. Nothing here owns anything, so a copy is a copy and nothing more.
///
/// It is still only ever *reached* by const reference from
/// [AssetManager](AssetManager.hpp), for the same reason textures are: one shared
/// instance per declared name.
class Animation
{
public:
    /// An animation that refers to nothing.
    ///
    /// A real state rather than a placeholder: a manager that has not loaded a
    /// name has to be able to hand back something honest. Nothing in this type
    /// interprets it, and the draw path rejects it rather than drawing nothing.
    Animation() = default;

    /// Builds a definition. Called only by the asset manager, which is the only
    /// thing that can supply a real frame size.
    ///
    /// The values are not validated here. `frameCount` and `speed` are checked by
    /// the parser, which is where a configuration mistake belongs, and the frame
    /// size is checked by the loader against the image it came from. This
    /// constructor only records what both of them already agreed on.
    Animation(std::string texture, const std::uint32_t frames, const std::uint32_t framesPerSecond,
              const int frameWidth, const int frameHeight)
        : m_textureName{std::move(texture)},
          m_frameCount{frames},
          m_speed{framesPerSecond},
          m_frameWidth{frameWidth},
          m_frameHeight{frameHeight}
    {
    }

    /// Name of the `Texture` asset this animation slices.
    ///
    /// A reference to another asset, never a path. The manager has already
    /// resolved it to a loaded texture, so a caller that wants the image asks the
    /// manager for `texture(animation.textureName())`.
    [[nodiscard]] const std::string& textureName() const noexcept { return m_textureName; }

    /// How many frames the texture is divided into. Always at least 1.
    [[nodiscard]] std::uint32_t frameCount() const noexcept { return m_frameCount; }

    /// **Game frames** between animation frames. Always at least 1.
    ///
    /// The course defines this in game frames, not seconds, and this engine keeps
    /// that meaning rather than quietly converting it: a `speed` of 8 means the
    /// frame changes on every eighth call to a system's `update`, whatever the
    /// wall-clock frame rate happens to be. See
    /// [engine::systems::AnimationSystem](engine/systems/AnimationSystem.hpp) for
    /// what one game frame is here.
    [[nodiscard]] std::uint32_t speed() const noexcept { return m_speed; }

    /// Width of one frame, in pixels, resolved from the loaded image.
    [[nodiscard]] int frameWidth() const noexcept { return m_frameWidth; }

    /// Height of one frame, in pixels. Equal to the texture's height, because
    /// frames are a single row.
    [[nodiscard]] int frameHeight() const noexcept { return m_frameHeight; }

    /// True when `frame` names a frame this animation actually has.
    ///
    /// Provided so a caller can check rather than assume. `frameRect()` does not
    /// check for its caller, because the playback state in
    /// [engine::components::Animation] keeps the index in range by construction
    /// and a check inside the draw path would be a per-entity branch guarding an
    /// invariant that is already maintained.
    [[nodiscard]] bool isValidFrame(const std::uint32_t frame) const noexcept { return frame < m_frameCount; }

    /// The region of the texture that shows `frame`.
    ///
    /// Frames run left to right in one row, so the region's left edge is
    /// `frame * frameWidth` and its top edge is always zero.
    ///
    /// @param frame A frame index. It must satisfy [isValidFrame](Animation.hpp);
    ///        a default-constructed `Animation` has no valid frames and returns an
    ///        empty rect for any of them, so a caller that skipped the check gets
    ///        nothing drawn rather than a wrong frame.
    [[nodiscard]] IntRect frameRect(const std::uint32_t frame) const noexcept
    {
        if (!isValidFrame(frame))
        {
            return {};
        }

        return IntRect{static_cast<int>(frame) * m_frameWidth, 0, m_frameWidth, m_frameHeight};
    }

private:
    std::string m_textureName;
    std::uint32_t m_frameCount = 0;
    std::uint32_t m_speed = 0;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
};

} // namespace engine::assets
