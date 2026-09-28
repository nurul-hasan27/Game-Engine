#pragma once

#include "engine/assets/AssetManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/Renderer.hpp"
#include "engine/input/ActionState.hpp"

#include <cstddef>
#include <memory>
#include <optional>

namespace engine::scene
{

/// Which scene is active.
///
/// A closed set, because the engine has to be able to build any scene from an id
/// alone with no knowledge of the concrete types. An open "any key the caller
/// likes" would need a registry, and a registry is a singleton with extra steps.
enum class SceneId
{
    /// The front end. The first scene the game ever shows.
    Menu,

    /// A level being played.
    Play
};

/// What a scene asked for at the end of its turn.
///
/// ### Why a scene asks rather than performs
///
/// A scene cannot change which scene is active, because it does not own the answer:
/// [engine::Application] does. So a scene *records a request* and the owner applies
/// it at a boundary it controls. That is the whole of the deferral, and
/// [Scene::pendingTransition] is the request.
///
/// ### The three answers
///
/// - **Stay.** The common case, and the default: a frame with no transition is a
///   frame where the same scene keeps going.
/// - **To a scene.** A switch. The old scene is destroyed and the new one built.
/// - **Quit.** Stop the application.
///
/// Quit is a separate answer rather than a scene, so that a scene never has to know
/// what "no scene" means or who would own it. A menu offering "QUIT" is a real
/// option, not a placeholder for a future state.
class SceneTransition
{
public:
    /// "Nothing was asked for" - the same value as [stay].
    ///
    /// Public because it is a state a caller legitimately holds, not just one the
    /// factory methods hand out: a scene's own pending request starts as this, a
    /// default-constructed [Scene] has one, and a test asserting on a transition
    /// needs to be able to write "no transition" without a verb. Declaring the
    /// two-argument constructor below suppresses the implicit default one, so this
    /// is written out rather than left to the compiler.
    SceneTransition() noexcept = default;

    /// Keep the current scene, and keep running.
    [[nodiscard]] static SceneTransition stay() noexcept { return {}; }

    /// Switch to `id`: destroy the current scene and build that one.
    [[nodiscard]] static SceneTransition to(const SceneId id) noexcept
    {
        // The `optional` is built explicitly rather than let to convert from the
        // enum inside a braced list. The converting constructor is a template, and
        // a braced list is exactly the context where a template constructor is
        // least likely to be picked - so naming the type is both clearer and the
        // only version that compiles.
        return SceneTransition{std::optional<SceneId>{id}, false};
    }

    /// Stop the application, with no scene change.
    [[nodiscard]] static SceneTransition quitApplication() noexcept
    {
        return SceneTransition{std::nullopt, true};
    }

    /// The scene to switch to, or `nullopt` to keep the current one.
    [[nodiscard]] const std::optional<SceneId>& scene() const noexcept { return m_scene; }

    /// True when the application should stop.
    [[nodiscard]] bool quits() const noexcept { return m_quit; }

private:
    SceneTransition(const std::optional<SceneId> scene, const bool quit) noexcept : m_scene{scene}, m_quit{quit} {}

    /// Neither member is `const`, and that is load bearing rather than an oversight.
    /// A `const` member would delete the copy assignment operator, and
    /// [Scene::requestTransition] assigns a whole `SceneTransition` over the
    /// previous one - so a `const` here would make it impossible for a scene to
    /// change its mind within a frame, which is exactly what it must be able to do.
    /// The values are still only ever *read* through the const accessors above, so
    /// no caller can change a transition after it has been asked for.
    std::optional<SceneId> m_scene;
    bool m_quit = false;
};

/// The infrastructure a scene may use, borrowed and never owned.
///
/// ### What it is for
///
/// A scene needs to draw, to read the camera, and to look assets up by name. All
/// three are owned by [engine::Application] and must outlive every scene, so they
/// arrive as borrowed references rather than being duplicated per scene.
///
/// ### Why it is not a back-pointer to Application
///
/// The course's reference gives a scene a `GameEngine*` - see
/// `Scene.h` in the reference, where `Scene::m_game` is the engine - which lets a
/// scene reach the window, the asset table, the running flag and every other scene.
/// This type names the three things a scene actually needs and cannot reach
/// anything else. The asset manager is handed out as the **interface** by const
/// reference, so a scene can look a name up and cannot load, replace or remove one.
///
/// ### Lifetime
///
/// The context borrows from the Application, and a scene borrows from the context,
/// so neither outlives the other and neither can dangle relative to the other.
/// There is no interface to keep in step, because it has no methods.
class SceneContext
{
public:
    /// Borrows `renderer`, `camera` and `assets`, all of which must outlive this.
    SceneContext(graphics::Renderer& renderer, graphics::Camera& camera, const assets::AssetManager& assets) noexcept
        : m_renderer{&renderer}, m_camera{&camera}, m_assets{&assets}
    {
    }

    /// The graphics boundary. Drawn through, never owned.
    ///
    /// A scene adds entities and submits draws; it does not own a window, and it
    /// cannot open or resize one.
    [[nodiscard]] graphics::Renderer& renderer() const noexcept { return *m_renderer; }

    /// The camera, mutable.
    ///
    /// Mutable for two reasons, and both are about matching what already exists
    /// rather than granting anything new. [engine::Application::camera] hands the
    /// camera out non-const, and [graphics::CameraSystem] takes it by mutable
    /// reference because following an entity *is* moving the view. A scene that owns
    /// the world whose entities the camera follows has to be able to give the
    /// camera system what it asks for, and a `const_cast` in the middle of that
    /// would be a lie about what the type allows.
    ///
    /// It is also where a scene reads the **window size**, through
    /// [graphics::Camera::viewport] and [graphics::Camera::screenCenter], which is
    /// how a menu centres a title without an `sf::RenderWindow` anywhere in its
    /// API.
    [[nodiscard]] graphics::Camera& camera() const noexcept { return *m_camera; }

    /// The loaded assets, by the interface and by const reference.
    ///
    /// Handed out the same way [engine::Application::assets] hands it out, and for
    /// the same reason: a scene gets reads, not the ability to invalidate the
    /// references every other scene and system is holding.
    [[nodiscard]] const assets::AssetManager& assets() const noexcept { return *m_assets; }

private:
    graphics::Renderer* m_renderer;
    graphics::Camera* m_camera;
    const assets::AssetManager* m_assets;
};

/// A game state: a world, the systems that run in it, and what it wants next.
///
/// ### Who owns what
///
/// ```text
/// Application                     owns, for the whole process
///   sf::RenderWindow                 the window
///   SfmlRenderer                     the graphics boundary
///   SfmlAssetManager                 every asset, loaded once
///   Camera                           the view
///   ActionMap / ActionState          input
///   current Scene    (optional)   owns, for as long as it is active
///     its own EntityManager            its world and nothing else's
///     its own systems                   the behaviour registered for that world
/// ```
///
/// The split is the point. **Assets and the renderer are above scenes**, so a menu
/// and a level share one loaded font and one graphics boundary rather than each
/// duplicating them. **Worlds and systems are below**, so a scene's entities
/// disappear with the scene and cannot leak into the next one - which is exactly
/// what a transition has to guarantee.
///
/// A scene therefore does *not* need an [engine::assets::AssetManager] of its own,
/// and is given the shared one through its [SceneContext]. Giving each scene its
/// own would mean loading every texture once per scene, which is the thing the
/// asset cache exists to prevent.
///
/// ### One scene is active at a time
///
/// [engine::Application] holds at most one. A scene's `update` and `render` are
/// called only while it is the active one, and a transition destroys the old scene
/// before the new one is built, so two scenes can never both be updating or both
/// be drawing.
///
/// ### Transitions are deferred, and the boundary is a frame
///
/// A scene cannot switch scenes itself; it can only *ask*, through
/// [requestTransition](Scene.hpp). The owner applies the request at the start of the
/// next frame, so:
///
/// - a scene is never destroyed while one of its own systems is still executing,
///   and
/// - a whole frame belongs to exactly one scene.
///
/// The second is the stronger and more useful guarantee. A transition requested
/// during frame N's update is applied before frame N+1's update, so frame N is
/// played and drawn entirely by the old scene and frame N+1 entirely by the new
/// one. Nothing observes a world that was updated by one scene and drawn by another.
///
/// ### Copying
///
/// Deleted. A scene owns a world, a system manager and systems that hold pointers
/// into both; copying one would duplicate that identity rather than move it, and
/// moving it is what destruction and construction already do.
class Scene
{
public:
    virtual ~Scene() = default;

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    /// Advances this scene's world by one frame.
    ///
    /// @param actions What the player is asking for this frame, already resolved
    ///        from physical input by the owner. A scene reads actions and never
    ///        names a key: that is what the action layer is for.
    /// @param deltaSeconds How long the frame actually took, in real seconds.
    ///
    /// ### Not virtual, and that is deliberate
    ///
    /// This is a template method over a virtual [onUpdate], so that the base class
    /// can clear the pending request *before* the scene runs.
    ///
    /// The alternative was to leave `update` virtual and ask every scene to reset
    /// its own transition first - which is a rule no compiler enforces and every
    /// subclass can forget. And a subclass that forgets does not fail loudly: the
    /// request simply never clears, and the owner re-applies it every frame, so the
    /// game ping-pongs between two scenes forever with no error anywhere. A rule
    /// whose failure mode is an infinite loop with no diagnostic belongs in the base
    /// class, not in a comment asking subclasses to be careful.
    ///
    /// The consequence is the rule it enforces: **a request lives exactly one
    /// frame.** A scene asks during its update; the owner reads it at the next
    /// frame boundary and applies it; by the time the scene runs again its request
    /// is gone, so a scene that asked and was not answered does not ask again.
    void update(const input::ActionState& actions, const float deltaSeconds)
    {
        m_transition = SceneTransition{};

        onUpdate(actions, deltaSeconds);
    }

    /// Draws this scene's world.
    ///
    /// Called only while this scene is active, and never in the same frame as
    /// another scene's `render`.
    virtual void render() = 0;

    /// A short name for diagnostics, tests and error messages.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Which scene this is.
    ///
    /// Asked of the scene rather than remembered beside it. The owner builds a
    /// scene *from* an id, so a separate record of "which id did we ask for" would
    /// be a second source of truth that can disagree with reality: a factory that
    /// returned the wrong scene for an id would leave the remembered id lying and
    /// the disagreement invisible. Having the scene answer for itself means the
    /// owner reports what is actually running, and a mismatch between the request
    /// and the result is something a test can see.
    [[nodiscard]] virtual SceneId id() const noexcept = 0;

    /// What this scene asked for, or "stay" if it has not asked for anything.
    ///
    /// Read by the owner, and read-only to it: the owner applies the transition, it
    /// does not invent one.
    [[nodiscard]] const SceneTransition& pendingTransition() const noexcept { return m_transition; }

    /// The shared infrastructure. Available to the scene, not to its systems.
    [[nodiscard]] const SceneContext& context() const noexcept { return *m_context; }

protected:
    /// Borrows `context`, which must outlive this scene.
    explicit Scene(const SceneContext& context) noexcept : m_context{&context} {}

    /// This scene's behaviour for one frame.
    ///
    /// Called by [update], which has already cleared the pending request. A scene
    /// overrides this and never `update` itself, so it cannot forget to clear.
    virtual void onUpdate(const input::ActionState& actions, float deltaSeconds) = 0;

    /// Asks for a scene switch. Applied at the start of the next frame.
    ///
    /// The last request in a frame wins, rather than the first. A scene that asks
    /// twice in one frame has changed its mind, and honouring the earlier one would
    /// run a state the scene had already decided against.
    void requestTransition(const SceneTransition transition) noexcept { m_transition = transition; }

private:
    const SceneContext* m_context;
    SceneTransition m_transition{SceneTransition::stay()};
};

/// Builds the scene `id` names, wired to `context`.
///
/// A free function rather than a registry or a switch inside Application, so that
/// "which scenes exist" is answered in one place that can be read top to bottom, and
/// so a caller that wants a scene without an Application - a test, a tool - can have
/// one.
///
/// The returned scene borrows `context`, so it must not outlive it.
[[nodiscard]] std::unique_ptr<Scene> makeScene(const SceneId id, const SceneContext& context);

} // namespace engine::scene
