#include "engine/Application.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/input/SfmlKeyMap.hpp"
#include "engine/math/Vec2.hpp"

#include <cstdlib>
#include <filesystem>

namespace engine
{
namespace
{

/// Builds an engine colour from the configured 0-255 background channels.
[[nodiscard]] constexpr Color toColor(const unsigned char red, const unsigned char green,
                                       const unsigned char blue) noexcept
{
    return Color{static_cast<float>(red) / 255.0F, static_cast<float>(green) / 255.0F,
                 static_cast<float>(blue) / 255.0F, 1.0F};
}

/// The background the renderer clears to, still driven by the Phase 1 config.
constexpr Color kWindowBackground = toColor(config::kBackgroundColorRed, config::kBackgroundColorGreen,
                                            config::kBackgroundColorBlue);

} // namespace

Application::Application()
    : m_window(sf::VideoMode{config::kWindowWidth, config::kWindowHeight}, config::kWindowTitle), m_time{},
      m_input{}, m_actionMap{input::defaultActionMap()}, m_actions{}, m_entityManager{}, m_systemManager{},
      m_camera{}, m_renderer{m_window},
      m_assets{std::filesystem::path{config::kAssetsConfig}},
      // Borrowed by const reference, and declared after all three so they are
      // already alive. See the member ordering note in Application.hpp for why the
      // declaration order, and not just this line, is what makes it safe.
      m_renderSystem{m_renderer, m_camera, m_assets},
      // Borrows the renderer, the camera and the asset manager, all constructed
      // above it. It stores only pointers and has no destructor logic, so this is
      // about the file reading in the order those pointers are valid in.
      m_sceneContext{m_renderer, m_camera, m_assets},
      // The one active scene, null until a transition asks for one. `unique_ptr`
      // does not appear in this list: it default-constructs to null.
      m_scene{},
      m_pendingTransition{},
      // The production builder. A `std::function` built from a plain function
      // pointer does not allocate; the indirection exists only so a test can
      // substitute a builder it can watch.
      m_sceneFactory{scene::makeScene}, m_isRunning{true}
{
    m_window.setFramerateLimit(config::kFramerateLimit);

    // The camera's viewport is the window's size in pixels. Position and zoom
    // keep their defaults, so a default camera looks at the world origin at 1:1.
    //
    // This is read once, at construction. The camera is deliberately not tied to
    // the window object and there is no resize handling yet: a fixed viewport is
    // enough for this phase, and wiring resize events in would mean deciding
    // what a camera should do when the window changes shape, which is a design
    // question rather than a plumbing one.
    m_camera.setViewport(Vec2{static_cast<float>(config::kWindowWidth), static_cast<float>(config::kWindowHeight)});
}

int Application::run(const std::optional<std::size_t> maxFrameCount)
{
    std::size_t frameCount = 0;

    while (m_isRunning)
    {
        // Clear only the frame-local input transients, and do it before events
        // are processed. Held keys survive; a press seen during this frame's
        // processEvents() is still visible to systems further down the frame.
        m_input.beginFrame();

        processEvents();

        if (!m_isRunning)
        {
            break; // the window was closed while we were handling events
        }

        update();
        render();
        ++frameCount;

        if (maxFrameCount.has_value() && frameCount >= *maxFrameCount)
        {
            break;
        }
    }

    return EXIT_SUCCESS;
}

void Application::processEvents()
{
    sf::Event event{};

    while (m_window.pollEvent(event))
    {
        switch (event.type)
        {
            case sf::Event::Closed:
                // Application lifecycle, and only Application. A window close is
                // never turned into a keyboard event, and keyboard Escape is
                // never turned into a window close: Input merely reports it, and
                // deciding what Escape means is a policy the game makes.
                m_isRunning = false;
                break;

            default:
                // The adapter recognises keyboard presses and releases and
                // ignores everything else, so the filter lives in one place
                // rather than in the case labels.
                input::applyKeyboardEvent(event, m_input);
                break;
        }
    }
}

void Application::changeScene(const scene::SceneId id)
{
    // Recorded, not performed. See the header for why there is no immediate path.
    m_pendingTransition = scene::SceneTransition::to(id);
}

std::optional<scene::SceneId> Application::sceneId() const noexcept
{
    // Asked of the scene, not remembered beside it, so that what is reported is
    // what is running. A pending request is *not* consulted: a transition that has
    // not been applied yet has not happened, and reporting it early would tell a
    // test that asked for the menu and ran no frame that it was already in one.
    if (m_scene == nullptr)
    {
        return std::nullopt;
    }
    return m_scene->id();
}

void Application::applyPendingTransition()
{
    // Harvest the active scene's request first, so that a scene asking to be
    // replaced is what causes the replacement.
    //
    // A request already recorded by `changeScene` wins, because that came from the
    // owner and is a deliberate instruction about what the game should be doing; a
    // scene's own opinion is a *request*, and an owner that has already decided
    // should not be argued out of it by the thing it is deciding about.
    //
    // Read through a const accessor and cleared by [Scene::update] before the scene
    // runs, so a request lasts exactly one frame. That is what stops a scene whose
    // request was never acted on from asking again on every frame forever.
    if (!m_pendingTransition.has_value() && m_scene != nullptr)
    {
        const scene::SceneTransition& asked = m_scene->pendingTransition();
        if (asked.scene().has_value() || asked.quits())
        {
            m_pendingTransition = asked;
        }
    }

    if (!m_pendingTransition.has_value())
    {
        return; // No scene asked, and nobody called `changeScene`: the common frame.
    }

    const scene::SceneTransition requested = *m_pendingTransition;
    m_pendingTransition.reset();

    if (requested.quits())
    {
        // Applied here rather than at the end of the frame, and with no scene
        // destroyed: the scene that asked has already finished its update, and
        // there is nothing left worth drawing. The scene is left in place so that
        // `currentScene()` still answers truthfully for the rest of this call.
        m_isRunning = false;
        return;
    }

    if (!requested.scene().has_value())
    {
        return; // `stay()`: the common case, and a no-op by construction.
    }

    const scene::SceneId next = *requested.scene();

    // Destroy the old scene *before* building the new one, and in this order
    // deliberately. Building first would mean two scenes alive at once, with two
    // worlds and two sets of systems, for as long as the new one's constructor
    // took - and a level load is not fast. Destroying first means a constructor
    // that throws leaves no scene at all rather than a stale one pretending to be
    // the current game.
    //
    // This runs at a frame boundary, so no system of the outgoing scene is
    // executing: `update()` calls this before it calls any of them.
    m_scene.reset();
    // The requested id is passed to the builder but *not* recorded. What is
    // reported later is asked of the scene that was actually built, so a builder
    // that ignored the request cannot make `sceneId()` lie - see [Scene::id],
    // which exists for exactly that reason.
    m_scene = m_sceneFactory(next, m_sceneContext);
}

const scene::Scene* Application::currentScene() const noexcept
{
    return m_scene.get();
}

void Application::setSceneFactory(SceneFactory factory)
{
    m_sceneFactory = std::move(factory);
}

void Application::update()
{
    // Measure the frame that just happened. This is real elapsed time, not
    // 1 / targetFps: the frame rate cap is a rendering decision and gameplay
    // correctness must not depend on it.
    m_time.tick();

    // Translate the frame's raw keyboard state into actions, once, right here.
    //
    // This is the only place in the engine that builds an action snapshot, and it
    // runs after the events have been recorded and before any system runs. Every
    // system therefore sees the same coherent picture of the frame: the same held
    // keys, the same just-pressed edges, the same everything.
    //
    // `update` overwrites rather than accumulates, so this cannot be left
    // half-done, and calling it once is enough.
    m_actions.update(m_actionMap, m_input);

    // The frame boundary. A scene that asked to be replaced during the previous
    // frame's update is replaced here, before anything of its own runs again and
    // before anything new is built. Placing it after the action snapshot rather
    // than before `tick()` is deliberate: a scene reading `actions` in its final
    // frame is reading the same snapshot the transition decision was made from.
    applyPendingTransition();

    if (m_scene != nullptr)
    {
        // A scene drives its own world with its own systems. The owner does not
        // reach into it: it has no world, no systems and no way to name a
        // component, which is what stops the loop from growing a second opinion
        // about what a menu is.
        m_scene->update(m_actions, m_time.deltaSeconds());
    }
    else
    {
        // Behaviour: every system gets the same action snapshot and the same delta
        // for this frame, and none of them can reach either another way. A system
        // is handed the snapshot rather than the keyboard, so it cannot name a key.
        m_systemManager.update(m_entityManager, m_actions, m_time.deltaSeconds());

        // Deferred destruction cleanup, after systems have run, so an entity a
        // system flagged this frame is erased only once nothing is iterating.
        //
        // A scene flushes its own world's deferred destructions in its own
        // `update`, for the same reason and at the same point: the scene owns the
        // world, so the scene cleans it up.
        m_entityManager.update();
    }
}

void Application::render()
{
    // The render pass: begin, clear to the configured background, let the render
    // system submit its draws, then present. Application performs no drawing
    // itself; it only sequences the pass.
    m_renderer.beginFrame();
    m_renderer.clear(kWindowBackground);

    if (m_scene != nullptr)
    {
        // The active scene draws, and nothing else does. `m_renderSystem` - which
        // draws `m_entityManager` - is skipped entirely while a scene is active, so
        // the two worlds cannot both be drawn into one frame.
        m_scene->render();
    }
    else
    {
        m_renderSystem.update(m_entityManager, m_actions, 0.0F);
    }

    m_renderer.endFrame();
}

} // namespace engine
