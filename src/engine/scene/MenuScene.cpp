#include "engine/scene/MenuScene.hpp"

#include "engine/components/ScreenSpace.hpp"
#include "engine/components/Text.hpp"
#include "engine/components/Transform.hpp"
#include "engine/input/Action.hpp"

#include <string>
#include <utility>

namespace engine::scene
{

namespace
{

/// The committed pixel font, at the size the course reference draws its menu title.
///
/// Chosen because the reference draws its own menu in this font at this size, so
/// this menu is the same *kind* of thing the course asked for rather than a new
/// invention. No font is added for it.
constexpr std::string_view kMenuFont = "fonts_pixeled";

/// Character sizes, matching the reference's menu: a 52-point title, 12-point body.
constexpr std::uint32_t kTitleSize = 52U;
constexpr std::uint32_t kOptionSize = 24U;
constexpr std::uint32_t kHintSize = 12U;

/// Fractions of the viewport height, so the menu lays itself out against whatever
/// window it was given rather than against a hardcoded 720.
constexpr float kTitleHeight = 0.22F;
constexpr float kFirstOptionHeight = 0.45F;
constexpr float kOptionSpacing = 56.0F;
constexpr float kHintHeight = 0.86F;

} // namespace

MenuScene::MenuScene(const SceneContext& context) : Scene{context}, m_renderSystem{context.renderer(), context.camera(), context.assets()}
{
    // Read once, from the engine's own view abstraction. This is how a menu knows
    // how big the screen is without an `sf::RenderWindow` in its API.
    const Vec2 viewport = context.camera().viewport();
    const float centreX = context.camera().screenCenter().x;

    // The title, in screen space so the camera cannot move it.
    addLabel("menu.title", "GAME ENGINE", kTitleSize, Vec2{centreX, viewport.y * kTitleHeight});

    // The options. Built in the constructor so their entities exist before the
    // first frame and the scene never has to create one mid-update.
    for (std::size_t index = 0U; index < m_options.size(); ++index)
    {
        const float y = viewport.y * kFirstOptionHeight + (static_cast<float>(index) * kOptionSpacing);
        // `std::string{m_options[index]}` and not the `string_view` itself:
        // `addLabel` takes an owning string and `string_view` does not convert.
        addLabel("menu.option." + std::to_string(index), std::string{m_options[index]}, kOptionSize,
                 Vec2{centreX, y});
    }

    // A caption naming the controls. Prose about keys is not reading keys: nothing
    // here queries the keyboard, and the actions it does read are named in the same
    // words the action layer uses.
    addLabel("menu.hint", "MOVE: ARROWS   SELECT: SPACE", kHintSize, Vec2{centreX, viewport.y * kHintHeight});

    refreshSelection();
}

void MenuScene::addLabel(const std::string_view tag, std::string content, const std::uint32_t characterSize,
                         const Vec2 position)
{
    ecs::Entity& entity = m_world.addEntity(std::string{tag});
    entity.addComponent<components::Transform>(components::Transform{position, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    components::Text text;
    text.content = std::move(content);
    // `std::string`, not `std::string_view`: the component owns the name, and
    // `string_view` does not convert implicitly.
    text.fontAssetName = std::string{kMenuFont};
    text.characterSize = characterSize;
    entity.addComponent<components::Text>(std::move(text));

    // The one line that makes this a menu rather than a level. Everything above is
    // the ordinary ECS path; this is what stops the camera being applied.
    entity.addComponent<components::ScreenSpace>();
}

void MenuScene::refreshSelection()
{
    for (std::size_t index = 0U; index < m_options.size(); ++index)
    {
        // Found by tag rather than through a stored pointer. The scene owns the
        // world, so a pointer would be valid - but a query cannot be wrong about
        // whether the entity is still alive, and this runs every frame on a menu
        // with four entities, so the cost is not worth the lifetime question.
        //
        // The tag is a **named** `std::string`, and that is load bearing rather than
        // style. `EntityView` holds its tag as a `std::string_view`, and
        // `getEntities` forwards its `string_view` parameter straight into it - so
        // `getEntities("menu.option." + std::to_string(index))` leaves the view
        // pointing at a `std::string` that died at the end of that same full
        // expression. `auto&&` extends the lifetime of the *view*; it does nothing at
        // all for the string the view points into.
        //
        // The Debug build happened to work, because the freed block still held the
        // bytes; the Release build did not, and the captions silently kept their
        // unprefixed text. Found by running the suite in Release, which is the only
        // reason it was found at all.
        const std::string tag = "menu.option." + std::to_string(index);
        const auto& labels = m_world.getEntities(tag);
        // `const Entity&`, because `getEntities` hands out a read-only view. The
        // component itself is still mutable: `Entity::getComponent` is const and
        // returns `T&`, which is how a read-only view of the world can still be
        // used to update the world. That asymmetry is the entity manager's existing
        // contract, not something introduced here.
        for (const ecs::Entity& entity : labels)
        {
            components::Text& text = entity.getComponent<components::Text>();
            text.content = (index == m_selected) ? "> " + std::string{m_options[index]}
                                                : "  " + std::string{m_options[index]};
        }
    }
}

void MenuScene::onUpdate(const input::ActionState& actions, const float deltaSeconds)
{
    // No simulation: a menu has no physics and no timing. The parameter is accepted
    // only so every scene shares one signature, exactly as every system does.
    static_cast<void>(deltaSeconds);

    // Navigation, on the press edge rather than while held, so holding a direction
    // does not run the selection through every option and stop on the last.
    //
    // `MoveUp` steps backwards and wraps, so "up" from the first option lands on the
    // last. Wrapping both ways is what makes a two-item menu usable without a
    // "you are at the end" case.
    if (actions.wasPressed(input::Action::MoveUp))
    {
        m_selected = (m_selected + m_options.size() - 1U) % m_options.size();
    }

    if (actions.wasPressed(input::Action::MoveDown))
    {
        m_selected = (m_selected + 1U) % m_options.size();
    }

    // Confirmation, also on the press edge: one tap starts the game, and holding
    // `Space` would otherwise start it and immediately quit it.
    if (actions.wasPressed(input::Action::Shoot))
    {
        if (m_selected == 0U)
        {
            requestTransition(SceneTransition::to(SceneId::Play));
        }
        else
        {
            // Quitting is a request, not a call. The scene cannot stop the
            // application; it can only say it should stop, and the owner decides
            // when to honour that.
            requestTransition(SceneTransition::quitApplication());
        }
    }

    // ### Escape quits here, and it is the second half of the course's sentence
    //
    // Assignment 3: *"The 'ESC' key should go 'back' to the Main Menu, or quit if
    // on the Main Menu"*. A level has somewhere to go back to; the main menu does
    // not, and the same sentence says what happens instead. So `Quit` - the action
    // the action layer already bound to `Escape` - means "leave the game" here,
    // while [engine::scene::PlayScene] reads the very same action as "go back".
    //
    // Read on the press edge like everything else, and **after** the confirmation
    // above on purpose: `Escape` and `Space` are different keys, so the order is
    // not about which one won, and the last request in a frame is the one honoured
    // ([engine::scene::Scene::requestTransition]). If this block came first, a
    // future key that drove both would quit instead of starting the game.
    //
    // It is a request, exactly like the menu's own QUIT option above, so nothing
    // here closes a window or touches the application: the scene says what it wants
    // and [engine::Application] decides when to honour it. There is deliberately no
    // `Application::close()` to call - the owner is not reachable from a scene, and
    // [engine::scene::SceneTransition::quitApplication] is the whole of what a
    // scene may say about the application's lifetime.
    //
    // The action layer documents `Quit` as *"Deliberately not acted on by
    // Application"*, which this is consistent with: the engine reports Escape, and
    // the **game** decides what it means. Here the game has decided.
    if (actions.wasPressed(input::Action::Quit))
    {
        requestTransition(SceneTransition::quitApplication());
    }

    refreshSelection();
}

void MenuScene::render()
{
    // `RenderSystem::update` takes an action snapshot and a delta because it shares
    // one signature with every other system, and it reads neither - drawing is not
    // time dependent and does not read the keyboard. A render pass has no snapshot
    // to pass, so it is given the empty one, and the delta is zero for the same
    // reason.
    //
    // A member rather than a temporary: constructing an `ActionState` is three
    // `fill` calls per frame, and this is not a place to spend them.
    m_renderSystem.update(m_world, m_noActions, 0.0F);
}

std::size_t MenuScene::optionCount() noexcept
{
    return 2U;
}

} // namespace engine::scene
