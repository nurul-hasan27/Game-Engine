#include "engine/assets/Font.hpp"

// The concrete graphics type for this handle. It is defined in a private header
// rather than here, because the loader has to populate it and therefore needs to
// see it too, and a private nested class can be defined out of line. This file
// still includes no SFML header itself.
#include "AssetHandleNative.hpp"

#include <utility>

namespace engine::assets
{

Font::Font() noexcept = default;

Font::~Font() = default;

Font::Font(Font&& other) noexcept = default;

Font& Font::operator=(Font&& other) noexcept
{
    // The self-move check is not defensive noise. std::unique_ptr's own move
    // assignment is unspecified for self-move and in practice deletes the
    // resource and then stores the freed pointer, so a handle that skipped this
    // would hand back a dangling resource from a perfectly ordinary std::swap.
    if (this != &other)
    {
        m_impl = std::move(other.m_impl);
    }

    return *this;
}

} // namespace engine::assets
