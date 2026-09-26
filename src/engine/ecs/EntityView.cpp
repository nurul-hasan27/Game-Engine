#include "engine/ecs/EntityView.hpp"

namespace engine::ecs
{

EntityView::Iterator EntityView::begin() const
{
    return Iterator{m_entities->begin(), m_entities->end(), m_tag};
}

EntityView::Iterator EntityView::end() const
{
    return Iterator{m_entities->end(), m_entities->end(), m_tag};
}

} // namespace engine::ecs
