#pragma once

#include <juce_graphics/juce_graphics.h>

// An original 5x7 pixel font, in the spirit of the tile fonts 16-bit games
// used for menus. Drawn from rectangles, so it stays crisp at any scale and
// needs no font files. Uppercase only (lowercase is shown as uppercase),
// like most game menus of the era.
namespace genisys::ui
{
    struct PixelFont
    {
        static constexpr int glyphWidth = 5;
        static constexpr int glyphHeight = 7;
        static constexpr int advance = 6; // glyph + 1 pixel gap

        // `px` is the size of one font pixel. Text height is 7 * px.
        static float textWidth (const juce::String& text, float px);
        static float lineHeight (float px) { return (glyphHeight + 3) * px; }

        static juce::Path textPath (const juce::String& text, float px);

        // Draws with a 1-pixel drop shadow, like game text over a backdrop.
        static void draw (juce::Graphics&, const juce::String& text, float x, float y, float px,
                          juce::Colour colour, bool shadow = true);

        static void drawIn (juce::Graphics&, const juce::String& text, juce::Rectangle<float> area, float px,
                            juce::Colour colour, juce::Justification justification, bool shadow = true);

        // Word-wrapped paragraph; returns the height used.
        static float drawWrapped (juce::Graphics&, const juce::String& text, juce::Rectangle<float> area, float px,
                                  juce::Colour colour, bool shadow = true);
    };
}
