# ECS Core

This document describes the Entity Component System as it exists in this
engine today. It covers what the pieces are, who owns what, and — most
importantly — the lifetime rules that keep systems from holding onto things
that have gone away.

Systems are not part of this phase. The ECS here is the storage and lifetime
layer that systems will be built on top of.

---

## 1. What ECS means in this engine

ECS separates **identity**, **data** and **behaviour**:

```
Entity  ──  the identity: "thing 7, tagged player, currently alive"
Component ──  the data: "a position", "a hit point count"
System  ──  the behaviour: "every frame, move everything that has a position"
```

An entity is an identity. A component is plain data attached to it. A system is
a function over entities that own a particular component. Nothing in an entity
knows how to move, render, attack, collide or take damage, because behaviour
belongs to systems.

### Worked example

```
Entity
  ├── TransformComponent  { position: Vec2, rotation: float }   (data)
  ├── VelocityComponent  { value: Vec2 }                        (data)
  └── HealthComponent     { current: int, maximum: int }         (data)
```

The *behaviour* is not on the entity. It is somewhere else entirely:

- A `MovementSystem` reads `TransformComponent` and `VelocityComponent` and
  writes `TransformComponent`.
- A `DamageSystem` reads and writes `HealthComponent`.
- A `RenderSystem` reads `TransformComponent` to decide where to draw.

Neither the components nor the entity know those systems exist. That is what
makes it possible to change how a thing behaves without touching its data, and
to combine behaviours that were never written to know about each other.

Phase 2's `engine::Vec2` is the natural payload for position and velocity, which
is why the math foundation came before the ECS.

---

## 2. What an Entity is

`engine::ecs::Entity` (`include/engine/ecs/Entity.hpp`) holds four things:

| Member | Purpose |
| ------ | ------- |
| `EntityId m_id` | stable unique integer identity |
| `std::string m_tag` | a string label such as `"player"` or `"coin"` |
| `bool m_isAlive` | whether destruction has been requested |
| `ComponentStorage m_components` | the components it owns |

That is all. An entity has no `update()`, no `draw()`, no virtual behaviour, and
no knowledge of any specific component type.

**Ids** are `std::uint64_t`, handed out by the manager and starting at 1
(`kInvalidEntityId` is 0). They are never reused, so an id always identifies
exactly one entity for the lifetime of the process. They are plain integers, not
pointers, so they are cheap to copy, safe to print, and usable as map keys.

**Tags** are metadata for lookup only. A tag is a string, never a type, never a
base class, and never a replacement for a component query. Use a tag when you
want "all the things the designer called an enemy"; use a component when you
want "all the things that move".

---

## 3. What a Component is

A component is a plain struct of data. The ECS imposes no requirements beyond
that, and requires no base class, no registration and no reflection.

```cpp
struct HealthComponent
{
    int current;
    int maximum;
};
```

A component must not contain behaviour. No member functions, no virtuals, no
self-updating logic. If a struct needs to do something, that something is a
system.

Components must be **default-constructible or constructible from the arguments
you pass to `addComponent<T>()`**, and their type must be usable as a
`std::type_index`, which in practice means any ordinary class type.

---

## 4. What a System will be

Not implemented yet. The design already assumes this shape:

```cpp
class MovementSystem
{
public:
    void update(ecs::EntityManager& entities, float deltaSeconds);
};
```

A system receives the manager, asks for the entities it cares about, and mutates
their components. It never creates, destroys or reorders entities itself except
by asking the manager, and it never caches entity references across a frame
boundary (see the lifetime rules below). Registration and ordering of systems
belong to a later phase.

---

## 5. EntityManager responsibilities

`engine::ecs::EntityManager` (`include/engine/ecs/EntityManager.hpp`) is the
owner of every entity. It is the only way to create one and the only way to
request that one be destroyed.

| Method | Responsibility |
| ------ | -------------- |
| `addEntity(tag)` | assign a fresh id, create the entity, return a reference |
| `getEntities()` | view of all currently alive entities |
| `getEntities(tag)` | view of all alive entities carrying that tag |
| `destroyEntity(entity)` | flag an entity dead; does not erase |
| `update()` | erase everything flagged dead |
| `aliveEntityCount()` | how many are alive right now |
| `storedEntityCount()` | how many are stored, including ones awaiting cleanup |

Entities cannot be created outside the manager. `Entity`'s constructor requires
an `Entity::ManagerAccess` token whose only producer is `EntityManager`, so
even though the constructor itself must be public (a standard container has to be
able to construct elements in place), no code outside the engine can call it.
Entities are also non-copyable, so two owners can never share one identity.

---

## 6. Component lifetime

An entity **owns** its components outright, and destroys them when it is
destroyed. Storage is `std::vector<std::unique_ptr<ComponentBase>>`, with one
type-erased holder per component; `ComponentBase` exposes only
`virtual std::type_index type()`, and each `ComponentHolder<T>` stores a `T`.

This is a linear scan over the components of a *single* entity. That is
deliberate: entities carry few components, and nothing here ever scans all
entities. Component lookup is O(components on this entity), independent of world
size.

What this buys, and what it costs:

| Valid | Invalid |
| ----- | ------- |
| A `T&` stays valid when **other** components are added or removed | A `T&` is invalidated by removing **that** component |
| A `T&` stays valid while the entity lives and the component is attached | A `T&` is invalidated when the entity is destroyed |
| Two entities with the same component type have fully independent storage | Adding a second component of the same type throws rather than overwriting |

Each component is individually heap-allocated, which is why a reference to one
component survives unrelated changes to the entity's composition.

### Error behaviour

| Call | Behaviour |
| ---- | --------- |
| `addComponent<T>()` with `T` already present | throws `std::logic_error` |
| `getComponent<T>()` with `T` absent | throws `std::logic_error` |
| `removeComponent<T>()` with `T` absent | throws `std::logic_error` |
| `hasComponent<T>()` | returns false, never throws, never creates |
| `tryGetComponent<T>()` | returns `nullptr`, never throws |
| `tryRemoveComponent<T>()` | returns `bool`, never throws |

A missing component is a programming error, so the strict functions report it
rather than quietly returning a default-constructed value that would be
overwritten and lose data. The `try*` variants exist for the cases where absence
is genuinely expected. No component is ever silently created by a read.

---

## 7. Entity lifetime

Entities are stored in a `std::deque`. A deque is used rather than a vector
specifically because inserting at the end of a deque never invalidates
references to existing elements, so `addEntity()` does not invalidate the entity
references systems are holding.

The manager hands out `Entity&` from `addEntity()` and `const Entity&` through
views. Both are references into its own storage, and both are **borrowed**: the
manager owns the entity, and the entity outlives the reference only as long as
the manager's entity set is unchanged.

### Lifetime rules, precisely

| Event | Effect on outstanding `Entity&` and `const Entity&` |
| ----- | ---------------------------------------------------- |
| `addEntity()` | **Stay valid.** Deque end insertion does not move existing elements. |
| `getEntity`-style lookups, reading `tag()`, `id()` | No effect |
| `destroyEntity(e)` | **Stay valid.** Nothing is erased; `e.isAlive()` becomes false |
| `update()` that erases something | **Invalidated** for all entities: erasure moves survivors to fill the gap |
| `update()` that erases nothing | Still invalidated in principle, because the call inspects the container; re-acquire and do not rely on it |
| Entity destroyed (manager goes out of scope) | Invalidated, and its components are freed |

### The rule to actually follow

Hold an entity reference or a view for the duration of one system's pass. Do not
cache one across a frame boundary, and do not use one after calling
`EntityManager::update()`. Re-acquire it by tag or by view when you need it
again:

```cpp
// Fine: the reference lives only inside the loop.
for (const auto& entity : entities.getEntities("player"))
{
    process(entity);
}

// Wrong: `player` is stale after update() moved the survivors.
Entity& player = entities.getEntities("player").begin().operator*();
entities.update();
use(player);  // dangling

// Right: look it up again.
entities.update();
for (const auto& entity : entities.getEntities("player"))
{
    use(entity);
}
```

### Constness

A `const Entity` protects the **identity** and nothing else. Every component
operation, including `addComponent()` and `removeComponent()`, is const and
mutates through it, and `getComponent<T>()` returns a mutable `T&`. This is the
usual ECS "logical const" arrangement, and it is what allows a system to walk a
read-only `const Entity&` view and still create, change and destroy components.
Only the id, the tag and liveness are protected by const, and only
`EntityManager` may change those.

---

## 8. Deferred destruction

`destroyEntity()` only sets a flag. It never erases. `update()` does the erasing.

This is the single most important lifetime rule in the ECS, and it exists so
that a system can decide an entity must die *while it is iterating*:

```cpp
for (const auto& entity : entities.getEntities())
{
    if (shouldDie(entity))
    {
        entities.destroyEntity(entity);   // safe: only sets a flag
    }
}                                          // iteration is still valid throughout

entities.update();                          // now, and only now, entities go away
```

A flagged entity stops appearing in every view immediately, so the rest of the
pass will not visit it again, and its components remain intact and readable
until cleanup. If it needs to be restored before `update()`, there is no API for
that in this phase; the design assumes destruction is final once requested.

Erasing during iteration would have been the alternative, and it is exactly what
would invalidate the loop above.

---

## 9. Tag lookup

```cpp
entities.getEntities();          // every alive entity
entities.getEntities("player");  // alive entities tagged "player"
```

Both return an `EntityView` (`include/engine/ecs/EntityView.hpp`): a
non-owning, non-allocating range that skips dead entities and, for the tagged
form, entities whose tag does not match. The view deliberately does **not**
expose the manager's internal container, so callers cannot break the ownership
and liveness rules.

A view holds a `std::string_view` of the tag, so the tag must outlive the view.
That is natural for a literal or a named string; it is a trap for a temporary
`std::string`, which is why `getEntities()` with no tag shares the same code path
via an empty tag meaning "any".

There is no query language here. Component-based system querying, which is what
systems will mostly need, is a later phase. Tag lookup exists because it is
genuinely useful for a designer-facing label and simple to reason about now.

---

## 10. Current limitations

Honest list of what is deliberately not here yet:

- **No systems.** The ECS provides storage and lifetime only.
- **No component-based queries.** Only whole-entity views and tag views exist.
  Systems will need something like "every entity with a `TransformComponent`",
  which is a later phase.
- **Linear component lookup.** O(components on this entity). Fine at this scale;
  a per-entity type index would be the change if profiling ever justified it.
- **One heap allocation per component.** A deliberate simplification of
  ownership. Archetype or sparse-set storage would remove it, but that is
  explicitly out of scope for this phase.
- **No component iteration.** You must know the type you want; there is no way
  to ask an entity what it contains.
- **No entity handles.** Code holds `Entity&` or an `EntityView`. There is no
  id-based handle that survives cleanup, so a system cannot stash an entity
  across frames. That is a limitation, not an oversight, and it will need
  addressing when systems keep state.
- **No structural sharing or prefabs.** Every `addEntity` builds from scratch.
- **No serialization.** Component data cannot be saved or loaded.
- **Single-threaded.** No synchronisation anywhere, by design.
- **No SFML dependency.** The ECS is engine logic and knows nothing about
  rendering; the boundary conversion from `Vec2` to `sf::Vector2f` belongs to
  whichever later phase first draws something.
