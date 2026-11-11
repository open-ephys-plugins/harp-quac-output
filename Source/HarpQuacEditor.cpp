/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "HarpQuacEditor.h"

#include "AnalogPatternGenerator.h"
#include "HarpQuacPlayer.h"
#include "HarpQuacNode.h"
#include "WaveformView.h"

HarpQuacEditor::HarpQuacEditor (GenericProcessor* p)
    : GenericEditor (p)
{
    desiredWidth = 380;

    wavePlayer = std::make_unique<HarpQuacPlayer>();

    addParameterRow ("Device", deviceLabel, deviceSelector, 29);
    addParameterRow ("Config", configLabel, configSelector, 54);
    addParameterRow ("Trigger", triggerLabel, triggerSelector, 79);
    addParameterRow ("Output", outputLabel, outputSelector, 104);

    configSelector->addItem ("Local", 1);
    configSelector->addItem ("Remote", 2);
    configSelector->setSelectedId (1, dontSendNotification);

    triggerSelector->addItem ("Manual", 1);
    triggerSelector->addItem ("Network", 2);
    for (int i = 1; i <= 8; i++)
        triggerSelector->addItem ("TTL" + String (i), i + 2);
    triggerSelector->setSelectedId (1, dontSendNotification);

    for (int i = 0; i < kNumAoChannels; i++)
        outputSelector->addItem ("AO" + String (i), i + 1);
    outputSelector->setSelectedId (1, dontSendNotification);

    waveformLabel = std::make_unique<Label> ("WaveformLabel", "Waveform:");
    waveformLabel->setFont (Font (13.0f));
    waveformLabel->setBounds (170, 29, 65, 20);
    addAndMakeVisible (waveformLabel.get());

    waveformSelector = std::make_unique<ComboBox> ("WaveformSelector");
    waveformSelector->setBounds (235, 29, 70, 20);
    waveformSelector->addItem ("Pulse", (int) PatternType::pulse + 1);
    waveformSelector->addItem ("Sine", (int) PatternType::sine + 1);
    //waveformSelector->addItem ("Custom", (int) PatternType::custom + 1);
    waveformSelector->setSelectedId ((int) PatternType::pulse + 1, dontSendNotification);
    waveformSelector->addListener (this);
    addAndMakeVisible (waveformSelector.get());

    settingsButton = std::make_unique<UtilityButton> ("*");
    settingsButton->setBounds (310, 29, 20, 20);
    settingsButton->addListener (this);
    settingsButton->setAlpha (0.5f);
    settingsButton->setTooltip ("Configure waveform");
    addAndMakeVisible (settingsButton.get());

    waveformView = std::make_unique<WaveformView>();
    waveformView->setBounds (170, 52, 210, 80);
    waveformView->onPlayRequested = [this]()
    {
        if (wavePlayer != nullptr && wavePlayer->onRunRequested)
            wavePlayer->onRunRequested();
    };
    addAndMakeVisible (waveformView.get());

    if (auto* node = dynamic_cast<HarpQuacNode*> (getProcessor()))
        node->getPlayer();

    wavePlayer->onPreviewUpdated = [this]()
    {
        refreshWaveformView();
        syncConfigFromPlayer();
    };

    storeTrigger();
    refreshDeviceSelector();
    syncConfigFromPlayer();
    refreshWaveformView();
    startTimer (500);
}

HarpQuacEditor::~HarpQuacEditor()
{
    stopTimer();
}

void HarpQuacEditor::addParameterRow (const String& name,
                                        std::unique_ptr<Label>& label,
                                        std::unique_ptr<ComboBox>& combo,
                                        int y)
{
    const int x = 15;
    const int rowWidth = 170;
    const int rowHeight = 18;

    combo = std::make_unique<ComboBox> (name);
    combo->setBounds (x, y, rowWidth / 2, rowHeight);
    combo->setJustificationType (Justification::centred);
    combo->addListener (this);
    addAndMakeVisible (combo.get());

    label = std::make_unique<Label> (name, name);
    label->setFont (FontOptions ("Inter", "Regular", int (0.75 * rowHeight)));
    label->setBounds (x + rowWidth / 2 + 4, y, rowWidth / 2, rowHeight);
    label->setJustificationType (Justification::centredLeft);
    addAndMakeVisible (label.get());
}

void HarpQuacEditor::buttonClicked (Button* button)
{
    if (button == settingsButton.get())
        openWaveformSettings (button);
}

void HarpQuacEditor::comboBoxChanged (ComboBox* comboBox)
{
    if (wavePlayer == nullptr)
        return;

    if (comboBox == deviceSelector.get())
    {
        if (auto* node = dynamic_cast<HarpQuacNode*> (getProcessor()))
            node->setSelectedPort (deviceSelector->getText());
    }
    else if (comboBox == configSelector.get())
    {
        wavePlayer->setMode (configSelector->getSelectedId() == 2
                                 ? WaveOutputMode::LISTEN
                                 : WaveOutputMode::GENERATE);
        updateConfigUI();
    }
    else if (comboBox == triggerSelector.get())
    {
        storeTrigger();
        updateConfigUI();
    }
    else if (comboBox == outputSelector.get())
    {
        wavePlayer->setAnalogOutputChannel (outputSelector->getSelectedId() - 1);
    }
    else if (comboBox == waveformSelector.get())
    {
        wavePlayer->applyPatternType ((PatternType) (waveformSelector->getSelectedId() - 1));
    }
}

void HarpQuacEditor::timerCallback()
{
    refreshDeviceSelector();
}

void HarpQuacEditor::updateConfigUI()
{
    const bool isLocal = configSelector == nullptr || configSelector->getSelectedId() != 2;

    if (triggerSelector != nullptr)
        triggerSelector->setVisible (isLocal);
    if (triggerLabel != nullptr)
        triggerLabel->setVisible (isLocal);
    if (outputSelector != nullptr)
        outputSelector->setEnabled (isLocal);

    if (waveformView != nullptr)
    {
        const bool isManual = triggerSelector == nullptr || triggerSelector->getSelectedId() <= 1;
        waveformView->setPlayButtonVisible (isLocal && isManual);
    }
}

void HarpQuacEditor::refreshDeviceSelector()
{
    auto* node = dynamic_cast<HarpQuacNode*> (getProcessor());
    if (node == nullptr || deviceSelector == nullptr)
        return;

    StringArray ports = HarpQuacNode::listSerialPorts();
    String selected = node->getSelectedPort();

    if (selected.isEmpty())
        selected = (ports.contains ("COM7") || ports.isEmpty()) ? "COM7" : ports[0];
    if (! ports.contains (selected))
        ports.insert (0, selected);

    String signature;
    for (const auto& port : ports)
        signature += port + "|";
    signature += "selected=" + selected;

    deviceSelectorHasChoice = ports.size() > 1;

    if (signature == deviceSelectorSignature)
    {
        updateDeviceSelectorEnabled();
        return;
    }

    deviceSelectorSignature = signature;
    deviceSelector->clear (dontSendNotification);

    for (int i = 0; i < ports.size(); ++i)
        deviceSelector->addItem (ports[i], i + 1);

    deviceSelector->setSelectedId (ports.indexOf (selected) + 1, dontSendNotification);

    if (node->getSelectedPort() != selected)
        node->setSelectedPort (selected);

    updateDeviceSelectorEnabled();
}

void HarpQuacEditor::updateDeviceSelectorEnabled()
{
    if (deviceSelector == nullptr)
        return;

    const bool acquisitionActive = acquisitionIsActive || CoreServices::getAcquisitionStatus();
    deviceSelector->setEnabled (deviceSelectorHasChoice && ! acquisitionActive);
}

void HarpQuacEditor::refreshWaveformView()
{
    if (wavePlayer == nullptr || wavePlayer->currentPattern == nullptr)
        return;

    Pattern* pattern = wavePlayer->currentPattern;

    if (waveformSelector != nullptr)
        waveformSelector->setSelectedId ((int) pattern->patternType + 1, dontSendNotification);

    if (outputSelector != nullptr)
    {
        const int id = jlimit (0, kNumAoChannels - 1, pattern->analogOutputChannel) + 1;
        outputSelector->setSelectedId (id, dontSendNotification);
    }

    if (waveformView != nullptr)
        waveformView->updateFromPattern (pattern);
}

void HarpQuacEditor::openWaveformSettings (Button* button)
{
    if (wavePlayer == nullptr || wavePlayer->currentPattern == nullptr)
        return;

    AnalogPatternGenerator* patternGenerator = nullptr;
    const auto type = wavePlayer->currentPattern->patternType;

    if (type == PatternType::pulse)
        patternGenerator = new PulsePatternGenerator (wavePlayer.get(), wavePlayer->currentPattern);
    else if (type == PatternType::sine)
        patternGenerator = new SinePatternGenerator (wavePlayer.get(), wavePlayer->currentPattern);
    else
        patternGenerator = new CustomPatternGenerator (wavePlayer.get(), wavePlayer->currentPattern);

    patternGenerator->buildWaveform();
    wavePlayer->updateWaveform();

    CallOutBox::launchAsynchronously (std::unique_ptr<Component> (patternGenerator),
                                      button->getScreenBounds(),
                                      nullptr);
}

void HarpQuacEditor::storeTrigger()
{
    if (wavePlayer == nullptr || wavePlayer->currentPattern == nullptr || triggerSelector == nullptr)
        return;

    const int id = triggerSelector->getSelectedId();

    if (id == 2)
        wavePlayer->currentPattern->triggerChannel = -2;
    else if (id >= 3)
        wavePlayer->currentPattern->triggerChannel = id - 2;
    else
        wavePlayer->currentPattern->triggerChannel = -1;

    if (auto* node = dynamic_cast<HarpQuacNode*> (getProcessor()))
        node->updateArmedState (wavePlayer.get());
}

void HarpQuacEditor::syncTriggerSelector()
{
    if (triggerSelector == nullptr)
        return;

    int id = 1;
    if (wavePlayer != nullptr && wavePlayer->currentPattern != nullptr)
    {
        const int channel = wavePlayer->currentPattern->triggerChannel;
        if (channel == -2)
            id = 2;
        else if (channel >= 1 && channel <= 8)
            id = channel + 2;
    }

    triggerSelector->setSelectedId (id, dontSendNotification);
    updateConfigUI();
}

void HarpQuacEditor::applyTriggerAttribute (const String& trigger)
{
    if (triggerSelector == nullptr)
        return;

    if (trigger.isNotEmpty())
    {
        for (int i = 0; i < triggerSelector->getNumItems(); i++)
        {
            if (triggerSelector->getItemText (i) == trigger)
            {
                triggerSelector->setSelectedId (triggerSelector->getItemId (i), dontSendNotification);
                storeTrigger();
                updateConfigUI();
                return;
            }
        }
    }

    syncTriggerSelector();
}

void HarpQuacEditor::syncConfigFromPlayer()
{
    if (wavePlayer == nullptr || configSelector == nullptr)
        return;

    const int id = wavePlayer->getMode() == WaveOutputMode::LISTEN ? 2 : 1;
    configSelector->setSelectedId (id, dontSendNotification);
    updateConfigUI();
}

void HarpQuacEditor::startAcquisition()
{
    acquisitionIsActive = true;
    updateDeviceSelectorEnabled();
}

void HarpQuacEditor::stopAcquisition()
{
    acquisitionIsActive = false;
    updateDeviceSelectorEnabled();
}

void HarpQuacEditor::saveCustomParametersToXml (XmlElement* xml)
{
    storeTrigger();

    if (auto* node = dynamic_cast<HarpQuacNode*> (getProcessor()))
        xml->setAttribute ("comPort", node->getSelectedPort());

    if (triggerSelector != nullptr)
        xml->setAttribute ("trigger", triggerSelector->getText());

    if (wavePlayer != nullptr)
        wavePlayer->saveCustomParameters (xml);
}

void HarpQuacEditor::loadCustomParametersFromXml (XmlElement* xml)
{
    if (xml == nullptr)
        return;

    if (wavePlayer != nullptr)
        wavePlayer->loadCustomParameters (xml);

    if (auto* node = dynamic_cast<HarpQuacNode*> (getProcessor()))
        node->setSelectedPort (xml->getStringAttribute ("comPort", node->getSelectedPort().isEmpty() ? "COM7" : node->getSelectedPort()));

    applyTriggerAttribute (xml->getStringAttribute ("trigger"));
    syncConfigFromPlayer();
    refreshDeviceSelector();
    refreshWaveformView();
}
