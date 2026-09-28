#pragma once

#include "engine/ecs/EntityManager.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/systems/RenderSystem.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace engine::scene
{

/// The front end: a title, two options, and a caption.
///
/// ### What it is for
///
/// The first thing in this engine that is a **game state** rather than a level. It
/// exists to prove the scene architecture is real: a world that is entirely its own,
/// a renderer it did not create, assets it did not load, and input it receives
/// already resolved into actions.
///
/// ### Screen space, and why
///
/// Every menu element is marked [components::ScreenSpace](components/ScreenSpace.hpp),
/// so none of them moves or scales when the camera does. A title that slid away
/// when the player walked right would be a bug, and the whole point of a menu is
/// that it does not.
///
/// Positions are screen pixels taken from [graphics::Camera::viewport], so the
/// menu lays itself out against the window it was given and needs no
/// `sf::RenderWindow` anywhere in its API. `Camera` is already the engine's
/// abstraction for "where am I looking and how big is the view", and a menu that
/// wanted a second source of truth for the window size would be the first place
/// they could disagree.
///
/// ### Selection
///
/// Shown by prefixing the selected option with `"> "`. A separate marker entity was
/// considered and rejected: it would have needed screen-space support for a shape,
/// and this engine deliberately supports screen space for **text only** until
/// something needs more.
///
/// ### Input
///
/// [input::Action] only, never a key. Navigation is `MoveUp`/`MoveDown` and
/// confirmation is `Shoot`, all three already bound by [input::defaultActionMap] -
/// no parallel set of names was invented. `MoveUp` happens to share the `W` key
/// with `Jump` and `Shoot` shares `Space`; that is the action layer's business, and
/// a scene that needed a distinct "confirm" action would add one to the vocabulary
/// rather than read a key.
///
/// Each is read with `wasPressed` rather than `isActive`, because a menu option must
/// activate once per press. Holding `S` would otherwise wrap the selection through
/// every option and fire the last one, sixty times a second.
class MenuScene final : public Scene
{
public:
    /// Builds the menu's world, entirely in screen space.
    ///
    /// @param context The shared infrastructure, borrowed. Must outlive this scene.
    explicit MenuScene(const SceneContext& context);

    /// Reads navigation and confirmation, and refreshes the option captions.
    ///
    /// Overrides [Scene::onUpdate] rather than `update`, because the base class
    /// clears the pending request before calling this. See [Scene::update] for why
    /// that is not left to each scene.
    void onUpdate(const input::ActionState& actions, float deltaSeconds) override;

    /// Submits the menu's world to the shared renderer.
    void render() override;

    [[nodiscard]] const char* name() const noexcept override { return "MenuScene"; }

    [[nodiscard]] SceneId id() const noexcept override { return SceneId::Menu; }

    /// The number of options, exposed for tests and for a future scene that wants
    /// to know how tall the menu is.
    [[nodiscard]] static std::size_t optionCount() noexcept;

    /// Which option is selected, for tests and diagnostics.
    [[nodiscard]] std::size_t selectedIndex() const noexcept { return m_selected; }

    /// The menu's own world, for tests that want to count its entities.
    [[nodiscard]] const ecs::EntityManager& world() const noexcept { return m_world; }

private:
    /// Adds one screen-space string, centred on `position`.
    void addLabel(std::string_view tag, std::string content, std::uint32_t characterSize, const Vec2 position);

    /// Rewrites the two option captions to show which is selected.
    void refreshSelection();

    ecs::EntityManager m_world;

    /// Declared after `m_world` because a render pass reads the world, and a render
    /// system borrows both the renderer and the camera from the context.
    engine::systems::RenderSystem m_renderSystem;

    /// The empty action snapshot a render pass submits, because
    /// [engine::systems::RenderSystem] reads no input and a render pass has none to give.
    /// A member rather than a temporary, so the cost is not paid per frame.
    input::ActionState m_noActions;

    std::array<std::string_view, 2U> m_options{"START", "QUIT"};
    std::size_t m_selected = 0U;
};

} // namespace engine::scene
