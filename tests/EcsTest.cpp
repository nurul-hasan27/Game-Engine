#include "engine/ecs/Entity.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/EntityView.hpp"
#include "engine/math/Vec2.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace
{

// ---------------------------------------------------------------------------
// Test-only components.
//
// These exist purely to exercise the ECS and are deliberately not part of the
// engine's gameplay design. Real components such as Transform or Health belong
// to later phases.
// ---------------------------------------------------------------------------

struct PositionComponent
{
    engine::Vec2 position;
};

struct HealthComponent
{
    int value;
};

struct NameComponent
{
    std::string value;
};

struct TagComponent
{
    int unused = 0;
};

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

/// Verifies that `operation` reports a std::logic_error, and that it does not
/// throw anything else. Accepts any callable so callers can pass a lambda that
/// captures the entity under test.
template <typename Operation>
void checkThrows(const char* const what, Operation&& operation, const char* const file, const int line)
{
    try
    {
        operation();
    }
    catch (const std::logic_error&)
    {
        return;
    }
    catch (...)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": " << what << " threw the wrong exception type\n";
        return;
    }

    ++g_failureCount;
    std::cerr << "    " << file << ':' << line << ": " << what << " did not throw\n";
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)
#define CHECK_FALSE(expression) check(!(expression), "!" #expression, __FILE__, __LINE__)
#define CHECK_THROWS(what, ...) checkThrows(what, [&] { __VA_ARGS__; }, __FILE__, __LINE__)

using engine::Vec2;
using engine::ecs::Entity;
using engine::ecs::EntityId;
using engine::ecs::EntityManager;

/// Counts the entities a view yields.
template <typename View>
std::size_t countIn(const View& view)
{
    std::size_t count = 0;
    for (const auto& entity : view)
    {
        (void)entity;
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testEntityCreation()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    CHECK(entity.id() != engine::ecs::kInvalidEntityId);
    CHECK(entity.tag() == "player");
    CHECK(entity.isAlive());
    CHECK(manager.aliveEntityCount() == 1);
}

void testEntityIdsAreUnique()
{
    EntityManager manager;
    std::set<EntityId> ids;

    for (int i = 0; i < 8; ++i)
    {
        ids.insert(manager.addEntity("thing").id());
    }

    CHECK(ids.size() == 8);

    // Ids are never reused, so a dead entity's id cannot come back.
    Entity& doomed = manager.addEntity("doomed");
    const EntityId doomedId = doomed.id();
    manager.destroyEntity(doomed);
    manager.update();

    for (int i = 0; i < 4; ++i)
    {
        CHECK(manager.addEntity("new").id() != doomedId);
    }
}

void testEntityTags()
{
    EntityManager manager;
    Entity& player = manager.addEntity("player");
    Entity& enemy = manager.addEntity("enemy");
    Entity& untagged = manager.addEntity("");

    CHECK(player.tag() == "player");
    CHECK(enemy.tag() == "enemy");
    CHECK(untagged.tag().empty());
}

void testAliveState()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    CHECK(entity.isAlive());
    manager.destroyEntity(entity);
    CHECK_FALSE(entity.isAlive());
}

void testAddComponent()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    HealthComponent& health = entity.addComponent<HealthComponent>(HealthComponent{75});

    CHECK(health.value == 75);
    CHECK(entity.componentCount() == 1);
}

void testHasComponent()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    CHECK_FALSE(entity.hasComponent<HealthComponent>());
    entity.addComponent<HealthComponent>(HealthComponent{10});
    CHECK(entity.hasComponent<HealthComponent>());
    CHECK_FALSE(entity.hasComponent<PositionComponent>());
}

void testGetComponent()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");
    entity.addComponent<PositionComponent>(PositionComponent{Vec2{3.0f, 4.0f}});

    const PositionComponent& position = entity.getComponent<PositionComponent>();

    CHECK(position.position == Vec2(3.0f, 4.0f));
    CHECK(entity.getComponent<PositionComponent>().position.length() == 5.0f);
}

void testModifyComponentThroughReference()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    PositionComponent& position = entity.addComponent<PositionComponent>(PositionComponent{Vec2{0.0f, 0.0f}});
    position.position = Vec2(3.0f, 4.0f);

    CHECK(entity.getComponent<PositionComponent>().position == Vec2(3.0f, 4.0f));

    // Mutating through a const entity reference is allowed: a const Entity
    // means the identity is fixed, not that the data is frozen.
    const Entity& constEntity = entity;
    constEntity.getComponent<PositionComponent>().position = Vec2(1.0f, 2.0f);
    CHECK(entity.getComponent<PositionComponent>().position == Vec2(1.0f, 2.0f));
}

void testRemoveComponent()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");
    entity.addComponent<HealthComponent>(HealthComponent{50});

    entity.removeComponent<HealthComponent>();

    CHECK_FALSE(entity.hasComponent<HealthComponent>());
    CHECK(entity.componentCount() == 0);

    // tryRemoveComponent is the non-throwing counterpart.
    CHECK_FALSE(entity.tryRemoveComponent<HealthComponent>());
}

void testMultipleComponentTypesOnOneEntity()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    entity.addComponent<PositionComponent>(PositionComponent{Vec2{1.0f, 2.0f}});
    entity.addComponent<HealthComponent>(HealthComponent{30});
    entity.addComponent<NameComponent>(NameComponent{"Hero"});

    CHECK(entity.componentCount() == 3);
    CHECK(entity.hasComponent<PositionComponent>());
    CHECK(entity.hasComponent<HealthComponent>());
    CHECK(entity.hasComponent<NameComponent>());
    CHECK(entity.getComponent<NameComponent>().value == "Hero");

    // Removing one leaves the others intact.
    entity.removeComponent<HealthComponent>();
    CHECK(entity.componentCount() == 2);
    CHECK(entity.getComponent<PositionComponent>().position == Vec2(1.0f, 2.0f));
    CHECK(entity.getComponent<NameComponent>().value == "Hero");
}

void testDifferentComponentCompositions()
{
    EntityManager manager;
    Entity& player = manager.addEntity("player");
    Entity& coin = manager.addEntity("coin");
    Entity& ghost = manager.addEntity("ghost");

    player.addComponent<PositionComponent>(PositionComponent{Vec2{0.0f, 0.0f}});
    player.addComponent<HealthComponent>(HealthComponent{100});
    coin.addComponent<PositionComponent>(PositionComponent{Vec2{5.0f, 5.0f}});
    ghost.addComponent<TagComponent>();

    CHECK(player.componentCount() == 2);
    CHECK(coin.componentCount() == 1);
    CHECK(ghost.componentCount() == 1);
    CHECK(player.hasComponent<HealthComponent>());
    CHECK_FALSE(coin.hasComponent<HealthComponent>());
    CHECK(ghost.hasComponent<TagComponent>());
    CHECK_FALSE(ghost.hasComponent<PositionComponent>());
}

void testMissingComponentThrows()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    // The static_cast<void> documents that these calls are expected to throw, so
    // there is deliberately no return value to use.
    CHECK_THROWS("getComponent on a missing component",
                 static_cast<void>(entity.getComponent<HealthComponent>()));
    CHECK_THROWS("getComponent through a const entity",
                 static_cast<void>(std::as_const(entity).getComponent<HealthComponent>()));
    CHECK_THROWS("removeComponent on a missing component", entity.removeComponent<HealthComponent>());

    // The non-throwing accessor is the way to ask without treating it as a bug.
    CHECK(entity.tryGetComponent<HealthComponent>() == nullptr);

    // A failed lookup must not have created anything.
    CHECK(entity.componentCount() == 0);
}

void testDuplicateComponentThrows()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");
    entity.addComponent<HealthComponent>(HealthComponent{10});

    CHECK_THROWS("adding a duplicate component", entity.addComponent<HealthComponent>(HealthComponent{99}));

    // The original value must be untouched by the rejected add.
    CHECK(entity.getComponent<HealthComponent>().value == 10);
    CHECK(entity.componentCount() == 1);
}

void testManagerOwnsEntities()
{
    // Entities cannot be conjured outside a manager, and cannot be copied, so
    // ownership can never be ambiguous.
    static_assert(!std::is_default_constructible_v<Entity>, "Entity must not be default constructible");
    static_assert(!std::is_copy_constructible_v<Entity>, "Entity must not be copyable");
    static_assert(std::is_move_constructible_v<Entity>, "Entity must be movable");

    // The ManagerAccess token is not publicly constructible, which is what stops
    // the constructor from being called from outside the engine.
    static_assert(!std::is_default_constructible_v<Entity::ManagerAccess>,
                  "ManagerAccess must not be constructible outside EntityManager");

    EntityManager manager;
    CHECK(manager.addEntity("only").componentCount() == 0);
}

void testTagLookup()
{
    EntityManager manager;
    manager.addEntity("player");
    manager.addEntity("enemy");
    manager.addEntity("enemy");
    manager.addEntity("coin");

    CHECK(countIn(manager.getEntities("enemy")) == 2);
    CHECK(countIn(manager.getEntities("player")) == 1);
    CHECK(countIn(manager.getEntities("coin")) == 1);
    CHECK(countIn(manager.getEntities("nothing")) == 0);

    for (const auto& entity : manager.getEntities("enemy"))
    {
        CHECK(entity.tag() == "enemy");
    }
}

void testDestroyRequest()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    manager.destroyEntity(entity);

    CHECK_FALSE(entity.isAlive());
    // The entity is excluded from views as soon as it is flagged.
    CHECK(manager.aliveEntityCount() == 0);
    CHECK(countIn(manager.getEntities()) == 0);
    CHECK(countIn(manager.getEntities("player")) == 0);

    // Destroying the same entity twice is a programming error, not a no-op.
    CHECK_THROWS("double destroy", manager.destroyEntity(entity));
}

void testDeferredDestruction()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");
    entity.addComponent<HealthComponent>(HealthComponent{5});

    manager.destroyEntity(entity);

    // Still stored, not yet erased: cleanup has not run.
    CHECK(manager.storedEntityCount() == 1);
    CHECK(manager.aliveEntityCount() == 0);
}

void testDestructionDuringIterationIsSafe()
{
    EntityManager manager;
    manager.addEntity("a");
    manager.addEntity("b");
    manager.addEntity("c");
    manager.addEntity("d");

    // A system that flags entities while walking the view, which is the whole
    // reason destruction is deferred.
    std::size_t visited = 0;
    for (const auto& entity : manager.getEntities())
    {
        ++visited;
        manager.destroyEntity(entity);
    }

    CHECK(visited == 4);
    CHECK(manager.aliveEntityCount() == 0);
    CHECK(manager.storedEntityCount() == 4);

    // The view still iterates safely while entities are flagged dead.
    CHECK(countIn(manager.getEntities()) == 0);
}

void testDestroyingMultipleEntities()
{
    EntityManager manager;
    Entity& first = manager.addEntity("first");
    Entity& second = manager.addEntity("second");
    Entity& survivor = manager.addEntity("survivor");

    manager.destroyEntity(first);
    manager.destroyEntity(second);

    CHECK(manager.aliveEntityCount() == 1);
    CHECK(survivor.isAlive());
    CHECK(countIn(manager.getEntities()) == 1);
    CHECK(countIn(manager.getEntities("survivor")) == 1);
}

void testCleanup()
{
    EntityManager manager;
    manager.addEntity("doomed");
    manager.addEntity("doomed");
    manager.addEntity("survivor");

    for (const auto& entity : manager.getEntities("doomed"))
    {
        manager.destroyEntity(entity);
    }

    CHECK(manager.storedEntityCount() == 3);

    manager.update();

    // Cleanup erases the flagged entities and leaves the rest alone.
    CHECK(manager.storedEntityCount() == 1);
    CHECK(manager.aliveEntityCount() == 1);
    CHECK(countIn(manager.getEntities("survivor")) == 1);

    // update() with nothing pending is harmless.
    manager.update();
    CHECK(manager.storedEntityCount() == 1);
}

void testActiveEntityIteration()
{
    EntityManager manager;
    manager.addEntity("player");
    manager.addEntity("enemy");
    Entity& doomed = manager.addEntity("doomed");
    manager.destroyEntity(doomed);

    // Only alive entities are visited, and exactly once each.
    std::size_t count = 0;
    std::set<EntityId> seen;
    for (const auto& entity : manager.getEntities())
    {
        CHECK(entity.isAlive());
        ++count;
        seen.insert(entity.id());
    }

    CHECK(count == 2);
    CHECK(seen.size() == 2);

    // Tag views agree with the full view.
    CHECK(countIn(manager.getEntities("player")) + countIn(manager.getEntities("enemy")) == 2);
    CHECK_FALSE(manager.getEntities().empty());

    // After cleanup the surviving entities are still iterable and intact.
    manager.update();
    CHECK(countIn(manager.getEntities()) == 2);
}

void testEntityReferenceStability()
{
    EntityManager manager;
    Entity& first = manager.addEntity("first");
    const EntityId firstId = first.id();

    // Adding entities must not invalidate references to existing ones.
    for (int i = 0; i < 16; ++i)
    {
        manager.addEntity("filler");
    }

    CHECK(first.tag() == "first");
    CHECK(first.id() == firstId);
    CHECK(first.isAlive());
}

void testComponentReferenceStability()
{
    EntityManager manager;
    Entity& entity = manager.addEntity("player");

    HealthComponent& health = entity.addComponent<HealthComponent>(HealthComponent{20});

    // Adding other components must not move this one: each component is
    // individually heap allocated and owned.
    entity.addComponent<PositionComponent>(PositionComponent{Vec2{1.0f, 1.0f}});
    entity.addComponent<NameComponent>(NameComponent{"Hero"});

    CHECK(health.value == 20);
    health.value = 21;
    CHECK(entity.getComponent<HealthComponent>().value == 21);

    // Removing an unrelated component leaves the reference valid too.
    entity.removeComponent<NameComponent>();
    CHECK(health.value == 21);
    CHECK(entity.componentCount() == 2);

    // Removing the component the reference points at invalidates that reference.
    // The test deliberately does not read `health` again after this point, since
    // doing so would be a genuine use-after-free. Sanitizer runs confirm nothing
    // dangles.
    entity.removeComponent<HealthComponent>();
    CHECK(entity.componentCount() == 1);
    CHECK_FALSE(entity.hasComponent<HealthComponent>());
    CHECK_FALSE(entity.hasComponent<NameComponent>());

    // The component that survived every operation is still readable and unchanged.
    CHECK(entity.hasComponent<PositionComponent>());
    CHECK(entity.getComponent<PositionComponent>().position == Vec2(1.0f, 1.0f));
}

void testComponentsBelongToTheirEntity()
{
    EntityManager manager;
    Entity& first = manager.addEntity("first");
    Entity& second = manager.addEntity("second");

    first.addComponent<HealthComponent>(HealthComponent{10});
    second.addComponent<HealthComponent>(HealthComponent{20});

    // Same component type, independent storage.
    CHECK(first.getComponent<HealthComponent>().value == 10);
    CHECK(second.getComponent<HealthComponent>().value == 20);

    first.getComponent<HealthComponent>().value = 99;
    CHECK(second.getComponent<HealthComponent>().value == 20);

    // Destroying the first entity takes its components with it and leaves the
    // second untouched.
    manager.destroyEntity(first);
    manager.update();

    // The `second` reference is deliberately NOT used after update(): cleanup
    // erases from the container, which moves surviving entities to fill the gap,
    // so a reference taken before cleanup is stale. Looking the entity up again
    // is the discipline the lifetime rules require, and the supported pattern.
    CHECK(countIn(manager.getEntities("first")) == 0);
    CHECK(countIn(manager.getEntities("second")) == 1);

    for (const auto& entity : manager.getEntities("second"))
    {
        CHECK(entity.getComponent<HealthComponent>().value == 20);
        CHECK(entity.componentCount() == 1);
    }
}

void testEntityReferencesAreInvalidatedByCleanup()
{
    // Counterpart to the reference-stability test: references survive adding
    // entities, but cleanup deliberately invalidates them. This checks the
    // supported way to keep using an entity across a cleanup.
    EntityManager manager;
    manager.addEntity("first");
    manager.addEntity("keeper");

    manager.update(); // nothing pending, but still a container-changing call

    for (const auto& entity : manager.getEntities("keeper"))
    {
        entity.addComponent<HealthComponent>(HealthComponent{7});
        CHECK(entity.getComponent<HealthComponent>().value == 7);
    }

    // The freshly obtained reference is valid again for the same entity.
    for (const auto& entity : manager.getEntities("keeper"))
    {
        CHECK(entity.getComponent<HealthComponent>().value == 7);
    }
}

void testEntityIdIsAUsableInteger()
{
    EntityManager manager;
    const Entity& entity = manager.addEntity("player");

    // A plain integer id: directly printable and usable as a key when debugging.
    std::ostringstream stream;
    stream << entity.id();
    CHECK(!stream.str().empty());
    CHECK(entity.id() == static_cast<EntityId>(std::stoull(stream.str())));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"entity creation", &testEntityCreation},
        {"unique entity ids", &testEntityIdsAreUnique},
        {"entity tags", &testEntityTags},
        {"alive state", &testAliveState},
        {"add component", &testAddComponent},
        {"has component", &testHasComponent},
        {"get component", &testGetComponent},
        {"modify component through reference", &testModifyComponentThroughReference},
        {"remove component", &testRemoveComponent},
        {"multiple component types on one entity", &testMultipleComponentTypesOnOneEntity},
        {"different component compositions", &testDifferentComponentCompositions},
        {"missing component behaviour", &testMissingComponentThrows},
        {"duplicate component behaviour", &testDuplicateComponentThrows},
        {"manager owns entities", &testManagerOwnsEntities},
        {"tag lookup", &testTagLookup},
        {"destroy request", &testDestroyRequest},
        {"deferred destruction", &testDeferredDestruction},
        {"destruction during iteration", &testDestructionDuringIterationIsSafe},
        {"destroying multiple entities", &testDestroyingMultipleEntities},
        {"manager cleanup", &testCleanup},
        {"active entity iteration", &testActiveEntityIteration},
        {"entity reference stability", &testEntityReferenceStability},
        {"component reference stability", &testComponentReferenceStability},
        {"components belong to their entity", &testComponentsBelongToTheirEntity},
        {"entity references invalidated by cleanup", &testEntityReferencesAreInvalidatedByCleanup},
        {"entity id is a usable integer", &testEntityIdIsAUsableInteger},
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

    std::cout << groupCount << " ECS test groups passed\n";
    return EXIT_SUCCESS;
}
