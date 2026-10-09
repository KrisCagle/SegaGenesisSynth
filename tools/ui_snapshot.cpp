// Renders every tab of the Genisys editor to PNG files, without a DAW or a
// visible window, so UI changes can be reviewed (and compared over time):
//
//   genisys_ui_snapshot <output-directory>
//
// Writes ui_0_voice.png ... ui_5_mod.png at the editor's design size.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File outDir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])
                                       : juce::File::getCurrentWorkingDirectory();
    outDir.createDirectory();

    GenisysProcessor processor;
    processor.setCurrentProgram (1); // a real preset rather than Init
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* genisysEditor = dynamic_cast<GenisysEditor*> (editor.get());
    if (genisysEditor == nullptr)
        return 1;

    static const char* names[] = { "voice", "operators", "psg", "drums", "fx", "mod" };
    for (int tab = 0; tab < GenisysEditor::numTabs; ++tab)
    {
        genisysEditor->selectTab (tab);
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        const auto file = outDir.getChildFile ("ui_" + juce::String (tab) + "_" + names[tab] + ".png");
        juce::FileOutputStream stream (file);
        if (! stream.openedOk())
            return 1;
        stream.setPosition (0);
        stream.truncate();
        juce::PNGImageFormat().writeImageToStream (image, stream);
        std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
    }
    editor.reset();
    return 0;
}
