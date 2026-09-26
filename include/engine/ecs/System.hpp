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
///     void update(EntityManager& entities) override
///     {
///         for (auto&& [entity, position, velocity] : entities.query<Position, Velocity>())
///         {
///             position.value += velocity.value;
///         }
///     }
///
///     [[nodiscard]] const char* name() const override { return "MovementSystem"; }
/// };
/// ```
class System
{
public:
    virtual ~System() = default;

    /// Runs this system's behaviour for one frame.
    ///
    /// Implementations iterate the EntityManager with query<> and write to the
    /// component data they find. They must not create or destroy entities, and
    /// must not add or remove components: see "Structural mutation" in
    /// Query.hpp. A system that needs to do that should collect the request and
    /// apply it after its loop.
    virtual void update(EntityManager& entities) = 0;

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
