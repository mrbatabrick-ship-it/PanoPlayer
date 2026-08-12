#include "PluginProcessor.h"
#include "PluginEditor.h"

PanoramixMasterAudioProcessorEditor::PanoramixMasterAudioProcessorEditor (PanoramixMasterAudioProcessor& p)
    : AudioProcessorEditor (p), audioProcessor (p)
{
    titleLabel.setText("PanoPlayer (OSC)", juce::dontSendNotification);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    titleLabel.setFont(juce::Font(17.0f, juce::Font::bold));
    addAndMakeVisible(titleLabel);

    modeCombo.addItem("Sender Node", 1);
    modeCombo.addItem("Master Hub", 2);
    addAndMakeVisible(modeCombo);
    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(audioProcessor.apvts, "plugin_mode", modeCombo);
    modeCombo.onChange = [this] {
        updateUIVisibility();
        resized();
        repaint();
    };

    addAndMakeVisible(optionsButton);
    optionsButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    optionsButton.onClick = [this]()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "New Session (Reset All)");

        juce::Component::SafePointer<PanoramixMasterAudioProcessorEditor> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&optionsButton), [safeThis](int result)
        {
            if (safeThis == nullptr) return;
            if (result == 1)
            {
                juce::AlertWindow::showOkCancelBox(
                    juce::AlertWindow::QuestionIcon,
                    "New Session",
                    "Apakah Anda yakin ingin mereset seluruh session ke kondisi awal?",
                    "Ya, Reset",
                    "Batal",
                    safeThis,
                    juce::ModalCallbackFunction::create([safeThis](int buttonResult)
                    {
                        if (safeThis == nullptr) return;
                        if (buttonResult == 1)
                        {
                            safeThis->audioProcessor.resetToNewSession();
                            safeThis->updateUIVisibility();
                            safeThis->repaint();
                        }
                    })
                );
            }
        });
    };

    addAndMakeVisible(playAudioButton);
    playAudioButton.setButtonText("Play All");
    playAudioButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
    playAudioButton.onClick = [this]()
    {
        if (audioProcessor.isGlobalPlaying())
        {
            audioProcessor.stopAudioPlayback();
            playAudioButton.setButtonText("Play All");
            playAudioButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
        }
        else
        {
            audioProcessor.startAudioPlayback();
            playAudioButton.setButtonText("Stop All");
            playAudioButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
        }
    };

    addAndMakeVisible(engineModeBtn);
    engineModeBtn.setButtonText(audioProcessor.engineMode.load() == 0 ? "RAM Mode" : "Disk Mode");
    engineModeBtn.setColour(juce::TextButton::buttonColourId, juce::Colours::darkorange);
    engineModeBtn.onClick = [this]()
    {
        int current = audioProcessor.engineMode.load();
        int next = (current == 0) ? 1 : 0;
        audioProcessor.setEngineMode(next);
        engineModeBtn.setButtonText(next == 0 ? "RAM Mode" : "Disk Mode");
    };

    addAndMakeVisible(showPlaylistButton);
    showPlaylistButton.setButtonText("Playlist");
    showPlaylistButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkblue);
    showPlaylistButton.onClick = [this]()
    {
        if (playlistWindow != nullptr)
        {
            playlistWindow->setVisible(true);
            playlistWindow->toFront(true);
        }
    };

    idLabel.setText("Source ID:", juce::dontSendNotification);
    addChildComponent(idLabel);
    idSlider.setSliderStyle(juce::Slider::IncDecButtons);
    idSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 20);
    addChildComponent(idSlider);
    idAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(audioProcessor.apvts, "source_id", idSlider);

    ipLabel.setText("IP:", juce::dontSendNotification);
    addChildComponent(ipLabel);
    ipEditor.setText(audioProcessor.currentIP);
    addChildComponent(ipEditor);
    
    portLabel.setText("Send:", juce::dontSendNotification);
    addChildComponent(portLabel);
    portEditor.setText(juce::String(audioProcessor.currentPort));
    addChildComponent(portEditor);

    recvPortLabel.setText("Recv:", juce::dontSendNotification);
    addChildComponent(recvPortLabel);
    recvPortEditor.setText(juce::String(audioProcessor.receivePort));
    addChildComponent(recvPortEditor);

    rxLabel.setText("RX: IDLE", juce::dontSendNotification);
    rxLabel.setFont(juce::Font(11.0f, juce::Font::bold));
    rxLabel.setColour(juce::Label::textColourId, juce::Colours::darkgrey);
    addChildComponent(rxLabel);

    morphLabel.setText("Fade (s):", juce::dontSendNotification);
    addChildComponent(morphLabel);
    morphSlider.setSliderStyle(juce::Slider::IncDecButtons);
    morphSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 40, 20);
    addChildComponent(morphSlider);
    morphAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(audioProcessor.apvts, "morph_time", morphSlider);

    storeToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::orange);
    addChildComponent(storeToggle);

    for (int i = 0; i < 4; ++i)
    {
        presetButtons[i].setButtonText("P" + juce::String(i + 1));
        presetButtons[i].onClick = [this, i]()
        {
            if (storeToggle.getToggleState())
            {
                audioProcessor.storePreset(i);
                storeToggle.setToggleState(false, juce::dontSendNotification);
            }
            else
            {
                audioProcessor.triggerPreset(i);
            }
        };
        addChildComponent(presetButtons[i]);
    }

    motionToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::cyan);
    addChildComponent(motionToggle);
    motionToggleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(audioProcessor.apvts, "motion_enable", motionToggle);

    motionShapeCombo.addItem("Circle", 1);
    motionShapeCombo.addItem("Figure-8", 2);
    motionShapeCombo.addItem("Sine X", 3);
    motionShapeCombo.addItem("Lissajous", 4);
    motionShapeCombo.addItem("Spiral", 5);
    motionShapeCombo.addItem("Random", 6);
    motionShapeCombo.addItem("Perlin Walk", 7);
    motionShapeCombo.addItem("3D Helix", 8);
    motionShapeCombo.addItem("Elliptic Orbit", 9);
    motionShapeCombo.addItem("Pendulum Arc", 10);
    motionShapeCombo.addItem("Rose Curve", 11);
    motionShapeCombo.addItem("Flyby", 12);
    motionShapeCombo.addItem("Box Bounce", 13);
    addChildComponent(motionShapeCombo);
    motionShapeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(audioProcessor.apvts, "motion_shape", motionShapeCombo);

    motionRateLabel.setText("Rate:", juce::dontSendNotification);
    addChildComponent(motionRateLabel);
    motionRateSlider.setSliderStyle(juce::Slider::IncDecButtons);
    motionRateSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 36, 20);
    addChildComponent(motionRateSlider);
    motionRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(audioProcessor.apvts, "motion_rate", motionRateSlider);

    motionRadiusLabel.setText("Rad:", juce::dontSendNotification);
    addChildComponent(motionRadiusLabel);
    motionRadiusSlider.setSliderStyle(juce::Slider::IncDecButtons);
    motionRadiusSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 36, 20);
    addChildComponent(motionRadiusSlider);
    motionRadiusAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(audioProcessor.apvts, "motion_radius", motionRadiusSlider);

    motionSpreadLabel.setText("Spread:", juce::dontSendNotification);
    addChildComponent(motionSpreadLabel);
    motionSpreadSlider.setSliderStyle(juce::Slider::IncDecButtons);
    motionSpreadSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 36, 20);
    addChildComponent(motionSpreadSlider);
    motionSpreadAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(audioProcessor.apvts, "motion_spread", motionSpreadSlider);

    connectButton.setButtonText(audioProcessor.isNetworkConnected.load() ? "Disconnect" : "Connect");
    connectButton.onClick = [this]()
    {
        if (audioProcessor.isNetworkConnected.load())
        {
            audioProcessor.disconnectNetwork();
            statusLabel.setText("Disconnected", juce::dontSendNotification);
            statusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
            connectButton.setButtonText("Connect");
        }
        else
        {
            int sendPort = portEditor.getText().getIntValue();
            int recvPort = recvPortEditor.getText().getIntValue();

            if (sendPort < 1 || sendPort > 65535 || recvPort < 1 || recvPort > 65535)
            {
                statusLabel.setText("Invalid Port", juce::dontSendNotification);
                statusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
                return;
            }

            connectButton.setEnabled(false);
            statusLabel.setText("Connecting...", juce::dontSendNotification);

            juce::Component::SafePointer<PanoramixMasterAudioProcessorEditor> safeThis(this);
            juce::String ip = ipEditor.getText();

            juce::Thread::launch([safeThis, ip, sendPort, recvPort]()
            {
                bool sendOk = safeThis != nullptr && safeThis->audioProcessor.connectToNetwork(ip, sendPort);
                bool recvOk = safeThis != nullptr && safeThis->audioProcessor.updateReceivePort(recvPort);

                juce::MessageManager::callAsync([safeThis, sendOk, recvOk]()
                {
                    if (safeThis == nullptr) return;
                    safeThis->connectButton.setEnabled(true);

                    if (sendOk && recvOk)
                    {
                        bool alive = safeThis->audioProcessor.isHeartbeatAlive.load();
                        safeThis->statusLabel.setText(alive ? "Active (OK)" : "Active", juce::dontSendNotification);
                        safeThis->statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
                        safeThis->connectButton.setButtonText("Disconnect");
                    }
                    else
                    {
                        safeThis->statusLabel.setText("Connect Fail", juce::dontSendNotification);
                        safeThis->statusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
                        safeThis->connectButton.setButtonText("Connect");
                    }
                });
            });
        }
    };
    addChildComponent(connectButton);

    statusLabel.setText(audioProcessor.isNetworkConnected.load() ? "Active" : "Disconnected", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, audioProcessor.isNetworkConnected.load() ? juce::Colours::lightgreen : juce::Colours::grey);
    addChildComponent(statusLabel);

    radarComponent = std::make_unique<DualRadarComponent>(audioProcessor);
    addChildComponent(radarComponent.get());

    playlistWindow = std::make_unique<PlaylistWindow>(audioProcessor);

    audioProcessor.onMissingFileDetected = [this]() { triggerNextMissingFileLocate(); };

    setResizable(true, true);
    setResizeLimits(1000, 420, 2560, 1440);
    setSize (1380, 550);
    
    updateUIVisibility();
    startTimerHz(15);
}

PanoramixMasterAudioProcessorEditor::~PanoramixMasterAudioProcessorEditor()
{
    audioProcessor.onMissingFileDetected = nullptr;
    stopTimer();
    playlistWindow = nullptr;
}

void PanoramixMasterAudioProcessorEditor::timerCallback()
{
    if (!isShowing()) return;

    if (audioProcessor.isGlobalPlaying())
    {
        if (playAudioButton.getButtonText() != "Stop All")
        {
            playAudioButton.setButtonText("Stop All");
            playAudioButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
        }
    }
    else
    {
        if (playAudioButton.getButtonText() != "Play All")
        {
            playAudioButton.setButtonText("Play All");
            playAudioButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgreen);
        }
    }

    engineModeBtn.setButtonText(audioProcessor.engineMode.load() == 0 ? "RAM Mode" : "Disk Mode");

    auto now = juce::Time::getMillisecondCounter();
    auto lastRx = audioProcessor.lastOscMsgTime.load();
    bool isRxActive = (lastRx > 0) && ((now - lastRx) < 1000);

    rxLabel.setText(isRxActive ? "RX: ON" : "RX: IDLE", juce::dontSendNotification);
    rxLabel.setColour(juce::Label::textColourId, isRxActive ? juce::Colours::lime : juce::Colours::darkgrey);
}

void PanoramixMasterAudioProcessorEditor::triggerNextMissingFileLocate()
{
    if (isLocatingMissingFile) return;

    MissingFileItem missingItem;
    if (audioProcessor.getNextMissingFile(missingItem))
    {
        isLocatingMissingFile = true;
        juce::File originalFile(missingItem.missingPath);
        juce::String promptTitle = "Locate Missing File for Track " + juce::String(missingItem.trackId) + " (" + originalFile.getFileName() + ")";

        locateFileChooser = std::make_unique<juce::FileChooser>(promptTitle, originalFile.getParentDirectory(), "*.wav;*.mp3;*.aiff;*.flac");
        
        juce::Component::SafePointer<PanoramixMasterAudioProcessorEditor> safeThis(this);
        locateFileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis, missingItem](const juce::FileChooser& fc)
            {
                if (safeThis == nullptr) return;
                auto relocatedFile = fc.getResult();
                if (relocatedFile.existsAsFile())
                {
                    safeThis->audioProcessor.loadAudioFile(missingItem.trackId, relocatedFile);
                }
                safeThis->isLocatingMissingFile = false;
                safeThis->triggerNextMissingFileLocate();
            });
    }
}

void PanoramixMasterAudioProcessorEditor::updateUIVisibility()
{
    bool isMaster = modeCombo.getSelectedId() == 2;
    idLabel.setVisible(!isMaster);
    idSlider.setVisible(!isMaster);
    ipLabel.setVisible(isMaster);
    ipEditor.setVisible(isMaster);
    portLabel.setVisible(isMaster);
    portEditor.setVisible(isMaster);
    recvPortLabel.setVisible(isMaster);
    recvPortEditor.setVisible(isMaster);
    rxLabel.setVisible(true);
    connectButton.setVisible(isMaster);
    statusLabel.setVisible(isMaster);

    morphLabel.setVisible(isMaster);
    morphSlider.setVisible(isMaster);
    storeToggle.setVisible(isMaster);
    for (int i = 0; i < 4; ++i) {
        presetButtons[i].setVisible(isMaster);
    }

    motionToggle.setVisible(isMaster);
    motionShapeCombo.setVisible(isMaster);
    motionRateLabel.setVisible(isMaster);
    motionRateSlider.setVisible(isMaster);
    motionRadiusLabel.setVisible(isMaster);
    motionRadiusSlider.setVisible(isMaster);
    motionSpreadLabel.setVisible(isMaster);
    motionSpreadSlider.setVisible(isMaster);
    
    radarComponent->setVisible(true);
}

void PanoramixMasterAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour(0xff181818));
    g.setColour (juce::Colours::darkgrey);
    g.drawRect (getLocalBounds(), 1);
}

void PanoramixMasterAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(10);
    
    auto row1 = area.removeFromTop(28);
    titleLabel.setBounds(row1.removeFromLeft(145));
    modeCombo.setBounds(row1.removeFromLeft(120).withSizeKeepingCentre(120, 24));
    
    row1.removeFromLeft(8);
    optionsButton.setBounds(row1.removeFromLeft(60).withSizeKeepingCentre(60, 24));

    row1.removeFromLeft(12);
    playAudioButton.setBounds(row1.removeFromLeft(80).withSizeKeepingCentre(80, 24));
    row1.removeFromLeft(6);
    engineModeBtn.setBounds(row1.removeFromLeft(85).withSizeKeepingCentre(85, 24));
    row1.removeFromLeft(6);
    showPlaylistButton.setBounds(row1.removeFromLeft(75).withSizeKeepingCentre(75, 24));

    bool isMaster = modeCombo.getSelectedId() == 2;
    if (isMaster)
    {
        row1.removeFromLeft(15);
        ipLabel.setBounds(row1.removeFromLeft(24));
        ipEditor.setBounds(row1.removeFromLeft(120));
        row1.removeFromLeft(8);
        
        portLabel.setBounds(row1.removeFromLeft(38));
        portEditor.setBounds(row1.removeFromLeft(48));
        row1.removeFromLeft(8);
        
        recvPortLabel.setBounds(row1.removeFromLeft(38));
        recvPortEditor.setBounds(row1.removeFromLeft(48));
        row1.removeFromLeft(10);

        rxLabel.setBounds(row1.removeFromLeft(55));
        connectButton.setBounds(row1.removeFromLeft(80));
        row1.removeFromLeft(10);
        statusLabel.setBounds(row1);

        area.removeFromTop(6);

        auto row2 = area.removeFromTop(28);

        morphLabel.setBounds(row2.removeFromLeft(52));
        morphSlider.setBounds(row2.removeFromLeft(95));
        row2.removeFromLeft(8);
        
        storeToggle.setBounds(row2.removeFromLeft(58));
        row2.removeFromLeft(4);
        for (int i = 0; i < 4; ++i) {
            presetButtons[i].setBounds(row2.removeFromLeft(34));
            row2.removeFromLeft(3);
        }

        row2.removeFromLeft(15);

        motionToggle.setBounds(row2.removeFromLeft(95));
        row2.removeFromLeft(6);
        motionShapeCombo.setBounds(row2.removeFromLeft(110));
        row2.removeFromLeft(8);
        
        motionRateLabel.setBounds(row2.removeFromLeft(34));
        motionRateSlider.setBounds(row2.removeFromLeft(95));
        row2.removeFromLeft(8);
        
        motionRadiusLabel.setBounds(row2.removeFromLeft(38));
        motionRadiusSlider.setBounds(row2.removeFromLeft(95));
        row2.removeFromLeft(8);
        
        motionSpreadLabel.setBounds(row2.removeFromLeft(48));
        motionSpreadSlider.setBounds(row2.removeFromLeft(95));
    }
    else
    {
        row1.removeFromLeft(15);
        idLabel.setBounds(row1.removeFromLeft(70));
        idSlider.setBounds(row1.removeFromLeft(130));
        row1.removeFromLeft(15);
        rxLabel.setBounds(row1.removeFromLeft(60));
    }

    area.removeFromTop(8);
    radarComponent->setBounds(area);
}
