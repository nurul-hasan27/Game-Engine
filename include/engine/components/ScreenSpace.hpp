#pragma once

namespace engine::components
{

/// Marks an entity as drawn in **screen space**: its [Transform](Transform.hpp)
/// is already in screen pixels, and the camera is not applied to it.
///
/// ### Why this exists
///
/// Every renderable in this engine has been world space since Lecture 11 section 5,
/// and that is right for everything that belongs to a level. A menu is the first
/// thing here that does not: a title at the top of the screen is a property of the
/// *window*, not of the world, and a title that slid away when the player walked
/// right would be a bug rather than a feature.
///
/// ### How it is applied
///
/// By *omission*, in one place. [engine::graphics::toRenderTransform] applies the
/// camera and [engine::graphics::toScreenTransform] does not, and
/// [engine::systems::RenderSystem]'s text query picks between them by looking for
/// this component. The alternative - a second renderer, a second query, or a
/// boolean parameter threaded through the draw call - would each be a second answer
/// to a question this one answers.
///
/// The rule lives in the query rather than in the component, which is the same
/// arrangement [RenderSystem] already uses to decide that an animation wins over a
/// plain texture: the components stay unaware of each other and the system owns the
/// policy.
///
/// ### Deliberately honoured by the text query only
///
/// A menu needs text, so text is what this applies to. Extending it to the
/// rectangle and texture queries would be a small change, and it is *not* made
/// here: doing so would be the first step towards a general screen-space UI system,
/// and the course has not asked for one. When something needs a screen-space panel
/// or sprite, that is the moment to decide how.
///
/// ### Empty on purpose
///
/// No fields, because it is a marker and not a configuration. An entity is either
/// placed on the screen or in the world, and there is no third thing to say about
/// which.
struct ScreenSpace
{
};

} // namespace engine::components
