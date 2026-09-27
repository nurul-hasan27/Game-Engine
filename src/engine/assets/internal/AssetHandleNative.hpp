#pragma once

// Internal to the SFML-backed asset implementation.
//
// This is the only place in the asset system where the concrete graphics types
// are named, and it is deliberately NOT on the public include path. It lives
// under src/ and is added as a PRIVATE include directory of the library that
// needs it, so:
//
//   - the public headers, Texture.hpp and Font.hpp, stay includable with no
//     SFML header in reach, and
//   - engine_assets, which holds the parser, still has no SFML dependency at
//     all, because this directory is not on its include path either.
//
// The implementation types are defined here rather than in Texture.cpp and
// Font.cpp because both the handle's own translation unit (which must destroy
// the unique_ptr, so needs the type complete) and the loader (which must
// populate it) need to see them. A private nested class may be defined out of
// line like this, so keeping the definition here costs the public header
// nothing.

#include "engine/assets/Font.hpp"
#include "engine/assets/Texture.hpp"

#include <SFML/Graphics/Font.hpp>
#include <SFML/Graphics/Texture.hpp>

namespace engine::assets
{

/// The concrete texture state: the platform resource itself.
///
/// Deliberately a single member and nothing else. A name, a path or a manager
/// pointer would make the handle carry configuration, which is what it was
/// designed not to do, and the whole class is still exactly one pointer wide
/// regardless of how large this grows.
struct Texture::Impl
{
    sf::Texture native;
};

/// The concrete font state: the platform resource itself.
///
/// Phase 10 loads and caches fonts and nothing more. There is no measurement, no
/// glyph lookup and no drawing surface on this, because no consumer for one
/// exists yet.
struct Font::Impl
{
    sf::Font native;
};

} // namespace engine::assets
