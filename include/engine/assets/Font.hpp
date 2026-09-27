#pragma once

#include <memory>

namespace engine::assets
{

/// An opaque, loaded font resource.
///
/// ### What this type is for
///
/// A `Font` is a **handle to a resource**, not a loader and not a manager. It
/// cannot open a file, cannot name one, and does not know where its glyphs came
/// from. It is the font half of the pair that begins with [Texture](Texture.hpp),
/// and it obeys the same rules for the same reasons.
///
/// ### Scope: loaded and cached, never drawn
///
/// Phase 10 deliberately goes no further than loading and caching fonts. There
/// is no text rendering here, no draw call, and no measurement API, because there
/// is no consumer for one yet. Inventing a text interface now, before anything
/// needs to place a glyph, would mean guessing at questions this engine has not
/// been asked: what anchors text, how it wraps, what order it draws in relative
/// to sprites. Those questions deserve answers shaped by a real requirement.
///
/// What that leaves is still useful and fully testable: whether a font file
/// parsed, how many glyphs it carries, and whether a cached name really is the
/// same object as the one loaded before. So this type exists now, and its
/// rendering surface is deferred to a phase that has a reason to design it.
///
/// ### The public API is free of SFML
///
/// This header includes no SFML header, mentions no `sf::` type, and does not
/// even forward declare one. Game code, components and systems can include it
/// directly and stay clear of the graphics library:
///
/// ```cpp
/// #include <engine/assets/Font.hpp>   // no SFML anywhere in reach
/// ```
///
/// The implementation lives behind a pointer to a type that is never named here.
/// A forward declaration of `sf::Font` would be exactly the leak this avoids.
///
/// ### Movable, never copyable
///
/// Copying is deleted, and the reason is resource ownership rather than taste: a
/// copy would either duplicate the glyph cache or quietly share it, and the
/// second is worse because two owners would believe they could each destroy it.
/// Moving transfers that single ownership outright.
///
/// ### An empty handle is a real state
///
/// A default-constructed `Font` holds no resource, the same as an empty
/// [Texture](Texture.hpp). A manager that has not loaded a name, or a lookup
/// that found nothing, both need to be able to hand back something honest.
///
/// A default-constructed handle performs no file access and creates no graphics
/// object, so constructing one is safe in a test with no window, no GPU and no
/// files on disk.
class Font
{
public:
    /// Creates an empty handle that owns no resource.
    ///
    /// Loads nothing, reads no file and touches no graphics context.
    Font() noexcept;

    /// Releases the resource, if one is held.
    ///
    /// Defined out of line because destroying the pointer requires the complete
    /// implementation type, which this header deliberately does not have. A
    /// destructor written inline here would not compile.
    ~Font();

    /// Takes over the other handle's resource, leaving the source empty.
    ///
    /// Declared here and defaulted in the .cpp, so the implementation type is not
    /// needed where it is declared. `noexcept` because a move allocates nothing:
    /// the pointer is simply handed over.
    Font(Font&& other) noexcept;

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
    /// delete the resource and then store the dangling pointer. `std::swap` can
    /// produce a self-move, so this is reachable by ordinary code rather than
    /// only by a mistake.
    ///
    /// `noexcept`: taking over a resource allocates nothing and cannot fail.
    Font& operator=(Font&& other) noexcept;

    /// Copying a resource handle is deleted, deliberately.
    ///
    /// A copy would either duplicate the glyph cache or share it behind two
    /// owners' backs. Both are worse than the compiler error.
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

private:
    /// The opaque implementation. Defined in the .cpp and never named here, so
    /// its contents — including any graphics type it needs — stay private.
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
