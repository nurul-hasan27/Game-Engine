#pragma once

#include <cstdint>
#include <string>

namespace engine::components
{

/// A string to draw, and the font to draw it in.
///
/// ### What the course actually asks for
///
/// Almost nothing, and this component is shaped by that rather than by what a
/// text object *could* hold. Assignment 3 specifies fonts as an asset type -
/// *"Entities in the game will be rendered using various Textures and Animations
/// which we will be calling Assets (along with Fonts)"*, and a `Font N P` line in
/// the asset file - and stops there. It says nothing about a text component, a
/// character size, a colour, an alignment or an origin.
///
/// The only concrete text anywhere in the course corpus is the course reference's
/// own use, and it uses exactly three things: a string, a font, and a character
/// size. From `Scene_Play`, drawing the grid overlay:
///
/// ```cpp
/// m_gridText.setFont(m_game->getAssets().getFont(eFontTypes::NUMBERS));
/// m_gridText.setCharacterSize(12);
/// m_gridText.setString("(" + std::to_string(x / 64) + "," + ... + ")");
/// ```
///
/// and from `Scene_Menu`, character sizes of 52 for a title and 12 for items.
/// So those three are what this holds, and each field below says which of them it
/// is.
///
/// ### The reference does it differently, and this engine does not copy it
///
/// The reference has no text component at all. Its text is an `sf::Text` **member
/// of a scene** - `sf::Text m_gridText;`, `sf::Text m_menuText;` - drawn by calling
/// `window().draw(m_gridText)` directly.
///
/// That is precisely the arrangement this engine does not have. A scene does not
/// exist here, a system does not own graphics objects, and an `sf::Text` is a
/// graphics type. Putting one in a component would put SFML in the ECS, and
/// putting one in a system would make a system own a drawable. The engine's
/// established answer is a plain data component that *names* its font, exactly as
/// [Texture](Texture.hpp) and [Animation](Animation.hpp) name theirs, with the
/// resolution and the drawing both on the far side of a boundary.
///
/// ### Why there is no colour, alignment or origin here
///
/// Each was considered and each was left out, for a reason rather than by omission.
///
/// - **No colour.** `Texture` and `Animation` carry no colour, and text is in the
///   same family: it draws from a font rather than being a coloured primitive.
///   `Rectangle` is the one visual component with a colour, and the reason is that
///   a filled rectangle has *nothing else* to be orange or blue with. Nothing has
///   asked for coloured text yet, and the course does not. The renderer still takes
///   a colour, so adding one later is a field on this component and nothing else.
/// - **No alignment.** The reference never sets one, and it has no use yet. Left to
///   SFML's default, which is left-aligned.
/// - **No origin.** [Transform](Transform.hpp) has a position, and the renderer's
///   convention is that a position is the **centre** of the thing drawn - the
///   engine's rule from Lecture 11 section 5, and what `drawRectangle` and
///   `drawTexture` already do. A second origin field would be a second answer to a
///   question that has one, and the two could disagree.
/// - **No scale.** [Transform](Transform.hpp) has it, and it means the same thing
///   for text as for everything else. See `characterSize` for why that is not
///   enough on its own.
/// - **No position.** Same reason, and same component.
struct Text
{
    /// The string to draw.
    ///
    /// An empty string draws nothing, and that is a real answer rather than an
    /// error: measured against the committed fonts, an empty `sf::Text` measures
    /// 0x0, so there is nothing to place and nothing to draw. A caller that wants
    /// text to disappear sets the content to nothing, which is what a label with
    /// no value *is*.
    std::string content;

    /// The name of the **font** asset to draw it in, not the name of a texture.
    ///
    /// By name, and never a handle, for the same reason
    /// [Texture::assetName](Texture.hpp) is a name: a component refers to an asset
    /// and resolves it at the point of use, so a font's glyph cache is not copied
    /// into every entity that mentions it. Resolution goes through the
    /// [engine::assets::AssetManager], and a name that is not declared throws - the
    /// same rule every other asset name in this engine follows.
    ///
    /// A *font* and not a texture, deliberately. A font is a different asset type
    /// with a different loader, and the engine already keeps the two apart.
    std::string fontAssetName;

    /// The character size to request from the font, in points.
    ///
    /// ### This is not the same thing as [Transform::scale](Transform.hpp)
    ///
    /// The two do different jobs, and conflating them is the easy mistake here:
    ///
    /// | | `characterSize` | `Transform::scale` |
    /// | --- | --- | --- |
    /// | what it is | a font request | a geometric multiplier |
    /// | when it applies | **before** rasterization | **after** |
    /// | what it does | picks a glyph outline for that size | moves the drawn result |
    /// | applies to | glyphs only | every renderable, identically |
    ///
    /// Asking a font for size 24 and scaling the result by 2 are not the same
    /// picture. Scaling a rasterized glyph enlarges its pixels and leaves the hinting
    /// and spacing that were chosen for the original size; requesting a different
    /// size makes the rasterizer choose again. The course's reference relies on
    /// exactly this: a 52-point menu title and a 12-point debug label come from one
    /// font by asking for two sizes.
    ///
    /// So both exist. `characterSize` chooses the glyphs, and
    /// [Transform::scale](Transform.hpp) - which text reads like every other
    /// renderable - moves and enlarges the result, in the same units and with the
    /// same camera behaviour as a sprite.
    ///
    /// `unsigned` because a character size is a whole number of points and a
    /// negative or fractional one is not a thing a font can be asked for. The
    /// default of 24 is the size a 1280x720 window wants for a label; nothing in
    /// the course states one, so this is a documented default rather than a
    /// requirement.
    std::uint32_t characterSize = 24U;
};

} // namespace engine::components
