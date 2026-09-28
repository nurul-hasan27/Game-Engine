#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets
{

/// What kind of resource an asset entry names.
///
/// These are exactly the three entry kinds in the course asset format, and all
/// three are now honoured. `Animation` arrived last because it is the only one
/// that names *another* asset rather than a file: it slices a texture that some
/// earlier line must already have declared.
enum class AssetType
{
    /// An image, loaded as a texture.
    Texture,

    /// A font, loaded for later text use.
    Font,

    /// A run of frames inside an existing texture, plus how fast to play them.
    Animation
};

/// One asset, exactly as the configuration file declared it.
///
/// This is plain data. It names a file, or another asset; it does not open it,
/// stat it, resolve it, or hold anything loaded from it. Deciding what a path
/// *means* is the loader's job, not the parser's, which is why `path` stays a
/// raw string here.
///
/// `type` and `name` are the two things a later lookup will be asked for.
/// `path` is carried along so the same entry can be handed straight to a
/// loader without re-reading the file.
///
/// ### Two shapes, not one
///
/// A `Texture` or `Font` entry is `name` plus a file. An `Animation` entry has no
/// file at all: it names a texture that must already exist, and two integers.
/// The extra fields therefore default to their empty values and are meaningful
/// only when `type` is `Animation`, and `path` is meaningful only when it is
/// not. One struct rather than a variant, because the parser returns a flat list
/// in file order and every consumer iterates that list exactly once.
struct AssetEntry
{
    /// Which kind of resource this is.
    AssetType type = AssetType::Texture;

    /// The name the rest of the engine refers to this asset by.
    std::string name;

    /// The path as written in the file, relative to the configuration file.
    ///
    /// Empty for an `Animation`, which names a texture rather than a file.
    std::string path;

    /// The name of the `Texture` this animation slices. `Animation` entries only.
    ///
    /// This is a **reference to another asset in the same file**, not a path, and
    /// the parser guarantees it names a texture declared on an earlier line.
    std::string textureName;

    /// How many frames the texture is divided into. `Animation` entries only.
    ///
    /// Always at least 1, so a zero can never reach the loader and divide by it.
    std::uint32_t frameCount = 0;

    /// Game frames between animation frames. `Animation` entries only.
    ///
    /// Always at least 1, for the same reason. The course defines this in game
    /// frames, never in seconds, and this engine keeps that meaning.
    std::uint32_t speed = 0;
};

/// Thrown when a line of an asset configuration file cannot be accepted.
///
/// Derives from `std::invalid_argument`, because that is exactly what it is:
/// the argument passed to `parseAssetFile` was not a valid configuration. A
/// caller that does not care can catch `std::invalid_argument` and still be
/// correct, while a caller that wants to report "your asset file is broken"
/// distinctly from other bad arguments can catch this type.
///
/// The `what()` text always begins with `line <n>: `, so a caller that knows
/// the file's path can prefix it and produce a fully located message. The
/// parser cannot include the filename itself, because it is deliberately never
/// given one.
class AssetParseError : public std::invalid_argument
{
public:
    using std::invalid_argument::invalid_argument;
};

/// Parses asset configuration **contents** into a list of entries.
///
/// ```text
/// Texture <name> <path>
/// Font      <name> <path>
/// Animation <name> <textureName> <frameCount> <speed>
/// ```
///
/// Blank lines are ignored, and so is any line whose first non-whitespace
/// character is `#`.
///
/// ### Animations reference textures declared earlier
///
/// An `Animation` names a texture rather than a file, and that reference is
/// resolved **here**, at parse time. Given these two lines, in this order:
///
/// ```text
/// Texture   mario_run     library/images/megaman/megaRun.png
/// Animation mario_running mario_run 3 5
/// ```
///
/// the configuration is accepted, and the same two lines the other way round is
/// rejected. The course states the rule directly - a texture used by an
/// animation must already have been defined before the animation is loaded - and
/// this is where "before" is enforced. Resolving it in the loader instead would
/// let a configuration parse cleanly and then fail halfway through loading,
/// which is a later and much less specific place to be told.
///
///
/// ### This function is pure
///
/// It takes the file's **contents**, not a path, and does no filesystem I/O
/// whatsoever: it never opens, stats, canonicalises or resolves anything, and
/// it never loads a texture or a font. A configuration naming a file that does
/// not exist parses perfectly happily, which is the observable consequence of
/// that separation.
///
/// What it *does* need to know is the shape of an animation, and shape is partly
/// file content. Whether a frame count actually divides a texture's width can
/// only be answered once the image is loaded, so that check belongs to the
/// loader. Everything knowable from the text alone is checked here: the fields
/// are present, the two integers are positive decimal numbers, and the texture
/// they name exists on an earlier line.
///
/// It also uses no SFML type and no graphics context, so it can be tested with
/// no window, no GPU and no files on disk.
///
/// `path` is stored verbatim, relative to whichever file contained it.
/// Resolving that against a directory is the loader's responsibility, because
/// only the loader knows where the configuration file actually lives.
///
/// ### Ordering
///
/// Entries come back in the order they appear in the file, because a
/// configuration is an ordered document and callers should not have to sort to
/// reproduce it. Every duplicate is rejected, so the order is also stable and
/// unambiguous.
///
/// ### Names are unique across the whole file
///
/// A name may be used once, whether it names a texture, a font or an animation.
/// flat namespace is a deliberate choice: the component that will eventually
/// hold an asset reference holds nothing but a name, so if a texture and an
/// animation could share a name, a name would stop uniquely identifying a
/// resource. The error names the type that already claimed the name, which is
/// the case most likely to be a genuine mistake.
///
/// ### Rejection, not repair
///
/// Malformed input is never guessed at and never skipped. Every error carries
/// the 1-based line number. The cases are: an unknown leading token, a missing
/// name, a missing path or texture reference, a frame count or speed that is not
/// a positive decimal number, a token count that does not match the entry's type,
/// an animation naming a texture that no earlier line declared, and a duplicate
/// name.
///
/// A name or path that is present but blank cannot be distinguished from one
/// that is absent, because the format is whitespace separated; both are
/// reported as missing.
///
/// Trailing comments are **not** part of the format. Only a whole line may be
/// commented out, so `Texture mario mario.png # note` has four tokens and is
/// rejected rather than quietly accepted. This is deliberate: a `#` that looks
/// like a comment but is a path separator, or a path that legitimately
/// contains `#`, should not change meaning depending on its position.
///
/// ### Throws
///
/// `AssetParseError` on the first line it cannot accept. Parsing stops there;
/// a partially populated vector is never returned, so a caller cannot
/// accidentally load half a file.
[[nodiscard]] std::vector<AssetEntry> parseAssetFile(std::string_view contents);

} // namespace engine::assets
