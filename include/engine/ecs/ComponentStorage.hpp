#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace engine::ecs
{

/// Type-erased interface for a single stored component.
///
/// Components are required to have no virtual functions of their own: all
/// behaviour belongs to systems, and the component itself is pure data.
class ComponentBase
{
public:
    virtual ~ComponentBase() = default;

    /// Identifies the concrete component type held by this object.
    [[nodiscard]] virtual std::type_index type() const noexcept = 0;
};

/// Holds one component of type T behind the type-erased ComponentBase
/// interface.
template <typename T>
class ComponentHolder final : public ComponentBase
{
public:
    template <typename... Args>
    explicit ComponentHolder(Args&&... args) : m_value(std::forward<Args>(args)...) {}

    [[nodiscard]] std::type_index type() const noexcept override { return std::type_index{typeid(T)}; }

    T m_value;
};

/// Type-erased, owning storage for one entity's components.
///
/// The public API is templated on the concrete component type, so it is
/// type-safe, while the storage itself only ever sees ComponentBase. This lets
/// an entity hold any set of component types without the entity having to know
/// those types at compile time.
///
/// ### Ownership
///
/// Each component is owned by a separate heap-allocated ComponentHolder held by
/// a std::unique_ptr. The holder never moves when the vector grows or shrinks,
/// so a reference or pointer to a component stays valid when *other* components
/// are added or removed. It is invalidated only by:
///
/// - removing that same component type, or
/// - destroying the owning entity.
///
/// ### Complexity
///
/// Lookup is a linear scan over the components of a single entity, comparing
/// std::type_index. That is deliberate: entities carry few components, and a
/// short contiguous vector of pointers beats a hash map here. Nothing in this
/// class ever scans all entities. A per-entity hash index would be the change
/// to make if profiling ever justified it.
class ComponentStorage
{
public:
    ComponentStorage() = default;

    /// Components are owned outright, so copying them is not meaningful.
    ComponentStorage(const ComponentStorage&) = delete;
    ComponentStorage& operator=(const ComponentStorage&) = delete;
    ComponentStorage(ComponentStorage&&) = default;
    ComponentStorage& operator=(ComponentStorage&&) = default;
    ~ComponentStorage() = default;

    /// Constructs a component of type T in place from `args` and returns a
    /// reference to it.
    ///
    /// Throws std::logic_error if a component of type T is already present,
    /// rather than silently replacing it, because overwriting a component is
    /// almost always a bug rather than an intent.
    template <typename T, typename... Args>
    T& emplace(Args&&... args)
    {
        const std::type_index type{typeid(T)};

        if (findBase(type) != nullptr)
        {
            throw std::logic_error{"entity already has a component of this type"};
        }

        // Construct first, then store, so a throwing constructor leaves the
        // storage unchanged.
        auto holder = std::make_unique<ComponentHolder<T>>(std::forward<Args>(args)...);
        T& component = holder->m_value;
        m_components.push_back(std::move(holder));

        return component;
    }

    /// Returns a pointer to the component of type T, or nullptr if absent.
    template <typename T>
    [[nodiscard]] T* find() noexcept
    {
        // Safe downcast: findBase only returns a holder whose type() matches.
        auto* const base = findBase(std::type_index{typeid(T)});
        return base != nullptr ? &static_cast<ComponentHolder<T>*>(base)->m_value : nullptr;
    }

    /// Returns a pointer to the component of type T, or nullptr if absent.
    template <typename T>
    [[nodiscard]] const T* find() const noexcept
    {
        const auto* const base = findBase(std::type_index{typeid(T)});
        return base != nullptr ? &static_cast<const ComponentHolder<T>*>(base)->m_value : nullptr;
    }

    /// Returns true if a component of type T is present.
    template <typename T>
    [[nodiscard]] bool contains() const noexcept
    {
        return findBase(std::type_index{typeid(T)}) != nullptr;
    }

    /// Removes the component of type T, returning true if one was present.
    template <typename T>
    [[nodiscard]] bool erase() noexcept
    {
        return eraseType(std::type_index{typeid(T)});
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_components.size(); }

private:
    /// Removes the component of the given type, returning whether one existed.
    bool eraseType(std::type_index type) noexcept;

    [[nodiscard]] ComponentBase* findBase(std::type_index type) noexcept;
    [[nodiscard]] const ComponentBase* findBase(std::type_index type) const noexcept;

    std::vector<std::unique_ptr<ComponentBase>> m_components;
};

} // namespace engine::ecs
