#pragma once

#include <memory>

namespace engine::graphics
{

/// The SFML renderer, which has to reach the platform font in order to draw with
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

/// An opaque, loaded font resource.
///
/// ### What this type is for
///
/// A `Font` is a **handle to a resource**, not a loader and not a manager. It
/// cannot open a file, cannot name one, and does not know where its glyphs came
/// from. It is the font half of the pair that begins with [Texture](Texture.hpp),
/// and it obeys the same rules for the same reasons.
///
/// ### Scope: loaded, cached, and drawn
///
/// Phase 10 stopped at loading and caching, deliberately: there was no consumer for
/// a drawing surface, and inventing one would have meant guessing at questions the
/// engine had not been asked - what anchors text, how it wraps, what order it draws
/// in relative to sprites.
///
/// Phase 14 answers those from evidence rather than guesswork. The course
/// reference draws its grid overlay at world pixel coordinates, so text joins the
/// same world-to-screen path as every other renderable; and the engine already has
/// one rule for where a thing sits relative to its position - the centre, from
/// Lecture 11 section 5 - so text follows it rather than inventing a second anchor.
/// The renderer, not this type, is where that is decided; see
/// [engine::graphics::Renderer::drawText].
///
/// What this type still deliberately does not have is a measurement API. Nothing
/// needs to ask a font how wide a string is from outside the renderer: the renderer
/// measures the string it was handed, at the size it was asked for, which is the
/// only way to get an answer that matches what is actually drawn. An engine-level
/// `measure()` would duplicate the renderer's reasoning and could disagree with it.
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
    /// The concrete implementation populates a handle from the platform resource
    /// it owns. Being the loader, and only the loader, is what keeps a handle
    /// meaning "empty, or holding exactly what was loaded for it" instead of
    /// "anything with a pointer to an implementation can reach in and set it".
    friend class SfmlAssetManager;

    /// The renderer, which has to reach the platform font to draw a string with it
    /// - the same second friend [Texture](Texture.hpp) has, for the same reason.
    /// Between them the two are the *only* ways in: the loader, which fills a
    /// handle with something it loaded, and the renderer, which reads that
    /// something to draw it. A third would be a new kind of access rather than a
    /// new kind of caller, so there is not one.
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
