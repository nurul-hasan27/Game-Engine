#pragma once

namespace engine
{
namespace input
{
class Input;
}
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
///     void update(EntityManager& entities, input::Input& input, float deltaSeconds) override
///     {
///         for (auto&& [entity, transform] : entities.query<Transform>())
///         {
///             transform.position += directionFrom(input) * speed * deltaSeconds;
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
/// the input state, and how long the frame took. None of it is a global, a
/// singleton, or something the system fetches by reaching into `Application`.
///
/// That is what keeps the dependency direction honest. A system can never
/// acquire input the way it would acquire a clock or a keyboard, so the only
/// way to give it input is to hand it some, and the only way to test it is to
/// hand it a fake.
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
    /// @param input Keyboard state for this frame. Never owned by the system.
    /// @param deltaSeconds Real seconds elapsed since the previous frame, in
    ///        seconds, already clamped by engine::Time. Ignoring it is fine.
    ///
    /// Implementations iterate the EntityManager with query<> and write to the
    /// component data they find. They must not create or destroy entities, and
    /// must not add or remove components: see "Structural mutation" in
    /// Query.hpp. A system that needs to do that should collect the request and
    /// apply it after its loop.
    virtual void update(EntityManager& entities, input::Input& input, float deltaSeconds) = 0;

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
