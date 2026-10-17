#include "MidiPanels.h"

namespace genisys::ui
{
namespace
{
    const juce::Identifier mappableId ("genisysParam");

    // Knobs: over the value readout, so the knob's name stays readable.
    // Short controls (switches, choices): just above the outline.
    void drawTag (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> outline, juce::Colour fill)
    {
        const float w = PixelFont::textWidth (text, 1.0f) + 6.0f;
        const auto box = outline.getHeight() >= 60.0f
                             ? juce::Rectangle<float> (outline.getCentreX() - w * 0.5f, outline.getY() + outline.getHeight() * 0.73f, w, 11.0f)
                             : juce::Rectangle<float> (outline.getX(), outline.getY() - 12.0f, w, 11.0f);
        g.setColour (fill);
        g.fillRect (box);
        PixelFont::drawIn (g, text, box, 1.0f, juce::Colour (0xff111111), juce::Justification::centred, false);
    }
}

// ---------------------------------------------------------------------------
LearnOverlay::LearnOverlay (MidiMap& m) : map (m) {}

void LearnOverlay::collect (juce::Component& parent, std::vector<Target>& out) const
{
    for (auto* child : parent.getChildren())
    {
        if (! child->isVisible())
            continue;
        const auto id = child->getProperties()[mappableId].toString();
        if (id.isNotEmpty())
            out.push_back ({ id, getLocalArea (child, child->getLocalBounds()) });
        else
            collect (*child, out);
    }
}

std::vector<LearnOverlay::Target> LearnOverlay::findTargets() const
{
    std::vector<Target> targets;
    if (page != nullptr)
        collect (*page, targets);
    return targets;
}

void LearnOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.35f));
    const auto learning = map.learningParamId();

    for (const auto& t : findTargets())
    {
        const auto r = t.area.toFloat().reduced (1.0f);
        const int cc = map.ccFor (t.paramId);
        if (t.paramId == learning)
        {
            g.setColour (blinkOn ? theme::yellow : theme::yellow.withAlpha (0.3f));
            g.drawRect (r, 3.0f);
            drawTag (g, "MOVE A KNOB", r, theme::yellow);
        }
        else if (cc >= 0)
        {
            g.setColour (theme::cyan);
            g.drawRect (r, 2.0f);
            drawTag (g, "CC " + juce::String (cc), r, theme::cyan);
        }
        else
        {
            g.setColour (theme::textDim.withAlpha (0.6f));
            g.drawRect (r, 1.0f);
        }
    }
}

void LearnOverlay::mouseDown (const juce::MouseEvent& e)
{
    // The smallest outline under the mouse (a knob inside a bigger panel).
    const Target* hit = nullptr;
    const auto targets = findTargets();
    for (const auto& t : targets)
        if (t.area.contains (e.getPosition()) && (hit == nullptr || t.area.getWidth() * t.area.getHeight() < hit->area.getWidth() * hit->area.getHeight()))
            hit = &t;
    if (hit == nullptr)
        return;

    if (e.mods.isPopupMenu())
    {
        if (onForget)
            onForget (hit->paramId);
    }
    else if (onPick)
    {
        onPick (hit->paramId);
    }
    repaint();
}

// ---------------------------------------------------------------------------
MappingList::MappingList (MidiMap& m) : map (m)
{
    refresh();
}

void MappingList::refresh()
{
    rows = map.getMappings();
    scroll = juce::jlimit (0, juce::jmax (0, (int) rows.size() - visibleRows()), scroll);
    repaint();
}

int MappingList::visibleRows() const
{
    return juce::jmax (1, (getHeight() - headerHeight) / rowHeight);
}

juce::Rectangle<float> MappingList::cell (int visibleRow, Column column) const
{
    static constexpr float x[] = { 0.0f, 64.0f, 346.0f, 470.0f, 600.0f };
    static constexpr float w[] = { 60.0f, 278.0f, 120.0f, 120.0f, 30.0f };
    const float y = (float) (headerHeight + visibleRow * rowHeight);
    return { x[column], y, w[column], (float) rowHeight - 3.0f };
}

int MappingList::rowAt (juce::Point<float> p) const
{
    if (p.y < (float) headerHeight)
        return -1;
    const int row = scroll + (int) ((p.y - (float) headerHeight) / (float) rowHeight);
    return row < (int) rows.size() ? row : -1;
}

MappingList::Column MappingList::columnAt (juce::Point<float> p) const
{
    for (auto c : { ccColumn, nameColumn, minColumn, maxColumn, forgetColumn })
        if (p.x >= cell (0, c).getX() && p.x < cell (0, c).getRight())
            return c;
    return none;
}

void MappingList::paint (juce::Graphics& g)
{
    const auto header = [&] (Column c, const char* text)
    {
        PixelFont::drawIn (g, text, cell (0, c).withY (0.0f).withHeight ((float) headerHeight - 4.0f), 1.0f, theme::yellow,
                           juce::Justification::centredLeft);
    };
    header (ccColumn, "CC");
    header (nameColumn, "CONTROL");
    header (minColumn, "MIN");
    header (maxColumn, "MAX");

    if (rows.empty())
    {
        PixelFont::drawWrapped (g,
                                "NO KNOBS LINKED YET. PRESS LEARN, CLICK A KNOB ON ANY PAGE, THEN MOVE A KNOB OR FADER ON YOUR "
                                "MIDI KEYBOARD. OR PICK A FACTORY LAYOUT ON THE LEFT.",
                                getLocalBounds().toFloat().withTrimmedTop ((float) headerHeight + 6.0f).reduced (4.0f, 0.0f), 1.2f,
                                theme::textDim);
        return;
    }

    for (int i = 0; i < visibleRows(); ++i)
    {
        const int row = scroll + i;
        if (row >= (int) rows.size())
            break;
        const auto& m = rows[(size_t) row];
        auto* param = map.getParameter (m.paramId); // null for preset buttons

        const auto rowArea = cell (i, ccColumn).withRight ((float) getWidth());
        g.setColour (juce::Colour (0xff061650).withAlpha (i % 2 == 0 ? 0.9f : 0.5f));
        g.fillRect (rowArea);

        PixelFont::drawIn (g, juce::String (m.cc), cell (i, ccColumn).reduced (4.0f, 0.0f), 1.15f, theme::cyan,
                           juce::Justification::centredLeft);
        PixelFont::drawIn (g, map.parameterName (m.paramId).toUpperCase(), cell (i, nameColumn), 1.0f, theme::text,
                           juce::Justification::centredLeft);
        PixelFont::drawIn (g, "X", cell (i, forgetColumn), 1.3f, theme::red, juce::Justification::centred);
        if (param == nullptr)
        {
            PixelFont::drawIn (g, "BUTTON: PRESS TO STEP", cell (i, minColumn).withRight (cell (i, maxColumn).getRight()), 1.0f,
                               theme::textDim, juce::Justification::centred);
            continue;
        }
        for (auto c : { minColumn, maxColumn })
        {
            const auto box = cell (i, c).reduced (0.0f, 2.0f);
            const bool active = dragRow == row && dragColumn == c;
            g.setColour (theme::screen);
            g.fillRect (box);
            g.setColour (active ? theme::yellow : theme::screenEdge);
            g.drawRect (box, 1.0f);
            const float value = c == minColumn ? m.min : m.max;
            PixelFont::drawIn (g, param->getText (value, 12).toUpperCase(), box, 1.0f, active ? theme::yellow : theme::cyan,
                               juce::Justification::centred, false);
        }
    }

    if ((int) rows.size() > visibleRows())
        PixelFont::drawIn (g, juce::String (scroll + 1) + "-" + juce::String (juce::jmin ((int) rows.size(), scroll + visibleRows())) + " OF "
                                  + juce::String ((int) rows.size()) + "  (SCROLL FOR MORE)",
                           getLocalBounds().toFloat().removeFromBottom (12.0f), 1.0f, theme::textDim, juce::Justification::centredRight);
}

void MappingList::mouseDown (const juce::MouseEvent& e)
{
    const int row = rowAt (e.position);
    if (row < 0)
        return;
    const auto column = columnAt (e.position);
    const auto& m = rows[(size_t) row];
    if (column == forgetColumn)
    {
        map.forgetCc (m.cc);
        refresh();
        return;
    }
    if ((column == minColumn || column == maxColumn) && map.getParameter (m.paramId) != nullptr)
    {
        dragRow = row;
        dragColumn = column;
        dragStart = column == minColumn ? m.min : m.max;
        repaint();
    }
}

void MappingList::mouseDrag (const juce::MouseEvent& e)
{
    if (dragRow < 0 || dragRow >= (int) rows.size())
        return;
    auto& m = rows[(size_t) dragRow];
    const float pixelsForFull = e.mods.isShiftDown() ? 1200.0f : 200.0f;
    const float value = juce::jlimit (0.0f, 1.0f, dragStart - (float) e.getDistanceFromDragStartY() / pixelsForFull);
    (dragColumn == minColumn ? m.min : m.max) = value;
    map.setRange (m.cc, m.min, m.max);
    repaint();
}

void MappingList::mouseUp (const juce::MouseEvent&)
{
    dragRow = -1;
    dragColumn = none;
    repaint();
}

void MappingList::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click MIN or MAX: back to the control's full range.
    const int row = rowAt (e.position);
    const auto column = columnAt (e.position);
    if (row < 0 || (column != minColumn && column != maxColumn) || map.getParameter (rows[(size_t) row].paramId) == nullptr)
        return;
    auto& m = rows[(size_t) row];
    (column == minColumn ? m.min : m.max) = column == minColumn ? 0.0f : 1.0f;
    map.setRange (m.cc, m.min, m.max);
    repaint();
}

void MappingList::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    scroll = juce::jlimit (0, juce::jmax (0, (int) rows.size() - visibleRows()), scroll + (wheel.deltaY > 0 ? -1 : 1));
    repaint();
}
}
