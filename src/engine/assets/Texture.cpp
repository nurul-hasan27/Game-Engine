#include "engine/assets/Texture.hpp"

#include <utility>

namespace engine::assets
{

// The implementation is defined here and nowhere else, so the public header
// needs neither the definition nor even a forward declaration of it. Keeping
// this file free of SFML as well is not an accident to be maintained by hand:
// the build links it into engine_assets, which has no SFML dependency at all, so
// adding an SFML include below would fail to compile.
struct Texture::Impl
{
};

Texture::Texture() noexcept = default;

Texture::~Texture() = default;

Texture::Texture(Texture&& other) noexcept = default;

Texture& Texture::operator=(Texture&& other) noexcept
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
