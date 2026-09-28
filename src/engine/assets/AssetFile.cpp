#include "engine/assets/AssetFile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace engine::assets
{
namespace
{

/// The most tokens any entry kind has on one line, counting the keyword:
/// `Animation <name> <texture> <frames> <speed>`.
///
/// A `Texture` or `Font` uses three. The array is sized for the widest kind so
/// that one tokenizer serves all three, and so that a too-long animation line is
/// still counted accurately for its error message rather than being truncated at
/// three and reported as if it had exactly three.
constexpr std::size_t kMaxTokenCount = 5;

/// The exact token count an entry of `type` must have, keyword included.
[[nodiscard]] constexpr std::size_t tokenCountOf(const AssetType type) noexcept
{
    // An animation names a texture and two integers instead of a path, so it is
    // two tokens wider than the other two. There is no shared default, because
    // the whole point of the check is that each kind has its own shape.
    return type == AssetType::Animation ? kMaxTokenCount : 3U;
}

/// ASCII whitespace, spelled out rather than delegated to `std::isspace`.
///
/// `std::isspace` is locale dependent, which would make this parser behave
/// differently on different machines: a byte that counts as whitespace in one
/// locale and not another would change which files parse. Every engine concept
/// is deterministic, and a parser is the one place where a surprise is worst,
/// because it decides what a whole configuration means. `\r` is included so a
/// file written on Windows parses exactly like the same file written here.
[[nodiscard]] constexpr bool isWhitespace(const char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\v' ||
           character == '\f';
}

/// Splits `line` into whitespace separated tokens and returns how many there are,
/// writing the first `kMaxTokenCount` of them into `tokens`.
///
/// The count is the **true** token count, not the number stored. A line with six
/// tokens reports six even though only five are kept, because that count goes
/// straight into the error message: telling someone their line has four tokens
/// when it has six would send them editing the wrong field. Anything past the
/// fifth is counted and discarded, so the first five always line up with the
/// keyword, the name, the reference and the two numbers.
[[nodiscard]] std::size_t tokenize(const std::string_view line,
                                  std::array<std::string_view, kMaxTokenCount>& tokens) noexcept
{
    std::size_t total = 0;
    std::size_t stored = 0;
    std::size_t cursor = 0;

    while (cursor < line.size())
    {
        while (cursor < line.size() && isWhitespace(line[cursor]))
        {
            ++cursor;
        }

        if (cursor == line.size())
        {
            break;
        }

        const std::size_t start = cursor;
        while (cursor < line.size() && !isWhitespace(line[cursor]))
        {
            ++cursor;
        }

        if (stored < kMaxTokenCount)
        {
            tokens[stored] = line.substr(start, cursor - start);
            ++stored;
        }

        ++total;
    }

    return total;
}

/// The keyword as it appears in the file, so error messages quote what the user
/// actually wrote rather than a re-spelling of it.
[[nodiscard]] std::string_view keywordOf(const AssetType type) noexcept
{
    switch (type)
    {
    case AssetType::Texture:
        return "Texture";
    case AssetType::Font:
        return "Font";
    case AssetType::Animation:
        return "Animation";
    }

    return "Texture";
}

[[nodiscard]] std::string describe(const AssetType type) { return std::string{keywordOf(type)}; }

/// The expected spelling for `type`, appended to the errors that are really about
/// a missing or malformed field.
///
/// It matters most for the animation line, which has the most fields and so the
/// most ways to be short by one, and for which "expected ..." is the only
/// self-documenting part of the message.
[[nodiscard]] const char* expectedSyntax(const AssetType type) noexcept
{
    return type == AssetType::Animation ? "expected 'Animation <name> <textureName> <frameCount> <speed>'"
                                        : "expected 'Texture <name> <path>'";
}

/// Throws a located parse error. `[[noreturn]]` so every caller can treat this
/// as terminating without a dummy return afterwards.
[[noreturn]] void fail(const std::size_t lineNumber, const std::string& message)
{
    throw AssetParseError{"line " + std::to_string(lineNumber) + ": " + message};
}

/// Parses `token` as a positive decimal number, or throws.
///
/// ### Why not `std::stoi`
///
/// Because `std::stoi` is far too permissive for a configuration format. It
/// accepts a leading `+`, it skips leading whitespace, it stops at the first
/// character that is not a digit and **ignores the rest**, and it throws
/// `std::out_of_range` for something too large, which no caller here can describe
/// usefully. So `3x` would silently parse as `3`. This accepts ASCII digits and
/// nothing else, and a token with any other character in it is an error that
/// quotes the token back.
///
/// ### Why zero is rejected
///
/// `frameCount` and `speed` are both divisors downstream - one splits a texture,
/// the other divides a tick count - so a zero would either divide by zero or
/// produce an infinitely wide frame. There is no sensible reading of
/// `Animation x tex 0 5`, and a configuration error at load time beats one that
/// loads cleanly and then misbehaves at draw time. The same reasoning rejects a
/// negative number, which is not even lexically possible here.
[[nodiscard]] std::uint32_t parsePositiveInteger(const std::string_view token, const char* const fieldName,
                                                const AssetType type, const std::size_t lineNumber)
{
    if (token.empty() || token.size() > 10U)
    {
        fail(lineNumber, describe(type) + " entry has " + fieldName + " '" + std::string{token} +
                             "', which is not a positive decimal number; " + expectedSyntax(type));
    }

    std::uint64_t value = 0;
    for (const char character : token)
    {
        // An explicit range rather than `isdigit`, for the same locale-independent
        // reason `isWhitespace` is spelled out: only ASCII '0' to '9' count.
        if (character < '0' || character > '9')
        {
            fail(lineNumber, describe(type) + " entry has " + fieldName + " '" + std::string{token} +
                                 "', which is not a positive decimal number; " + expectedSyntax(type));
        }

        value = (value * 10U) + static_cast<std::uint64_t>(character - '0');
    }

    // Ten digits can reach 9999999999, which overflows uint32_t, so the bound is
    // checked against the widest type before the result is narrowed.
    if (value == 0U || value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        fail(lineNumber, describe(type) + " entry has " + fieldName + " '" + std::string{token} +
                             "', which is not a positive number this engine can use; " + expectedSyntax(type));
    }

    return static_cast<std::uint32_t>(value);
}

/// Maps the leading token to a type, or throws.
[[nodiscard]] AssetType typeOfToken(const std::string_view token, const std::size_t lineNumber)
{
    if (token == "Texture")
    {
        return AssetType::Texture;
    }

    if (token == "Font")
    {
        return AssetType::Font;
    }

    if (token == "Animation")
    {
        return AssetType::Animation;
    }

    fail(lineNumber, "unknown asset type '" + std::string{token} + "'");
}

/// The type that already claimed `name`, or `nullptr`-equivalent via `claimedBy`
/// when the name is free.
///
/// `claimedBy` is only written when the function returns true, and it is an out
/// parameter rather than a return value so the caller can phrase the error with
/// the name it already has in hand.
///
/// Linear in the number of entries parsed so far, which for a configuration file
/// means tens. That is the deliberate trade for keeping the parser to a single
/// container that holds nothing but the returned data: a side table of declared
/// names would be a second thing to keep in step, and a file that parsed could
/// still disagree with it.
[[nodiscard]] bool nameAlreadyDeclared(const std::vector<AssetEntry>& entries, const std::string_view name,
                                       AssetType& claimedBy)
{
    for (const AssetEntry& existing : entries)
    {
        if (existing.name == name)
        {
            claimedBy = existing.type;
            return true;
        }
    }

    return false;
}

/// True when a `Texture` entry with this exact name appears in `entries`.
///
/// This is the texture-before-animation rule, and it is checked against the
/// entries already parsed rather than against a separate set, so there is exactly
/// one place that knows what the file has declared so far. A new entry kind
/// cannot accidentally forget to register itself here.
///
/// Only `Texture` counts. Naming a font, or another animation, is not a texture,
/// and treating it as one would let a chain of animations reference each other
/// with no image anywhere in it.
[[nodiscard]] bool textureDeclaredBefore(const std::vector<AssetEntry>& entries, const std::string_view name)
{
    for (const AssetEntry& existing : entries)
    {
        if (existing.type == AssetType::Texture && existing.name == name)
        {
            return true;
        }
    }

    return false;
}

/// Everything known about an entry before the field that is missing: its name
/// once that has been read, and the texture it animates once that has been read
/// too.
///
/// Built from what is actually present rather than from the position of the
/// missing field, because the missing field is by definition not readable and
/// asking for it would defeat the purpose. A message about a missing name has
/// nothing to say, one about a missing texture says the name, and one about a
/// missing frame count says both.
[[nodiscard]] std::string describeSoFar(const std::array<std::string_view, kMaxTokenCount>& tokens)
{
    if (tokens[1].empty())
    {
        return {};
    }

    if (tokens[2].empty())
    {
        return " '" + std::string{tokens[1]} + "'";
    }

    return " '" + std::string{tokens[1]} + "' animating texture '" + std::string{tokens[2]} + "'";
}

/// Reports the field a line of `type` is missing, or does nothing if it has them
/// all.
///
/// The exact token count is deliberately not used for a *short* line. A line with
/// two tokens could be "no name" or "a name and no path", and those are different
/// mistakes with different fixes, so each is named. A line with too many tokens
/// is a different situation again and is reported by the caller, which knows the
/// true count.
///
/// Called only once the line is known to be a recognised keyword with at least one
/// token, so `tokens[0]` is safe to read and `type` is real.
void reportMissingField(const AssetType type, const std::array<std::string_view, kMaxTokenCount>& tokens,
                        const std::size_t tokenCount, const std::size_t lineNumber)
{
    // Each kind walks its own field list, so a new field is added in one place
    // rather than by widening a single shared count. The two lists differ in
    // their second field: an animation names a texture there, where the others
    // name a path, and the message has to say which.
    constexpr std::array<const char*, kMaxTokenCount> kAnimationFields{"name", "textureName", "frameCount",
                                                                     "speed"};

    if (type == AssetType::Animation)
    {
        // `tokens[0]` is the keyword, so a token at index i names
        // `kAnimationFields[i - 1]`. The table is indexed by token position
        // minus the keyword rather than the other way round, because the missing
        // field's token position is exactly the token count.
        for (std::size_t token = 1U; token < tokenCount; ++token)
        {
            if (tokens[token].empty())
            {
                fail(lineNumber, std::string{"Animation entry"} + describeSoFar(tokens) + " is missing a " +
                                     kAnimationFields[token - 1U] + "; " + expectedSyntax(type));
            }
        }

        if (tokenCount < kMaxTokenCount)
        {
            fail(lineNumber, std::string{"Animation entry"} + describeSoFar(tokens) + " is missing a " +
                                 kAnimationFields[tokenCount - 1U] + "; " + expectedSyntax(type));
        }

        return;
    }

    if (tokenCount < 2U)
    {
        fail(lineNumber, describe(type) + " entry is missing a name; " + expectedSyntax(type));
    }

    if (tokens[1].empty())
    {
        fail(lineNumber, describe(type) + " entry is missing a name; " + expectedSyntax(type));
    }

    if (tokenCount < 3U || tokens[2].empty())
    {
        fail(lineNumber, describe(type) + " entry '" + std::string{tokens[1]} + "' is missing a path; " +
                             expectedSyntax(type));
    }
}

/// The two integers an animation line carries, validated.
///
/// Split out so the field-presence problem and the field-content problem are
/// reported in that order: a caller that reached here has a line whose arity is
/// right or short, and a line that says `three` instead of `3` is telling the
/// truth about its shape, so the shape is not the thing to report.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> parseAnimationNumbers(
    const std::array<std::string_view, kMaxTokenCount>& tokens, const std::size_t lineNumber)
{
    constexpr AssetType kAnimation = AssetType::Animation;

    return {parsePositiveInteger(tokens[3], "frameCount", kAnimation, lineNumber),
            parsePositiveInteger(tokens[4], "speed", kAnimation, lineNumber)};
}

/// Parses one `Animation` line: keyword, name, texture name, frame count, speed.
[[nodiscard]] AssetEntry parseAnimationEntry(const std::array<std::string_view, kMaxTokenCount>& tokens,
                                             const std::vector<AssetEntry>& entries, const std::size_t lineNumber)
{
    // The two numbers are validated before the texture reference is looked up. A
    // line whose frame count is not a number makes the rest of the line
    // unreadable, so that is the mistake worth naming first.
    const auto [frameCount, speed] = parseAnimationNumbers(tokens, lineNumber);

    if (!textureDeclaredBefore(entries, tokens[2]))
    {
        fail(lineNumber, std::string{"Animation entry '"} + std::string{tokens[1]} + "' animates texture '" +
                             std::string{tokens[2]} +
                             "', which no earlier Texture entry declares; a texture must be defined before an "
                             "animation that uses it");
    }

    return AssetEntry{AssetType::Animation, std::string{tokens[1]}, {}, std::string{tokens[2]}, frameCount, speed};
}

void parseLine(const std::string_view line, const std::size_t lineNumber, std::vector<AssetEntry>& entries)
{
    std::array<std::string_view, kMaxTokenCount> tokens{};

    const std::size_t tokenCount = tokenize(line, tokens);

    // A blank line, or a whole-line comment. The comment test is on the first
    // token rather than a scan for '#', so it accepts both '# note' and
    // '#Texture note' without treating a '#' further along the line as one.
    if (tokenCount == 0 || tokens[0].front() == '#')
    {
        return;
    }

    // The type is resolved before the arity is checked, so a line that is wrong
    // in both ways still reports the more fundamental problem. An unknown keyword
    // with six tokens says the keyword is unknown, rather than complaining about
    // a token count derived from a type that was never resolved.
    const AssetType type = typeOfToken(tokens[0], lineNumber);

    if (tokenCount > tokenCountOf(type))
    {
        fail(lineNumber, describe(type) + " entry has " + std::to_string(tokenCount) + " tokens; " +
                             expectedSyntax(type));
    }

    // A line that is too short, or whose field is present but blank, is reported
    // by field name rather than by count. See reportMissingField.
    reportMissingField(type, tokens, tokenCount, lineNumber);

    // The duplicate check runs for every kind alike, so a name is claimed at most
    // once no matter which kind claims it. It runs after the arity check, so a
    // line that is both a duplicate and short a field reports the short field,
    // which is the more fundamental of the two.
    AssetType claimedBy = AssetType::Texture;
    if (nameAlreadyDeclared(entries, tokens[1], claimedBy))
    {
        fail(lineNumber, "duplicate asset name '" + std::string{tokens[1]} + "'; it is already declared as a " +
                             describe(claimedBy));
    }

    if (type == AssetType::Animation)
    {
        entries.push_back(parseAnimationEntry(tokens, entries, lineNumber));
    }
    else
    {
        // Only the path can still be blank here; the name was checked above.
        if (tokens[2].empty())
        {
            fail(lineNumber, describe(type) + " entry '" + std::string{tokens[1]} + "' is missing a path; " +
                                 expectedSyntax(type));
        }

        entries.push_back(AssetEntry{type, std::string{tokens[1]}, std::string{tokens[2]}, {}, 0U, 0U});
    }
}


} // namespace

std::vector<AssetEntry> parseAssetFile(const std::string_view contents)
{
    std::vector<AssetEntry> entries;

    std::size_t lineNumber = 0;
    std::size_t cursor = 0;

    // The loop condition is `<=` rather than `<` so that a file ending in a
    // newline still visits the empty final line, which keeps the line counter
    // honest if that ever stops being a harmless no-op.
    while (cursor <= contents.size())
    {
        const std::size_t newline = contents.find('\n', cursor);
        const std::size_t lineEnd = (newline == std::string_view::npos) ? contents.size() : newline;
        const std::string_view line = contents.substr(cursor, lineEnd - cursor);

        ++lineNumber;
        parseLine(line, lineNumber, entries);

        if (newline == std::string_view::npos)
        {
            break;
        }

        cursor = newline + 1U;
    }

    return entries;
}

} // namespace engine::assets
