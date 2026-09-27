#include "engine/input/SfmlKeyMap.hpp"

namespace engine::input
{

Key toEngineKey(const sf::Keyboard::Key key) noexcept
{
    switch (key)
    {
        case sf::Keyboard::A:
            return Key::A;
        case sf::Keyboard::D:
            return Key::D;
        case sf::Keyboard::S:
            return Key::S;
        case sf::Keyboard::W:
            return Key::W;
        case sf::Keyboard::Left:
            return Key::Left;
        case sf::Keyboard::Right:
            return Key::Right;
        case sf::Keyboard::Up:
            return Key::Up;
        case sf::Keyboard::Down:
            return Key::Down;
        case sf::Keyboard::Space:
            return Key::Space;
        case sf::Keyboard::Escape:
            return Key::Escape;
        case sf::Keyboard::Z:
            return Key::Z;
        case sf::Keyboard::X:
            return Key::X;
        default:
            // Everything else, including sf::Keyboard::Unknown, is not tracked.
            return Key::Unknown;
    }
}

void applyKeyboardEvent(const sf::Event& event, Input& input) noexcept
{
    switch (event.type)
    {
        case sf::Event::KeyPressed:
            input.processKeyDown(toEngineKey(event.key.code));
            break;

        case sf::Event::KeyReleased:
            input.processKeyUp(toEngineKey(event.key.code));
            break;

        default:
            // Mouse, text, resize, focus and everything else are not this
            // function's business. The window close case is handled by
            // Application, not here.
            break;
    }
}

} // namespace engine::input
