#pragma once

#include "engine/level/Level.hpp"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace engine::level
{

/// A level file that could not be read.
///
/// Derived from [std::invalid_argument] to match [engine::assets::AssetParseError],
/// so a caller that already handles a bad configuration file handles a bad level
/// file without learning a second exception type.
///
/// ### Every message names a line
///
/// The whole reason this type exists separately from a bare `invalid_argument` is
/// that a level file is **edited by hand**, often a long one, and "expected 10
/// fields" is useless without "line 47". Every message this parser throws begins
/// `line N: `, and `N` is 1-based so it matches what an editor shows.
///
/// The record's line number is also kept on the parsed record, so a failure that
/// happens *after* parsing - a missing animation, most often - can point back at
/// the same line. See [TileRecord::lineNumber](Level.hpp).
class LevelParseError : public std::invalid_argument
{
public:
    using std::invalid_argument::invalid_argument;
};

/// Which of the three record kinds a line declares.
enum class LevelRecordKind
{
    Tile,
    Decoration,
    Player
};

/// The record's syntax line, spelled the way the level file spells it.
///
/// Returned as a `std::string_view` so an error can quote what the user needs to
/// type rather than a re-spelling that might differ, exactly as the asset parser
/// does with its `expectedSyntax`. A field-count error that does not say what a
/// correct line looks like is a worse error than one that does.
[[nodiscard]] std::string_view syntaxOf(const LevelRecordKind kind) noexcept;

/// Parses a whole level file into a [Level].
///
/// ### The format, as the assignment specifies it
///
/// ```text
/// Tile   <animationName> <gridX> <gridY>
/// Dec    <animationName> <x> <y>
/// Player <gridX> <gridY> <width> <height> <leftRightSpeed> <jumpSpeed> <maxSpeed> <gravity> <bulletAnimation>
/// ```
///
/// Every position is a `float` in the assignment, so a half-cell offset is legal
/// and is preserved exactly.
///
/// ### What is rejected, and why each one
///
/// - **An unknown keyword.** A typo would otherwise be a line that silently does
///   nothing, and a level missing half its ground is a bug that takes an hour to
///   find by eye.
/// - **The wrong number of fields**, in both directions. A missing field is not
///   defaulted and an extra one is not ignored, because either way the file means
///   something other than what it says.
/// - **A number that is not a number.** `12abc` is an error, not `12`.
///   [std::from_chars] is used rather than `std::stof` for the same reason the
///   asset parser avoids `std::stoi`: `std::stof` stops at the first character it
///   cannot use and *ignores the rest*, so a typo becomes a valid number.
/// - **`nan` and `inf`.** [std::from_chars] accepts both spellings, and both are
///   finite-looking arithmetic that quietly poisons every position derived from
///   it, so they are refused by name.
/// - **A second `Player` line.** The assignment says the file *"will also contain
///   a single line which specifies properties of the player in that level"*, so
///   one is a stated rule rather than an assumption. A file with **no** player line
///   is refused for the same reason: the format promises one, and a level that
///   silently has no player configuration would fail much later and much less
///   clearly.
/// - **A zero or negative bounding box.** The course gives the player *"a
///   CBoundingBox of a size specified in the level file"*, and a box of zero width
///   can never overlap anything, so every collision involving it would silently
///   fail.
/// - **A negative speed, jump speed, cap or gravity.** These are magnitudes.
/// - **A blank animation name.** It is a reference to something, and an empty
///   reference is a reference to nothing.
///
/// ### What is *not* rejected
///
/// Negative grid and pixel **positions**. The assignment's note is about what
/// `(GX, GY)` means, not about which values are legal, and a decoration hanging
/// off the left edge of a level is a legitimate thing to author. Refusing it would
/// be inventing a rule the course does not state.
///
/// ### Blank lines and comments
///
/// A line with no tokens is skipped, and a line whose **first token** starts with
/// `#` is a comment. Testing the first token rather than scanning for `#` accepts
/// both `# a note` and `#Tile a note` without treating a `#` further along a line
/// as one. This is the asset parser's rule, unchanged, so the two configuration
/// files in this project behave identically.
///
/// There are no end-of-line comments: `#` is a token like any other, so a trailing
/// one is reported as an unexpected extra field rather than silently swallowed.
/// That is a deliberate difference from a language where `#` means "comment" -
/// here it means "a token", and there is exactly one way to write a note.
///
/// ### Throws
///
/// [LevelParseError] on anything above, or [std::invalid_argument] from
/// [LevelGrid] if a height is ever needed and is not positive. Never returns a
/// partially populated level: a failed parse throws rather than yielding whatever
/// it managed to read, so a caller cannot accidentally spawn half a level.
[[nodiscard]] Level parseLevelFile(std::string_view contents);

/// Reads the file at `path` and parses it.
///
/// The same two-step the asset manager uses - read the whole file into a string,
/// then hand the string to a parser that takes a `string_view` - and for the same
/// two reasons. The parser stays testable with a string literal and no filesystem
/// at all, and the file-system boundary stays in one place per format rather than
/// being re-implemented at every call site.
///
/// ### Throws
///
/// [std::runtime_error] naming the path when the file cannot be opened, and
/// [LevelParseError] for anything wrong with its contents - carrying the line
/// number, because by the time the parser sees the text the path is no longer
/// available to it and a line number alone would send the reader hunting through
/// the wrong file.
[[nodiscard]] Level loadLevelFile(const std::filesystem::path& path);

} // namespace engine::level
