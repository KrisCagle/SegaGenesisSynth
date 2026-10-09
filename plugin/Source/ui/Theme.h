#pragma once

#include <juce_graphics/juce_graphics.h>

// The Genisys look: a black console chassis, blue beveled menu windows like
// 16-bit game menus, a yellow highlight and cyan "lit" knobs.
namespace genisys::ui::theme
{
    inline const juce::Colour chassis { 0xff0b0b0f };
    inline const juce::Colour chassisLight { 0xff15151c };
    inline const juce::Colour menuTop { 0xff1a3fb8 };
    inline const juce::Colour menuBottom { 0xff071a5c };
    inline const juce::Colour bevelLight { 0xff8fd3ff };
    inline const juce::Colour bevelDark { 0xff021047 };
    inline const juce::Colour text { 0xfff4f7ff };
    inline const juce::Colour textDim { 0xff8ea3d9 };
    inline const juce::Colour yellow { 0xffffd400 };
    inline const juce::Colour red { 0xffe60012 };
    inline const juce::Colour cyan { 0xff29d3ff };
    inline const juce::Colour cyanDim { 0xff0d2b55 };
    inline const juce::Colour green { 0xff3dff7a };
    inline const juce::Colour screen { 0xff020a24 };
    inline const juce::Colour screenEdge { 0xff1d3f8f };

    // Design size: everything is laid out at this size and scaled as a whole.
    constexpr int width = 1000;
    constexpr int height = 690;
}
