#pragma once

#include <JuceHeader.h>
#include "../Globals.h"
#include "UIUtils.h"
#include "Rotary.h"

using namespace globals;
class QDelayAudioProcessorEditor;

class AutoClearWidget
    : public juce::Component
    , private juce::AudioProcessorValueTreeState::Listener
{
public:
    TextButton enabledBtn;
    TextButton sourceBtn;
    std::unique_ptr<Rotary> fade;
    std::unique_ptr<Rotary> threshold;
    std::unique_ptr<Rotary> hold;
    std::unique_ptr<Rotary> amount;

    AutoClearWidget(QDelayAudioProcessorEditor& e);
    ~AutoClearWidget() override;

    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void paint(Graphics& g) override;
    void resized() override;
    void showSourceMenu();
    void refresh();

private:
    QDelayAudioProcessorEditor& editor;
};
