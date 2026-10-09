#include "Widgets.h"

namespace genisys::ui
{
namespace
{
    const juce::Identifier helpId ("genisysHelp");

    void drawBevelBox (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour top, juce::Colour bottom)
    {
        g.setColour (theme::bevelDark);
        g.fillRect (r);
        r = r.reduced (2.0f);
        g.setColour (theme::bevelLight);
        g.fillRect (r);
        r = r.reduced (2.0f);
        g.setGradientFill (juce::ColourGradient (top, r.getX(), r.getY(), bottom, r.getX(), r.getBottom(), false));
        g.fillRect (r);
    }
}

void setHelp (juce::Component& c, const juce::String& title, const juce::String& text)
{
    c.getProperties().set (helpId, title + "|" + text);
}

juce::String getHelp (const juce::Component& c)
{
    for (auto* p = &c; p != nullptr; p = p->getParentComponent())
        if (p->getProperties().contains (helpId))
            return p->getProperties()[helpId].toString();
    return {};
}

// ---------------------------------------------------------------------------
MenuWindow::MenuWindow (juce::String t) : title (std::move (t)) {}

void MenuWindow::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    // Drop shadow, then the beveled window.
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (r.withTrimmedLeft (4).withTrimmedTop (4));
    drawBevelBox (g, r.withTrimmedRight (4).withTrimmedBottom (4), theme::menuTop, theme::menuBottom);
    if (title.isNotEmpty())
        PixelFont::draw (g, title, 12.0f, 10.0f, 1.5f, theme::yellow);
}

juce::Rectangle<int> MenuWindow::getContentArea() const
{
    return getLocalBounds().withTrimmedRight (4).withTrimmedBottom (4).reduced (10).withTrimmedTop (title.isNotEmpty() ? 18 : 0);
}

// ---------------------------------------------------------------------------
RetroKnob::RetroKnob (juce::RangedAudioParameter& p, juce::String l, bool isSmall)
    : param (p),
      attachment (p, [this] (float v) { value = v; repaint(); }),
      label (std::move (l)),
      small (isSmall)
{
    attachment.sendInitialUpdate();
    setSize (small ? smallWidth : normalWidth, small ? smallHeight : normalHeight);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

RetroKnob& RetroKnob::inverted (bool shouldInvert) { invert = shouldInvert; repaint(); return *this; }
RetroKnob& RetroKnob::withFormatter (Formatter f) { formatter = std::move (f); repaint(); return *this; }
RetroKnob& RetroKnob::withHelp (const juce::String& t, const juce::String& text) { setHelp (*this, t, text); return *this; }
void RetroKnob::setLabel (const juce::String& l) { if (l != label) { label = l; repaint(); } }

float RetroKnob::position() const
{
    const float norm = param.convertTo0to1 (value);
    return invert ? 1.0f - norm : norm;
}

void RetroKnob::setFromPosition (float pos, bool asGesture)
{
    pos = juce::jlimit (0.0f, 1.0f, pos);
    const float denorm = param.convertFrom0to1 (invert ? 1.0f - pos : pos);
    if (asGesture)
        attachment.setValueAsPartOfGesture (denorm);
    else
        attachment.setValueAsCompleteGesture (denorm);
}

void RetroKnob::mouseDown (const juce::MouseEvent&)
{
    dragStartPos = position();
    attachment.beginGesture();
}

void RetroKnob::mouseDrag (const juce::MouseEvent& e)
{
    const float pixelsForFullTurn = e.mods.isShiftDown() ? 1200.0f : 200.0f;
    setFromPosition (dragStartPos - (float) e.getDistanceFromDragStartY() / pixelsForFullTurn, true);
}

void RetroKnob::mouseUp (const juce::MouseEvent&) { attachment.endGesture(); }

void RetroKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
}

void RetroKnob::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const int steps = param.getNumSteps();
    const float step = (steps > 1 && steps < 1000) ? 1.0f / (float) (steps - 1) : 0.02f;
    setFromPosition (position() + (wheel.deltaY > 0 ? step : -step), false);
}

void RetroKnob::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float labelPx = small ? 1.0f : 1.3f;
    const bool hot = isMouseOverOrDragging();

    PixelFont::drawIn (g, label.toUpperCase(), bounds.withHeight (12.0f), labelPx,
                       hot ? theme::yellow : theme::text, juce::Justification::centredTop);

    const float diameter = small ? 40.0f : 52.0f;
    const float radius = diameter * 0.5f;
    const juce::Point<float> centre (bounds.getCentreX(), 14.0f + radius);
    const float t = position();

    // Ring of segments, lit up to the current value.
    constexpr int segments = 21;
    const float stroke = small ? 2.5f : 3.0f;
    for (int i = 0; i < segments; ++i)
    {
        const float frac = (float) i / (float) (segments - 1);
        const float angle = juce::degreesToRadians (-135.0f + frac * 270.0f);
        const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
        const auto a = centre + dir * (radius * 0.74f);
        const auto b = centre + dir * (radius * 0.95f);
        const bool lit = frac <= t + 1.0e-4f;
        if (lit)
        {
            g.setColour (theme::cyan.withAlpha (0.35f));
            g.drawLine ({ a, b }, stroke + 2.5f);
        }
        g.setColour (lit ? theme::cyan : theme::cyanDim);
        g.drawLine ({ a, b }, stroke);
    }

    // Knob body and pointer.
    const float bodyR = radius * 0.6f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4f63), centre.x - bodyR * 0.4f, centre.y - bodyR * 0.5f,
                                             juce::Colour (0xff0d0e14), centre.x + bodyR, centre.y + bodyR, true));
    g.fillEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
    g.setColour (juce::Colours::black);
    g.drawEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f, 2.0f);
    const float pointerAngle = juce::degreesToRadians (-135.0f + t * 270.0f);
    const auto tip = centre + juce::Point<float> (std::sin (pointerAngle), -std::cos (pointerAngle)) * (bodyR * 0.8f);
    g.setColour (theme::yellow);
    g.drawLine ({ centre, tip }, 3.0f);

    // Value readout.
    juce::String text = formatter ? formatter (value) : param.getText (param.convertTo0to1 (value), 12);
    text = text.toUpperCase();
    const float valuePx = 1.0f;
    const float boxW = juce::jmax (small ? 50.0f : 60.0f, PixelFont::textWidth (text, valuePx) + 8.0f);
    const juce::Rectangle<float> box (bounds.getCentreX() - boxW * 0.5f, centre.y + radius + 4.0f, boxW, 13.0f);
    g.setColour (theme::screen);
    g.fillRect (box);
    g.setColour (theme::screenEdge);
    g.drawRect (box, 1.0f);
    PixelFont::drawIn (g, text, box, valuePx, theme::cyan, juce::Justification::centred, false);
}

// ---------------------------------------------------------------------------
SegmentedChoice::SegmentedChoice (juce::RangedAudioParameter& p, juce::StringArray l)
    : param (p),
      attachment (p, [this] (float v) { selected = (int) std::lround (v - param.convertFrom0to1 (0.0f)); repaint(); }),
      labels (std::move (l))
{
    attachment.sendInitialUpdate();
}

void SegmentedChoice::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::bevelDark);
    g.fillRect (r);
    r = r.reduced (2.0f);
    const float w = r.getWidth() / (float) labels.size();
    for (int i = 0; i < labels.size(); ++i)
    {
        const auto seg = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w - (i < labels.size() - 1 ? 2.0f : 0.0f), r.getHeight());
        const bool on = i == selected;
        g.setColour (on ? theme::yellow : juce::Colour (0xff061650));
        g.fillRect (seg);
        PixelFont::drawIn (g, labels[i].toUpperCase(), seg, 1.0f, on ? juce::Colour (0xff111111) : theme::textDim,
                           juce::Justification::centred, false);
    }
}

void SegmentedChoice::mouseDown (const juce::MouseEvent& e)
{
    const int index = juce::jlimit (0, labels.size() - 1, (int) ((float) e.x / (float) getWidth() * (float) labels.size()));
    attachment.setValueAsCompleteGesture (param.convertFrom0to1 (0.0f) + (float) index);
}

// ---------------------------------------------------------------------------
LedToggle::LedToggle (juce::RangedAudioParameter& p, juce::String l)
    : param (&p), label (std::move (l))
{
    attachment = std::make_unique<juce::ParameterAttachment> (p, [this] (float v) { on = v >= 0.5f; repaint(); });
    attachment->sendInitialUpdate();
}

LedToggle::LedToggle (juce::String l) : label (std::move (l)) {}

void LedToggle::setOn (bool shouldBeOn)
{
    on = shouldBeOn;
    if (attachment != nullptr)
        attachment->setValueAsCompleteGesture (on ? 1.0f : 0.0f);
    repaint();
}

void LedToggle::mouseDown (const juce::MouseEvent&)
{
    setOn (! on);
    if (onChange)
        onChange (on);
}

void LedToggle::paint (juce::Graphics& g)
{
    const float h = (float) getHeight();
    const float d = 11.0f;
    const juce::Rectangle<float> led (1.0f, h * 0.5f - d * 0.5f, d, d);
    if (on)
    {
        g.setColour (theme::red.withAlpha (0.35f));
        g.fillEllipse (led.expanded (3.0f));
    }
    g.setColour (on ? theme::red : juce::Colour (0xff2a0000));
    g.fillEllipse (led);
    g.setColour (juce::Colours::black);
    g.drawEllipse (led, 1.0f);
    PixelFont::drawIn (g, label.toUpperCase(), getLocalBounds().toFloat().withTrimmedLeft (d + 7.0f), 1.15f,
                       isMouseOver() ? theme::yellow : theme::text, juce::Justification::centredLeft);
}

// ---------------------------------------------------------------------------
PixelButton::PixelButton (juce::String t) : text (std::move (t)) {}

void PixelButton::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    if (pressed)
        r = r.translated (0.0f, 1.0f);
    g.setColour (theme::bevelDark);
    g.fillRect (r);
    g.setColour (isMouseOver() ? theme::yellow : theme::bevelLight);
    g.fillRect (r.reduced (1.0f));
    g.setColour (juce::Colour (0xff061650));
    g.fillRect (r.reduced (3.0f));
    PixelFont::drawIn (g, text.toUpperCase(), r, 1.1f, isMouseOver() ? theme::yellow : theme::text,
                       juce::Justification::centred);
}

void PixelButton::mouseDown (const juce::MouseEvent&) { pressed = true; repaint(); }

void PixelButton::mouseUp (const juce::MouseEvent& e)
{
    pressed = false;
    repaint();
    if (getLocalBounds().contains (e.getPosition()) && onClick)
        onClick();
}

// ---------------------------------------------------------------------------
HelpBox::HelpBox()
{
    show ("WELCOME!|Hover over anything to learn what it does. Drag knobs up and down, Shift for fine control, double-click to reset.");
}

void HelpBox::show (const juce::String& help)
{
    const auto parts = juce::StringArray::fromTokens (help, "|", "");
    const auto newTitle = parts[0].toUpperCase();
    const auto newBody = parts.size() > 1 ? parts[1].toUpperCase() : juce::String();
    if (newTitle == title && newBody == body)
        return;
    title = newTitle;
    body = newBody;
    shown = 0;
    startTimerHz (60);
}

void HelpBox::timerCallback()
{
    shown += 4; // typewriter, like game dialog
    blink = (blink + 1) % 60;
    if (shown >= body.length() && blink % 30 == 0)
        repaint();
    else if (shown <= body.length() + 4)
        repaint();
}

void HelpBox::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black);
    g.fillRect (r);
    g.setColour (juce::Colours::white);
    g.drawRect (r.reduced (2.0f), 3.0f);
    auto inner = r.reduced (12.0f, 9.0f);
    PixelFont::draw (g, title, inner.getX(), inner.getY(), 1.5f, theme::yellow);
    inner.removeFromTop (16.0f);
    PixelFont::drawWrapped (g, body.substring (0, shown), inner, 1.3f, theme::text);
    if (shown >= body.length() && blink < 30)
        PixelFont::draw (g, juce::String::charToString (0x25BC), r.getRight() - 18.0f, r.getBottom() - 14.0f, 1.0f, theme::yellow);
}

// ---------------------------------------------------------------------------
TabBar::TabBar (juce::StringArray n) : names (std::move (n))
{
    startTimerHz (2);
}

juce::Rectangle<float> TabBar::tabBounds (int index) const
{
    float x = 0.0f;
    for (int i = 0; i < names.size(); ++i)
    {
        const float w = PixelFont::textWidth (names[i], 1.5f) + 36.0f;
        if (i == index)
            return { x, 0.0f, w, (float) getHeight() };
        x += w + 4.0f;
    }
    return {};
}

void TabBar::timerCallback()
{
    cursorVisible = ! cursorVisible;
    repaint (tabBounds (selected).toNearestInt());
}

void TabBar::paint (juce::Graphics& g)
{
    for (int i = 0; i < names.size(); ++i)
    {
        const auto r = tabBounds (i);
        const bool on = i == selected;
        if (on)
        {
            g.setColour (theme::bevelLight);
            g.fillRect (r);
            g.setGradientFill (juce::ColourGradient (theme::menuTop, r.getX(), r.getY(), juce::Colour (0xff0e2a8a),
                                                     r.getX(), r.getBottom(), false));
            g.fillRect (r.reduced (2.0f, 0.0f).withTrimmedTop (2.0f));
            if (cursorVisible)
                PixelFont::draw (g, juce::String::charToString (0x25B6), r.getX() + 7.0f, r.getCentreY() - 5.0f, 1.3f, theme::yellow);
        }
        PixelFont::drawIn (g, names[i], r.withTrimmedLeft (22.0f), 1.5f, on ? theme::text : theme::textDim,
                           juce::Justification::centredLeft, on);
    }
}

void TabBar::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < names.size(); ++i)
    {
        if (tabBounds (i).contains (e.position))
        {
            selected = i;
            if (helpTexts.size() > i)
                setHelp (*this, names[i], helpTexts[i]);
            repaint();
            if (onChange)
                onChange (i);
            return;
        }
    }
}
}
