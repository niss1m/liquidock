#include "ui/PathIcon.h"

#include <cmath>
#include <cstdlib>

namespace liquidock {
namespace {

using Microsoft::WRL::ComPtr;

struct Cursor {
    const char* p = nullptr;

    void Space() {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') {
            ++p;
        }
    }
    bool More() {
        Space();
        return *p != '\0';
    }
    float Number() {
        Space();
        char* end = nullptr;
        const float value = std::strtof(p, &end);
        p = (end && end != p) ? end : p + 1;
        return value;
    }
    // A flag in an arc is a single digit with no separator after it, so it
    // cannot be read with the routine above: "0050" is four flags, not one
    // number.
    float Flag() {
        Space();
        const float value = (*p == '1') ? 1.0f : 0.0f;
        if (*p == '0' || *p == '1') {
            ++p;
        }
        return value;
    }
    bool NextIsCommand() {
        Space();
        const char c = *p;
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
};

} // namespace

ComPtr<ID2D1PathGeometry> BuildPathGeometry(ID2D1Factory* factory, const char* path) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (!factory || !path || FAILED(factory->CreatePathGeometry(&geometry))) {
        return nullptr;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) {
        return nullptr;
    }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);

    Cursor in{path};
    D2D1_POINT_2F at = D2D1::Point2F(0.0f, 0.0f);
    D2D1_POINT_2F start = at;
    D2D1_POINT_2F lastControl = at;
    bool open = false;
    char command = 0;
    bool lastWasCubic = false;

    auto rel = [&](bool relative, D2D1_POINT_2F point) {
        return relative ? D2D1::Point2F(at.x + point.x, at.y + point.y) : point;
    };

    while (in.More()) {
        if (in.NextIsCommand()) {
            command = *in.p++;
        } else if (command == 'M') {
            command = 'L'; // a moveto carrying extra pairs is a polyline
        } else if (command == 'm') {
            command = 'l';
        }
        const bool relative = (command >= 'a' && command <= 'z');
        const char op = static_cast<char>(relative ? command - 32 : command);
        const bool cubic = (op == 'C' || op == 'S');

        switch (op) {
            case 'M': {
                if (open) {
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                }
                const float x = in.Number();
                const float y = in.Number();
                at = rel(relative, D2D1::Point2F(x, y));
                start = at;
                sink->BeginFigure(at, D2D1_FIGURE_BEGIN_FILLED);
                open = true;
                break;
            }
            case 'L': {
                const float x = in.Number();
                const float y = in.Number();
                at = rel(relative, D2D1::Point2F(x, y));
                sink->AddLine(at);
                break;
            }
            case 'H': {
                const float x = in.Number();
                at = D2D1::Point2F(relative ? at.x + x : x, at.y);
                sink->AddLine(at);
                break;
            }
            case 'V': {
                const float y = in.Number();
                at = D2D1::Point2F(at.x, relative ? at.y + y : y);
                sink->AddLine(at);
                break;
            }
            case 'C': {
                const float x1 = in.Number();
                const float y1 = in.Number();
                const float x2 = in.Number();
                const float y2 = in.Number();
                const float x = in.Number();
                const float y = in.Number();
                const D2D1_POINT_2F c1 = rel(relative, D2D1::Point2F(x1, y1));
                const D2D1_POINT_2F c2 = rel(relative, D2D1::Point2F(x2, y2));
                const D2D1_POINT_2F end = rel(relative, D2D1::Point2F(x, y));
                sink->AddBezier(D2D1::BezierSegment(c1, c2, end));
                lastControl = c2;
                at = end;
                break;
            }
            case 'S': {
                // The first control point mirrors the previous curve's last
                // one; after anything that was not a curve it sits on the
                // current point.
                const float x2 = in.Number();
                const float y2 = in.Number();
                const float x = in.Number();
                const float y = in.Number();
                const D2D1_POINT_2F c1 =
                    lastWasCubic
                        ? D2D1::Point2F(2.0f * at.x - lastControl.x, 2.0f * at.y - lastControl.y)
                        : at;
                const D2D1_POINT_2F c2 = rel(relative, D2D1::Point2F(x2, y2));
                const D2D1_POINT_2F end = rel(relative, D2D1::Point2F(x, y));
                sink->AddBezier(D2D1::BezierSegment(c1, c2, end));
                lastControl = c2;
                at = end;
                break;
            }
            case 'A': {
                // SVG states an arc by its endpoint and so does D2D, so this is
                // a translation rather than a conversion.
                const float rx = in.Number();
                const float ry = in.Number();
                const float rotation = in.Number();
                const float large = in.Flag();
                const float sweep = in.Flag();
                const float x = in.Number();
                const float y = in.Number();
                const D2D1_POINT_2F end = rel(relative, D2D1::Point2F(x, y));
                sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(std::fabs(rx), std::fabs(ry)),
                                              rotation,
                                              sweep != 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                                            : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                              large != 0.0f ? D2D1_ARC_SIZE_LARGE
                                                            : D2D1_ARC_SIZE_SMALL));
                at = end;
                break;
            }
            case 'Z': {
                if (open) {
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    open = false;
                }
                at = start;
                break;
            }
            default:
                // A command this parser does not know. Stop rather than guess:
                // a wrong outline is worse than a missing one.
                if (open) {
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    open = false;
                }
                sink->Close();
                return geometry;
        }
        lastWasCubic = cubic;
    }

    if (open) {
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }
    if (FAILED(sink->Close())) {
        return nullptr;
    }
    return geometry;
}

} // namespace liquidock
