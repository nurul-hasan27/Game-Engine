#pragma once

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
///     void update(EntityManager& entities, const float deltaSeconds) override
///     {
///         for (auto&& [entity, transform] : entities.query<Transform>())
///         {
///             transform.position += transform.velocity * deltaSeconds;
///         }
///     }
///
///     [[nodiscard]] const char* name() const override { return "MovementSystem"; }
/// };
/// ```
///
/// ### Delta time is passed in, never reached for
///
/// `deltaSeconds` is a parameter, not a global, not a singleton clock, and not
/// something a system fetches from an `engine::Time` it happens to know about.
/// Timing is a property of the frame, so it arrives with the frame. That keeps
/// systems testable, because a test can hand them an exact delta instead of
/// sleeping, and it keeps a system honest about depending only on what it was
/// given. Systems that do not care about time simply ignore the parameter.
class System
{
public:
    virtual ~System() = default;

    /// Runs this system's behaviour for one frame.
    ///
    /// @param entities The world to act on. Never owned by the system.
    /// @param deltaSeconds Real seconds elapsed since the previous frame, in
    ///        seconds, already clamped by engine::Time. Ignoring it is fine.
    ///
    /// Implementations iterate the EntityManager with query<> and write to the
    /// component data they find. They must not create or destroy entities, and
    /// must not add or remove components: see "Structural mutation" in
    /// Query.hpp. A system that needs to do that should collect the request and
    /// apply it after its loop.
    virtual void update(EntityManager& entities, float deltaSeconds) = 0;

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
