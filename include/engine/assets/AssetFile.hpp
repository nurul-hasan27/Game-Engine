#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets
{

/// What kind of resource an asset entry names.
///
/// Only the two kinds the engine can currently resolve. `Animation` is a real
/// entry in the course asset format and is deliberately **not** listed here,
/// because this engine cannot honour one yet: an animation needs a frame count
/// and a speed on top of a texture, and there is no component or system to put
/// them in. Accepting the type now would mean accepting a promise the engine
/// cannot keep, so `parseAssetFile` rejects it with a clear error instead of
/// silently dropping it.
enum class AssetType
{
    /// An image, loaded as a texture.
    Texture,

    /// A font, loaded for later text use.
    Font
};

/// One asset, exactly as the configuration file declared it.
///
/// This is plain data. It names a file; it does not open it, stat it, resolve
/// it, or hold anything loaded from it. Deciding what a path *means* is the
/// loader's job, not the parser's, which is why `path` stays a raw string here.
///
/// `type` and `name` are the two things a later lookup will be asked for.
/// `path` is carried along so the same entry can be handed straight to a
/// loader without re-reading the file.
struct AssetEntry
{
    /// Which kind of resource this is.
    AssetType type = AssetType::Texture;

    /// The name the rest of the engine refers to this asset by.
    std::string name;

    /// The path as written in the file, relative to the configuration file.
    std::string path;
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
/// Font    <name> <path>
/// ```
///
/// Blank lines are ignored, and so is any line whose first non-whitespace
/// character is `#`.
///
/// ### This function is pure
///
/// It takes the file's **contents**, not a path, and does no filesystem I/O
/// whatsoever: it never opens, stats, canonicalises or resolves anything, and
/// it never loads a texture or a font. A configuration naming a file that does
/// not exist parses perfectly happily, which is the observable consequence of
/// that separation.
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
/// A name may be used once, whether it names a texture or a font. Enforcing one
/// flat namespace is a deliberate choice: the component that will eventually
/// hold an asset reference holds nothing but a name, so if a texture and a font
/// could share a name, a name would stop uniquely identifying a resource.
/// The error names the type that already claimed the name, which is the case
/// most likely to be a genuine mistake.
///
/// ### Rejection, not repair
///
/// Malformed input is never guessed at and never skipped. Every error carries
/// the 1-based line number. The cases are: an unknown leading token, the
/// reserved-but-unsupported `Animation` keyword, a missing name, a missing
/// path, too many tokens, and a duplicate name.
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
