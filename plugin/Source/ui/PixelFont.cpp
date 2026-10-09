#include "PixelFont.h"

#include <array>
#include <map>

namespace genisys::ui
{
namespace
{
    using Glyph = std::array<const char*, 7>;

    const std::map<juce::juce_wchar, Glyph>& glyphs()
    {
        static const std::map<juce::juce_wchar, Glyph> table {
            { 'A', { ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
            { 'B', { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." } },
            { 'C', { ".###.", "#...#", "#....", "#....", "#....", "#...#", ".###." } },
            { 'D', { "####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####." } },
            { 'E', { "#####", "#....", "#....", "####.", "#....", "#....", "#####" } },
            { 'F', { "#####", "#....", "#....", "####.", "#....", "#....", "#...." } },
            { 'G', { ".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####" } },
            { 'H', { "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
            { 'I', { ".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###." } },
            { 'J', { "..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.." } },
            { 'K', { "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#" } },
            { 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
            { 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#" } },
            { 'N', { "#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#" } },
            { 'O', { ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
            { 'P', { "####.", "#...#", "#...#", "####.", "#....", "#....", "#...." } },
            { 'Q', { ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#" } },
            { 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
            { 'S', { ".####", "#....", "#....", ".###.", "....#", "....#", "####." } },
            { 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
            { 'U', { "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
            { 'V', { "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.." } },
            { 'W', { "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#." } },
            { 'X', { "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#" } },
            { 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.." } },
            { 'Z', { "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####" } },
            { '0', { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." } },
            { '1', { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." } },
            { '2', { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" } },
            { '3', { "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###." } },
            { '4', { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." } },
            { '5', { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." } },
            { '6', { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." } },
            { '7', { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." } },
            { '8', { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." } },
            { '9', { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." } },
            { '.', { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." } },
            { ',', { ".....", ".....", ".....", ".....", ".##..", "..#..", ".#..." } },
            { ':', { ".....", ".##..", ".##..", ".....", ".##..", ".##..", "....." } },
            { ';', { ".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#..." } },
            { '-', { ".....", ".....", ".....", "#####", ".....", ".....", "....." } },
            { '+', { ".....", "..#..", "..#..", "#####", "..#..", "..#..", "....." } },
            { '/', { ".....", "....#", "...#.", "..#..", ".#...", "#....", "....." } },
            { '%', { "##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##" } },
            { '!', { "..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.." } },
            { '?', { ".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.." } },
            { '(', { "...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#." } },
            { ')', { ".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..." } },
            { '\'', { "..#..", "..#..", ".#...", ".....", ".....", ".....", "....." } },
            { '"', { ".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....." } },
            { '#', { ".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#." } },
            { '&', { ".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#" } },
            { '*', { ".....", "..#..", "#.#.#", ".###.", "#.#.#", "..#..", "....." } },
            { '=', { ".....", ".....", "#####", ".....", "#####", ".....", "....." } },
            { '<', { "...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#." } },
            { '>', { ".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..." } },
            { '_', { ".....", ".....", ".....", ".....", ".....", ".....", "#####" } },
            { '[', { ".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###." } },
            { ']', { ".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###." } },
            { 0x25B6, { "#....", "##...", "###..", "####.", "###..", "##...", "#...." } }, // right triangle
            { 0x25C0, { "....#", "...##", "..###", ".####", "..###", "...##", "....#" } }, // left triangle
            { 0x25BC, { ".....", "#####", ".###.", "..#..", ".....", ".....", "....." } }, // down triangle
        };
        return table;
    }

    const Glyph* glyphFor (juce::juce_wchar c)
    {
        const auto& table = glyphs();
        const auto it = table.find (juce::CharacterFunctions::toUpperCase (c));
        return it != table.end() ? &it->second : nullptr;
    }
}

float PixelFont::textWidth (const juce::String& text, float px)
{
    const int n = text.length();
    return n == 0 ? 0.0f : (float) (n * advance - 1) * px;
}

juce::Path PixelFont::textPath (const juce::String& text, float px)
{
    juce::Path path;
    float x = 0.0f;
    for (auto c : text)
    {
        if (const auto* glyph = glyphFor (c))
        {
            for (int row = 0; row < glyphHeight; ++row)
            {
                const char* bits = (*glyph)[(size_t) row];
                // Merge runs of lit pixels into one rectangle.
                for (int col = 0; col < glyphWidth; ++col)
                {
                    if (bits[col] != '#')
                        continue;
                    int end = col;
                    while (end + 1 < glyphWidth && bits[end + 1] == '#')
                        ++end;
                    path.addRectangle (x + (float) col * px, (float) row * px, (float) (end - col + 1) * px, px);
                    col = end;
                }
            }
        }
        x += (float) advance * px;
    }
    return path;
}

void PixelFont::draw (juce::Graphics& g, const juce::String& text, float x, float y, float px,
                      juce::Colour colour, bool shadow)
{
    const auto path = textPath (text, px);
    if (shadow)
    {
        g.setColour (juce::Colours::black.withAlpha (colour.getFloatAlpha()));
        g.fillPath (path, juce::AffineTransform::translation (x + px, y + px));
    }
    g.setColour (colour);
    g.fillPath (path, juce::AffineTransform::translation (x, y));
}

void PixelFont::drawIn (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float px,
                        juce::Colour colour, juce::Justification justification, bool shadow)
{
    const float w = textWidth (text, px), h = (float) glyphHeight * px;
    float x = area.getX(), y = area.getY();
    if (justification.testFlags (juce::Justification::horizontallyCentred))
        x = area.getCentreX() - w * 0.5f;
    else if (justification.testFlags (juce::Justification::right))
        x = area.getRight() - w;
    if (justification.testFlags (juce::Justification::verticallyCentred))
        y = area.getCentreY() - h * 0.5f;
    else if (justification.testFlags (juce::Justification::bottom))
        y = area.getBottom() - h;
    draw (g, text, std::round (x), std::round (y), px, colour, shadow);
}

float PixelFont::drawWrapped (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float px,
                              juce::Colour colour, bool shadow)
{
    const auto words = juce::StringArray::fromTokens (text, " ", "");
    juce::String line;
    float y = area.getY();
    const auto flush = [&]
    {
        if (line.isNotEmpty())
            draw (g, line, area.getX(), y, px, colour, shadow);
        y += lineHeight (px);
        line.clear();
    };
    for (const auto& word : words)
    {
        const auto candidate = line.isEmpty() ? word : line + " " + word;
        if (textWidth (candidate, px) > area.getWidth() && line.isNotEmpty())
        {
            flush();
            line = word;
        }
        else
        {
            line = candidate;
        }
    }
    if (line.isNotEmpty())
        flush();
    return y - area.getY();
}
}
