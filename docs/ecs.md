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

## 4. What a System is

A system is the only place behaviour lives. It holds no entities and no component
data: it is handed the `EntityManager`, queries the components it cares about, and
writes behaviour into them.

```cpp
class MovementSystem final : public ecs::System
{
public:
    void update(ecs::EntityManager& entities) override
    {
        for (auto&& [entity, position, velocity] : entities.query<Position, Velocity>())
        {
            position.value += velocity.value;
        }
    }

    [[nodiscard]] const char* name() const override { return "MovementSystem"; }
};
```

`System` itself is deliberately empty of data: a virtual destructor, a pure
`update(EntityManager&)`, and a `name()` for diagnostics. There is no
"system entity", and a system is not a specialised Entity.

Full detail is in sections 11 to 14.

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

There is no query language here. Tag lookup exists because it is genuinely useful
for a designer-facing label and simple to reason about. System-scale selection is
a different concern and is handled by component queries, below.

---

## 10. Component queries

A tag says "what the designer called it". A component query says "what it is
made of", which is the question systems actually ask. Both are views over the
same `EntityManager` storage.

```cpp
// every alive entity holding a Position
for (auto&& [entity, position] : entities.query<Position>())
{
    ...
}

// every alive entity holding both a Position and a Velocity
for (auto&& [entity, position, velocity] : entities.query<Position, Velocity>())
{
    ...
}
```

An entity qualifies when it is **alive** and holds **every** requested component.
Dead entities are skipped exactly as `EntityView` skips them, and an entity
missing any one of the requested types is excluded. The fold is short-circuiting,
so once one type is known to be absent the rest are not looked up.

### What a query yields

Dereferencing a query yields a `std::tuple` of the entity followed by a reference
to each requested component. That works with structured bindings and copies
nothing:

| Query | Yields |
| ----- | ------ |
| `query<Position>()` from a mutable manager | `std::tuple<Entity&, Position&>` |
| `query<Position>()` from a const manager | `std::tuple<const Entity&, const Position&>` |

### What a query owns

Nothing. A query is a single borrowed pointer to the manager's storage, which the
test suite pins down at compile time with
`static_assert(sizeof(Query<false, Position>) == sizeof(void*))`. It allocates
nothing, materialises no collection, and visits each qualifying entity exactly
once. A query is also recomputed on every use, so it is a live view rather than a
snapshot: an entity that gains a component starts matching on the next query.

### Two deliberate restrictions

- `query<>()` with no component types is rejected at compile time.
- `query<T, T>()` is rejected at compile time, because it would yield two
  references to the same component. Write `query<T>()` instead.

### Const queries

A `const EntityManager` only ever produces read-only queries, and a read-only
query yields `const Entity&` plus `const T&` for every component. This is
enforced by the type system, with no `const_cast` anywhere: the query's borrowed
pointer is itself const for a read-only query, and it reaches components through
`Entity::tryGetConstComponent<T>()`.

**One honest seam.** `const Entity&` protects identity, not data, so from an
entity taken out of a const query you can still call `getComponent<T>()`, which
returns `T&` by Phase 3's logical-const design. The *query* is const-correct; the
*entity* is not, by prior decision. Closing that seam would mean changing Entity's
constness, which is out of scope here, so it is recorded as a limitation rather
than hidden.

---

## 11. Systems

`System` is the behaviour half of the architecture:

```cpp
class System
{
public:
    virtual ~System() = default;
    virtual void update(EntityManager& entities) = 0;
    [[nodiscard]] virtual const char* name() const = 0;
};
```

The base class contains no data. There is no `SystemEntity`, and a system is not
a kind of entity. A system is handed the manager, queries what it needs, and
writes behaviour into component data.

### The worked example

```
Entity
  ├── PositionComponent { value: Vec2(10, 20) }   DATA
  └── VelocityComponent { value: Vec2(2, -1) }    DATA
        │
        │  query<Position, Velocity>()
        ↓
  MovementSystem                                     BEHAVIOUR
        │
        │  position.value += velocity.value
        ↓
  PositionComponent { value: Vec2(12, 19) }
```

The components did not move and they did not know anything happened. The
behaviour lives entirely in `MovementSystem`, which means it can be removed,
replaced, reordered or run twice without touching a single line of component
data.

---

## 12. System ownership and execution order

`SystemManager` owns systems, and systems own nothing:

```cpp
SystemManager systems;
systems.add<InputSystem>();
systems.add<MovementSystem>();
systems.add<PhysicsSystem>();
systems.update(entities);
```

- **Ownership is one-directional.** `SystemManager` → `System`. Entities always
  belong to the `EntityManager`. A system has no entity storage, and destroying
  the `SystemManager` leaves the world completely intact, which the test suite
  verifies.
- **Order is registration order, and nothing else.** No sorting, no priorities,
  no dependency graph. Systems run where they were added, so the order is the
  same on every run and every platform. "Input, then movement, then physics" is
  expressed by adding them in that order.
- **References stay valid.** `add<S>()` returns a reference that survives further
  registrations, because each system is separately owned rather than stored by
  value in a `vector<System>`.

---

## 13. Query lifetime and structural mutation

### Lifetime

A query borrows the manager's storage, so it is valid only while that storage is
unchanged. It must not be held across a frame boundary; a system creates the
query it needs, iterates it, and lets it go.

The same `EntityManager::update()` rule from section 7 applies and is the one
that bites: cleanup erases and move-assigns entities, so any query or entity
reference taken before `update()` is stale afterwards.

### Structural mutation policy

**Writing component data during iteration is fully supported.** Components are
individually allocated, so writing through a reference never moves anything and
never invalidates the iterator.

**Structural changes during iteration are not supported**, because a query holds
references into the manager's own containers:

| Call | Supported mid-iteration? | Why |
| ---- | ------------------------ | --- |
| Writing `position.value = ...` | **Yes** | Component storage is separately allocated and never relocated |
| `destroyEntity(entity)` | **Yes** | Only sets a flag; the erase happens in `update()` |
| `addEntity()` | **No** | May move entities, invalidating the iterator |
| `update()` | **No** | Erases and move-assigns entities |
| `addComponent()` | **No** | Changes the entity's match set mid-walk |
| `removeComponent()` | **No** | Removing a component being iterated dangles the reference |

This is documented rather than enforced at runtime. A system that needs to
create, destroy or re-compose entities should collect those requests and apply
them after its loop. A deferred command buffer is deliberately **not** built
yet, because nothing in the engine needs it and it would be speculative.

---

## 14. Current limitations

Honest list of what is deliberately not here yet:

- **No gameplay systems.** The `System` abstraction and `SystemManager` exist and
  are proven by test-only systems, but no rendering, physics, collision, input,
  animation, camera, audio or AI system has been written. Those are later phases.
- **`update()` has no delta time.** A system is called as `update(EntityManager&)`,
  so a test system advances state by one step per call. Real systems will need
  `deltaSeconds`, and the signature will change when the first one does.
- **A const query is not a hard security boundary.** The query yields
  `const T&`, but a `const Entity&` taken from it can still reach a mutable
  component through `getComponent<T>()`, because Entity's constness protects
  identity rather than data. Fixing this means revisiting Phase 3's constness
  decision, not adding anything here.
- **Structural mutation mid-iteration is documented, not enforced.** Nothing
  stops a system from calling `update()` inside its own loop; the rules are
  written down and the test suite documents the supported patterns, but there is
  no runtime guard and no command buffer.
- **No entity handles.** Code holds `Entity&`, an `EntityView` or a `Query`. None
  of them survive `update()`, so a system cannot stash an entity across frames.
  This is a real limitation and will need addressing when systems keep state.
- **No entity lookup by id.** Ids are stable and never reused, but nothing can
  resolve an id back to an entity yet.
- **Linear component lookup.** O(components on this entity) per query step, so a
  query is O(entities * components). Fine at this scale; a per-entity type index
  or a cached query would be the change if profiling ever justified it.
- **One heap allocation per component.** A deliberate simplification of
  ownership. Archetype or sparse-set storage would remove it, but that is
  explicitly out of scope.
- **No component iteration.** You must name the types you want; there is no way
  to ask an entity what it contains, and no way to query "any component".
- **No queries over the absence of a component.** `query<Position, Velocity>()`
  means "has both". There is no "has Position but not Velocity".
- **No structural sharing or prefabs.** Every `addEntity` builds from scratch.
- **No serialization.** Component data cannot be saved or loaded.
- **Single-threaded.** No synchronisation anywhere, by design.
- **No SFML dependency.** The ECS and the system layer are engine logic and know
  nothing about rendering; the boundary conversion from `Vec2` to `sf::Vector2f`
  belongs to whichever later phase first draws something.
