#pragma once

#include "engine/input/Input.hpp"

#include <SFML/Window/Event.hpp>
#include <SFML/Window/Keyboard.hpp>

namespace engine::input
{

/// Translates an SFML physical key into an engine key.
///
/// Any key the engine does not track maps to `Key::Unknown`, which is inert.
/// That is the whole mechanism by which unmapped keys are handled: they are
/// recognised, discarded and never become state.
[[nodiscard]] Key toEngineKey(sf::Keyboard::Key key) noexcept;

/// Feeds one SFML window event into engine input state.
///
/// Keyboard press and release events are translated and recorded. Every other
/// event type is ignored, so `Application` can forward everything it polls
/// without filtering first.
///
/// `sf::Event::Closed` is deliberately **not** handled here. Closing the window
/// is an application lifecycle concern and stays with `Application`; a window
/// close is never dressed up as a keyboard event.
///
/// This function is the entire SFML-to-input boundary for the keyboard, and it
/// is a pure translation: it holds no state, so the tests can drive real
/// `sf::Event`s through it and reach the real Input state machine.
void applyKeyboardEvent(const sf::Event& event, Input& input) noexcept;

} // namespace engine::input
