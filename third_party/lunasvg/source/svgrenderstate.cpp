#include "svgrenderstate.h"
#include "svgfilterelement.h"

namespace lunasvg {

SVGBlendInfo::SVGBlendInfo(const SVGElement* element)
    : m_clipper(element->clipper())
    , m_masker(element->masker())
    , m_opacity(element->opacity())
{
    if(element->filter() || element->hasInvalidFilter() || !element->filterFunctions().empty())
        m_filtered = element;
}

bool SVGBlendInfo::requiresCompositing(SVGRenderMode mode) const
{
    return (m_clipper && m_clipper->requiresMasking()) || (mode == SVGRenderMode::Painting && (m_masker || m_opacity < 1.f || m_filtered));
}

bool SVGRenderState::hasCycleReference(const SVGElement* element) const
{
    auto current = this;
    do {
        if(element == current->element())
            return true;
        current = current->parent();
    } while(current);
    return false;
}

void SVGRenderState::beginGroup(const SVGBlendInfo& blendInfo)
{
    auto requiresCompositing = blendInfo.requiresCompositing(m_mode);
    if(requiresCompositing) {
        auto boundingBox = m_currentTransform.mapRect(m_element->paintBoundingBox());
        // cctext patch: a filter reads pixels outside the visible canvas
        // (offsets and blurs bring them in), so its canvas covers the whole
        // filter region when that fits the filter pixel limit.
        if(m_mode == SVGRenderMode::Painting && blendInfo.filtered())
            boundingBox = filterCanvasRect(blendInfo.filtered(), boundingBox, m_canvas->extents(), m_currentTransform);
        else
            boundingBox.intersect(m_canvas->extents());
        m_canvas = Canvas::create(boundingBox);
    } else {
        m_canvas->save();
    }

    if(!requiresCompositing && blendInfo.clipper()) {
        blendInfo.clipper()->applyClipPath(*this);
    }
}

void SVGRenderState::endGroup(const SVGBlendInfo& blendInfo)
{
    if(m_canvas == m_parent->canvas()) {
        m_canvas->restore();
        return;
    }

    auto opacity = m_mode == SVGRenderMode::Clipping ? 1.f : blendInfo.opacity();
    if(m_mode == SVGRenderMode::Painting && blendInfo.filtered()) {
        // cctext patch: the filter runs first, then clipping, masking and
        // opacity (Filter Effects 1, 2 and CSS Masking 1).
        if(!applyFilter(blendInfo.filtered(), *m_canvas, m_currentTransform))
            return;
    }
    if(blendInfo.clipper())
        blendInfo.clipper()->applyClipMask(*this);
    if(m_mode == SVGRenderMode::Painting && blendInfo.masker()) {
        blendInfo.masker()->applyMask(*this);
    }

    m_parent->m_canvas->blendCanvas(*m_canvas, BlendMode::Src_Over, opacity);
}

} // namespace lunasvg
