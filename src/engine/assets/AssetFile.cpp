#include "engine/assets/AssetFile.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace engine::assets
{
namespace
{

/// An entry has exactly three fields: a type, a name and a path. Only those are
/// ever stored, so a line with more fields is counted but not kept.
constexpr std::size_t kFieldCount = 3;

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

/// Splits `line` into whitespace separated tokens and returns how many there
/// are, writing the first `kFieldCount` of them into `tokens`.
///
/// The count is the **true** token count, not the number stored. A line with six
/// fields reports six even though only three are kept, because that count goes
/// straight into the error message: telling someone their line has four tokens
/// when it has six would send them editing the wrong field. Anything past the
/// third token is counted and discarded, so the first three always line up with
/// the type, the name and the path.
[[nodiscard]] std::size_t tokenize(const std::string_view line,
                                  std::array<std::string_view, kFieldCount>& tokens) noexcept
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

        if (stored < kFieldCount)
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
    }

    return "Texture";
}

[[nodiscard]] std::string describe(const AssetType type) { return std::string{keywordOf(type)}; }

/// Throws a located parse error. `[[noreturn]]` so every caller can treat this
/// as terminating without a dummy return afterwards.
[[noreturn]] void fail(const std::size_t lineNumber, const std::string& message)
{
    throw AssetParseError{"line " + std::to_string(lineNumber) + ": " + message};
}

/// The expected spelling, appended to the errors that are really about a
/// missing field. It makes the format self-documenting from the error alone.
[[nodiscard]] const char* expectedSyntax() noexcept { return "expected 'Texture <name> <path>'"; }

/// Maps the leading token to a type, or throws.
///
/// `Animation` gets its own message because it is a keyword the course asset
/// format really does use, so telling the user it is unrecognised would be
/// wrong. It is recognised, and deliberately not yet supported.
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
        fail(lineNumber,
             "'Animation' is not supported yet; this engine understands only 'Texture' and 'Font' entries");
    }

    fail(lineNumber, "unknown asset type '" + std::string{token} + "'");
}

void parseLine(const std::string_view line, const std::size_t lineNumber, std::vector<AssetEntry>& entries)
{
    std::array<std::string_view, kFieldCount> tokens{};

    const std::size_t tokenCount = tokenize(line, tokens);

    // A blank line, or a whole-line comment. The comment test is on the first
    // token rather than a scan for '#', so it accepts both '# note' and
    // '#Texture note' without treating a '#' further along the line as one.
    if (tokenCount == 0 || tokens[0].front() == '#')
    {
        return;
    }

    // The type is resolved before the arity is checked, so a line that is wrong
    // in both ways still reports the more fundamental problem. `Animation` with
    // a stray extra token says so, instead of complaining about token counts.
    const AssetType type = typeOfToken(tokens[0], lineNumber);

    if (tokenCount > kFieldCount)
    {
        fail(lineNumber,
             describe(type) + " entry has " + std::to_string(tokenCount) + " tokens; " + expectedSyntax());
    }

    // A blank field is indistinguishable from an absent one in a whitespace
    // separated format, so both spellings are reported as missing.
    if (tokenCount < 2U)
    {
        fail(lineNumber, describe(type) + " entry is missing a name; " + expectedSyntax());
    }

    if (tokenCount < 3U)
    {
        fail(lineNumber, describe(type) + " entry '" + std::string{tokens[1]} + "' is missing a path; " +
                                expectedSyntax());
    }

    // Names share one namespace, so a duplicate is looked for across every
    // entry parsed so far rather than per type. Linear in the number of
    // entries, which for a configuration file means tens, and it keeps the
    // parser to a single container with nothing but the returned data in it.
    for (const AssetEntry& existing : entries)
    {
        if (existing.name == tokens[1])
        {
            fail(lineNumber, "duplicate asset name '" + std::string{tokens[1]} + "'; it is already declared as a " +
                                 describe(existing.type));
        }
    }

    entries.push_back(AssetEntry{type, std::string{tokens[1]}, std::string{tokens[2]}});
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
