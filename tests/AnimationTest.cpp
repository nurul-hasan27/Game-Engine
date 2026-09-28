#include "engine/Application.hpp"
#include "engine/assets/AssetManager.hpp"
#include "engine/assets/SfmlAssetManager.hpp"
#include "engine/components/Animation.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Texture.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/Renderer.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/IntRect.hpp"
#include "engine/systems/AnimationSystem.hpp"
#include "engine/systems/RenderSystem.hpp"

#include <SFML/Graphics/Texture.hpp>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// Minimal harness, matching the style already used by the other test files.
// ---------------------------------------------------------------------------

int g_failureCount = 0;

void check(const bool condition, const char* const expression, const char* const file, const int line)
{
    if (!condition)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed\n";
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)

using engine::IntRect;
using engine::Vec2;
using engine::assets::Animation;
using engine::assets::AssetManager;
using engine::assets::AssetNotFoundError;
using engine::assets::SfmlAssetManager;
using engine::assets::Texture;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::graphics::Camera;
using engine::graphics::RenderTransform;
using engine::graphics::Renderer;
using engine::input::ActionState;
using engine::systems::advanceAnimation;
using engine::systems::AnimationSystem;

// ---------------------------------------------------------------------------
// Doubles
// ---------------------------------------------------------------------------

/// An asset manager holding definitions only, with no images behind them.
///
/// Every group here is about playback arithmetic and about what a system submits,
/// and neither needs pixels. The groups that need a real image use a real
/// `SfmlAssetManager` and are marked as such.
class FakeAssetManager final : public AssetManager
{
public:
    const Texture& texture(const std::string_view name) const override
    {
        const auto found = m_textures.find(std::string{name});
        if (found == m_textures.end())
        {
            throw AssetNotFoundError{"no texture named '" + std::string{name} + "'"};
        }
        return found->second;
    }

    const engine::assets::Font& font(const std::string_view) const override
    {
        throw AssetNotFoundError{"this double declares no fonts"};
    }

    const Animation& animation(const std::string_view name) const override
    {
        ++m_lookups;
        const auto found = m_animations.find(std::string{name});
        if (found == m_animations.end())
        {
            throw AssetNotFoundError{"no animation named '" + std::string{name} + "'"};
        }
        return found->second;
    }

    void declare(const std::string& name, const std::uint32_t frameCount, const std::uint32_t speed,
                 const int frameWidth = 32, const int frameHeight = 32)
    {
        m_animations.emplace(name, Animation{name + "_texture", frameCount, speed, frameWidth, frameHeight});
        m_textures.emplace(name + "_texture", Texture{});
    }

    /// The texture name an animation declared through `declare` resolves to.
    [[nodiscard]] std::string textureNameFor(const std::string& animationName) const
    {
        return animationName + "_texture";
    }

    [[nodiscard]] std::size_t animationLookups() const noexcept { return m_lookups; }

private:
    std::map<std::string, Animation> m_animations;
    std::map<std::string, Texture> m_textures;
    mutable std::size_t m_lookups = 0;
};

/// A renderer that records what it was asked to draw, including the region.
struct DrawCall
{
    const Texture* texture = nullptr;
    RenderTransform placement;
    std::optional<IntRect> source;
    std::string order;
};

class RecordingRenderer final : public Renderer
{
public:
    void beginFrame() override
    {
        m_draws.clear();
    }

    void clear(const engine::Color&) override {}

    void drawRectangle(const Vec2&, const engine::Color&, const RenderTransform&) override {}

    void drawTexture(const Texture& texture, const RenderTransform& placement,
                     const std::optional<IntRect>& source) override
    {
        m_draws.push_back(DrawCall{&texture, placement, source, source.has_value() ? "region" : "whole"});
    }

    void endFrame() override { ++m_frames; }

    [[nodiscard]] std::uint64_t frameCount() const noexcept override { return m_frames; }

    [[nodiscard]] const std::vector<DrawCall>& draws() const noexcept { return m_draws; }

private:
    std::vector<DrawCall> m_draws;
    std::uint64_t m_frames = 0;
};

/// A real manager over the committed configuration.
[[nodiscard]] SfmlAssetManager shippedManager()
{
    return SfmlAssetManager{std::filesystem::path{ENGINE_ASSET_CONFIG}};
}

[[nodiscard]] Camera identityCamera() { return {}; }

/// The animation component of the first **alive** entity.
///
/// Only safe to call while something is still alive: `getEntities()` skips entities
/// flagged dead, so on an empty view this dereferences the end iterator. Where a
/// group needs to inspect a component after `destroyEntity()` has flagged it, it
/// holds the `Entity&` instead - the component is still stored until the manager's
/// cleanup, and reading it then is how "flagged, not erased" is observed.
[[nodiscard]] engine::components::Animation& firstAnimation(EntityManager& entities)
{
    return entities.getEntities().begin()->getComponent<engine::components::Animation>();
}

/// An entity with a Transform and an animation component, for the systems to act
/// on. The tag is not used by anything here but keeps the ECS honest about what
/// it is building.
/// The common case: an entity that is only ever reached through a query, so
/// there is no reason to name it. Not `[[nodiscard]]`, because not naming it is
/// the point.
Entity& addAnimated(EntityManager& entities, const std::string& tag, const std::string& animationName,
                    const bool repeat = true)
{
    Entity& entity = entities.addEntity(tag);
    entity.addComponent<Transform>(Transform{Vec2{100.0F, 100.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});

    engine::components::Animation animation;
    animation.assetName = animationName;
    animation.repeat = repeat;
    entity.addComponent<engine::components::Animation>(animation);

    return entity;
}

// ---------------------------------------------------------------------------
// Compile-time guarantees
//
// The shape of the component and the system, checked where only the compiler can.
// ---------------------------------------------------------------------------

// The component is plain data: no SFML, no asset types, no resource ownership. If
// it ever grew a handle the ECS would be dragging the graphics library into every
// system that reads an entity.
static_assert(std::is_same_v<decltype(engine::components::Animation::assetName), std::string>,
              "the component must name its animation, not hold it");
static_assert(std::is_same_v<decltype(engine::components::Animation::currentFrame), std::uint32_t>,
              "the current frame must be a plain integer");
static_assert(std::is_same_v<decltype(engine::components::Animation::repeat), bool>,
              "repeat must be a plain flag");

// It is copyable, unlike a texture handle, because it owns nothing. A component
// the ECS could not copy would not be a component.
static_assert(std::is_copy_constructible_v<engine::components::Animation>,
              "an animation component owns nothing and must be copyable");
static_assert(std::is_copy_assignable_v<engine::components::Animation>,
              "an animation component must be copy assignable");
static_assert(std::is_default_constructible_v<engine::components::Animation>,
              "an animation component must have a default state");

// The system takes the manager by const reference and cannot be built without one.
static_assert(!std::is_default_constructible_v<AnimationSystem>,
              "a system that resolves animation names must be given a manager");
static_assert(std::is_constructible_v<AnimationSystem, const AssetManager&>,
              "the system must accept the manager by const reference");
// The manager arrives by const reference, so the system cannot reach a mutating
// member through it. Asserted on the *interface* rather than on the constructor,
// because a `const&` parameter also accepts a non-const argument and so says
// nothing by itself - what matters is that every lookup through the interface is
// const-qualified, which is checkable.
static_assert(std::is_same_v<decltype(&AssetManager::animation),
                             const Animation& (AssetManager::*)(std::string_view) const>,
              "the animation lookup must be const-qualified on the interface");
static_assert(std::is_same_v<decltype(&AssetManager::texture),
                             const Texture& (AssetManager::*)(std::string_view) const>,
              "the texture lookup must be const-qualified on the interface");
static_assert(!std::is_copy_constructible_v<AnimationSystem>, "a system must not be copyable");

// The advance rule is a free function, so playback is testable with no ECS, no
// manager and no graphics library. That is the reason it is not a private method.
static_assert(std::is_same_v<decltype(advanceAnimation), void(engine::components::Animation&, std::uint32_t, std::uint32_t) noexcept>,
              "the advance rule must be a free function taking the state and the definition's two numbers");

// ---------------------------------------------------------------------------
// The component
// ---------------------------------------------------------------------------

void testTheComponentStartsOnTheFirstFrame()
{
    const engine::components::Animation animation;

    // Frame zero, not frame one. The first frame is what a sprite shows before
    // anything has animated, so a one-frame animation and an unstarted one look
    // the same rather than an entity being briefly invisible.
    CHECK(animation.currentFrame == 0U);
    CHECK(animation.ticksOnFrame == 0U);
    CHECK(animation.assetName.empty());
    CHECK_FALSE(animation.ended);
}

void testTheComponentRepeatsByDefault()
{
    // A looping animation is the ordinary case - a running character, a spinning
    // coin - so it is the default and an effect that plays once has to say so.
    const engine::components::Animation animation;
    CHECK(animation.repeat);
}

void testTheComponentIsAggregateInitialisable()
{
    // The house style for a component: plain public fields, no constructor. A
    // component with a constructor and private state would need a builder, and
    // every spawn site would have to know the field order.
    const engine::components::Animation animation{"walk", 1U, 3U, false, true};

    CHECK(animation.assetName == "walk");
    CHECK(animation.currentFrame == 1U);
    CHECK(animation.ticksOnFrame == 3U);
    CHECK_FALSE(animation.repeat);
    CHECK(animation.ended);
}

void testTwoComponentsAreIndependent()
{
    // The single most important property of the design. Two entities naming the
    // same animation must be able to be on different frames, and copying a
    // component must not couple the two.
    engine::components::Animation first;
    first.assetName = "walk";

    engine::components::Animation second = first;
    second.currentFrame = 1U;
    second.ticksOnFrame = 5U;

    CHECK(first.currentFrame == 0U);
    CHECK(first.ticksOnFrame == 0U);
    CHECK(second.currentFrame == 1U);
    CHECK(second.ticksOnFrame == 5U);
    CHECK(first.assetName == second.assetName);
}

// ---------------------------------------------------------------------------
// The advance rule
//
// Every case below is the course's rule expressed as an exact sequence. With
// `speed` game frames per animation frame, frame n is showing on game frames
// n * speed through (n + 1) * speed - 1.
// ---------------------------------------------------------------------------

/// Runs `ticks` game frames and returns the frame showing after each one,
/// including the frame before the first tick as element zero.
[[nodiscard]] std::vector<std::uint32_t> frameSequence(const std::uint32_t frameCount, const std::uint32_t speed,
                                                       const bool repeat, const std::size_t ticks)
{
    engine::components::Animation state;
    state.repeat = repeat;

    std::vector<std::uint32_t> frames;
    frames.push_back(state.currentFrame);
    for (std::size_t tick = 0; tick < ticks; ++tick)
    {
        advanceAnimation(state, frameCount, speed);
        frames.push_back(state.currentFrame);
    }

    return frames;
}

void testSpeedOneChangesEveryGameFrame()
{
    // The course's one-frame animations declare speed 1, so the sequence has no
    // repeated entries at all.
    const std::vector<std::uint32_t> frames = frameSequence(4U, 1U, true, 6U);

    CHECK(frames.size() == 7U);
    if (frames.size() != 7U)
    {
        return;
    }

    const std::vector<std::uint32_t> expected{0U, 1U, 2U, 3U, 0U, 1U, 2U};
    CHECK(frames == expected);
}

void testSpeedTwoHoldsEachFrameForTwoGameFrames()
{
    // The example the course's speed definition produces: four frames at speed 2
    // is 0, 0, 1, 1, 2, 2, ...
    const std::vector<std::uint32_t> frames = frameSequence(4U, 2U, true, 8U);

    const std::vector<std::uint32_t> expected{0U, 0U, 1U, 1U, 2U, 2U, 3U, 3U, 0U};
    CHECK(frames == expected);
}

void testSpeedThreeHoldsEachFrameForThreeGameFrames()
{
    const std::vector<std::uint32_t> frames = frameSequence(3U, 3U, true, 9U);

    const std::vector<std::uint32_t> expected{0U, 0U, 0U, 1U, 1U, 1U, 2U, 2U, 2U, 0U};
    CHECK(frames == expected);
}

void testTheFirstChangeHappensOnGameFrameSpeed()
{
    // Not on game frame 1 and not on game frame speed - 1. An animation that
    // changed early would show its second frame for one tick too few, and the
    // error would be invisible in a still and obvious in motion.
    for (std::uint32_t speed = 1U; speed <= 6U; ++speed)
    {
        engine::components::Animation state;
        state.repeat = true;

        for (std::uint32_t tick = 1U; tick < speed; ++tick)
        {
            advanceAnimation(state, 4U, speed);
            CHECK(state.currentFrame == 0U);
        }

        advanceAnimation(state, 4U, speed);
        CHECK(state.currentFrame == 1U);
    }
}

void testARepeatingAnimationWrapsToFrameZero()
{
    engine::components::Animation state;
    state.repeat = true;

    // Three frames, speed 1, so the wrap happens on the third tick.
    advanceAnimation(state, 3U, 1U);
    CHECK(state.currentFrame == 1U);
    advanceAnimation(state, 3U, 1U);
    CHECK(state.currentFrame == 2U);
    advanceAnimation(state, 3U, 1U);
    CHECK(state.currentFrame == 0U);
    CHECK_FALSE(state.ended);
}

void testARepeatingAnimationNeverReportsEnded()
{
    // The rule the reference implementation gets wrong. Its `hasEnded()` returns
    // true whenever the last frame happens to be on screen, which for a looping
    // animation is once per cycle - so a caller that destroyed on it would delete
    // a running character every time it reached the end of its run cycle.
    engine::components::Animation state;
    state.repeat = true;

    for (std::uint32_t tick = 0; tick < 200U; ++tick)
    {
        advanceAnimation(state, 4U, 1U);
        CHECK_FALSE(state.ended);
    }
}

void testARepeatingAnimationCanBeRunForEver()
{
    // A long run, to catch an off-by-one that would eventually walk the index off
    // the end or stop wrapping. 1000 game frames of a 3-frame animation at speed 2
    // is 166 complete cycles.
    engine::components::Animation state;
    state.repeat = true;

    for (std::uint32_t tick = 0; tick < 1000U; ++tick)
    {
        advanceAnimation(state, 3U, 2U);
        CHECK(state.currentFrame < 3U);
        CHECK(state.ticksOnFrame < 2U);
        CHECK_FALSE(state.ended);
    }
}

void testANonRepeatingAnimationStopsOnItsFinalFrame()
{
    engine::components::Animation state;
    state.repeat = false;

    advanceAnimation(state, 3U, 1U);
    CHECK(state.currentFrame == 1U);
    CHECK_FALSE(state.ended);

    advanceAnimation(state, 3U, 1U);
    CHECK(state.currentFrame == 2U);
    CHECK_FALSE(state.ended);

    // Still frame 2, and now ended.
    advanceAnimation(state, 3U, 1U);
    CHECK(state.currentFrame == 2U);
    CHECK(state.ended);
}

void testANonRepeatingAnimationShowsItsFinalFrameForAFullPeriod()
{
    // The reason the advance rule is shaped the way it is. The course's reference
    // implementation updates, checks for the end and destroys all *before*
    // rendering, so the final frame is selected and the entity removed in the same
    // frame - the last frame is never drawn.
    //
    // Here the animation only reports ended once the final frame has been held for
    // its full speed period, so it has genuinely been seen.
    //
    // Two frames at speed 3, numbered by the number of `update()` calls made so
    // far, which is the engine's definition of a game frame:
    //
    //   game frames 1, 2   frame 0
    //   game frames 3, 4, 5   frame 1, the final frame, on screen for exactly
    //                              `speed` frames
    //   game frame 6    ended
    //
    // The transition lands on the *third* update because that is the first one
    // where the tick counter reaches the speed - which is exactly what "every
    // `speed` game frames" means.
    engine::components::Animation state;
    state.repeat = false;

    std::vector<std::uint32_t> frames;
    for (std::uint32_t tick = 0; tick < 9U; ++tick)
    {
        advanceAnimation(state, 2U, 3U);
        frames.push_back(state.currentFrame);
    }

    const std::vector<std::uint32_t> expected{0U, 0U, 1U, 1U, 1U, 1U, 1U, 1U, 1U};
    CHECK(frames == expected);
    CHECK(state.ended);

    // The state is settled: back on frame 0 of the counter, so an ended animation
    // is not left half-way through a period.
    CHECK(state.ticksOnFrame == 0U);
    CHECK(state.currentFrame == 1U);
}

void testANonRepeatingAnimationStaysEnded()
{
    // Once it has finished it is finished. Advancing an ended animation would
    // either restart it or walk the index off the end, and there is nothing to
    // observe the difference - but a component that could un-end itself would be a
    // bug waiting for whoever destroyed the entity a frame too late.
    engine::components::Animation state;
    state.repeat = false;

    for (std::uint32_t tick = 0; tick < 10U; ++tick)
    {
        advanceAnimation(state, 2U, 1U);
    }
    CHECK(state.ended);

    const std::uint32_t settledFrame = state.currentFrame;
    for (std::uint32_t tick = 0; tick < 20U; ++tick)
    {
        advanceAnimation(state, 2U, 1U);
        CHECK(state.ended);
        CHECK(state.currentFrame == settledFrame);
    }
}

void testASingleFrameNonRepeatingAnimationEndsAfterOneGameFrame()
{
    // The course's level format makes every entity name an animation, including
    // ones that never change, so a one-frame animation is ordinary. It plays once
    // and is over, which is what "non-repeating" means for something with no
    // second frame to reach.
    engine::components::Animation state;
    state.repeat = false;

    advanceAnimation(state, 1U, 1U);
    CHECK(state.ended);
    CHECK(state.currentFrame == 0U);
}

void testASingleFrameRepeatingAnimationNeverEnds()
{
    // The same animation with repeat set: a static sprite, shown for ever.
    engine::components::Animation state;
    state.repeat = true;

    for (std::uint32_t tick = 0; tick < 50U; ++tick)
    {
        advanceAnimation(state, 1U, 1U);
        CHECK(state.currentFrame == 0U);
        CHECK_FALSE(state.ended);
    }
}

void testTheAdvanceIsDeterministic()
{
    // The same inputs must give the same sequence every time, on every platform.
    // A system that behaved differently on a second run could not be reasoned
    // about, and animation state is exactly the sort of thing that hides such a
    // bug until something looks slightly wrong.
    const std::vector<std::uint32_t> first = frameSequence(5U, 3U, true, 40U);
    const std::vector<std::uint32_t> second = frameSequence(5U, 3U, true, 40U);

    CHECK(first == second);
    CHECK(first.size() == 41U);
}

void testTheAdvanceResumesFromAStoredState()
{
    // Playback state is per entity, so a component can be written by hand and then
    // advanced. A goomba dropped into the world on its second frame must carry on
    // from there rather than snapping back to the start.
    engine::components::Animation state;
    state.currentFrame = 1U;
    state.ticksOnFrame = 4U;
    state.repeat = true;

    // Speed 8, so one more tick completes the period and moves to frame 2.
    advanceAnimation(state, 3U, 8U);
    CHECK(state.currentFrame == 1U);
    CHECK(state.ticksOnFrame == 5U);

    for (int tick = 0; tick < 3; ++tick)
    {
        advanceAnimation(state, 3U, 8U);
    }
    CHECK(state.currentFrame == 2U);
    CHECK(state.ticksOnFrame == 0U);
}

void testTheTicksOnFrameCounterNeverExceedsTheSpeed()
{
    // The counter is reset on every change, so it is always less than the speed.
    // A counter that could reach the speed would make the arithmetic ambiguous
    // about whether it had already changed.
    for (std::uint32_t speed = 1U; speed <= 8U; ++speed)
    {
        engine::components::Animation state;
        state.repeat = true;

        for (std::uint32_t tick = 0; tick < 40U; ++tick)
        {
            advanceAnimation(state, 5U, speed);
            CHECK(state.ticksOnFrame < speed);
        }
    }
}

void testAFrameCountOfZeroLeavesTheStateAlone()
{
    // Unreachable from a loaded asset - the parser refuses a frame count below 1 -
    // but the rule has to say what happens rather than index past the end.
    engine::components::Animation state;
    state.currentFrame = 0U;
    state.ticksOnFrame = 0U;
    state.repeat = true;

    for (std::uint32_t tick = 0; tick < 10U; ++tick)
    {
        advanceAnimation(state, 0U, 4U);
    }

    CHECK(state.currentFrame == 0U);
    CHECK(state.ticksOnFrame == 0U);
    CHECK_FALSE(state.ended);
}

void testASpeedOfZeroLeavesTheStateAlone()
{
    // The reference implementation's divide-by-zero. The parser refuses it, and
    // this says what would happen if it ever arrived anyway: nothing moves, rather
    // than dividing by zero or advancing every game frame by accident.
    engine::components::Animation state;
    state.currentFrame = 0U;
    state.ticksOnFrame = 0U;
    state.repeat = true;

    for (std::uint32_t tick = 0; tick < 10U; ++tick)
    {
        advanceAnimation(state, 4U, 0U);
    }

    CHECK(state.currentFrame == 0U);
    CHECK(state.ticksOnFrame == 0U);
    CHECK_FALSE(state.ended);
}

void testTheAdvanceDoesNotTouchTheName()
{
    // The asset is chosen by name and never rewritten by playback. A system that
    // changed the name would be re-pointing the entity at a different animation
    // mid-play, which nothing asked for.
    engine::components::Animation state;
    state.assetName = "walk";

    for (std::uint32_t tick = 0; tick < 10U; ++tick)
    {
        advanceAnimation(state, 3U, 1U);
    }

    CHECK(state.assetName == "walk");
}

void testTheAdvanceDoesNotTouchTheRepeatFlag()
{
    // Repeat is the caller's decision and is read, never written. A system that
    // cleared it would turn a looping animation into a one-shot without anything
    // asking.
    engine::components::Animation looping;
    looping.repeat = true;
    engine::components::Animation once;
    once.repeat = false;

    for (std::uint32_t tick = 0; tick < 20U; ++tick)
    {
        advanceAnimation(looping, 2U, 1U);
        advanceAnimation(once, 2U, 1U);
    }

    CHECK(looping.repeat);
    CHECK_FALSE(once.repeat);
}

// ---------------------------------------------------------------------------
// The system
// ---------------------------------------------------------------------------

void testTheSystemAdvancesEveryAnimatedEntity()
{
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "a", "walk");
    addAnimated(entities, "b", "walk");
    addAnimated(entities, "c", "walk");

    system.update(entities, actions, 0.0F);

    for (auto&& [entity, animation] : entities.query<engine::components::Animation>())
    {
        static_cast<void>(entity);
        CHECK(animation.currentFrame == 1U);
    }

    CHECK(assets.animationLookups() == 3U);
}

void testTheSystemIgnoresEntitiesWithoutAnAnimation()
{
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    Entity& plain = entities.addEntity("plain");
    plain.addComponent<Transform>(Transform{Vec2{1.0F, 1.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});

    // Must not throw, and must not reach the manager at all: there is nothing to
    // resolve.
    system.update(entities, actions, 0.0F);

    CHECK(assets.animationLookups() == 0U);
    CHECK(entities.aliveEntityCount() == 1U);
}

void testTheSystemIgnoresDeadEntities()
{
    // A destroyed entity is not advanced. It has already stopped existing, and
    // touching it would be the sort of thing that only shows up as a crash much
    // later.
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    Entity& entity = addAnimated(entities, "gone", "walk");
    entities.destroyEntity(entity);
    entities.update();

    system.update(entities, actions, 0.0F);

    CHECK(assets.animationLookups() == 0U);
    CHECK(entities.aliveEntityCount() == 0U);
}

void testTwoEntitiesShareAnAssetAndPlayIndependently()
{
    // The property the whole design exists for. One shared, immutable definition;
    // two entities on it; different frames at the same time, and they stay
    // different.
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    Entity& first = addAnimated(entities, "first", "walk");
    Entity& second = addAnimated(entities, "second", "walk");

    // Offset the second by starting it one frame along.
    second.getComponent<engine::components::Animation>().currentFrame = 2U;

    system.update(entities, actions, 0.0F);

    const engine::components::Animation& a = first.getComponent<engine::components::Animation>();
    const engine::components::Animation& b = second.getComponent<engine::components::Animation>();

    CHECK(a.currentFrame == 1U);
    CHECK(b.currentFrame == 3U);
    CHECK(a.currentFrame != b.currentFrame);

    // The asset itself is untouched by either of them.
    CHECK(assets.animation("walk").frameCount() == 4U);
    CHECK(assets.animation("walk").frameRect(0U) == (IntRect{0, 0, 32, 32}));
}

void testEntitiesStayOutOfStepOnlyIfTheyStartThatWay()
{
    // Two entities that start identical stay identical, and two that start
    // different stay different. Neither leaks into the other, which is the other
    // half of independence: it is not enough that they *can* differ.
    FakeAssetManager assets;
    assets.declare("walk", 3U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "twin_a", "walk");
    addAnimated(entities, "twin_b", "walk");
    Entity& offset = addAnimated(entities, "offset", "walk");
    offset.getComponent<engine::components::Animation>().currentFrame = 1U;

    for (std::uint32_t tick = 0; tick < 30U; ++tick)
    {
        system.update(entities, actions, 0.0F);
    }

    std::vector<std::uint32_t> frames;
    for (auto&& [entity, animation] : entities.query<engine::components::Animation>())
    {
        static_cast<void>(entity);
        frames.push_back(animation.currentFrame);
    }

    // Two on the same frame, one a frame behind: 30 ticks of a 3-frame animation
    // at speed 1 is 10 complete cycles, so the twins are back at frame 0 and the
    // offset one is on frame 1.
    CHECK(frames.size() == 3U);
    if (frames.size() == 3U)
    {
        CHECK(frames[0] == frames[1]);
        CHECK(frames[2] != frames[0]);
    }
}

void testTheSystemDestroysAFinishedNonRepeatingEntity()
{
    // The course's rule, and the reason the final frame is shown for a full period
    // first. `Application` runs this system, then the manager's cleanup, then
    // renders - so the entity is removed after its last frame has been presented.
    FakeAssetManager assets;
    assets.declare("blast", 2U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    // The entity is held rather than looked up, because the second update flags it
    // dead and from then on it is no longer in any view.
    Entity& entity = addAnimated(entities, "explosion", "blast", false);

    // Two frames at speed 1: the first update selects frame 1, and the second is
    // when it has been held for its full period and reports ended.
    system.update(entities, actions, 0.0F);
    CHECK(entities.aliveEntityCount() == 1U);
    CHECK_FALSE(entity.getComponent<engine::components::Animation>().ended);
    CHECK(entity.getComponent<engine::components::Animation>().currentFrame == 1U);

    system.update(entities, actions, 0.0F);
    CHECK(entity.getComponent<engine::components::Animation>().ended);

    // `destroyEntity` sets a flag rather than erasing, so the entity stops being
    // alive at once but is still stored. That is exactly what makes the system safe
    // to call from inside its own query, and it is why the removal happens after
    // this frame has been rendered.
    CHECK(entities.aliveEntityCount() == 0U);
    CHECK(entities.storedEntityCount() == 1U);

    // The manager's cleanup is what actually frees it, and `Application` runs that
    // between the systems and the render pass.
    entities.update();
    CHECK(entities.aliveEntityCount() == 0U);
    CHECK(entities.storedEntityCount() == 0U);
}

void testTheSystemNeverDestroysALoopingEntity()
{
    // The mistake the reference implementation invites. A looping animation's last
    // frame comes round every cycle; if that counted as "finished", a running
    // character would be deleted once per run cycle.
    FakeAssetManager assets;
    assets.declare("run", 4U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "runner", "run", true);

    for (std::uint32_t tick = 0; tick < 500U; ++tick)
    {
        system.update(entities, actions, 0.0F);
        entities.update();
    }

    CHECK(entities.aliveEntityCount() == 1U);
    CHECK_FALSE(firstAnimation(entities).ended);
}

void testTheSystemDestroysOnlyTheFinishedEntity()
{
    // Two entities, one looping and one not. Only the finished one goes, and the
    // survivor keeps playing: destruction is per entity, not a sweep.
    FakeAssetManager assets;
    assets.declare("run", 3U, 1U);
    assets.declare("blast", 2U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "runner", "run", true);
    addAnimated(entities, "explosion", "blast", false);

    for (std::uint32_t tick = 0; tick < 20U; ++tick)
    {
        system.update(entities, actions, 0.0F);
        entities.update();
    }

    CHECK(entities.aliveEntityCount() == 1U);
    CHECK(entities.getEntities().begin()->tag() == "runner");
}

void testTheSystemFailsLoudlyOnAnUndeclaredAnimation()
{
    // Not skipped, not defaulted. An entity naming an animation that does not
    // exist is a mistake in a level file or in code, and it surfaces here rather
    // than as an entity that mysteriously does not animate.
    FakeAssetManager assets;
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "ghost", "no_such_animation");

    std::string message;
    try
    {
        system.update(entities, actions, 0.0F);
    }
    catch (const AssetNotFoundError& error)
    {
        message = error.what();
    }

    CHECK(message == "no animation named 'no_such_animation'");
}

void testTheSystemIgnoresDeltaSeconds()
{
    // `speed` is counted in game frames, so the wall-clock length of a frame
    // cannot change the answer. Two runs with wildly different deltas must give
    // identical frame sequences - which is the observable form of "speed is not
    // seconds".
    FakeAssetManager assets;
    assets.declare("walk", 4U, 3U);
    AnimationSystem systemA{assets};
    AnimationSystem systemB{assets};
    ActionState actions;

    EntityManager a;
    EntityManager b;
    addAnimated(a, "x", "walk");
    addAnimated(b, "x", "walk");

    std::vector<std::uint32_t> framesA;
    std::vector<std::uint32_t> framesB;

    for (std::uint32_t tick = 0; tick < 30U; ++tick)
    {
        // Alternating between a 1 ms frame and a 100 ms one - a 100,000x
        // difference in wall-clock time, which is roughly the range between a
        // 1000 FPS machine and a debugger pause.
        systemA.update(a, actions, (tick % 2U == 0U) ? 0.001F : 0.1F);
        systemB.update(b, actions, 0.0F);
        framesA.push_back(firstAnimation(a).currentFrame);
        framesB.push_back(firstAnimation(b).currentFrame);
    }

    CHECK(framesA == framesB);
    CHECK(framesA.size() == 30U);
}

void testTheSystemResolvesOncePerEntityPerGameFrame()
{
    // One lookup per entity per frame, not zero and not several. Zero would mean
    // the name was never resolved; several would mean the definition was being
    // re-read from somewhere else, which is where shared state would creep in.
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U);
    AnimationSystem system{assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "a", "walk");
    addAnimated(entities, "b", "walk");

    system.update(entities, actions, 0.0F);
    CHECK(assets.animationLookups() == 2U);

    system.update(entities, actions, 0.0F);
    CHECK(assets.animationLookups() == 4U);
}

void testTheSystemDoesNotWriteToTheAsset()
{
    // Ten game frames of playback leave the shared definition exactly as it was.
    // Read by value, so a write to the copy could not be observed - which is the
    // point: the component's arithmetic has nothing to write to.
    FakeAssetManager assets;
    assets.declare("walk", 3U, 2U);
    AnimationSystem system{assets};
    ActionState actions;

    const Animation before = assets.animation("walk");

    EntityManager entities;
    addAnimated(entities, "a", "walk");

    for (std::uint32_t tick = 0; tick < 10U; ++tick)
    {
        system.update(entities, actions, 0.0F);
    }

    const Animation& after = assets.animation("walk");
    CHECK(after.frameCount() == before.frameCount());
    CHECK(after.speed() == before.speed());
    CHECK(after.textureName() == before.textureName());
    CHECK(after.frameWidth() == before.frameWidth());
    CHECK(after.frameHeight() == before.frameHeight());
    CHECK(after.frameRect(0U) == before.frameRect(0U));
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void testRenderSystemDrawsTheCurrentFrame()
{
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U, 32, 32);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    AnimationSystem animation{assets};
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "a", "walk");

    // Frame 2 of four, 32 pixels each: the region is the third column.
    entities.getEntities().begin()->getComponent<engine::components::Animation>().currentFrame = 2U;

    renderer.beginFrame();
    animation.update(entities, actions, 0.0F);
    render.update(entities, actions, 0.0F);

    // The advance moved it to frame 3, and the render submitted frame 3's region.
    CHECK(renderer.draws().size() == 1U);
    if (renderer.draws().size() != 1U)
    {
        return;
    }

    CHECK(renderer.draws()[0].source.has_value());
    if (renderer.draws()[0].source.has_value())
    {
        CHECK(*renderer.draws()[0].source == (IntRect{96, 0, 32, 32}));
    }
}

void testRenderSystemResolvesTheAnimationsOwnTexture()
{
    // The animation names a texture; the renderer follows that reference. The
    // component never carries the texture, so this is the only place the two are
    // joined.
    FakeAssetManager assets;
    assets.declare("walk", 2U, 1U, 50, 41);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "a", "walk");

    renderer.beginFrame();
    render.update(entities, actions, 0.0F);

    CHECK(renderer.draws().size() == 1U);
    if (renderer.draws().size() != 1U)
    {
        return;
    }

    // The handle the render system submitted is the one the manager holds for the
    // texture the animation names - not a copy, and not some other image.
    CHECK(renderer.draws()[0].texture == &assets.texture(assets.textureNameFor("walk")));
    CHECK(assets.animation("walk").textureName() == assets.textureNameFor("walk"));
}

void testRenderSystemFollowsTheFrameAsItChanges()
{
    // The whole animation path through a render system: a frame index that moves
    // becomes a region that moves, with nothing else changing.
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U, 32, 32);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    AnimationSystem animation{assets};
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "a", "walk");

    std::vector<IntRect> regions;
    for (std::uint32_t tick = 0; tick < 4U; ++tick)
    {
        renderer.beginFrame();
        animation.update(entities, actions, 0.0F);
        render.update(entities, actions, 0.0F);

        if (renderer.draws().size() == 1U && renderer.draws()[0].source.has_value())
        {
            regions.push_back(*renderer.draws()[0].source);
        }
    }

    CHECK(regions.size() == 4U);
    if (regions.size() != 4U)
    {
        return;
    }

    CHECK(regions[0] == (IntRect{32, 0, 32, 32}));
    CHECK(regions[1] == (IntRect{64, 0, 32, 32}));
    CHECK(regions[2] == (IntRect{96, 0, 32, 32}));
    CHECK(regions[3] == (IntRect{0, 0, 32, 32}));

    // Every region distinct, and they wrap.
    CHECK(regions[0] != regions[1]);
    CHECK(regions[3].left == 0U);
}

void testRenderSystemDrawsTwoEntitiesAtTheirOwnFrames()
{
    // Two entities, one asset, two regions in the same frame. This is what shared
    // assets plus per-entity state buys, and it is the thing the course's
    // reference design cannot do without copying the artwork per entity.
    FakeAssetManager assets;
    assets.declare("walk", 2U, 1U, 50, 41);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "a", "walk");
    addAnimated(entities, "b", "walk");
    entities.getEntities().begin()->getComponent<engine::components::Animation>().currentFrame = 1U;

    renderer.beginFrame();
    render.update(entities, actions, 0.0F);

    CHECK(renderer.draws().size() == 2U);
    if (renderer.draws().size() != 2U)
    {
        return;
    }

    // Creation order, so the first entity drew first.
    CHECK(*renderer.draws()[0].source == (IntRect{50, 0, 50, 41}));
    CHECK(*renderer.draws()[1].source == (IntRect{0, 0, 50, 41}));

    // Both reached the same texture, so the images are genuinely shared.
    CHECK(renderer.draws()[0].texture == renderer.draws()[1].texture);
}

void testAnAnimatedEntityIsNotAlsoDrawnAsAPlainTexture()
{
    // The priority rule. An entity holding both components is drawn once, as an
    // animation frame. Drawing it twice would put a whole sprite sheet on screen
    // behind a sprite of it.
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U, 32, 32);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    Entity& entity = addAnimated(entities, "a", "walk");
    entity.addComponent<engine::components::Texture>(engine::components::Texture{"walk_texture"});

    renderer.beginFrame();
    render.update(entities, actions, 0.0F);

    CHECK(renderer.draws().size() == 1U);
    if (renderer.draws().size() != 1U)
    {
        return;
    }

    // One call, and it is a region - so the animation won.
    CHECK(renderer.draws()[0].source.has_value());
    if (renderer.draws()[0].source.has_value())
    {
        CHECK(*renderer.draws()[0].source == (IntRect{0, 0, 32, 32}));
    }
}

void testAPlainTextureIsStillDrawnWhole()
{
    // The other half of the rule: an entity with only a `components::Texture` is
    // unaffected and still gets a whole-image draw.
    FakeAssetManager assets;
    assets.declare("ground", 1U, 1U, 64, 64);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    Entity& entity = entities.addEntity("brick");
    entity.addComponent<Transform>(Transform{Vec2{10.0F, 10.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<engine::components::Texture>(engine::components::Texture{"ground_texture"});

    renderer.beginFrame();
    render.update(entities, actions, 0.0F);

    CHECK(renderer.draws().size() == 1U);
    if (renderer.draws().size() != 1U)
    {
        return;
    }

    CHECK_FALSE(renderer.draws()[0].source.has_value());
}

void testAnimationsAreDrawnAfterPlainTexturesAndRectangles()
{
    // Three queries in a fixed order, and the order is the only thing deciding
    // what covers what. There is no layer field and no sorting, and a mutation that
    // moved a query would change this order.
    FakeAssetManager assets;
    assets.declare("ground", 1U, 1U, 64, 64);
    assets.declare("walk", 2U, 1U, 50, 41);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;

    Entity& rectangle = entities.addEntity("rect");
    rectangle.addComponent<Transform>(Transform{Vec2{1.0F, 1.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<engine::components::Rectangle>(engine::components::Rectangle{});

    Entity& texture = entities.addEntity("tex");
    texture.addComponent<Transform>(Transform{Vec2{2.0F, 2.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});
    texture.addComponent<engine::components::Texture>(engine::components::Texture{"ground_texture"});

    addAnimated(entities, "anim", "walk");

    renderer.beginFrame();
    render.update(entities, actions, 0.0F);

    // The recording renderer counts rectangles separately from textures, so this
    // checks the two texture-bearing queries in the right relative order: the plain
    // texture first, then the animation.
    CHECK(renderer.draws().size() == 2U);
    if (renderer.draws().size() != 2U)
    {
        return;
    }

    CHECK_FALSE(renderer.draws()[0].source.has_value());
    CHECK(renderer.draws()[1].source.has_value());
}

void testAnAnimationWithoutATransformIsNotDrawn()
{
    // A frame with nowhere to put it. The query needs both components, so an
    // animated entity with no position is simply not drawn - the same rule every
    // other renderable follows.
    FakeAssetManager assets;
    assets.declare("walk", 2U, 1U, 50, 41);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    Entity& entity = entities.addEntity("floating");
    engine::components::Animation animation;
    animation.assetName = "walk";
    entity.addComponent<engine::components::Animation>(animation);

    renderer.beginFrame();
    render.update(entities, actions, 0.0F);

    CHECK(renderer.draws().empty());
}

void testRenderSystemAppliesTheCameraToAnAnimation()
{
    // The camera is applied to the placement exactly as it is for a rectangle and
    // for a plain texture, so an animated sprite moves with the world and does not
    // sit still in the corner.
    FakeAssetManager assets;
    assets.declare("walk", 2U, 1U, 50, 41);
    RecordingRenderer renderer;

    Camera camera = identityCamera();
    camera.setPosition(Vec2{400.0F, 300.0F});
    camera.setZoom(2.0F);

    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "a", "walk");

    renderer.beginFrame();
    render.update(entities, actions, 0.0F);

    CHECK(renderer.draws().size() == 1U);
    if (renderer.draws().size() != 1U)
    {
        return;
    }

    const RenderTransform expected =
        toRenderTransform(Transform{Vec2{100.0F, 100.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F}, camera);

    CHECK(renderer.draws()[0].placement.position == expected.position);
    CHECK(renderer.draws()[0].placement.scale == expected.scale);
    CHECK(renderer.draws()[0].placement.position != Vec2{100.0F, 100.0F});
}

void testRenderSystemDoesNotWriteTheAnimationComponent()
{
    // Rendering reads the frame. If it advanced it as well, the animation would
    // move twice per game frame and the frame rate would depend on how many
    // systems happened to look at it.
    FakeAssetManager assets;
    assets.declare("walk", 4U, 1U, 32, 32);
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    engine::systems::RenderSystem render{renderer, camera, assets};
    ActionState actions;

    EntityManager entities;
    addAnimated(entities, "a", "walk");

    const std::uint32_t before = firstAnimation(entities).currentFrame;

    for (int pass = 0; pass < 5; ++pass)
    {
        renderer.beginFrame();
        render.update(entities, actions, 0.0F);
    }

    const std::uint32_t after = firstAnimation(entities).currentFrame;
    CHECK(after == before);
}

// ---------------------------------------------------------------------------
// Through the real manager and the real asset
// ---------------------------------------------------------------------------

void testTheShippedAnimationsPlay()
{
    // The entries `assets/assets.txt` declares, through the real loader and the real
    // artwork. Nine of them: the three multi-frame strips below, and the six
    // single-frame animations Phase 13 added for `assets/levels/level1.txt`. The
    // per-animation checks that follow are all about the multi-frame ones, because a
    // one-frame animation has a frame size and nothing else to assert.
    SfmlAssetManager assets = shippedManager();

    CHECK(assets.animationCount() == 9U);
    CHECK(assets.animation("mario_GoombaWalk_walk").frameCount() == 2U);
    CHECK(assets.animation("mario_GoombaWalk_walk").frameWidth() == 50);
    CHECK(assets.animation("mario_GoombaWalk_walk").frameHeight() == 41);
    CHECK(assets.animation("mario_GoombaDeath_flat").frameCount() == 2U);
    CHECK(assets.animation("mario_GoombaDeath_flat").frameWidth() == 30);
    CHECK(assets.animation("animations_explosion_burst").frameCount() == 12U);
    CHECK(assets.animation("animations_explosion_burst").frameWidth() == 96);
}

void testAShippedAnimationWalksThroughItsFrames()
{
    // The real two-frame Goomba at its real speed of 8 game frames, advanced the
    // way the system would.
    SfmlAssetManager assets = shippedManager();
    const Animation& definition = assets.animation("mario_GoombaWalk_walk");

    engine::components::Animation state;
    state.repeat = true;

    for (std::uint32_t tick = 0; tick < 8U; ++tick)
    {
        advanceAnimation(state, definition.frameCount(), definition.speed());
    }
    CHECK(state.currentFrame == 1U);
    CHECK_FALSE(state.ended);

    for (std::uint32_t tick = 0; tick < 8U; ++tick)
    {
        advanceAnimation(state, definition.frameCount(), definition.speed());
    }
    CHECK(state.currentFrame == 0U);
    CHECK_FALSE(state.ended);
}

void testAShippedNonRepeatingAnimationEnds()
{
    // The explosion: twelve frames at speed 8, played once. 96 game frames is one
    // full cycle plus the final frame's display period, so it must be ended by then
    // and still on frame 11.
    SfmlAssetManager assets = shippedManager();
    const Animation& definition = assets.animation("animations_explosion_burst");

    engine::components::Animation state;
    state.repeat = false;

    for (std::uint32_t tick = 0; tick < 104U; ++tick)
    {
        advanceAnimation(state, definition.frameCount(), definition.speed());
    }

    CHECK(state.ended);
    CHECK(state.currentFrame == 11U);
    CHECK(state.currentFrame < definition.frameCount());
}

void testEveryShippedAnimationHasAUsableFrame()
{
    // Whatever `assets/assets.txt` declares must be playable: a real texture, a
    // frame count that fits, and a frame size that is not zero. An animation whose
    // frame is empty would draw nothing and look like a missing asset.
    SfmlAssetManager assets = shippedManager();

    for (const char* name : {"mario_GoombaWalk_walk", "mario_GoombaDeath_flat", "animations_explosion_burst"})
    {
        const Animation& definition = assets.animation(name);

        CHECK(definition.frameCount() >= 1U);
        CHECK(definition.speed() >= 1U);
        CHECK(definition.frameWidth() > 0);
        CHECK(definition.frameHeight() > 0);
        CHECK(definition.isValidFrame(0U));
        CHECK_FALSE(definition.isValidFrame(definition.frameCount()));

        // Every frame is a real, non-empty region, and they tile the sheet.
        int expectedLeft = 0;
        for (std::uint32_t frame = 0U; frame < definition.frameCount(); ++frame)
        {
            const IntRect rect = definition.frameRect(frame);
            CHECK(rect.left == expectedLeft);
            CHECK(rect.top == 0);
            CHECK(rect.width == definition.frameWidth());
            CHECK(rect.height == definition.frameHeight());
            expectedLeft = rect.left + rect.width;
        }

        // The name is not a path and does not start with one.
        CHECK(!definition.textureName().empty());
        CHECK(definition.textureName().find('/') == std::string::npos);
    }
}

void testAnAnimatedEntityRunsThroughTheApplication()
{
    // The integration, through the `Application` the engine actually ships: its
    // manager resolves the name, its `AnimationSystem` advances the frame, and its
    // `RenderSystem` draws the region. The names exist only in the committed
    // configuration, so if any of the three were wired to something else the
    // lookup would throw out of `run()`.
    engine::Application application;

    // The engine registers **no** systems of its own - `main` does that, because
    // the engine stays unaware that a player or a camera exists. Registering here
    // is therefore the whole of the wiring, and it is also what the group is
    // checking: that `application.assets()` is enough to build a working system.
    application.systemManager().add<AnimationSystem>(application.assets());

    Entity& entity = application.entityManager().addEntity("goomba");
    entity.addComponent<Transform>(Transform{Vec2{200.0F, 200.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});

    engine::components::Animation animation;
    animation.assetName = "mario_GoombaWalk_walk";
    animation.repeat = true;
    entity.addComponent<engine::components::Animation>(animation);

    // 8 game frames is the first change for an animation of speed 8, so the
    // component really did advance rather than sitting still.
    CHECK(application.run(7) == EXIT_SUCCESS);
    CHECK(application.renderer().frameCount() == 7U);
    CHECK(entity.getComponent<engine::components::Animation>().currentFrame == 0U);

    // One more reaches the second frame, and eight more brings it back round.
    CHECK(application.run(1) == EXIT_SUCCESS);
    CHECK(application.renderer().frameCount() == 8U);
    CHECK(entity.getComponent<engine::components::Animation>().currentFrame == 1U);
    CHECK_FALSE(entity.getComponent<engine::components::Animation>().ended);

    CHECK(application.run(8) == EXIT_SUCCESS);
    CHECK(application.renderer().frameCount() == 16U);
    CHECK(entity.getComponent<engine::components::Animation>().currentFrame == 0U);
    CHECK(application.entityManager().aliveEntityCount() == 1U);
}

void testAFinishedAnimationIsRemovedByTheApplication()
{
    // The end-to-end lifetime, through the real loop: play once, show the last
    // frame, then go away. The entity is erased by the manager's cleanup, which
    // `Application` runs after the systems and before rendering.
    engine::Application application;
    application.systemManager().add<AnimationSystem>(application.assets());

    Entity& entity = application.entityManager().addEntity("explosion");
    entity.addComponent<Transform>(Transform{Vec2{200.0F, 200.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});

    engine::components::Animation animation;
    animation.assetName = "mario_GoombaDeath_flat";
    animation.repeat = false;
    entity.addComponent<engine::components::Animation>(animation);

    // Two frames at speed 6: frame 1 is selected on game frame 6 and is on screen
    // for game frames 6 through 11, and game frame 12 is when it reports ended.
    //
    // The entity must therefore still be there, still on its last frame, after 11
    // frames - that is the "the final frame is really shown" property, observed
    // through the real loop rather than through the advance function.
    CHECK(application.run(5) == EXIT_SUCCESS);
    CHECK(application.entityManager().aliveEntityCount() == 1U);
    CHECK(entity.getComponent<engine::components::Animation>().currentFrame == 0U);

    CHECK(application.run(6) == EXIT_SUCCESS);
    CHECK(application.entityManager().aliveEntityCount() == 1U);
    CHECK(entity.getComponent<engine::components::Animation>().currentFrame == 1U);
    CHECK_FALSE(entity.getComponent<engine::components::Animation>().ended);

    // Game frame 12: ended, flagged dead, and erased by the manager's cleanup in the
    // same update - so it is gone before the twelfth frame is rendered, and the
    // eleventh frame - the last one showing the final frame - was presented.
    CHECK(application.run(1) == EXIT_SUCCESS);
    CHECK(application.entityManager().aliveEntityCount() == 0U);
    CHECK(application.entityManager().storedEntityCount() == 0U);
    CHECK(application.renderer().frameCount() == 12U);
}

void testTheApplicationManagerIsReusedForBothSteps()
{
    // One manager, two lookups: the animation and the texture it names must come
    // from the same place. If the render system loaded its own texture, the
    // animation's frame geometry and the image it was measured from could come
    // from different files.
    engine::Application application;

    const engine::assets::Animation& definition = application.assets().animation("mario_GoombaWalk_walk");
    const Texture& image = application.assets().texture(definition.textureName());

    // The frame geometry was measured from *this* image: the platform texture
    // behind the handle really is the sheet the frames divide up. Compared against
    // the loaded size rather than assumed, so a definition measured from a
    // different file would fail here rather than only looking slightly wrong.
    const SfmlAssetManager& concrete = static_cast<const SfmlAssetManager&>(application.assets());
    const sf::Vector2u size = concrete.nativeTexture(image).getSize();

    CHECK(size == sf::Vector2u{100U, 41U});
    CHECK(definition.frameWidth() * static_cast<int>(definition.frameCount()) == static_cast<int>(size.x));
    CHECK(definition.frameHeight() == static_cast<int>(size.y));
}

// ---------------------------------------------------------------------------
// The SFML boundary
//
// The new public headers are checked from source, because nothing about a
// component's or a system's *behaviour* can reveal that it names a graphics type.
// Comments are stripped first, and that is not a nicety: several of these files
// name `sf::Sprite`, `sf::IntRect` or `SFML` while explaining that they never
// expose one, and a raw scan would match its own explanation.
// ---------------------------------------------------------------------------

/// Strips `//` comments and blank lines, leaving only code.
[[nodiscard]] std::string codeOf(const char* const path)
{
    std::ifstream file{path};
    if (!file)
    {
        std::cerr << "    unable to read source file: " << path << '\n';
        ++g_failureCount;
        return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    std::istringstream lines{buffer.str()};
    std::string code;
    std::string line;
    while (std::getline(lines, line))
    {
        const std::string withoutComment = line.substr(0, line.find("//"));
        if (withoutComment.find_first_not_of(" \t") != std::string::npos)
        {
            code += withoutComment;
            code += '\n';
        }
    }

    return code;
}

void testTheAnimationComponentHeaderNamesNoGraphicsType()
{
    const std::string code = codeOf(ENGINE_ANIMATION_COMPONENT_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK(code.find("sf::") == std::string::npos);
    CHECK(code.find("SFML") == std::string::npos);
    CHECK(code.find("#include <SFML") == std::string::npos);

    // Only the standard headers a component needs: a string for the name and a
    // fixed-width integer for the counters. Nothing else.
    CHECK(code.find("#include <string>") != std::string::npos);
    CHECK(code.find("#include <cstdint>") != std::string::npos);
    CHECK(code.find("#include") != std::string::npos);
}

void testTheAnimationComponentHoldsNoResource()
{
    const std::string code = codeOf(ENGINE_ANIMATION_COMPONENT_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // No asset type either. The component names an animation; it does not hold one.
    // An `assets::Animation` member would be a copy per entity of something every
    // entity shares, and an `assets::Texture` would drag the graphics library in.
    CHECK(code.find("assets::") == std::string::npos);
    CHECK(code.find("Texture") == std::string::npos);
    CHECK(code.find("Font") == std::string::npos);
    CHECK(code.find("IntRect") == std::string::npos);
    CHECK(code.find("Vec2") == std::string::npos);
}

void testTheAnimationSystemHeaderNamesNoGraphicsType()
{
    const std::string code = codeOf(ENGINE_ANIMATION_SYSTEM_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // The system names the asset *interface*, which is SFML-free by construction,
    // and nothing else graphics.
    CHECK(code.find("sf::") == std::string::npos);
    CHECK(code.find("SFML") == std::string::npos);
    CHECK(code.find("Sprite") == std::string::npos);
    CHECK(code.find("IntRect") == std::string::npos);
    CHECK(code.find("Texture") == std::string::npos);
}

void testTheAnimationSystemSourceNamesNoGraphicsType()
{
    // The .cpp too. A system that reached into SFML would compile fine and would
    // break the moment the engine was pointed at a different graphics library.
    const std::string code = codeOf(ENGINE_ANIMATION_SYSTEM_SOURCE);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK(code.find("sf::") == std::string::npos);
    CHECK(code.find("SFML") == std::string::npos);
    CHECK(code.find("Sprite") == std::string::npos);
}

void testTheRenderSystemNamesNoGraphicsType()
{
    // It gained a query and a source rectangle, and neither may have brought a
    // graphics type along.
    const std::string code = codeOf(ENGINE_RENDER_SYSTEM_SOURCE);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK(code.find("sf::") == std::string::npos);
    CHECK(code.find("SFML") == std::string::npos);
    CHECK(code.find("Sprite") == std::string::npos);

    // It uses the engine's own rect, not a graphics one.
    CHECK(code.find("IntRect") != std::string::npos);
}

void testTheRenderSystemDoesNotOwnPlaybackState()
{
    // The render query reads the frame and nothing else. If it advanced the
    // component as well, an animation would move twice per game frame and its rate
    // would depend on how many systems happened to look at it.
    const std::string code = codeOf(ENGINE_RENDER_SYSTEM_SOURCE);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK(code.find("advanceAnimation") == std::string::npos);
    CHECK(code.find("currentFrame =") == std::string::npos);
    CHECK(code.find("ticksOnFrame") == std::string::npos);

    // But it does read the frame, to select the region.
    CHECK(code.find("animation.currentFrame") != std::string::npos);
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string, void (*)()>> testCases{
        {"the component starts on the first frame", &testTheComponentStartsOnTheFirstFrame},
        {"the component repeats by default", &testTheComponentRepeatsByDefault},
        {"the component is aggregate initialisable", &testTheComponentIsAggregateInitialisable},
        {"two components are independent", &testTwoComponentsAreIndependent},
        {"speed one changes every game frame", &testSpeedOneChangesEveryGameFrame},
        {"speed two holds each frame for two game frames", &testSpeedTwoHoldsEachFrameForTwoGameFrames},
        {"speed three holds each frame for three game frames", &testSpeedThreeHoldsEachFrameForThreeGameFrames},
        {"the first change happens on game frame speed", &testTheFirstChangeHappensOnGameFrameSpeed},
        {"a repeating animation wraps to frame zero", &testARepeatingAnimationWrapsToFrameZero},
        {"a repeating animation never reports ended", &testARepeatingAnimationNeverReportsEnded},
        {"a repeating animation can be run for ever", &testARepeatingAnimationCanBeRunForEver},
        {"a non repeating animation stops on its final frame", &testANonRepeatingAnimationStopsOnItsFinalFrame},
        {"a non repeating animation shows its final frame for a full period",
         &testANonRepeatingAnimationShowsItsFinalFrameForAFullPeriod},
        {"a non repeating animation stays ended", &testANonRepeatingAnimationStaysEnded},
        {"a single frame non repeating animation ends after one game frame",
         &testASingleFrameNonRepeatingAnimationEndsAfterOneGameFrame},
        {"a single frame repeating animation never ends", &testASingleFrameRepeatingAnimationNeverEnds},
        {"the advance is deterministic", &testTheAdvanceIsDeterministic},
        {"the advance resumes from a stored state", &testTheAdvanceResumesFromAStoredState},
        {"the ticks on frame counter never exceeds the speed", &testTheTicksOnFrameCounterNeverExceedsTheSpeed},
        {"a frame count of zero leaves the state alone", &testAFrameCountOfZeroLeavesTheStateAlone},
        {"a speed of zero leaves the state alone", &testASpeedOfZeroLeavesTheStateAlone},
        {"the advance does not touch the name", &testTheAdvanceDoesNotTouchTheName},
        {"the advance does not touch the repeat flag", &testTheAdvanceDoesNotTouchTheRepeatFlag},
        {"the system advances every animated entity", &testTheSystemAdvancesEveryAnimatedEntity},
        {"the system ignores entities without an animation", &testTheSystemIgnoresEntitiesWithoutAnAnimation},
        {"the system ignores dead entities", &testTheSystemIgnoresDeadEntities},
        {"two entities share an asset and play independently", &testTwoEntitiesShareAnAssetAndPlayIndependently},
        {"entities stay out of step only if they start that way", &testEntitiesStayOutOfStepOnlyIfTheyStartThatWay},
        {"the system destroys a finished non repeating entity", &testTheSystemDestroysAFinishedNonRepeatingEntity},
        {"the system never destroys a looping entity", &testTheSystemNeverDestroysALoopingEntity},
        {"the system destroys only the finished entity", &testTheSystemDestroysOnlyTheFinishedEntity},
        {"the system fails loudly on an undeclared animation", &testTheSystemFailsLoudlyOnAnUndeclaredAnimation},
        {"the system ignores delta seconds", &testTheSystemIgnoresDeltaSeconds},
        {"the system resolves once per entity per game frame", &testTheSystemResolvesOncePerEntityPerGameFrame},
        {"the system does not write to the asset", &testTheSystemDoesNotWriteToTheAsset},
        {"render system draws the current frame", &testRenderSystemDrawsTheCurrentFrame},
        {"render system resolves the animation's own texture", &testRenderSystemResolvesTheAnimationsOwnTexture},
        {"render system follows the frame as it changes", &testRenderSystemFollowsTheFrameAsItChanges},
        {"render system draws two entities at their own frames", &testRenderSystemDrawsTwoEntitiesAtTheirOwnFrames},
        {"an animated entity is not also drawn as a plain texture",
         &testAnAnimatedEntityIsNotAlsoDrawnAsAPlainTexture},
        {"a plain texture is still drawn whole", &testAPlainTextureIsStillDrawnWhole},
        {"animations are drawn after plain textures and rectangles",
         &testAnimationsAreDrawnAfterPlainTexturesAndRectangles},
        {"an animation without a transform is not drawn", &testAnAnimationWithoutATransformIsNotDrawn},
        {"render system applies the camera to an animation", &testRenderSystemAppliesTheCameraToAnAnimation},
        {"render system does not write the animation component", &testRenderSystemDoesNotWriteTheAnimationComponent},
        {"the shipped animations play", &testTheShippedAnimationsPlay},
        {"a shipped animation walks through its frames", &testAShippedAnimationWalksThroughItsFrames},
        {"a shipped non repeating animation ends", &testAShippedNonRepeatingAnimationEnds},
        {"every shipped animation has a usable frame", &testEveryShippedAnimationHasAUsableFrame},
        {"an animated entity runs through the application", &testAnAnimatedEntityRunsThroughTheApplication},
        {"a finished animation is removed by the application", &testAFinishedAnimationIsRemovedByTheApplication},
        {"the animation component header names no graphics type", &testTheAnimationComponentHeaderNamesNoGraphicsType},
        {"the animation component holds no resource", &testTheAnimationComponentHoldsNoResource},
        {"the animation system header names no graphics type", &testTheAnimationSystemHeaderNamesNoGraphicsType},
        {"the animation system source names no graphics type", &testTheAnimationSystemSourceNamesNoGraphicsType},
        {"the render system names no graphics type", &testTheRenderSystemNamesNoGraphicsType},
        {"the render system does not own playback state", &testTheRenderSystemDoesNotOwnPlaybackState},
        {"the application manager is reused for both steps", &testTheApplicationManagerIsReusedForBothSteps},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;

        try
        {
            testCase();
        }
        catch (const std::exception& error)
        {
            ++g_failureCount;
            std::cerr << "    unexpected exception escaped group \"" << name << "\": " << error.what() << '\n';
        }

        const bool passed = g_failureCount == failuresBefore;
        if (!passed)
        {
            ++failedGroups;
        }

        std::cout << (passed ? "  PASS  " : "  FAIL  ") << name << '\n';
    }

    // `size()` rather than `sizeof(x) / sizeof(x[0])`: that idiom is for a C array
    // and silently truncates to zero for a `std::vector`, which made this file
    // report "0 test groups passed" while running 52 of them.
    const auto groupCount = testCases.size();

    if (g_failureCount != 0)
    {
        std::cerr << g_failureCount << " check(s) failed across " << failedGroups << " of " << groupCount
                  << " test groups\n";
        return EXIT_FAILURE;
    }

    std::cout << groupCount << " animation test groups passed\n";
    return EXIT_SUCCESS;
}
