#pragma once

namespace engine
{
namespace input
{
/// The per-frame action snapshot. Gameplay asks about actions, never about
/// physical keys; see [engine::input::ActionState].
class ActionState;
} // namespace input
} // namespace engine

namespace engine::ecs
{

class EntityManager;

/// The base class for every system.
///
/// A system is where behaviour lives. It holds no entities and no component
/// data of its own: it is handed the EntityManager, queries the components it
/// cares about, and writes behaviour into them.
///
/// The split is the whole point of the architecture. Components stay pure data,
/// entities stay bare identities, and "what happens each frame" is expressed
/// once, in a system, instead of being scattered through the things it acts on.
///
/// ```cpp
/// class MovementSystem final : public System
/// {
/// public:
///     void update(EntityManager& entities, const input::ActionState& actions, float deltaSeconds) override
///     {
///         for (auto&& [entity, transform] : entities.query<Transform>())
///         {
///             transform.velocity = directionFrom(actions) * speed;
///         }
///     }
///
///     [[nodiscard]] const char* name() const override { return "MovementSystem"; }
/// };
/// ```
///
/// ### Its dependencies are parameters
///
/// Everything a system is allowed to depend on arrives in the call: the world,
/// the frame's actions, and how long the frame took. None of it is a global, a
/// singleton, or something the system fetches by reaching into `Application`.
///
/// That is what keeps the dependency direction honest. A system can never
/// acquire input the way it would acquire a clock or a keyboard, so the only
/// way to give it input is to hand it some, and the only way to test it is to
/// hand it a fake.
///
/// ### Actions, not keys
///
/// The parameter is an [input::ActionState](engine/input/ActionState.hpp) and
/// **not** the raw keyboard. A gameplay system therefore *cannot* ask which
/// physical key is down: `ActionState` has no way to name one.
///
/// That is a stronger guarantee than a convention. Rebinding the controls,
/// supporting a controller, and replaying a recording are all changes to
/// [input::ActionMap](engine/input/ActionMap.hpp) alone, and no system is edited
/// for any of them.
///
/// Raw keyboard state is still available where it belongs - to the composition
/// root, to tools and to a debug overlay, through
/// [engine::Application::input](engine/Application.hpp). It is simply not
/// reachable from a system.
///
/// Systems that do not care about input simply ignore the parameter. One uniform
/// signature is worth more than a special case for systems that happen to be
/// input driven.
class System
{
public:
    virtual ~System() = default;

    /// Runs this system's behaviour for one frame.
    ///
    /// @param entities The world to act on. Never owned by the system.
    /// @param actions What the player is asking for this frame, in gameplay terms.
    ///        Never owned by the system. Held, just-pressed and just-released are
    ///        three different questions; see
    ///        [input::ActionState](engine/input/ActionState.hpp).
    /// @param deltaSeconds Real seconds elapsed since the previous frame, in
    ///        seconds, already clamped by engine::Time. Ignoring it is fine.
    ///
    /// Implementations iterate the EntityManager with query<> and write to the
    /// component data they find. They must not create or destroy entities, and
    /// must not add or remove components: see "Structural mutation" in
    /// Query.hpp. A system that needs to do that should collect the request and
    /// apply it after its loop.
    virtual void update(EntityManager& entities, const input::ActionState& actions, float deltaSeconds) = 0;

    /// A short stable name, used for diagnostics and for reporting execution
    /// order. Not used to select or sort systems.
    [[nodiscard]] virtual const char* name() const = 0;

    System(const System&) = delete;
    System& operator=(const System&) = delete;
    System(System&&) = delete;
    System& operator=(System&&) = delete;

protected:
    System() = default;
};

} // namespace engine::ecs
