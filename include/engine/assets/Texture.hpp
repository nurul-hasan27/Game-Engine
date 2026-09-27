#pragma once

#include <memory>

namespace engine::graphics
{

/// The SFML renderer, which has to reach the platform texture in order to draw
/// it. Declared here only so a handle can name it as a friend; nothing about it
/// reaches this header.
class SfmlRenderer;

} // namespace engine::graphics

namespace engine::assets
{

/// The concrete manager that loads assets through this graphics library.
///
/// Declared here only so a handle can name it as a friend. Its definition lives
/// elsewhere, and nothing about it reaches this header, which stays includable
/// with no SFML header in reach.
class SfmlAssetManager;

/// An opaque, loaded image resource.
///
/// ### What this type is for
///
/// A `Texture` is a **handle to a resource**, not a loader and not a manager. It
/// cannot open a file, cannot name one, and does not know where its pixels came
/// from. Whoever fills one in — the asset manager, later — owns the actual
/// resource; this type exists so that everything *above* it can refer to a
/// texture without ever naming a graphics type.
///
/// ### The public API is free of SFML
///
/// This header includes no SFML header, mentions no `sf::` type, and does not
/// even forward declare one. Game code, components and systems can include it
/// directly and stay clear of the graphics library:
///
/// ```cpp
/// #include <engine/assets/Texture.hpp>   // no SFML anywhere in reach
/// ```
///
/// The implementation lives behind a pointer to a type that is never named
/// here. That is not decoration: the whole point is that a future component
/// holding a `Texture` must not drag SFML into the public engine API, and a
/// forward declaration of `sf::Texture` would be exactly the leak this avoids.
///
/// ### Movable, never copyable
///
/// Copying is deleted, and the reason is resource ownership rather than taste: a
/// copy would either duplicate the pixels on the GPU or quietly share them, and
/// the second is worse because two owners would believe they could each destroy
/// it. Moving transfers that single ownership outright.
///
/// The manager will hand out references to textures it owns rather than
/// transferring them, so a texture is normally reached by `const Texture&` and
/// never copied or moved at all. These operations exist so a container can hold
/// a texture, not so that drawing code can duplicate one.
///
/// ### An empty handle is a real state
///
/// A default-constructed `Texture` holds no resource. That is a genuine state
/// rather than a placeholder for "not set up yet": a manager that has not loaded
/// a name, or a lookup that found nothing, both need to be able to hand back
/// something honest. Nothing in this type interprets that state — deciding
/// whether an empty handle is a bug or a choice belongs to the code that created
/// it — but it must be representable, and it is.
///
/// A default-constructed handle performs no file access and creates no graphics
/// object, so constructing one is safe in a test with no window, no GPU and no
/// files on disk.
class Texture
{
public:
    /// Creates an empty handle that owns no resource.
    ///
    /// Loads nothing, reads no file and touches no graphics context.
    Texture() noexcept;

    /// Releases the resource, if one is held.
    ///
    /// Defined out of line because destroying the pointer requires the complete
    /// implementation type, which this header deliberately does not have. A
    /// destructor written inline here would not compile.
    ~Texture();

    /// Takes over the other handle's resource, leaving the source empty.
    ///
    /// Declared here and defaulted in the .cpp, so the implementation type is not
    /// needed where it is declared. `noexcept` because a move allocates nothing:
    /// the pointer is simply handed over.
    Texture(Texture&& other) noexcept;

    /// Releases whatever this held, then takes over `other`'s resource,
    /// leaving the source empty.
    ///
    /// Written by hand in the .cpp rather than defaulted, for two reasons.
    ///
    /// It cannot be defaulted in the class body: declaring a deleted copy
    /// assignment below suppresses the implicitly declared move assignment, so
    /// this has to be declared in the class to exist at all, and a defaulted move
    /// assignment cannot live there either. A `std::unique_ptr` move assignment
    /// releases the target's current resource before taking over the source's,
    /// and releasing a resource means deleting through a pointer, which needs
    /// the complete type. The implementation type is deliberately incomplete
    /// here, so the body has to be written where it is complete.
    ///
    /// Self-move assignment is a genuine no-op, checked explicitly. Left to
    /// `std::unique_ptr` it would be unspecified, and in practice it would
    /// delete the resource and then store the dangling pointer. `std::swap` and
    /// `std::rotate` can both produce a self-move, so this is reachable by
    /// ordinary code rather than only by a mistake.
    ///
    /// `noexcept`: taking over a resource allocates nothing and cannot fail.
    Texture& operator=(Texture&& other) noexcept;

    /// Copying a resource handle is deleted, deliberately.
    ///
    /// A copy would either duplicate the pixels or share them behind two owners'
    /// backs. Both are worse than the compiler error.
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

private:
    /// Exactly two things are allowed to look inside a handle, and both are
    /// deliberate SFML boundaries for the engine rather than accidents: the
    /// loader, which fills the handle in, and the renderer, which has to read the
    /// pixels back out to draw them. Nobody else can, so a handle cannot be
    /// populated or emptied behind the asset manager's back, and no layer above
    /// either boundary can reach a graphics type through a handle.
    friend class SfmlAssetManager;
    friend class graphics::SfmlRenderer;

    /// The opaque implementation. Defined with the concrete graphics type in a
    /// private header, and never named here, so its contents stay private.
    ///
    /// Not forward declared as `struct Impl;` separately, because a nested type
    /// declaration inside the class is the same declaration and is less to
    /// forget.
    struct Impl;

    /// Null means "empty handle"; non-null means a resource is held. The
    /// manager in a later step populates this.
    ///
    /// This is the only member. That is deliberate: a move is a pointer handover
    /// and nothing else, and a second member added later would have to be given
    /// the same careful treatment rather than inherited for free.
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::assets
