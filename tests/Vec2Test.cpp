#include "engine/math/Vec2.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace
{

/// Tolerance used for results that go through sqrt, atan2, sin or cos. The
/// exact-equality cases use CHECK instead and are not affected by this.
constexpr float kTolerance = 1e-5f;

constexpr float kPi = 3.14159265358979323846f;

int g_failureCount = 0;

void check(const bool condition, const char* const expression, const char* const file, const int line)
{
    if (!condition)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed\n";
    }
}

void checkNear(const float actual, const float expected, const char* const expression, const char* const file,
               const int line)
{
    if (std::fabs(actual - expected) > kTolerance)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK_NEAR(" << expression << ") failed"
                  << "\n      actual = " << actual << ", expected = " << expected << '\n';
    }
}

/// The parenthesised macro arguments keep these macros safe in if/else chains.
#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)
#define CHECK_FALSE(expression) check(!(expression), "!" #expression, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected)                                                                        \
    checkNear((actual), (expected), #actual " ~= " #expected, __FILE__, __LINE__)

using engine::Vec2;

void testDefaultConstruction()
{
    const Vec2 zero;

    CHECK_NEAR(zero.x, 0.0f);
    CHECK_NEAR(zero.y, 0.0f);
    CHECK(zero == Vec2(0.0f, 0.0f));
}

void testParameterizedConstruction()
{
    const Vec2 vector(3.0f, 4.0f);

    CHECK_NEAR(vector.x, 3.0f);
    CHECK_NEAR(vector.y, 4.0f);
}

void testAddition()
{
    const Vec2 result = Vec2(1.0f, 2.0f) + Vec2(3.0f, 4.0f);

    CHECK(result == Vec2(4.0f, 6.0f));
}

void testSubtraction()
{
    const Vec2 result = Vec2(1.0f, 2.0f) - Vec2(3.0f, 4.0f);

    CHECK(result == Vec2(-2.0f, -2.0f));
    CHECK(-Vec2(1.0f, -2.0f) == Vec2(-1.0f, 2.0f));
}

void testScalarMultiplication()
{
    CHECK(Vec2(2.0f, 4.0f) * 3.0f == Vec2(6.0f, 12.0f));
    CHECK(3.0f * Vec2(2.0f, 4.0f) == Vec2(6.0f, 12.0f));
}

void testScalarDivision()
{
    CHECK(Vec2(6.0f, 12.0f) / 3.0f == Vec2(2.0f, 4.0f));
}

void testCompoundOperations()
{
    Vec2 vector(1.0f, 2.0f);
    vector += Vec2(3.0f, 4.0f);
    CHECK(vector == Vec2(4.0f, 6.0f));

    vector -= Vec2(1.0f, 2.0f);
    CHECK(vector == Vec2(3.0f, 4.0f));

    vector *= 2.0f;
    CHECK(vector == Vec2(6.0f, 8.0f));

    vector /= 2.0f;
    CHECK(vector == Vec2(3.0f, 4.0f));
}

void testEquality()
{
    CHECK(Vec2(1.0f, 2.0f) == Vec2(1.0f, 2.0f));
    CHECK_FALSE(Vec2(1.0f, 2.0f) == Vec2(1.0f, 2.5f));
}

void testInequality()
{
    CHECK(Vec2(1.0f, 2.0f) != Vec2(1.0f, 2.5f));
    CHECK_FALSE(Vec2(1.0f, 2.0f) != Vec2(1.0f, 2.0f));
}

void testLength()
{
    CHECK_NEAR(Vec2(3.0f, 4.0f).length(), 5.0f);
    CHECK_NEAR(Vec2().length(), 0.0f);
    CHECK_NEAR(Vec2(-3.0f, -4.0f).length(), 5.0f);
}

void testLengthSquared()
{
    CHECK_NEAR(Vec2(3.0f, 4.0f).lengthSquared(), 25.0f);
    CHECK_NEAR(Vec2().lengthSquared(), 0.0f);
}

void testDot()
{
    CHECK_NEAR(Vec2(1.0f, 2.0f).dot(Vec2(3.0f, 4.0f)), 11.0f);
    CHECK_NEAR(Vec2(1.0f, 0.0f).dot(Vec2(0.0f, 1.0f)), 0.0f);
    CHECK_NEAR(Vec2(1.0f, 0.0f).dot(Vec2(1.0f, 0.0f)), 1.0f);
}

void testNormalization()
{
    const Vec2 result = Vec2(3.0f, 4.0f).normalized();

    CHECK_NEAR(result.x, 0.6f);
    CHECK_NEAR(result.y, 0.8f);
    CHECK_NEAR(result.length(), 1.0f);
    CHECK_NEAR(Vec2(-2.0f, 0.0f).normalized().x, -1.0f);
}

void testNormalizationOfZeroVector()
{
    // Documented behaviour: the zero vector has no direction, so normalizing it
    // yields the zero vector rather than NaN or infinity.
    const Vec2 result = Vec2().normalized();

    CHECK(result == Vec2(0.0f, 0.0f));
    CHECK(!std::isnan(result.x));
    CHECK(!std::isnan(result.y));
    CHECK(std::isfinite(result.x));
    CHECK(std::isfinite(result.y));
}

void testDistance()
{
    CHECK_NEAR(Vec2(0.0f, 0.0f).distance(Vec2(3.0f, 4.0f)), 5.0f);
    CHECK_NEAR(Vec2(1.0f, 1.0f).distance(Vec2(4.0f, 5.0f)), 5.0f);

    // Distance is symmetric.
    CHECK_NEAR(Vec2(4.0f, 5.0f).distance(Vec2(1.0f, 1.0f)), 5.0f);
    CHECK_NEAR(Vec2(2.0f, 2.0f).distance(Vec2(2.0f, 2.0f)), 0.0f);
}

void testAngle()
{
    CHECK_NEAR(Vec2(1.0f, 0.0f).angle(), 0.0f);
    CHECK_NEAR(Vec2(0.0f, 1.0f).angle(), kPi / 2.0f);
    CHECK_NEAR(Vec2(-1.0f, 0.0f).angle(), kPi);
    CHECK_NEAR(Vec2(0.0f, -1.0f).angle(), -kPi / 2.0f);

    // The zero vector has no direction.
    CHECK_NEAR(Vec2().angle(), 0.0f);
}

void testRotation()
{
    const Vec2 result = Vec2(1.0f, 0.0f).rotated(kPi / 2.0f);

    CHECK_NEAR(result.x, 0.0f);
    CHECK_NEAR(result.y, 1.0f);

    // A quarter turn clockwise undoes a quarter turn counter-clockwise.
    const Vec2 roundTrip = Vec2(1.0f, 0.0f).rotated(kPi / 2.0f).rotated(-kPi / 2.0f);
    CHECK_NEAR(roundTrip.x, 1.0f);
    CHECK_NEAR(roundTrip.y, 0.0f);

    // Rotating a 3-4-5 vector must not change its length.
    CHECK_NEAR(Vec2(3.0f, 4.0f).rotated(0.7f).length(), 5.0f);

    // Rotating the zero vector leaves it zero.
    CHECK(Vec2().rotated(1.23f) == Vec2());
}

void testDivisionByZero()
{
    // Documented behaviour: division by zero yields the zero vector rather than
    // infinity or NaN.
    const Vec2 result = Vec2(3.0f, 4.0f) / 0.0f;

    CHECK(result == Vec2(0.0f, 0.0f));
    CHECK(std::isfinite(result.x));
    CHECK(std::isfinite(result.y));

    Vec2 inPlace(3.0f, 4.0f);
    inPlace /= 0.0f;
    CHECK(inPlace == Vec2(0.0f, 0.0f));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"default construction", &testDefaultConstruction},
        {"parameterized construction", &testParameterizedConstruction},
        {"addition", &testAddition},
        {"subtraction", &testSubtraction},
        {"scalar multiplication", &testScalarMultiplication},
        {"scalar division", &testScalarDivision},
        {"compound operations", &testCompoundOperations},
        {"equality", &testEquality},
        {"inequality", &testInequality},
        {"length", &testLength},
        {"lengthSquared", &testLengthSquared},
        {"dot product", &testDot},
        {"normalization", &testNormalization},
        {"normalization of zero vector", &testNormalizationOfZeroVector},
        {"distance", &testDistance},
        {"angle", &testAngle},
        {"rotation", &testRotation},
        {"division by zero", &testDivisionByZero},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;
        testCase();

        const bool passed = g_failureCount == failuresBefore;
        if (!passed)
        {
            ++failedGroups;
        }

        std::cout << (passed ? "  PASS  " : "  FAIL  ") << name << '\n';
    }

    const auto groupCount = sizeof(testCases) / sizeof(testCases[0]);

    if (g_failureCount != 0)
    {
        std::cerr << g_failureCount << " check(s) failed across " << failedGroups << " of " << groupCount
                  << " test groups\n";
        return EXIT_FAILURE;
    }

    std::cout << groupCount << " Vec2 test groups passed\n";
    return EXIT_SUCCESS;
}
