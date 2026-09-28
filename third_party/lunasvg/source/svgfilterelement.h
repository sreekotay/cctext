// cctext patch (0006-filters.patch): SVG / CSS filter effects for lunasvg.
//
// <filter> with feGaussianBlur, feOffset, feFlood, feMerge, feComposite,
// feColorMatrix, feBlend, feComponentTransfer and feDropShadow, filter
// regions and primitive subregions in objectBoundingBox or userSpaceOnUse
// units, color-interpolation-filters (linearRGB / sRGB), and the CSS filter
// functions (blur(), drop-shadow(), grayscale(), ...). The other primitives
// (feImage, feTile, feTurbulence, feMorphology, lighting, ...) pass their
// input through.
//
// A filter runs in device pixels on the element's offscreen canvas: blur
// deviations and offsets are scaled by the current transform (a rotated or
// skewed element blurs along the device axes). Its cost is bounded by
// lunasvg_set_filter_limits(): the filter region's pixels, the blur
// deviation, the primitives per filter and the bytes of live intermediate
// images.
#ifndef LUNASVG_SVGFILTERELEMENT_H
#define LUNASVG_SVGFILTERELEMENT_H

#include "svgelement.h"

namespace lunasvg {

class SVGFilterElement final : public SVGElement, public SVGURIReference {
public:
    SVGFilterElement(Document* document);

    // xlink:href: attributes and primitives missing here come from the
    // referenced <filter> (a chain, cycles ignored).
    const SVGFilterElement* attributeSource(PropertyID id) const;
    const SVGFilterElement* primitivesSource() const;

    const SVGLength& x() const { return m_x; }
    const SVGLength& y() const { return m_y; }
    const SVGLength& width() const { return m_width; }
    const SVGLength& height() const { return m_height; }
    Units filterUnits() const { return m_filterUnits.value(); }
    Units primitiveUnits() const { return m_primitiveUnits.value(); }

    // The filter region in the user space of `element` (empty: not rendered).
    Rect filterRegion(const SVGElement* element) const;

private:
    SVGLength m_x;
    SVGLength m_y;
    SVGLength m_width;
    SVGLength m_height;
    SVGEnumeration<Units> m_filterUnits;
    SVGEnumeration<Units> m_primitiveUnits;
};

class SVGFilterPrimitiveElement final : public SVGElement {
public:
    SVGFilterPrimitiveElement(Document* document, ElementID id);

    const SVGLength& x() const { return m_x; }
    const SVGLength& y() const { return m_y; }
    const SVGLength& width() const { return m_width; }
    const SVGLength& height() const { return m_height; }

    const Color& floodColor() const { return m_floodColor; }
    float floodOpacity() const { return m_floodOpacity; }
    bool linearRGB() const { return m_linearRGB; }

    void layoutElement(const SVGLayoutState& state) final;

private:
    SVGLength m_x;
    SVGLength m_y;
    SVGLength m_width;
    SVGLength m_height;
    Color m_floodColor = Color::Black;
    float m_floodOpacity = 1.f;
    bool m_linearRGB = true;
};

// CSS filter functions ("blur(2px) grayscale(50%)"): true when `value` is a
// valid list (`element` resolves lengths; may be null to only validate).
bool parseFilterFunctions(const std::string& value, const SVGElement* element);
// The region filter functions paint for an element whose paint box is `box`.
Rect filterFunctionsRegion(const SVGElement* element, const Rect& box);

// The offscreen canvas rect (device) for a filtered group whose paint box
// maps to `region`, drawn onto a canvas with `extents`.
Rect filterCanvasRect(const Rect& region, const Rect& extents);

// Run `element`'s filter on `canvas` (its SourceGraphic) in place; `ctm` maps
// the element's user space to device pixels. False: draw nothing.
bool applyFilter(const SVGElement* element, Canvas& canvas, const Transform& ctm);

} // namespace lunasvg

#endif // LUNASVG_SVGFILTERELEMENT_H
