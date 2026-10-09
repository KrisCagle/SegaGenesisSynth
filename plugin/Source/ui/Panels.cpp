#include "Panels.h"

#include "genisys_engine.h"
#include "genisys_presets.h"
#include "ym2612.h"

namespace genisys::ui
{
void sendMidi (GenisysProcessor& processor, const juce::MidiMessage& message)
{
    auto m = message;
    m.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    processor.editorMidi.addMessageToQueue (m);
}

// ---------------------------------------------------------------------------
void Logo::paint (juce::Graphics& g)
{
    const float px = 4.0f;
    const auto path = PixelFont::textPath ("GENISYS", px);
    const float h = 7.0f * px;
    g.setColour (juce::Colours::black);
    g.fillPath (path, juce::AffineTransform::translation (3.0f, 3.0f));
    juce::ColourGradient chrome (juce::Colours::white, 0.0f, 0.0f, juce::Colour (0xff9cc9ff), 0.0f, h, false);
    chrome.addColour (0.45, juce::Colour (0xffcfe6ff));
    chrome.addColour (0.55, juce::Colour (0xff3a7bd5));
    g.setGradientFill (chrome);
    g.fillPath (path);

    const juce::Rectangle<float> stripe (0.0f, h + 7.0f, PixelFont::textWidth ("16-BIT FM SYNTHESIZER", 1.0f) + 10.0f, 13.0f);
    g.setColour (juce::Colours::black);
    g.fillRect (stripe.translated (2.0f, 2.0f));
    g.setColour (theme::red);
    g.fillRect (stripe);
    PixelFont::drawIn (g, "16-BIT FM SYNTHESIZER", stripe, 1.0f, juce::Colours::white, juce::Justification::centred, false);
}

// ---------------------------------------------------------------------------
PresetCartridge::PresetCartridge (GenisysProcessor& p) : processor (p)
{
    setHelp (*this, "PRESET CARTRIDGE", "Choose a sound. Click the arrows to step through presets, or click the label for the full list by category.");
    refresh();
}

juce::Rectangle<float> PresetCartridge::leftArrow() const { return { 6.0f, 0.0f, 26.0f, (float) getHeight() }; }
juce::Rectangle<float> PresetCartridge::rightArrow() const { return { (float) getWidth() - 32.0f, 0.0f, 26.0f, (float) getHeight() }; }
juce::Rectangle<float> PresetCartridge::label() const { return getLocalBounds().toFloat().reduced (36.0f, 7.0f); }

void PresetCartridge::refresh()
{
    const int program = processor.getCurrentProgram();
    if (program == shownProgram)
        return;
    shownProgram = program;
    category = juce::String (genisys_preset_category (program)).toUpperCase();
    name = processor.getProgramName (program).toUpperCase();
    repaint();
}

void PresetCartridge::step (int delta)
{
    const int n = processor.getNumPrograms();
    processor.setCurrentProgram ((processor.getCurrentProgram() + delta + n) % n);
    refresh();
}

void PresetCartridge::mouseDown (const juce::MouseEvent& e)
{
    if (leftArrow().contains (e.position)) { step (-1); return; }
    if (rightArrow().contains (e.position)) { step (1); return; }

    juce::PopupMenu menu;
    juce::String currentCategory;
    juce::PopupMenu sub;
    const auto flush = [&]
    {
        if (currentCategory.isNotEmpty())
            menu.addSubMenu (currentCategory, sub);
        sub = juce::PopupMenu();
    };
    for (int i = 0; i < processor.getNumPrograms(); ++i)
    {
        const juce::String cat (genisys_preset_category (i));
        if (cat != currentCategory)
        {
            flush();
            currentCategory = cat;
        }
        sub.addItem (i + 1, processor.getProgramName (i), true, i == processor.getCurrentProgram());
    }
    flush();
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [this] (int result)
                        {
                            if (result > 0)
                            {
                                processor.setCurrentProgram (result - 1);
                                refresh();
                            }
                        });
}

void PresetCartridge::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    // Cartridge shell.
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b2b33), 0.0f, 0.0f, juce::Colour (0xff17171d), 0.0f, r.getHeight(), false));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (juce::Colour (0xff3a3a46));
    g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, 2.0f);

    // Label.
    const auto l = label();
    g.setColour (theme::bevelDark);
    g.fillRect (l.expanded (2.0f));
    g.setColour (theme::bevelLight);
    g.fillRect (l);
    g.setGradientFill (juce::ColourGradient (theme::menuTop, l.getX(), l.getY(), theme::menuBottom, l.getX(), l.getBottom(), false));
    g.fillRect (l.reduced (2.0f));
    PixelFont::drawIn (g, category, l.withHeight (l.getHeight() * 0.45f).withTrimmedTop (5.0f), 1.0f, theme::yellow,
                       juce::Justification::centred, false);
    PixelFont::drawIn (g, name, l.withTrimmedTop (l.getHeight() * 0.42f), 2.0f, theme::text, juce::Justification::centred);

    const auto mouse = getMouseXYRelative().toFloat();
    PixelFont::drawIn (g, juce::String::charToString (0x25C0), leftArrow(), 2.0f,
                       isMouseOver() && leftArrow().contains (mouse) ? juce::Colours::white : theme::yellow, juce::Justification::centred);
    PixelFont::drawIn (g, juce::String::charToString (0x25B6), rightArrow(), 2.0f,
                       isMouseOver() && rightArrow().contains (mouse) ? juce::Colours::white : theme::yellow, juce::Justification::centred);
}

// ---------------------------------------------------------------------------
PadButton::PadButton (juce::String l, juce::String c) : letter (std::move (l)), caption (std::move (c)) {}

void PadButton::paint (juce::Graphics& g)
{
    const float d = 30.0f;
    auto circle = juce::Rectangle<float> ((float) getWidth() * 0.5f - d * 0.5f, 1.0f, d, d);
    if (pressed)
        circle = circle.translated (0.0f, 2.0f);
    else
    {
        g.setColour (juce::Colours::black);
        g.fillEllipse (circle.translated (0.0f, 3.0f));
    }
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4a55), circle.getX() + d * 0.35f, circle.getY() + d * 0.3f,
                                             juce::Colour (0xff111111), circle.getRight(), circle.getBottom(), true));
    g.fillEllipse (circle);
    g.setColour (juce::Colours::black);
    g.drawEllipse (circle, 2.0f);
    PixelFont::drawIn (g, letter, circle, 1.3f, isMouseOver() ? theme::yellow : juce::Colour (0xffdddddd), juce::Justification::centred, false);
    PixelFont::drawIn (g, caption.toUpperCase(), getLocalBounds().toFloat().withTrimmedTop (d + 7.0f), 1.0f, theme::textDim,
                       juce::Justification::centredTop, false);
}

void PadButton::mouseDown (const juce::MouseEvent&) { pressed = true; repaint(); }

void PadButton::mouseUp (const juce::MouseEvent& e)
{
    pressed = false;
    repaint();
    if (getLocalBounds().contains (e.getPosition()) && onClick)
        onClick();
}

// ---------------------------------------------------------------------------
namespace
{
    // Column (depth) of each operator in a diagram, and the connections.
    constexpr int kColumns[8][4] = { { 0, 1, 2, 3 }, { 0, 0, 1, 2 }, { 0, 0, 1, 2 }, { 0, 1, 0, 2 },
                                     { 0, 1, 0, 1 }, { 0, 1, 1, 1 }, { 0, 1, 1, 1 }, { 0, 0, 0, 0 } };
    const std::vector<std::pair<int, int>> kEdges[8] = {
        { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 } }, { { 0, 2 }, { 1, 2 }, { 2, 3 }, { 3, 4 } },
        { { 0, 3 }, { 1, 2 }, { 2, 3 }, { 3, 4 } }, { { 0, 1 }, { 1, 3 }, { 2, 3 }, { 3, 4 } },
        { { 0, 1 }, { 1, 4 }, { 2, 3 }, { 3, 4 } }, { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 4 }, { 2, 4 }, { 3, 4 } },
        { { 0, 1 }, { 1, 4 }, { 2, 4 }, { 3, 4 } }, { { 0, 4 }, { 1, 4 }, { 2, 4 }, { 3, 4 } },
    };
}

AlgorithmPicker::AlgorithmPicker (juce::RangedAudioParameter& p)
    : param (p), attachment (p, [this] (float v) { selected = juce::jlimit (0, 7, (int) std::lround (v)); repaint(); })
{
    attachment.sendInitialUpdate();
}

juce::String AlgorithmPicker::describe (int a)
{
    static const char* text[8] = {
        "4 in a chain: one sound, the richest tone. Good for bass and leads.",
        "2 modulators merge, then chain: a thick, complex tone.",
        "Two paths into OP4: bright and full.",
        "Two paths into OP4, one short: punchy.",
        "Two pairs: two separate sounds layered. E-pianos, brass.",
        "One modulator drives three: bells and organ-like stacks.",
        "One pair plus two sines: soft, airy layers.",
        "4 sine waves added together: organ drawbars.",
    };
    return text[juce::jlimit (0, 7, a)];
}

juce::Rectangle<float> AlgorithmPicker::cell (int index) const
{
    const float gap = 6.0f;
    const float w = ((float) getWidth() - gap) / 2.0f;
    const float h = 58.0f;
    return { (float) (index % 2) * (w + gap), (float) (index / 2) * (h + gap), w, h };
}

void AlgorithmPicker::drawDiagram (juce::Graphics& g, juce::Rectangle<float> r, int a, bool on) const
{
    const uint8_t carriers = genisys_carrier_mask (a);
    int maxColumn = 0;
    for (int c : kColumns[a]) maxColumn = juce::jmax (maxColumn, c);

    std::array<juce::Point<float>, 5> pos;
    for (int op = 0; op < 4; ++op)
    {
        int count = 0, index = 0;
        for (int o = 0; o < 4; ++o)
            if (kColumns[a][o] == kColumns[a][op]) { if (o < op) ++index; ++count; }
        const float x = r.getX() + 11.0f + (maxColumn > 0 ? (float) kColumns[a][op] * (r.getWidth() - 46.0f) / (float) maxColumn : 0.0f);
        const float y = r.getCentreY() - 6.0f + ((float) index - (float) (count - 1) * 0.5f) * 13.0f;
        pos[(size_t) op] = { x, y };
    }
    pos[4] = { r.getRight() - 14.0f, r.getCentreY() - 6.0f };

    g.setColour (theme::cyan);
    for (const auto& [from, to] : kEdges[a])
        g.drawLine (pos[(size_t) from].x + 6.0f, pos[(size_t) from].y, pos[(size_t) to].x - 6.0f, pos[(size_t) to].y, 1.5f);

    for (int op = 0; op < 4; ++op)
    {
        const bool car = (carriers >> op) & 1;
        const auto box = juce::Rectangle<float> (12.0f, 11.0f).withCentre (pos[(size_t) op]);
        g.setColour (car ? theme::yellow : theme::cyanDim);
        g.fillRect (box);
        g.setColour (juce::Colours::white);
        g.drawRect (box, 1.0f);
        PixelFont::drawIn (g, juce::String (op + 1), box, 1.0f, car ? juce::Colour (0xff111111) : juce::Colours::white,
                           juce::Justification::centred, false);
    }
    const auto out = juce::Rectangle<float> (18.0f, 10.0f).withCentre (pos[4]);
    g.setColour (theme::green);
    g.fillRect (out);
    PixelFont::drawIn (g, "OUT", out, 0.85f, juce::Colour (0xff111111), juce::Justification::centred, false);
    PixelFont::drawIn (g, "ALG " + juce::String (a), r.withTrimmedTop (r.getHeight() - 12.0f), 1.0f,
                       on ? theme::yellow : theme::textDim, juce::Justification::centred, false);
}

void AlgorithmPicker::paint (juce::Graphics& g)
{
    for (int a = 0; a < 8; ++a)
    {
        const auto r = cell (a);
        const bool on = a == selected;
        if (on)
        {
            g.setColour (theme::yellow.withAlpha (0.35f));
            g.fillRect (r.expanded (2.0f));
        }
        g.setColour (theme::screen);
        g.fillRect (r);
        g.setColour (on ? theme::yellow : theme::screenEdge);
        g.drawRect (r, 2.0f);
        drawDiagram (g, r, a, on);
    }
    const float textTop = cell (7).getBottom() + 10.0f;
    PixelFont::drawWrapped (g, describe (selected).toUpperCase(),
                            juce::Rectangle<float> (0.0f, textTop, (float) getWidth(), (float) getHeight() - textTop), 1.2f, theme::text);
}

void AlgorithmPicker::mouseMove (const juce::MouseEvent& e)
{
    for (int a = 0; a < 8; ++a)
        if (cell (a).contains (e.position))
        {
            setHelp (*this, "ALGORITHM " + juce::String (a), describe (a) + " Yellow boxes are the operators you hear.");
            return;
        }
}

void AlgorithmPicker::mouseDown (const juce::MouseEvent& e)
{
    for (int a = 0; a < 8; ++a)
        if (cell (a).contains (e.position))
            attachment.setValueAsCompleteGesture ((float) a);
}

// ---------------------------------------------------------------------------
void EnvelopeGraph::setParams (int ar, int d1r, int d2r, int sl, int rr, int ks)
{
    const std::array<int, 6> p { ar, d1r, d2r, sl, rr, ks };
    if (p == params)
        return;
    params = p;

    // Run the chip's own envelope generator through a key-on, hold, key-off.
    Ym2612Operator op;
    ym2612_operator_init (&op);
    ym2612_operator_set_dt_mul (&op, 0x01);
    ym2612_operator_set_tl (&op, 0);
    ym2612_operator_set_ar_ksr (&op, (uint8_t) (((ks & 3) << 6) | (ar & 31)));
    ym2612_operator_set_d1r (&op, (uint8_t) (d1r & 31));
    ym2612_operator_set_d2r (&op, (uint8_t) (d2r & 31));
    ym2612_operator_set_sl_rr (&op, (uint8_t) (((sl & 15) << 4) | (rr & 15)));
    ym2612_operator_set_freq (&op, 700, 4);
    ym2612_operator_key_on (&op);
    for (size_t i = 0; i < curve.size(); ++i)
    {
        if (i == 70)
            ym2612_operator_key_off (&op);
        for (int k = 0; k < 300; ++k)
            ym2612_operator_clock (&op);
        const int v = juce::jlimit (0, 1023, (int) op.vol_out);
        curve[i] = 1.0f - (float) v / 1023.0f;
    }
    repaint();
}

void EnvelopeGraph::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (theme::screen);
    g.fillRect (r);
    g.setColour (theme::screenEdge);
    g.drawRect (r, 1.0f);
    const auto inner = r.reduced (3.0f);
    juce::Path line, fill;
    for (size_t i = 0; i < curve.size(); ++i)
    {
        const float x = inner.getX() + inner.getWidth() * (float) i / (float) (curve.size() - 1);
        const float y = inner.getBottom() - inner.getHeight() * curve[i];
        if (i == 0) { line.startNewSubPath (x, y); fill.startNewSubPath (x, inner.getBottom()); }
        else line.lineTo (x, y);
        fill.lineTo (x, y);
    }
    fill.lineTo (inner.getRight(), inner.getBottom());
    fill.closeSubPath();
    g.setColour (theme::cyan.withAlpha (0.15f));
    g.fillPath (fill);
    g.setColour (theme::cyan);
    g.strokePath (line, juce::PathStrokeType (1.5f));
    // Key-off marker.
    const float keyOffX = inner.getX() + inner.getWidth() * 70.0f / 99.0f;
    g.setColour (theme::textDim.withAlpha (0.5f));
    g.drawVerticalLine ((int) keyOffX, inner.getY(), inner.getBottom());
}

// ---------------------------------------------------------------------------
namespace
{
    juce::String opParam (int op, const char* field) { return "op" + juce::String (op + 1) + "_" + field; }

    // Rates count up as they get faster; show "time" so turning up = longer.
    RetroKnob::Formatter countdown (int max) { return [max] (float v) { return juce::String (max - (int) std::lround (v)); }; }
}

OperatorCard::OperatorCard (juce::AudioProcessorValueTreeState& s, int index)
    : MenuWindow ("OP" + juce::String (index + 1)), state (s), op (index)
{
    const auto knob = [this] (const char* field, const char* label) -> RetroKnob*
    {
        auto k = std::make_unique<RetroKnob> (*state.getParameter (opParam (op, field)), label, true);
        auto* raw = k.get();
        addAndMakeVisible (*raw);
        owned.push_back (std::move (k));
        return raw;
    };
    const auto toggle = [this] (const char* field, const char* label) -> LedToggle*
    {
        auto t = std::make_unique<LedToggle> (*state.getParameter (opParam (op, field)), label);
        auto* raw = t.get();
        addAndMakeVisible (*raw);
        owned.push_back (std::move (t));
        return raw;
    };

    ratio = knob ("mul", "RATIO");
    ratio->withFormatter ([] (float v) { const int m = (int) std::lround (v); return m == 0 ? juce::String ("X0.5") : "X" + juce::String (m); })
        .withHelp ("RATIO", "This operator's pitch as a multiple of the note. Whole numbers sound musical; high ones sound like bells.");
    level = knob ("tl", "LEVEL");
    level->inverted().withFormatter (countdown (127));
    detune = knob ("dt", "DETUNE");
    detune->withHelp ("DETUNE", "A tiny pitch offset. Detuned operators beat against each other for a chorus-like shimmer.");
    attack = knob ("ar", "ATTACK");
    attack->inverted().withFormatter (countdown (31)).withHelp ("ATTACK", "How long the operator takes to rise. 0 = instant.");
    decay = knob ("d1r", "DECAY");
    decay->inverted().withFormatter (countdown (31)).withHelp ("DECAY", "How long it takes to fall from the peak to the sustain level.");
    sustain = knob ("sl", "SUSTAIN");
    sustain->inverted().withFormatter (countdown (15)).withHelp ("SUSTAIN", "The level held while the key is down. 15 = no drop.");
    release = knob ("rr", "RELEASE");
    release->inverted().withFormatter (countdown (15)).withHelp ("RELEASE", "How long it rings after you let go.");
    fade = knob ("d2r", "FADE");
    fade->withHelp ("FADE", "A slow fade while the key is held (the chip's second decay). 0 = holds steady.");
    keyScale = knob ("ks", "KEY SCL");
    keyScale->withHelp ("KEY SCALE", "Higher notes get faster envelopes, like real instruments where high notes ring shorter.");
    shape = knob ("ssg_mode", "SHAPE");
    shape->withHelp ("SSG SHAPE", "Which looping shape the SSG envelope uses (when SSG is on).");
    tremolo = toggle ("am", "TREM");
    setHelp (*tremolo, "TREMOLO", "Lets the chip LFO pulse this operator's volume (set the depth on the MOD tab).");
    ssg = toggle ("ssg_enable", "SSG");
    setHelp (*ssg, "SSG-EG", "The chip's looping envelope: wobbling, evolving and gated sounds.");

    addAndMakeVisible (envelope);
    setHelp (envelope, "ENVELOPE", "The shape of this operator over time: attack, decay, sustain, then release after the line.");
    setCarrier (false);
    refreshEnvelope();
}

void OperatorCard::setCarrier (bool isCarrier)
{
    carrier = isCarrier;
    level->setLabel (carrier ? "VOLUME" : "BRIGHT");
    level->withHelp (carrier ? "VOLUME" : "BRIGHTNESS",
                     carrier ? "This operator is a carrier: you hear it directly, so this is its volume."
                             : "This operator is a modulator: you don't hear it, it colours another operator. More = brighter and buzzier.");
    repaint();
}

void OperatorCard::refreshEnvelope()
{
    const auto get = [this] (const char* field) { return (int) std::lround (state.getRawParameterValue (opParam (op, field))->load()); };
    envelope.setParams (get ("ar"), get ("d1r"), get ("d2r"), get ("sl"), get ("rr"), get ("ks"));
}

void OperatorCard::paint (juce::Graphics& g)
{
    MenuWindow::paint (g);
    const juce::String badge = carrier ? "YOU HEAR IT" : "SHAPES TONE";
    const float w = PixelFont::textWidth (badge, 1.0f) + 8.0f;
    const juce::Rectangle<float> r ((float) getWidth() - w - 14.0f, 9.0f, w, 12.0f);
    g.setColour (carrier ? theme::yellow : juce::Colours::transparentBlack);
    g.fillRect (r);
    g.setColour (carrier ? juce::Colours::white : theme::cyan);
    g.drawRect (r, 1.0f);
    PixelFont::drawIn (g, badge, r, 1.0f, carrier ? juce::Colour (0xff111111) : theme::cyan, juce::Justification::centred, false);
}

void OperatorCard::resized()
{
    const auto area = getContentArea();
    const auto row = [&area] (std::initializer_list<juce::Component*> items, int y, int itemWidth)
    {
        const int total = (int) items.size() * itemWidth;
        int x = area.getX() + (area.getWidth() - total) / 2;
        for (auto* c : items)
        {
            c->setTopLeftPosition (x, area.getY() + y);
            x += itemWidth;
        }
    };
    row ({ ratio, level, detune }, 0, RetroKnob::smallWidth);
    envelope.setBounds (area.getX(), area.getY() + 82, area.getWidth(), 44);
    row ({ attack, decay, sustain, release }, 132, RetroKnob::smallWidth);

    const int y3 = area.getY() + 216;
    int x = area.getX();
    for (auto* k : { fade, keyScale, shape })
    {
        k->setTopLeftPosition (x, y3);
        x += RetroKnob::smallWidth;
    }
    tremolo->setBounds (x + 2, y3 + 16, area.getRight() - x - 2, 16);
    ssg->setBounds (x + 2, y3 + 42, area.getRight() - x - 2, 16);
}

// ---------------------------------------------------------------------------
namespace
{
    struct Pad { const char* name; int note; bool sample; };
    constexpr Pad kPads[12] = {
        { "KICK", 36, true }, { "SNARE", 38, true }, { "CLAP", 39, true }, { "RIM", 37, true }, { "SNARE 2", 40, true },
        { "CL HAT", 42, false }, { "OPEN HAT", 46, false }, { "PEDAL HAT", 44, false }, { "CRASH", 49, false },
        { "RIDE", 51, false }, { "LO TOM", 41, true }, { "HI TOM", 50, true },
    };
    juce::String noteName (int note) { return juce::MidiMessage::getMidiNoteName (note, true, true, 4); }
}

DrumPads::DrumPads (GenisysProcessor& p) : processor (p) {}

juce::Rectangle<float> DrumPads::padBounds (int index) const
{
    const float gap = 8.0f;
    const float w = ((float) getWidth() - 5.0f * gap) / 6.0f;
    const float h = ((float) getHeight() - gap) / 2.0f;
    return { (float) (index % 6) * (w + gap), (float) (index / 6) * (h + gap), w, h };
}

int DrumPads::padAt (juce::Point<float> p) const
{
    for (int i = 0; i < 12; ++i)
        if (padBounds (i).contains (p))
            return i;
    return -1;
}

void DrumPads::paint (juce::Graphics& g)
{
    for (int i = 0; i < 12; ++i)
    {
        auto r = padBounds (i);
        const bool hit = i == down;
        g.setColour (juce::Colours::black);
        g.fillRect (r.translated (0.0f, 3.0f));
        if (hit)
            g.setGradientFill (juce::ColourGradient (theme::yellow, 0.0f, r.getY(), juce::Colour (0xffc79a00), 0.0f, r.getBottom(), false));
        else
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a2a35), 0.0f, r.getY(), juce::Colour (0xff121218), 0.0f, r.getBottom(), false));
        g.fillRect (r);
        g.setColour (juce::Colours::black);
        g.drawRect (r, 2.0f);
        const auto ink = hit ? juce::Colour (0xff111111) : (kPads[i].sample ? theme::yellow : theme::cyan);
        PixelFont::drawIn (g, kPads[i].name, r.reduced (7.0f), 1.15f, ink, juce::Justification::topLeft, ! hit);
        PixelFont::drawIn (g, noteName (kPads[i].note), r.reduced (7.0f), 1.0f, hit ? ink : theme::textDim,
                           juce::Justification::bottomLeft, false);
    }
}

void DrumPads::mouseMove (const juce::MouseEvent& e)
{
    const int i = padAt (e.position);
    if (i >= 0)
        setHelp (*this, kPads[i].name, juce::String (kPads[i].sample ? "An 8-bit sample played through the FM chip's DAC" : "The PSG noise channel")
                                           + ". MIDI note " + noteName (kPads[i].note) + " on channel 10.");
}

void DrumPads::mouseDown (const juce::MouseEvent& e)
{
    down = padAt (e.position);
    if (down < 0)
        return;
    // The kit only plays while Drums On is set: switch it on for the user.
    if (auto* drumsOn = processor.state.getParameter ("drums_on"))
        if (drumsOn->getValue() < 0.5f)
            drumsOn->setValueNotifyingHost (1.0f);
    sendMidi (processor, juce::MidiMessage::noteOn (10, kPads[down].note, (juce::uint8) 110));
    repaint();
}

void DrumPads::mouseUp (const juce::MouseEvent&)
{
    if (down >= 0)
        sendMidi (processor, juce::MidiMessage::noteOff (10, kPads[down].note));
    down = -1;
    repaint();
}

// ---------------------------------------------------------------------------
ScopeScreen::ScopeScreen (GenisysProcessor& p) : processor (p)
{
    setHelp (*this, "SCOPE", "The sound's waveform, live. The six lights show which of the chip's 6 FM channels are playing.");
    startTimerHz (30);
}

void ScopeScreen::timerCallback()
{
    float fresh[2048];
    const int n = processor.readScope (fresh, 2048);
    for (int i = 0; i < n; ++i)
    {
        history[(size_t) writePos] = fresh[i];
        writePos = (writePos + 1) % (int) history.size();
    }
    voices = processor.getActiveVoices();
    repaint();
}

void ScopeScreen::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff031008));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (juce::Colour (0xff333333));
    g.drawRoundedRectangle (r.reduced (1.5f), 6.0f, 3.0f);
    PixelFont::draw (g, "OUTPUT", 10.0f, 9.0f, 1.0f, theme::green, false);

    // Start the trace at a rising zero crossing so it stands still.
    const int size = (int) history.size();
    const int shownSamples = 256;
    int start = (writePos - shownSamples - 128 + size) % size;
    for (int k = 0; k < 128; ++k)
    {
        const int i = (start + k) % size, j = (i + 1) % size;
        if (history[(size_t) i] <= 0.0f && history[(size_t) j] > 0.0f) { start = i; break; }
    }
    const auto plot = r.reduced (8.0f, 26.0f);
    juce::Path trace;
    for (int k = 0; k < shownSamples; ++k)
    {
        const float v = juce::jlimit (-1.0f, 1.0f, history[(size_t) ((start + k) % size)] * 2.5f);
        const float x = plot.getX() + plot.getWidth() * (float) k / (float) (shownSamples - 1);
        const float y = plot.getCentreY() - v * plot.getHeight() * 0.5f;
        if (k == 0) trace.startNewSubPath (x, y); else trace.lineTo (x, y);
    }
    g.setColour (theme::green.withAlpha (0.25f));
    g.strokePath (trace, juce::PathStrokeType (5.0f));
    g.setColour (theme::green);
    g.strokePath (trace, juce::PathStrokeType (1.6f));

    for (int v = 0; v < 6; ++v)
    {
        const juce::Rectangle<float> led (10.0f + (float) v * 24.0f, r.getBottom() - 18.0f, 20.0f, 9.0f);
        const bool on = (voices >> v) & 1;
        g.setColour (on ? theme::green : juce::Colour (0xff0b2a14));
        g.fillRect (led);
        g.setColour (juce::Colour (0xff1b5a2e));
        g.drawRect (led, 1.0f);
    }
}

// ---------------------------------------------------------------------------
Wheels::Wheels (GenisysProcessor& p) : processor (p)
{
    setHelp (*this, "BEND / MOD", "Drag BEND to bend pitch (it springs back). Drag MOD up for vibrato.");
}

juce::Rectangle<float> Wheels::slot (int which) const
{
    const float w = (float) getWidth() * 0.5f;
    return { (float) which * w + 4.0f, 4.0f, w - 8.0f, (float) getHeight() - 22.0f };
}

void Wheels::send (int which, float value)
{
    if (which == 0)
    {
        bend = value;
        sendMidi (processor, juce::MidiMessage::pitchWheel (1, juce::jlimit (0, 16383, (int) std::lround (value * 16383.0f))));
    }
    else
    {
        mod = value;
        sendMidi (processor, juce::MidiMessage::controllerEvent (1, 1, juce::jlimit (0, 127, (int) std::lround (value * 127.0f))));
    }
    repaint();
}

void Wheels::mouseDown (const juce::MouseEvent& e)
{
    dragging = e.position.x < (float) getWidth() * 0.5f ? 0 : 1;
    mouseDrag (e);
}

void Wheels::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;
    const auto s = slot (dragging);
    send (dragging, juce::jlimit (0.0f, 1.0f, 1.0f - (e.position.y - s.getY()) / s.getHeight()));
}

void Wheels::mouseUp (const juce::MouseEvent&)
{
    if (dragging == 0)
        send (0, 0.5f); // pitch bend springs back to centre
    dragging = -1;
}

void Wheels::paint (juce::Graphics& g)
{
    g.setColour (juce::Colour (0xff111111));
    g.fillRect (getLocalBounds());
    for (int which = 0; which < 2; ++which)
    {
        const auto s = slot (which);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff050505), s.getX(), 0.0f, juce::Colour (0xff050505), s.getRight(), 0.0f, false));
        g.fillRect (s);
        g.setColour (juce::Colour (0xff2a2a2a));
        g.fillRect (s.reduced (s.getWidth() * 0.3f, 0.0f));
        const float v = which == 0 ? bend : mod;
        const float y = s.getBottom() - v * s.getHeight();
        const auto thumb = juce::Rectangle<float> (s.getX() + 1.0f, y - 7.0f, s.getWidth() - 2.0f, 14.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff777777), 0.0f, thumb.getY(), juce::Colour (0xff222222), 0.0f, thumb.getBottom(), false));
        g.fillRect (thumb);
        g.setColour (theme::yellow);
        g.fillRect (thumb.withHeight (2.0f).withCentre (thumb.getCentre()));
        PixelFont::drawIn (g, which == 0 ? "BEND" : "MOD", juce::Rectangle<float> (s.getX() - 4.0f, s.getBottom() + 4.0f, s.getWidth() + 8.0f, 10.0f),
                           0.85f, theme::textDim, juce::Justification::centred, false);
    }
}
}
