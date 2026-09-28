#include "engine/level/LevelFile.hpp"
#include "engine/level/LevelGrid.hpp"

#include <array>
#include <charconv>
#include <fstream>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>

namespace engine::level
{

namespace
{

/// The most tokens any record has on one line, counting the keyword.
///
/// `Player <gx> <gy> <cw> <ch> <sx> <sy> <sm> <gy> <b>` is ten. `Tile` and `Dec`
/// are four. The array is sized for the widest so one tokenizer serves all three,
/// and so that a player line with eleven fields is still *counted* accurately
/// rather than truncated and reported as if it had exactly ten - telling someone
/// their line has ten fields when it has eleven sends them editing the wrong one.
constexpr std::size_t kMaxTokenCount = 10;

/// The exact token count a record of `kind` must have, keyword included.
///
/// No shared default, because the whole point of the check is that each record has
/// its own shape and a wrong count should name the record that is wrong.
[[nodiscard]] constexpr std::size_t tokenCountOf(const LevelRecordKind kind) noexcept
{
    switch (kind)
    {
        case LevelRecordKind::Tile:
            return 4U;
        case LevelRecordKind::Decoration:
            return 4U;
        case LevelRecordKind::Player:
            return kMaxTokenCount;
    }

    return 0U;
}

/// ASCII whitespace, spelled out rather than delegated to `std::isspace`.
///
/// The asset parser's reason, unchanged: `std::isspace` is locale dependent, so a
/// byte that counts as whitespace on one machine and not another would change which
/// level files parse. `\r` is included so a file written on Windows reads exactly
/// like the same file written here.
[[nodiscard]] constexpr bool isWhitespace(const char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\v' || character == '\f';
}

/// Splits `line` into whitespace-separated tokens and returns how many there are,
/// writing the first [kMaxTokenCount] of them into `tokens`.
///
/// The count is the **true** token count, not the number stored, for the reason
/// given on [kMaxTokenCount]: that count goes straight into the error message.
[[nodiscard]] std::size_t tokenize(const std::string_view line,
                                  std::array<std::string_view, kMaxTokenCount>& tokens) noexcept
{
    std::size_t count = 0;
    std::size_t stored = 0;
    std::size_t cursor = 0;

    while (cursor < line.size())
    {
        while (cursor < line.size() && isWhitespace(line[cursor]))
        {
            ++cursor;
        }

        if (cursor >= line.size())
        {
            break;
        }

        const std::size_t start = cursor;
        while (cursor < line.size() && !isWhitespace(line[cursor]))
        {
            ++cursor;
        }

        ++count;

        // Anything past the array's end is counted and discarded, so the tokens
        // that *are* stored always line up with the first fields of the line.
        if (stored < tokens.size())
        {
            tokens[stored] = line.substr(start, cursor - start);
            ++stored;
        }
    }

    return count;
}

/// Throws a located parse error. `[[noreturn]]` so callers need no dummy return.
[[noreturn]] void fail(const std::size_t lineNumber, const std::string& message)
{
    throw LevelParseError{"line " + std::to_string(lineNumber) + ": " + message};
}

/// Parses `token` as a finite decimal number, or throws.
///
/// ### Why not `std::stof`
///
/// The same trap the asset parser documents for `std::stoi`, one level up:
/// `std::stof` stops at the first character it cannot use and **ignores the
/// rest**, so `12abc` parses as `12` and the typo vanishes. [std::from_chars] is
/// the opposite - locale independent, and it reports how much it consumed so the
/// caller can insist the *whole* token was a number.
///
/// ### Why `nan` and `inf` are named explicitly
///
/// `from_chars` accepts both spellings, and a level whose gravity is `nan` is a
/// level that parses, loads, and then produces `nan` positions that compare false
/// against everything. Refusing them by name means the message says which token
/// and why, instead of leaving a `nan` to be discovered in a renderer.
[[nodiscard]] float parseDecimal(const std::string_view token, const char* const fieldName,
                                 const LevelRecordKind kind, const std::size_t lineNumber)
{
    const std::string syntax = "expected: " + std::string{syntaxOf(kind)};

    if (token.empty())
    {
        fail(lineNumber, std::string{kind == LevelRecordKind::Player ? "Player" : "record"} + " entry has " +
                             fieldName + " '', which is not a number; " + syntax);
    }

    float value = 0.0F;
    const char* const first = token.data();
    const char* const last = token.data() + token.size();
    const std::from_chars_result result = std::from_chars(first, last, value);

    // `ec == result_out_of_range` is a separate message: "not a number" would be
    // wrong and would send the reader looking for a stray character.
    if (result.ec == std::errc::result_out_of_range)
    {
        fail(lineNumber, std::string{kind == LevelRecordKind::Player ? "Player" : "record"} + " entry has " +
                             fieldName + " '" + std::string{token} +
                             "', which is too large a number to use; " + syntax);
    }

    // Anything other than a clean, complete, in-range conversion is refused, and
    // the token is quoted back so the reader can see the character that broke it.
    if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(value))
    {
        fail(lineNumber, std::string{kind == LevelRecordKind::Player ? "Player" : "record"} + " entry has " +
                             fieldName + " '" + std::string{token} +
                             "', which is not a finite decimal number; " + syntax);
    }

    return value;
}

/// Parses `token` as a number that must additionally be non-negative.
[[nodiscard]] float parseNonNegative(const std::string_view token, const char* const fieldName,
                                     const LevelRecordKind kind, const std::size_t lineNumber)
{
    const float value = parseDecimal(token, fieldName, kind, lineNumber);

    if (value < 0.0F)
    {
        fail(lineNumber, std::string{kind == LevelRecordKind::Player ? "Player" : "record"} + " entry has " +
                             fieldName + " '" + std::string{token} +
                             "', which is negative; a speed, a cap and a gravity are magnitudes; " +
                             std::string{syntaxOf(kind)});
    }

    return value;
}

/// Parses `token` as a number that must additionally be strictly positive.
[[nodiscard]] float parsePositive(const std::string_view token, const char* const fieldName,
                                  const LevelRecordKind kind, const std::size_t lineNumber)
{
    const float value = parseDecimal(token, fieldName, kind, lineNumber);

    if (value <= 0.0F)
    {
        fail(lineNumber, std::string{kind == LevelRecordKind::Player ? "Player" : "record"} + " entry has " +
                             fieldName + " '" + std::string{token} +
                             "', which is not positive; a bounding box of no size can never overlap anything; " +
                             std::string{syntaxOf(kind)});
    }

    return value;
}

/// Reads an animation name and refuses a blank one.
///
/// A name here is a reference into the asset configuration, so an empty reference
/// is a reference to nothing. Catching it at parse time names the line; catching it
/// at spawn time would name the line too, via [TileRecord::lineNumber], but by then
/// the reason is much further from the cause.
void requireAnimationName(const std::string_view name, const char* const fieldName,
                          const LevelRecordKind kind, const std::size_t lineNumber)
{
    if (name.empty())
    {
        fail(lineNumber, std::string{kind == LevelRecordKind::Player ? "Player" : "record"} + " entry has " +
                             fieldName + " '', which is not an animation name; " + std::string{syntaxOf(kind)});
    }
}

/// Maps the leading token to a record kind, or throws.
[[nodiscard]] LevelRecordKind kindOfToken(const std::string_view token, const std::size_t lineNumber)
{
    if (token == "Tile")
    {
        return LevelRecordKind::Tile;
    }

    if (token == "Dec")
    {
        return LevelRecordKind::Decoration;
    }

    if (token == "Player")
    {
        return LevelRecordKind::Player;
    }

    fail(lineNumber, "unknown level record type '" + std::string{token} +
                         "'; expected one of 'Tile', 'Dec' or 'Player'");
}

/// Reports a line that is the wrong length, by field name where that is clearer.
///
/// The player record has ten fields, so it has nine ways to be short by one, and
/// counting to "expected 10, got 9" leaves the reader to work out which. Naming
/// the first field that is missing is the difference between a fixable message and
/// a counting exercise.
void reportWrongArity(const LevelRecordKind kind, const std::size_t tokenCount, const std::size_t lineNumber)
{
    static constexpr std::array<const char*, kMaxTokenCount> kPlayerFields{
        "the 'Player' keyword", "gridX", "gridY", "boundingBoxWidth", "boundingBoxHeight",
        "leftRightSpeed",     "jumpSpeed", "maxSpeed", "gravity", "bulletAnimationName"};

    static constexpr std::array<const char*, 4U> kTileFields{"the 'Tile' keyword", "animationName", "gridX",
                                                            "gridY"};

    static constexpr std::array<const char*, 4U> kDecFields{"the 'Dec' keyword", "animationName", "x", "y"};

    if (kind == LevelRecordKind::Player)
    {
        // Short: name the first field that is not there. Long: the extra field is
        // the interesting one, so name it too.
        fail(lineNumber, std::string{"Player entry is missing "} + kPlayerFields[tokenCount] + "; " +
                             std::string{syntaxOf(kind)});
    }

    if (kind == LevelRecordKind::Tile)
    {
        fail(lineNumber, std::string{"Tile entry is missing "} + kTileFields[tokenCount] + "; " +
                             std::string{syntaxOf(kind)});
    }

    fail(lineNumber, std::string{"Dec entry is missing "} + kDecFields[tokenCount] + "; " +
                         std::string{syntaxOf(kind)});
}

/// Parses one record. Appends to `level` unless it is a player, which is returned.
void parseRecord(const LevelRecordKind kind, const std::array<std::string_view, kMaxTokenCount>& tokens,
                 const std::size_t lineNumber, Level& level, bool& sawPlayer, PlayerRecord& player)
{
    if (kind == LevelRecordKind::Tile)
    {
        requireAnimationName(tokens[1], "animationName", kind, lineNumber);

        TileRecord tile;
        tile.animationName = std::string{tokens[1]};
        tile.gridX = parseDecimal(tokens[2], "gridX", kind, lineNumber);
        tile.gridY = parseDecimal(tokens[3], "gridY", kind, lineNumber);
        tile.lineNumber = lineNumber;
        level.addTile(std::move(tile));
        return;
    }

    if (kind == LevelRecordKind::Decoration)
    {
        requireAnimationName(tokens[1], "animationName", kind, lineNumber);

        DecorationRecord decoration;
        decoration.animationName = std::string{tokens[1]};
        decoration.x = parseDecimal(tokens[2], "x", kind, lineNumber);
        decoration.y = parseDecimal(tokens[3], "y", kind, lineNumber);
        decoration.lineNumber = lineNumber;
        level.addDecoration(std::move(decoration));
        return;
    }

    if (sawPlayer)
    {
        // Checked before the fields are read, so a second player line that is also
        // malformed still reports the duplication - the level having two players is
        // the more fundamental problem, and the more confusing one to find.
        fail(lineNumber, "a second Player line; the level file must contain exactly one, which specifies the "
                         "player for this level");
    }

    requireAnimationName(tokens[9], "bulletAnimationName", kind, lineNumber);

    PlayerRecord record;
    record.gridX = parseDecimal(tokens[1], "gridX", kind, lineNumber);
    record.gridY = parseDecimal(tokens[2], "gridY", kind, lineNumber);
    record.boundingBoxSize = Vec2{parsePositive(tokens[3], "boundingBoxWidth", kind, lineNumber),
                                  parsePositive(tokens[4], "boundingBoxHeight", kind, lineNumber)};
    record.leftRightSpeed = parseNonNegative(tokens[5], "leftRightSpeed", kind, lineNumber);
    record.jumpSpeed = parseNonNegative(tokens[6], "jumpSpeed", kind, lineNumber);
    record.maxSpeed = parseNonNegative(tokens[7], "maxSpeed", kind, lineNumber);
    record.gravity = parseNonNegative(tokens[8], "gravity", kind, lineNumber);
    record.bulletAnimationName = std::string{tokens[9]};
    record.lineNumber = lineNumber;

    player = record;
    sawPlayer = true;
}

} // namespace

std::string_view syntaxOf(const LevelRecordKind kind) noexcept
{
    switch (kind)
    {
        case LevelRecordKind::Tile:
            return "Tile   <animationName> <gridX> <gridY>";
        case LevelRecordKind::Decoration:
            return "Dec    <animationName> <x> <y>";
        case LevelRecordKind::Player:
            return "Player <gridX> <gridY> <boundingBoxWidth> <boundingBoxHeight> <leftRightSpeed> <jumpSpeed> "
                   "<maxSpeed> <gravity> <bulletAnimationName>";
    }

    return "";
}

Level parseLevelFile(const std::string_view contents)
{
    Level level;
    PlayerRecord player;
    bool sawPlayer = false;

    std::array<std::string_view, kMaxTokenCount> tokens{};
    std::size_t lineNumber = 0;
    std::size_t cursor = 0;

    while (cursor <= contents.size())
    {
        // Take one line, without its terminator. The file is split by hand rather
        // than with getline on an istringstream so that the function takes a
        // `string_view` and does not need the contents to be null-terminated or
        // copied.
        const std::size_t lineEnd = contents.find('\n', cursor);
        const std::size_t nextStart = (lineEnd == std::string_view::npos) ? contents.size() : lineEnd + 1;
        const std::string_view line =
            contents.substr(cursor, (lineEnd == std::string_view::npos) ? std::string_view::npos : lineEnd - cursor);

        ++lineNumber;

        const std::size_t tokenCount = tokenize(line, tokens);

        // A blank line, or a whole-line comment. Tested on the first token rather
        // than by scanning for '#', so both '# note' and '#Tile note' are comments
        // and a '#' further along a line is a token like any other.
        if (tokenCount == 0 || tokens[0].front() == '#')
        {
            if (lineEnd == std::string_view::npos)
            {
                break;
            }

            cursor = nextStart;
            continue;
        }

        // The kind is resolved before the arity is checked, so a line wrong in both
        // ways still reports the more fundamental problem. An unknown keyword with
        // eleven tokens says the keyword is unknown, rather than complaining about
        // a count derived from a kind that was never resolved.
        const LevelRecordKind kind = kindOfToken(tokens[0], lineNumber);

        if (tokenCount > tokenCountOf(kind))
        {
            fail(lineNumber, std::string{kind == LevelRecordKind::Player ? "Player" : "record"} +
                                 " entry has " + std::to_string(tokenCount) +
                                 " fields but must have exactly " + std::to_string(tokenCountOf(kind)) +
                                 "; the first unexpected field is '" + std::string{tokens[tokenCountOf(kind)]} +
                                 "'; " + std::string{syntaxOf(kind)});
        }

        if (tokenCount < tokenCountOf(kind))
        {
            reportWrongArity(kind, tokenCount, lineNumber);
        }

        parseRecord(kind, tokens, lineNumber, level, sawPlayer, player);

        if (lineEnd == std::string_view::npos)
        {
            break;
        }

        cursor = nextStart;
    }

    if (!sawPlayer)
    {
        // Reported without a line number because there is no line to blame: the
        // problem is an absence, and pointing at line 1 or the last line would be a
        // guess dressed up as a location.
        throw LevelParseError{"this level file has no Player line; the format requires exactly one, which "
                              "specifies the player for this level; " +
                              std::string{syntaxOf(LevelRecordKind::Player)}};
    }

    level.setPlayer(std::move(player));

    return level;
}

Level loadLevelFile(const std::filesystem::path& path)
{
    // The whole file, as a string, so the parser can take a `string_view` and never
    // need the text to be null-terminated or to exist on disk. The asset manager
    // reads its configuration the same way.
    std::ifstream file{path};

    if (!file)
    {
        // The path is in the message on purpose. A `LevelParseError` carries a line
        // number and no path, because the parser cannot see one; here, before
        // parsing, the path is the only thing that identifies *which* file failed,
        // and a game loading one level out of a directory needs it.
        throw std::runtime_error{"cannot open level file '" + path.string() + "'"};
    }

    std::string contents{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};

    return parseLevelFile(contents);
}

} // namespace engine::level
