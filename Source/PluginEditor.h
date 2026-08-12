#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <unordered_set>
#include <array>

class TrackRowComponent : public juce::Component
{
public:
    TrackRowComponent(PanoramixMasterAudioProcessor& p, int id) : processor(p), trackId(id)
    {
        idLabel.setText(juce::String(trackId), juce::dontSendNotification);
        idLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(idLabel);

        SpatialData sd;
        juce::String currentName = "Track " + juce::String(trackId);
        if (processor.sharedRegistry->getSource(trackId, sd)) {
            if (sd.trackName.isNotEmpty()) currentName = sd.trackName;
        }

        nameEditor.setText(currentName, juce::dontSendNotification);
        nameEditor.setEditable(true);
        nameEditor.setColour(juce::Label::backgroundColourId, juce::Colour(0xff222222));
        nameEditor.onTextChange = [this]() {
            SpatialData d;
            if (processor.sharedRegistry->getSource(trackId, d)) {
                d.trackName = nameEditor.getText();
                processor.sharedRegistry->updateSource(d);

                if (processor.isNetworkConnected.load()) {
                    try {
                        processor.oscSender.send("/track/" + juce::String(trackId) + "/name", d.trackName);
                    } catch (...) {}
                }
            }
        };
        addAndMakeVisible(nameEditor);

        loadBtn.setButtonText("Load...");
        loadBtn.onClick = [this]() {
            fileChooser = std::make_unique<juce::FileChooser>("Select Audio File...", juce::File(), "*.wav;*.mp3;*.aiff;*.flac");
            
            juce::Component::SafePointer<TrackRowComponent> safeThis(this);
            fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safeThis](const juce::FileChooser& fc) {
                    if (safeThis == nullptr) return;
                    auto file = fc.getResult();
                    if (file.exists()) {
                        safeThis->processor.loadAudioFile(safeThis->trackId, file);
                        safeThis->fileNameLabel.setText(file.getFileName(), juce::dontSendNotification);
                    }
                });
        };
        addAndMakeVisible(loadBtn);

        fileNameLabel.setText((trackId >= 1 && trackId <= 64 && processor.audioTracks[trackId - 1]) ? processor.audioTracks[trackId - 1]->fileName : "No file", juce::dontSendNotification);
        fileNameLabel.setFont(11.0f);
        addAndMakeVisible(fileNameLabel);

        radarToggle.setButtonText("Radar");
        radarToggle.setToggleState(false, juce::dontSendNotification);
        radarToggle.onClick = [this]() {
            bool st = radarToggle.getToggleState();
            SpatialData d;
            if (processor.sharedRegistry->getSource(trackId, d)) {
                d.showInRadar = st;
                d.isActive = st;
                processor.sharedRegistry->updateSource(d);
            }
        };
        addAndMakeVisible(radarToggle);

        muteBtn.setButtonText("M");
        muteBtn.setClickingTogglesState(true);
        muteBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colours::red);
        muteBtn.onClick = [this]() {
            bool m = muteBtn.getToggleState();
            if (trackId >= 1 && trackId <= 64 && processor.audioTracks[trackId - 1]) processor.audioTracks[trackId - 1]->mute.store(m);
            SpatialData d;
            if (processor.sharedRegistry->getSource(trackId, d)) {
                d.isMute = m;
                processor.sharedRegistry->updateSource(d);
            }
        };
        addAndMakeVisible(muteBtn);

        soloBtn.setButtonText("S");
        soloBtn.setClickingTogglesState(true);
        soloBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colours::goldenrod);
        soloBtn.onClick = [this]() {
            bool s = soloBtn.getToggleState();
            if (trackId >= 1 && trackId <= 64 && processor.audioTracks[trackId - 1]) processor.audioTracks[trackId - 1]->solo.store(s);
            SpatialData d;
            if (processor.sharedRegistry->getSource(trackId, d)) {
                d.isSolo = s;
                processor.sharedRegistry->updateSource(d);
            }
        };
        addAndMakeVisible(soloBtn);

        volSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        volSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 15);
        volSlider.setRange(-60.0, 6.0, 0.1);
        volSlider.setValue(0.0, juce::dontSendNotification);
        volSlider.setTextValueSuffix(" dB");
        volSlider.onValueChange = [this]() {
            if (trackId >= 1 && trackId <= 64 && processor.audioTracks[trackId - 1]) {
                float dbVal = (float)volSlider.getValue();
                float gainLinear = (dbVal <= -59.5f) ? 0.0f : juce::Decibels::decibelsToGain(dbVal);
                processor.audioTracks[trackId - 1]->gain.store(gainLinear);
            }
        };
        addAndMakeVisible(volSlider);

        for (int i = 1; i <= 64; ++i) {
            outChCombo.addItem("Out " + juce::String(i), i);
        }
        outChCombo.setSelectedId(trackId, juce::dontSendNotification);
        outChCombo.onChange = [this]() {
            if (trackId >= 1 && trackId <= 64 && processor.audioTracks[trackId - 1]) {
                processor.audioTracks[trackId - 1]->outCh.store(outChCombo.getSelectedId() - 1);
            }
        };
        addAndMakeVisible(outChCombo);

        seekSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        seekSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        seekSlider.setRange(0.0, 100.0);
        seekSlider.onValueChange = [this]() {
            if (isDraggingSlider && trackId >= 1 && trackId <= 64 && processor.audioTracks[trackId - 1] && processor.audioTracks[trackId - 1]->hasAudio.load()) {
                auto* t = processor.audioTracks[trackId - 1].get();
                juce::SpinLock::ScopedTryLockType sl(t->processLock);
                if (sl.isLocked()) {
                    if (processor.engineMode.load() == 0) {
                        double sr = (processor.getSampleRate() > 0.0) ? processor.getSampleRate() : 48000.0;
                        t->playPosition.store((int64_t)(seekSlider.getValue() * sr));
                    } else {
                        if (t->transportSource != nullptr) {
                            t->transportSource->setPosition(seekSlider.getValue());
                        }
                    }
                }
            }
        };
        seekSlider.onDragStart = [this]() { isDraggingSlider = true; };
        seekSlider.onDragEnd = [this]() { isDraggingSlider = false; };
        addAndMakeVisible(seekSlider);
    }

    ~TrackRowComponent() override = default;

    void paint(juce::Graphics& g) override
    {
        g.fillAll(trackId % 2 == 0 ? juce::Colour(0xff1e1e1e) : juce::Colour(0xff2a2a2a));
        
        if (meterBounds.getWidth() > 0)
        {
            g.setColour(juce::Colour(0xff101010));
            g.fillRect(meterBounds);

            if (currentLevel > 0.0001f)
            {
                auto boundsCopy = meterBounds;
                float normLevel = juce::jlimit(0.0f, 1.0f, (juce::Decibels::gainToDecibels(currentLevel, -60.0f) + 60.0f) / 60.0f);
                int fillHeight = juce::roundToInt(boundsCopy.getHeight() * normLevel);
                auto levelRect = boundsCopy.removeFromBottom(fillHeight);

                juce::Colour meterCol = (normLevel > 0.85f) ? juce::Colours::red :
                                        (normLevel > 0.60f) ? juce::Colours::yellow : juce::Colours::lime;
                g.setColour(meterCol);
                g.fillRect(levelRect);
            }
        }

        g.setColour(juce::Colours::grey);
        g.drawLine(0.0f, (float)getHeight(), (float)getWidth(), (float)getHeight(), 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(5, 2);
        idLabel.setBounds(area.removeFromLeft(28));
        nameEditor.setBounds(area.removeFromLeft(105).reduced(0, 2));
        area.removeFromLeft(4);
        loadBtn.setBounds(area.removeFromLeft(50).reduced(0, 2));
        area.removeFromLeft(4);
        fileNameLabel.setBounds(area.removeFromLeft(105));
        radarToggle.setBounds(area.removeFromLeft(55));
        area.removeFromLeft(4);
        
        muteBtn.setBounds(area.removeFromLeft(28).reduced(0, 2));
        area.removeFromLeft(2);
        soloBtn.setBounds(area.removeFromLeft(28).reduced(0, 2));
        area.removeFromLeft(6);
        
        volSlider.setBounds(area.removeFromLeft(140).reduced(0, 2));
        area.removeFromLeft(4);

        meterBounds = area.removeFromLeft(6).reduced(0, 3);
        area.removeFromLeft(6);

        outChCombo.setBounds(area.removeFromLeft(68).reduced(0, 2));
        area.removeFromLeft(8);
        seekSlider.setBounds(area);
    }

    void updateRowUI()
    {
        if (!isShowing() || trackId < 1 || trackId > 64) return;

        int idx = trackId - 1;
        auto* t = processor.audioTracks[idx].get();
        if (t == nullptr) return;

        float lvl = t->currentLevel.load(std::memory_order_relaxed);
        if (std::abs(lvl - currentLevel) > 0.005f || lvl > 0.0001f)
        {
            currentLevel = lvl;
            if (isShowing()) repaint(meterBounds);
        }

        if (t->hasAudio.load() && !isDraggingSlider) {
            juce::SpinLock::ScopedTryLockType sl(t->processLock);
            if (sl.isLocked()) {
                double len = 0.0;
                double pos = 0.0;
                
                if (processor.engineMode.load() == 0) { // RAM
                    double sr = (processor.getSampleRate() > 0.0) ? processor.getSampleRate() : 48000.0;
                    len = (double)t->audioData.getNumSamples() / sr;
                    pos = (double)t->playPosition.load() / sr;
                } else { // Disk
                    if (t->transportSource != nullptr) {
                        len = t->transportSource->getTotalLength() / (processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0);
                        pos = t->transportSource->getCurrentPosition();
                    }
                }
                
                if (seekSlider.getMaximum() != len && len > 0.0) seekSlider.setRange(0.0, len);
                seekSlider.setValue(pos, juce::dontSendNotification);
            }
        }
        
        fileNameLabel.setText(t->fileName, juce::dontSendNotification);

        muteBtn.setToggleState(t->mute.load(), juce::dontSendNotification);
        soloBtn.setToggleState(t->solo.load(), juce::dontSendNotification);
        outChCombo.setSelectedId(t->outCh.load() + 1, juce::dontSendNotification);

        float currentGainLinear = t->gain.load();
        float dbVal = (currentGainLinear <= 0.0001f) ? -60.0f : juce::Decibels::gainToDecibels(currentGainLinear);
        if (std::abs(volSlider.getValue() - dbVal) > 0.1)
            volSlider.setValue(dbVal, juce::dontSendNotification);

        SpatialData sd;
        if (processor.sharedRegistry->getSource(trackId, sd)) {
            if (sd.trackName.isNotEmpty() && sd.trackName != nameEditor.getText()) {
                nameEditor.setText(sd.trackName, juce::dontSendNotification);
            }
            if (radarToggle.getToggleState() != sd.showInRadar) {
                radarToggle.setToggleState(sd.showInRadar, juce::dontSendNotification);
            }
        }
    }

private:
    PanoramixMasterAudioProcessor& processor;
    int trackId;
    juce::Label idLabel;
    juce::Label nameEditor;
    juce::TextButton loadBtn;
    juce::Label fileNameLabel;
    juce::ToggleButton radarToggle;
    juce::TextButton muteBtn;
    juce::TextButton soloBtn;
    juce::Slider volSlider;
    juce::ComboBox outChCombo;
    juce::Slider seekSlider;
    juce::Rectangle<int> meterBounds;
    float currentLevel {0.0f};
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool isDraggingSlider = false;
};

class PlaylistContentComponent : public juce::Component, public juce::Timer
{
public:
    PlaylistContentComponent(PanoramixMasterAudioProcessor& p) : processor(p)
    {
        globalLoopBtn.setButtonText("Global Loop");
        globalLoopBtn.setToggleState(processor.isGlobalLooping.load(), juce::dontSendNotification);
        globalLoopBtn.onClick = [this]() {
            processor.setGlobalLooping(globalLoopBtn.getToggleState());
        };
        addAndMakeVisible(globalLoopBtn);

        timeLabel.setText("00:00 / 00:00", juce::dontSendNotification);
        timeLabel.setFont(juce::Font(12.0f, juce::Font::bold));
        addAndMakeVisible(timeLabel);

        globalSeekBar.setSliderStyle(juce::Slider::LinearHorizontal);
        globalSeekBar.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        globalSeekBar.setRange(0.0, 100.0);
        globalSeekBar.onValueChange = [this]() {
            if (isDraggingGlobalSeek) {
                processor.setGlobalPositionInSeconds(globalSeekBar.getValue());
            }
        };
        globalSeekBar.onDragStart = [this]() { isDraggingGlobalSeek = true; };
        globalSeekBar.onDragEnd = [this]() { isDraggingGlobalSeek = false; };
        addAndMakeVisible(globalSeekBar);

        listContainer = std::make_unique<juce::Component>();
        listContainer->setSize(1080, 64 * 35);
        for (int i = 1; i <= 64; ++i) {
            auto* row = new TrackRowComponent(p, i);
            row->setBounds(0, (i - 1) * 35, 1080, 35);
            listContainer->addAndMakeVisible(row);
            rows.add(row);
        }
        
        viewport.setViewedComponent(listContainer.get(), false);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport);

        startTimerHz(15);
    }

    ~PlaylistContentComponent() override
    {
        stopTimer();
        rows.clear();
        listContainer = nullptr;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff151515));
        g.setColour(juce::Colour(0xff252525));
        g.fillRect(0, 0, getWidth(), 35);
        g.setColour(juce::Colours::black);
        g.drawLine(0, 35, getWidth(), 35, 1.5f);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        
        auto topBar = area.removeFromTop(35).reduced(8, 4);
        globalLoopBtn.setBounds(topBar.removeFromLeft(100));
        topBar.removeFromLeft(10);
        timeLabel.setBounds(topBar.removeFromRight(110));
        globalSeekBar.setBounds(topBar);

        viewport.setBounds(area);
        if (listContainer != nullptr)
        {
            listContainer->setSize(viewport.getWidth() - 15, 64 * 35);
            for (int i = 0; i < 64; ++i) {
                if (i < rows.size() && rows[i] != nullptr)
                    rows[i]->setBounds(0, i * 35, listContainer->getWidth(), 35);
            }
        }
    }

    void timerCallback() override
    {
        if (!isShowing()) return;

        for (auto* row : rows)
        {
            if (row != nullptr) row->updateRowUI();
        }

        double maxLen = processor.getGlobalLengthInSeconds();
        double currentPos = processor.getGlobalPositionInSeconds();

        if (!isDraggingGlobalSeek && maxLen > 0.0) {
            globalSeekBar.setRange(0.0, maxLen);
            globalSeekBar.setValue(currentPos, juce::dontSendNotification);
        }

        auto formatTime = [](double seconds) {
            int mins = (int)(seconds / 60.0);
            int secs = (int)(std::fmod(seconds, 60.0));
            return juce::String::formatted("%02d:%02d", mins, secs);
        };

        timeLabel.setText(formatTime(currentPos) + " / " + formatTime(maxLen), juce::dontSendNotification);
        globalLoopBtn.setToggleState(processor.isGlobalLooping.load(), juce::dontSendNotification);
    }

private:
    PanoramixMasterAudioProcessor& processor;
    juce::ToggleButton globalLoopBtn;
    juce::Slider globalSeekBar;
    juce::Label timeLabel;
    juce::Viewport viewport;
    std::unique_ptr<juce::Component> listContainer;
    juce::Array<TrackRowComponent*> rows;
    bool isDraggingGlobalSeek = false;
};

class PlaylistWindow : public juce::DocumentWindow
{
public:
    PlaylistWindow(PanoramixMasterAudioProcessor& p)
        : DocumentWindow("64-Channel Audio Playlist & Mixer",
                         juce::Desktop::getInstance().getDefaultLookAndFeel()
                             .findColour(juce::ResizableWindow::backgroundColourId),
                         DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new PlaylistContentComponent(p), true);
        setResizable(true, false);
        setResizeLimits(950, 350, 1600, 1000);
        centreWithSize(1120, 620);
    }

    ~PlaylistWindow() override
    {
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }
};

class DualRadarComponent : public juce::Component, public juce::Timer
{
public:
    DualRadarComponent(PanoramixMasterAudioProcessor& p) : processor(p)
    {
        setOpaque(true);
        
        for (int i = 1; i <= 128; ++i) {
            idStrings[i] = juce::String(i);
            nodeColours[i] = juce::Colour::fromHSV(std::fmod(i * 0.31f, 1.0f), 0.85f, 0.95f, 1.0f);
        }
        for (int i = 0; i <= 128; ++i) {
            smoothedPositions[i] = {0.0f, 1.0f, 0.0f};
        }
        cachedSources = processor.sharedRegistry->getSourcesArray();
        startTimerHz(60);
    }

    ~DualRadarComponent() override { stopTimer(); }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.fillAll(juce::Colour(0xff121212));
        g.setColour(juce::Colours::darkgrey);
        g.drawRect(bounds, 1.0f);

        auto halfWidth = bounds.getWidth() * 0.5f;
        auto xyBounds = bounds.removeFromLeft(halfWidth);
        
        g.setColour(juce::Colour(0xff2a2a2a));
        g.drawLine(xyBounds.getRight(), xyBounds.getY(), xyBounds.getRight(), xyBounds.getBottom(), 1.5f);

        drawSingleRadar(g, xyBounds, true);
        drawSingleRadar(g, bounds, false);
    }

    void drawSingleRadar(juce::Graphics& g, juce::Rectangle<float> area, bool isXY)
    {
        g.saveState();
        g.reduceClipRegion(area.toNearestInt());

        auto center = area.getCentre();
        float maxMeters = 10.0f / zoomFactor;
        float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.44f;
        float scale = radius / maxMeters;
        
        g.setColour(juce::Colour(0xff2d2d2d));
        for (int i = 1; i <= 50; ++i)
        {
            float m = static_cast<float>(i);
            float r = m * scale;
            if (r > radius * 2.5f) break;
            
            g.drawEllipse(center.x - r, center.y - r, r * 2.0f, r * 2.0f, 1.0f);
            
            if (i % 2 == 0)
            {
                g.setColour(juce::Colours::grey.withAlpha(0.4f));
                g.setFont(9.0f);
                g.drawText(juce::String(m, 0) + "m", center.x + 2.0f, center.y - r - 6.0f, 30, 12, juce::Justification::left);
                g.setColour(juce::Colour(0xff2d2d2d));
            }
        }

        g.setColour(juce::Colours::darkgrey.withAlpha(0.6f));
        g.drawLine(center.x, center.y - radius, center.x, center.y + radius, 1.0f);
        g.drawLine(center.x - radius, center.y, center.x + radius, center.y, 1.0f);

        g.setColour(juce::Colours::lightgrey);
        g.setFont(11.0f);
        if (isXY)
        {
            g.drawText("+y front", center.x - 40, center.y - radius - 15, 80, 15, juce::Justification::centred);
            g.drawText("-y back", center.x - 40, center.y + radius + 2, 80, 15, juce::Justification::centred);
            g.drawText("-x left", center.x - radius - 50, center.y - 8, 45, 15, juce::Justification::right);
            g.drawText("+x right", center.x + radius + 5, center.y - 8, 45, 15, juce::Justification::left);
        }
        else
        {
            g.drawText("+z top", center.x - 40, center.y - radius - 15, 80, 15, juce::Justification::centred);
            g.drawText("-z bottom", center.x - 40, center.y + radius + 2, 80, 15, juce::Justification::centred);
            g.drawText("-x left", center.x - radius - 50, center.y - 8, 45, 15, juce::Justification::right);
            g.drawText("+x right", center.x + radius + 5, center.y - 8, 45, 15, juce::Justification::left);
        }

        float rulerMeters = 2.0f;
        float rulerPx = rulerMeters * scale;
        float rx = area.getRight() - rulerPx - 15.0f;
        float ry = area.getBottom() - 15.0f;
        
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.drawLine(rx, ry, rx + rulerPx, ry, 1.5f);
        g.drawLine(rx, ry - 3.0f, rx, ry + 3.0f, 1.5f);
        g.drawLine(rx + rulerPx, ry - 3.0f, rx + rulerPx, ry + 3.0f, 1.5f);
        g.setFont(10.0f);
        g.drawText(juce::String(rulerMeters, 2) + " m", rx, ry - 14.0f, rulerPx, 12, juce::Justification::centred);

        int currentMode = static_cast<int>(processor.apvts.getRawParameterValue("plugin_mode")->load());
        int currentId = static_cast<int>(processor.apvts.getRawParameterValue("source_id")->load());

        for (int id = 1; id <= 128; ++id)
        {
            const auto& data = cachedSources[id];
            
            if (!data.showInRadar || (!data.isActive && !(currentMode == 0 && currentId == id)))
                continue;

            bool isSelected = (currentMode == 0 && currentId == id) ||
                              (currentMode == 1 && selectedSourceIDs.find(id) != selectedSourceIDs.end());

            bool shouldDim = (currentMode == 0) && !isSelected;

            float posX = smoothedPositions[id][0];
            float posY = isXY ? smoothedPositions[id][1] : smoothedPositions[id][2];

            float px = center.x + (posX * scale);
            float py = center.y - (posY * scale);
            
            float nodeRadius = isSelected ? 8.0f : 6.0f;

            float trackAudioLevel = 0.0f;
            if (id >= 1 && id <= 64 && processor.audioTracks[id - 1] != nullptr)
            {
                trackAudioLevel = processor.audioTracks[id - 1]->currentLevel.load(std::memory_order_relaxed);
            }

            if (trackAudioLevel > 0.0001f)
            {
                float normLevel = juce::jlimit(0.0f, 1.0f, (juce::Decibels::gainToDecibels(trackAudioLevel, -60.0f) + 60.0f) / 60.0f);
                
                float auraRadius = nodeRadius + (normLevel * 14.0f);
                g.setColour(nodeColours[id].withAlpha(0.25f + (normLevel * 0.35f)));
                g.fillEllipse(px - auraRadius, py - auraRadius, auraRadius * 2.0f, auraRadius * 2.0f);

                float ringRadius = nodeRadius + (normLevel * 8.0f);
                g.setColour(nodeColours[id].withAlpha(0.6f + (normLevel * 0.4f)));
                g.drawEllipse(px - ringRadius, py - ringRadius, ringRadius * 2.0f, ringRadius * 2.0f, 1.5f);
            }

            if (isSelected)
            {
                g.setColour(juce::Colours::white);
                g.drawEllipse(px - 10.0f, py - 10.0f, 20.0f, 20.0f, 1.0f);
                g.setColour(nodeColours[id]);
            }
            else
            {
                if (shouldDim) {
                    g.setColour(nodeColours[id].withAlpha(0.25f));
                } else {
                    g.setColour(nodeColours[id]);
                }
            }

            g.fillEllipse(px - nodeRadius, py - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);
            
            if (data.isLocked)
            {
                g.setColour(juce::Colours::orange);
                float sz = nodeRadius * 0.75f;
                g.drawLine(px - sz, py - sz, px + sz, py + sz, 1.5f);
                g.drawLine(px + sz, py - sz, px - sz, py + sz, 1.5f);
            }

            juce::String nodeName = data.trackName.isNotEmpty() ? data.trackName : ("Node " + idStrings[id]);

            if (isSelected)
            {
                g.setColour(juce::Colours::white);
                g.setFont(11.0f);
                juce::String labelText = isXY ?
                    nodeName + " (" + juce::String(posX, 1) + ", " + juce::String(smoothedPositions[id][1], 1) + ")" :
                    nodeName + " (" + juce::String(posY, 1) + "m)";
                g.drawText(labelText, px - 60.0f, py + nodeRadius + 3.0f, 120, 15, juce::Justification::centred);
            }
            else
            {
                if (shouldDim) {
                    g.setColour(juce::Colours::lightgrey.withAlpha(0.3f));
                } else {
                    g.setColour(juce::Colours::lightgrey.withAlpha(0.8f));
                }
                g.setFont(10.0f);
                g.drawText(nodeName, px - 30.0f, py + nodeRadius + 2.0f, 60, 12, juce::Justification::centred);
            }
        }

        g.restoreState();
    }

    void timerCallback() override
    {
        if (!isShowing()) return;

        cachedSources = processor.sharedRegistry->getSourcesArray();
        
        bool shouldRepaint = isDragging;
        int currentMode = static_cast<int>(processor.apvts.getRawParameterValue("plugin_mode")->load());
        int activeApvtsId = static_cast<int>(processor.apvts.getRawParameterValue("source_id")->load());

        for (int id = 1; id <= 128; ++id)
        {
            const auto& data = cachedSources[id];
            bool isActiveNode = data.showInRadar && (data.isActive || (currentMode == 0 && activeApvtsId == id));
            
            if (isActiveState[id] != isActiveNode) {
                isActiveState[id] = isActiveNode;
                shouldRepaint = true;
            }

            if (!isActiveNode) continue;

            float targetX = data.x;
            float targetY = data.y;
            float targetZ = data.z;
            auto& sm = smoothedPositions[id];
            bool isApvtsAutomating = (id == activeApvtsId) && ((juce::Time::getMillisecondCounter() - data.lastApvtsUpdateTime) < 250);

            if (data.isDraggedLocally || isApvtsAutomating)
            {
                if (sm[0] != targetX || sm[1] != targetY || sm[2] != targetZ) shouldRepaint = true;
                sm[0] = targetX; sm[1] = targetY; sm[2] = targetZ;
            }
            else
            {
                if (std::abs(sm[0] - targetX) > 0.002f || std::abs(sm[1] - targetY) > 0.002f || std::abs(sm[2] - targetZ) > 0.002f)
                {
                    float alpha = 0.35f;
                    sm[0] += (targetX - sm[0]) * alpha;
                    sm[1] += (targetY - sm[1]) * alpha;
                    sm[2] += (targetZ - sm[2]) * alpha;
                    shouldRepaint = true;
                }
                else
                {
                    if (sm[0] != targetX || sm[1] != targetY || sm[2] != targetZ) shouldRepaint = true;
                    sm[0] = targetX; sm[1] = targetY; sm[2] = targetZ;
                }
            }

            if (id >= 1 && id <= 64 && processor.audioTracks[id - 1] != nullptr) {
                if (processor.audioTracks[id - 1]->currentLevel.load(std::memory_order_relaxed) > 0.0001f)
                    shouldRepaint = true;
            }
        }
        
        if (shouldRepaint) repaint();
    }
    
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        zoomFactor += wheel.deltaY * 1.5f;
        zoomFactor = juce::jlimit(0.2f, 5.0f, zoomFactor);
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int currentMode = static_cast<int>(processor.apvts.getRawParameterValue("plugin_mode")->load());
        auto bounds = getLocalBounds().toFloat();
        auto halfWidth = bounds.getWidth() * 0.5f;
        auto xyBounds = bounds.removeFromLeft(halfWidth);
        
        bool clickedOnXY = (e.position.x < halfWidth);
        auto area = clickedOnXY ? xyBounds : bounds;
        auto center = area.getCentre();
        float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.44f;
        float scale = radius / (10.0f / zoomFactor);

        float clickValX = (e.position.x - center.x) / scale;
        float clickValY = (center.y - e.position.y) / scale;

        if (e.mods.isPopupMenu())
        {
            float minDistance = 38.0f;
            int rightClickedID = -1;

            for (int id = 1; id <= 128; ++id)
            {
                const auto& data = cachedSources[id];
                if (!data.isActive || !data.showInRadar) continue;

                float targetPosX = data.x;
                float targetPosY = clickedOnXY ? data.y : data.z;
                float px = center.x + (targetPosX * scale);
                float py = center.y - (targetPosY * scale);
                float dist = std::hypot(px - e.position.x, py - e.position.y);
                if (dist < minDistance)
                {
                    minDistance = dist;
                    rightClickedID = id;
                }
            }

            if (rightClickedID != -1)
            {
                SpatialData sd;
                if (processor.sharedRegistry->getSource(rightClickedID, sd))
                {
                    juce::PopupMenu menu;
                    menu.addItem(1, sd.isLocked ? "Unlock Node Position" : "Lock Node Position");

                    juce::Component::SafePointer<DualRadarComponent> safeThis(this);
                    menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis, rightClickedID](int result) {
                        if (safeThis == nullptr) return;
                        if (result == 1)
                        {
                            SpatialData dataToToggle;
                            if (safeThis->processor.sharedRegistry->getSource(rightClickedID, dataToToggle))
                            {
                                dataToToggle.isLocked = !dataToToggle.isLocked;
                                safeThis->processor.sharedRegistry->updateSource(dataToToggle);
                            }
                        }
                    });
                }
            }
            return;
        }

        if (currentMode == 0)
        {
            int myId = static_cast<int>(processor.apvts.getRawParameterValue("source_id")->load());
            SpatialData data;
            if (processor.sharedRegistry->getSource(myId, data))
            {
                if (data.isLocked || !data.showInRadar) return;

                float targetPosX = data.x;
                float targetPosY = clickedOnXY ? data.y : data.z;
                float px = center.x + (targetPosX * scale);
                float py = center.y - (targetPosY * scale);

                if (std::hypot(px - e.position.x, py - e.position.y) < 38.0f)
                {
                    isDragging = true;
                    isDraggingXY = clickedOnXY;
                    dragX = data.x;
                    dragY = data.y;
                    dragZ = data.z;
                    
                    processor.isDraggingNode.store(true);
                    beginAllGestures();
                    
                    data.isDraggedLocally = true;
                    data.isActive = true;
                    data.lastLocalDragTime = (juce::int64)juce::Time::getMillisecondCounter();
                    processor.sharedRegistry->updateSource(data);
                    
                    touchParameters(data);
                    updatePositionFromMouse(e, area, clickedOnXY);
                }
            }
        }
        else if (currentMode == 1)
        {
            float minDistance = 38.0f;
            int clickedSourceID = -1;

            for (int id = 1; id <= 128; ++id)
            {
                const auto& data = cachedSources[id];
                if (!data.isActive || !data.showInRadar) continue;

                float targetPosX = data.x;
                float targetPosY = clickedOnXY ? data.y : data.z;
                float px = center.x + (targetPosX * scale);
                float py = center.y - (targetPosY * scale);
                float dist = std::hypot(px - e.position.x, py - e.position.y);
                if (dist < minDistance)
                {
                    minDistance = dist;
                    clickedSourceID = id;
                }
            }

            if (clickedSourceID != -1)
            {
                SpatialData clickedSd;
                processor.sharedRegistry->getSource(clickedSourceID, clickedSd);

                if (clickedSd.isLocked) return;

                bool modifier = e.mods.isCommandDown() || e.mods.isShiftDown();
                if (modifier)
                {
                    if (selectedSourceIDs.find(clickedSourceID) != selectedSourceIDs.end())
                    {
                        selectedSourceIDs.erase(clickedSourceID);
                        return;
                    }
                    else
                    {
                        selectedSourceIDs.insert(clickedSourceID);
                    }
                }
                else
                {
                    if (selectedSourceIDs.find(clickedSourceID) == selectedSourceIDs.end())
                    {
                        selectedSourceIDs.clear();
                        selectedSourceIDs.insert(clickedSourceID);
                    }
                }

                primaryDragID = clickedSourceID;

                if (auto* p = processor.apvts.getParameter("source_id")) {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost(processor.apvts.getParameterRange("source_id").convertTo0to1(static_cast<float>(clickedSourceID)));
                    p->endChangeGesture();
                }

                isDragging = true;
                isDraggingXY = clickedOnXY;
                dragStartMousePos = e.position;
                dragStartValX = clickValX;
                dragStartValY = clickValY;

                SpatialData primarySd;
                if (processor.sharedRegistry->getSource(primaryDragID, primarySd))
                {
                    dragX = primarySd.x;
                    dragY = primarySd.y;
                    dragZ = primarySd.z;
                }

                processor.isDraggingNode.store(true);
                beginAllGestures();

                for (int id : selectedSourceIDs)
                {
                    SpatialData data;
                    if (processor.sharedRegistry->getSource(id, data))
                    {
                        if (data.isLocked) continue;

                        selectedInitialPositions[id] = { data.x, data.y, data.z };
                        
                        data.groupLeader = primaryDragID;
                        data.relX = data.x - primarySd.x;
                        data.relY = data.y - primarySd.y;
                        data.relZ = data.z - primarySd.z;

                        data.remoteDragStarted = true;
                        data.isBeingDraggedRemotely = true;
                        data.isDraggedLocally = true;
                        data.isActive = true;
                        data.hasBeenPushedToAPVTS = false;
                        data.lastLocalDragTime = (juce::int64)juce::Time::getMillisecondCounter();
                        processor.sharedRegistry->updateSource(data);
                    }
                }

                SpatialData primaryData;
                if (processor.sharedRegistry->getSource(primaryDragID, primaryData)) {
                    touchParameters(primaryData);
                }
            }
            else
            {
                if (!e.mods.isCommandDown() && !e.mods.isShiftDown())
                {
                    selectedSourceIDs.clear();
                }
            }
        }
    }
            
    void mouseDrag(const juce::MouseEvent& e) override
    {
        int currentMode = static_cast<int>(processor.apvts.getRawParameterValue("plugin_mode")->load());
        if (isDragging)
        {
            auto bounds = getLocalBounds().toFloat();
            auto halfWidth = bounds.getWidth() * 0.5f;
            auto xyBounds = bounds.removeFromLeft(halfWidth);
            auto area = isDraggingXY ? xyBounds : bounds;

            if (currentMode == 0)
            {
                updatePositionFromMouse(e, area, isDraggingXY);
            }
            else if (currentMode == 1 && !selectedSourceIDs.empty())
            {
                auto center = area.getCentre();
                float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.44f;
                float scale = radius / (10.0f / zoomFactor);

                float currentValX = (e.position.x - center.x) / scale;
                float currentValY = (center.y - e.position.y) / scale;

                float totalDeltaX = currentValX - dragStartValX;
                float totalDeltaY = currentValY - dragStartValY;

                for (int id : selectedSourceIDs)
                {
                    SpatialData data;
                    if (processor.sharedRegistry->getSource(id, data))
                    {
                        if (data.isLocked) continue;

                        float initX = selectedInitialPositions[id][0];
                        float initY = selectedInitialPositions[id][1];
                        float initZ = selectedInitialPositions[id][2];
                        
                        data.x = juce::jlimit(-50.0f, 50.0f, initX + totalDeltaX);
                        data.y = juce::jlimit(-50.0f, 50.0f, isDraggingXY ? (initY + totalDeltaY) : initY);
                        data.z = juce::jlimit(-50.0f, 50.0f, isDraggingXY ? initZ : (initZ + totalDeltaY));

                        data.distance = std::sqrt(data.x*data.x + data.y*data.y + data.z*data.z);
                        data.azimuth = std::atan2(data.x, data.y) * (180.0f / juce::MathConstants<float>::pi);
                        data.elevation = std::atan2(data.z, std::sqrt(data.x*data.x + data.y*data.y)) * (180.0f / juce::MathConstants<float>::pi);
                        
                        data.isLocal = true;
                        data.hasBeenPushedToAPVTS = false;
                        data.isBeingDraggedRemotely = true;
                        data.isDraggedLocally = true;
                        data.isActive = true;
                        data.groupLeader = primaryDragID;
                        
                        SpatialData primarySd;
                        if (processor.sharedRegistry->getSource(primaryDragID, primarySd)) {
                            data.relX = data.x - primarySd.x;
                            data.relY = data.y - primarySd.y;
                            data.relZ = data.z - primarySd.z;
                        }

                        data.lastLocalDragTime = (juce::int64)juce::Time::getMillisecondCounter();
                        processor.sharedRegistry->updateSource(data);

                        if (id == primaryDragID)
                        {
                            touchParameters(data);
                        }
                    }
                }
            }
        }
    }
            
    void mouseUp(const juce::MouseEvent&) override
    {
        if (isDragging)
        {
            int currentMode = static_cast<int>(processor.apvts.getRawParameterValue("plugin_mode")->load());
            
            endAllGestures();
            processor.isDraggingNode.store(false);

            if (currentMode == 0)
            {
                int myId = static_cast<int>(processor.apvts.getRawParameterValue("source_id")->load());
                SpatialData sd;
                if (processor.sharedRegistry->getSource(myId, sd)) {
                    sd.isDraggedLocally = false;
                    processor.sharedRegistry->updateSource(sd);
                }
            }
            else if (currentMode == 1)
            {
                for (int id : selectedSourceIDs)
                {
                    SpatialData sd;
                    if (processor.sharedRegistry->getSource(id, sd))
                    {
                        sd.remoteDragEnded = true;
                        sd.isBeingDraggedRemotely = false;
                        sd.isDraggedLocally = false;
                        sd.hasBeenPushedToAPVTS = false;
                        processor.sharedRegistry->updateSource(sd);
                    }
                }
            }

            isDragging = false;
        }
    }

private:
    PanoramixMasterAudioProcessor& processor;
    float zoomFactor = 1.0f;
    bool isDragging = false;
    bool isDraggingXY = true;
    
    int primaryDragID {-1};
    float dragStartValX {0.0f};
    float dragStartValY {0.0f};
    
    float dragX = 0.0f;
    float dragY = 0.0f;
    float dragZ = 0.0f;

    juce::Point<float> dragStartMousePos;
    std::unordered_set<int> selectedSourceIDs;
    std::array<SpatialData, 129> cachedSources;
    
    std::array<bool, 129> isActiveState {};
    std::array<juce::String, 129> idStrings;
    std::array<juce::Colour, 129> nodeColours;
    std::array<std::array<float, 3>, 129> selectedInitialPositions {};
    std::array<std::array<float, 3>, 129> smoothedPositions {};

    void touchParameters(const SpatialData& data)
    {
        auto setParam = [&](const juce::String& paramId, float val) {
            if (auto* p = processor.apvts.getParameter(paramId)) {
                p->setValueNotifyingHost(processor.apvts.getParameterRange(paramId).convertTo0to1(val));
            }
        };

        setParam("x", data.x);
        setParam("y", data.y);
        setParam("z", data.z);
        processor.lastX.store(data.x);
        processor.lastY.store(data.y);
        processor.lastZ.store(data.z);
    }

    void updatePositionFromMouse(const juce::MouseEvent& e, juce::Rectangle<float> area, bool isXY)
    {
        auto center = area.getCentre();
        float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.44f;
        float scale = radius / (10.0f / zoomFactor);

        float valX = juce::jlimit(-50.0f, 50.0f, (e.position.x - center.x) / scale);
        float valY = juce::jlimit(-50.0f, 50.0f, (center.y - e.position.y) / scale);

        float cx = isDragging ? dragX : processor.apvts.getRawParameterValue("x")->load();
        float cy = isDragging ? dragY : processor.apvts.getRawParameterValue("y")->load();
        float cz = isDragging ? dragZ : processor.apvts.getRawParameterValue("z")->load();

        if (isXY)
        {
            cx = valX;
            cy = valY;
        }
        else
        {
            cx = valX;
            cz = valY;
        }
        
        dragX = cx;
        dragY = cy;
        dragZ = cz;

        auto setParam = [&](const juce::String& id, float val) {
            if (auto* p = processor.apvts.getParameter(id))
            {
                p->setValueNotifyingHost(processor.apvts.getParameterRange(id).convertTo0to1(val));
            }
        };

        setParam("x", cx);
        setParam("y", cy);
        setParam("z", cz);
        
        int currentMode = static_cast<int>(processor.apvts.getRawParameterValue("plugin_mode")->load());
        if (currentMode == 0)
        {
            SpatialData sd;
            int myId = static_cast<int>(processor.apvts.getRawParameterValue("source_id")->load());
            if (processor.sharedRegistry->getSource(myId, sd))
            {
                if (sd.isLocked) return;

                sd.x = cx; sd.y = cy; sd.z = cz;
                sd.distance = std::sqrt(cx*cx + cy*cy + cz*cz);
                sd.azimuth = std::atan2(cx, cy) * (180.0f / juce::MathConstants<float>::pi);
                sd.elevation = std::atan2(cz, std::sqrt(cx*cx + cy*cy)) * (180.0f / juce::MathConstants<float>::pi);
                sd.isLocal = true;
                sd.hasBeenPushedToAPVTS = true;
                sd.isActive = true;
                sd.showInRadar = true;
                sd.lastLocalDragTime = (juce::int64)juce::Time::getMillisecondCounter();
                processor.sharedRegistry->updateSource(sd);
                
                processor.lastX.store(cx);
                processor.lastY.store(cy);
                processor.lastZ.store(cz);
            }
        }
    }

    void beginAllGestures()
    {
        const juce::String params[] = {"source_id", "x", "y", "z"};
        for (auto& id : params) if (auto* p = processor.apvts.getParameter(id)) p->beginChangeGesture();
    }

    void endAllGestures()
    {
        const juce::String params[] = {"source_id", "x", "y", "z"};
        for (auto& id : params) if (auto* p = processor.apvts.getParameter(id)) p->endChangeGesture();
    }
};

class PanoramixMasterAudioProcessorEditor : public juce::AudioProcessorEditor,
                                             public juce::Timer
{
public:
    PanoramixMasterAudioProcessorEditor (PanoramixMasterAudioProcessor&);
    ~PanoramixMasterAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void updateUIVisibility();
    void timerCallback() override;
    void triggerNextMissingFileLocate();

private:
    PanoramixMasterAudioProcessor& audioProcessor;
    juce::Label titleLabel;
    juce::ComboBox modeCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;

    juce::TextButton optionsButton {"Options"};
    juce::TextButton showPlaylistButton {"Playlist"};
    juce::TextButton playAudioButton {"Play All"};
    juce::TextButton engineModeBtn {"RAM Mode"};

    juce::Label idLabel;
    juce::Slider idSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> idAttachment;

    juce::Label ipLabel, portLabel, recvPortLabel;
    juce::TextEditor ipEditor, portEditor, recvPortEditor;
    juce::Label rxLabel;
    juce::TextButton connectButton;
    juce::Label statusLabel;

    juce::Label morphLabel;
    juce::Slider morphSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> morphAttachment;
    juce::ToggleButton storeToggle {"Store"};
    juce::TextButton presetButtons[4];

    juce::ToggleButton motionToggle {"Auto Motion"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> motionToggleAttachment;
    
    juce::ComboBox motionShapeCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> motionShapeAttachment;

    juce::Label motionRateLabel, motionRadiusLabel, motionSpreadLabel;
    juce::Slider motionRateSlider, motionRadiusSlider, motionSpreadSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> motionRateAttachment, motionRadiusAttachment, motionSpreadAttachment;

    std::unique_ptr<DualRadarComponent> radarComponent;
    std::unique_ptr<PlaylistWindow> playlistWindow;
    std::unique_ptr<juce::FileChooser> locateFileChooser;
    bool isLocatingMissingFile {false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanoramixMasterAudioProcessorEditor)
};
