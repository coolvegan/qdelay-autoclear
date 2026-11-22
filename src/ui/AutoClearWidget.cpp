#include "AutoClearWidget.h"
#include "../PluginEditor.h"

AutoClearWidget::AutoClearWidget(QDelayAudioProcessorEditor& e)
    : editor(e)
{
    editor.audioProcessor.params.addParameterListener("autoclear", this);
    editor.audioProcessor.params.addParameterListener("clear_source", this);

    addAndMakeVisible(enabledBtn);
    enabledBtn.setComponentID("button");
    enabledBtn.setButtonText("ON");
    enabledBtn.onClick = [this]
        {
            auto param = editor.audioProcessor.params.getParameter("autoclear");
            param->setValueNotifyingHost(param->getValue() > 0.f ? 0.f : 1.f);
            refresh();
        };

    addAndMakeVisible(sourceBtn);
    sourceBtn.setAlpha(0.f);
    sourceBtn.onClick = [this] { showSourceMenu(); };

    fade = std::make_unique<Rotary>(editor.audioProcessor, "clear_fade", "Fade", Rotary::kMillis);
    fade->setTooltip("Smooth fade time used before the tail is cleared and while it returns.");
    addAndMakeVisible(fade.get());

    threshold = std::make_unique<Rotary>(editor.audioProcessor, "clear_thresh", "Thresh", Rotary::dBfloat1);
    threshold->setTooltip("Input level that counts as a new note when no MIDI is available.");
    addAndMakeVisible(threshold.get());

    hold = std::make_unique<Rotary>(editor.audioProcessor, "clear_hold", "Hold", Rotary::kMillis);
    hold->setTooltip("How long the wet path stays down after the tail has been cleared.");
    addAndMakeVisible(hold.get());

    amount = std::make_unique<Rotary>(editor.audioProcessor, "clear_amt", "Amount", Rotary::percx100);
    amount->setTooltip("100% removes the whole old tail; lower values leave a ducked tail.");
    addAndMakeVisible(amount.get());

    refresh();
}

AutoClearWidget::~AutoClearWidget()
{
    editor.audioProcessor.params.removeParameterListener("autoclear", this);
    editor.audioProcessor.params.removeParameterListener("clear_source", this);
}

void AutoClearWidget::parameterChanged(const juce::String&, float)
{
    MessageManager::callAsync([this] { refresh(); });
}

void AutoClearWidget::refresh()
{
    bool on = (bool) editor.audioProcessor.params.getRawParameterValue("autoclear")->load();
    enabledBtn.setButtonText(on ? "ON" : "OFF");
    enabledBtn.setToggleState(on, dontSendNotification);
    repaint();
}

void AutoClearWidget::paint(Graphics& g)
{
    auto mode = (int) editor.audioProcessor.params.getRawParameterValue("clear_source")->load();

    g.setColour(Colour(COLOR_NEUTRAL));
    g.setFont(FontOptions(16.f));
    g.drawText("AUTO CLEAR", enabledBtn.getBounds().withY(enabledBtn.getY() - 18).withHeight(16),
        Justification::centredLeft);

    UIUtils::drawBevel(g, sourceBtn.getBounds().toFloat().reduced(0.5f), BEVEL_CORNER, Colour(COLOR_BEVEL));
    g.setFont(FontOptions(16.f));
    g.setColour(Colours::white);
    g.drawText(mode == 0 ? "Audio" : mode == 1 ? "MIDI" : "Audio + MIDI",
        sourceBtn.getBounds().toFloat(), Justification::centred);
}

void AutoClearWidget::resized()
{
    auto b = getLocalBounds();

    enabledBtn.setBounds(b.getX(), b.getY() + 6, 70, 25);
    sourceBtn.setBounds(b.getX() + 70, b.getY() + 6, 140, 25);

    fade->setBounds(b.getX(), b.getY() + 42, KNOB_WIDTH, KNOB_HEIGHT);
    threshold->setBounds(b.getX() + KNOB_WIDTH, b.getY() + 42, KNOB_WIDTH, KNOB_HEIGHT);
    amount->setBounds(b.getX() + KNOB_WIDTH * 2, b.getY() + 42, KNOB_WIDTH, KNOB_HEIGHT);

    hold->setBounds(b.getX(), b.getY() + 42 + KNOB_HEIGHT, KNOB_WIDTH, KNOB_HEIGHT);
}

void AutoClearWidget::showSourceMenu()
{
    auto mode = (int) editor.audioProcessor.params.getRawParameterValue("clear_source")->load();

    PopupMenu menu;
    menu.addItem(1, "Audio", true, mode == 0);
    menu.addItem(2, "MIDI", true, mode == 1);
    menu.addItem(3, "Audio + MIDI", true, mode == 2);

    auto menuPos = localPointToGlobal(sourceBtn.getBounds().getBottomLeft());
    menu.showMenuAsync(PopupMenu::Options()
        .withTargetComponent(*this)
        .withTargetScreenArea({ menuPos.getX(), menuPos.getY(), 1, 1 }),
        [this](int result)
        {
            if (result == 0) return;
            auto param = editor.audioProcessor.params.getParameter("clear_source");
            param->setValueNotifyingHost(param->convertTo0to1(float(result - 1)));
            repaint();
        }
    );
}
