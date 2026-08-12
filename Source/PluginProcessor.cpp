#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cctype>
#include <cstring>

void PanoramixMasterAudioProcessor::NetworkThread::run()
{
    struct SentState {
        float x = -999.0f;
        float y = -999.0f;
        float z = -999.0f;
        juce::String trackName = "";
    };
    std::array<SentState, 129> lastSent;
    bool wasConnectedLastLoop = false;

    while (!threadShouldExit())
    {
        bool currentlyConnected = processor.isNetworkConnected.load();

        if (currentlyConnected && !wasConnectedLastLoop)
        {
            for (auto& state : lastSent)
                state = SentState();
        }
        wasConnectedLastLoop = currentlyConnected;

        if (static_cast<int>(processor.pluginModeParam->load()) == 1 && currentlyConnected)
        {
            juce::OSCBundle bundle;
            auto sources = processor.sharedRegistry->getSourcesArray();
            
            for (int id = 1; id <= 128; ++id)
            {
                const auto& data = sources[id];
                if (data.isLocal && data.isActive && data.showInRadar)
                {
                    auto& last = lastSent[id];
                    
                    if (std::abs(data.x - last.x) > 0.002f ||
                        std::abs(data.y - last.y) > 0.002f ||
                        std::abs(data.z - last.z) > 0.002f)
                    {
                        juce::String trackPrefix = "/track/" + juce::String(id);
                        bundle.addElement(juce::OSCMessage(trackPrefix + "/xyz", data.x, data.y, data.z));
                        bundle.addElement(juce::OSCMessage(trackPrefix + "/aed", data.azimuth, data.elevation, data.distance));
                        
                        last.x = data.x;
                        last.y = data.y;
                        last.z = data.z;
                    }
                    
                    if (data.trackName != last.trackName && data.trackName.isNotEmpty())
                    {
                        bundle.addElement(juce::OSCMessage("/track/" + juce::String(id) + "/name", data.trackName));
                        last.trackName = data.trackName;
                    }
                }
            }

            if (bundle.size() > 0)
            {
                try {
                    processor.oscSender.send(bundle);
                } catch (...) {}
            }
        }

        auto now = juce::Time::getMillisecondCounter();
        auto lastRx = processor.lastHeartbeatRxTime.load();
        processor.isHeartbeatAlive.store((lastRx > 0) && ((now - lastRx) < 3000));

        wait(15);
    }
}

PanoramixMasterAudioProcessor::PanoramixMasterAudioProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::discreteChannels(64), true)
                     #endif
                       ),
       apvts (*this, nullptr, juce::Identifier ("Parameters"), createParameterLayout()),
       diskReadThread("DiskReadThread")
{
    diskReadThread.startThread(juce::Thread::Priority::normal);

    pluginModeParam = apvts.getRawParameterValue("plugin_mode");
    sourceIdParam = apvts.getRawParameterValue("source_id");
    azimParam = apvts.getRawParameterValue("azim");
    elevParam = apvts.getRawParameterValue("elev");
    distParam = apvts.getRawParameterValue("dist");
    xParam = apvts.getRawParameterValue("x");
    yParam = apvts.getRawParameterValue("y");
    zParam = apvts.getRawParameterValue("z");
    morphTimeParam = apvts.getRawParameterValue("morph_time");

    motionEnableParam = apvts.getRawParameterValue("motion_enable");
    motionShapeParam = apvts.getRawParameterValue("motion_shape");
    motionRateParam = apvts.getRawParameterValue("motion_rate");
    motionRadiusParam = apvts.getRawParameterValue("motion_radius");
    motionSpreadParam = apvts.getRawParameterValue("motion_spread");

    for (int slot = 0; slot < 4; ++slot) {
        for (int id = 1; id <= 128; ++id) {
            presetSlots[slot][id] = {0.0f, 1.0f, 0.0f};
        }
    }

    {
        auto sourcesSnapshot = sharedRegistry->getSourcesArray();
        int freeId = 1;
        for (int id = 1; id <= 128; ++id) {
            if (!sourcesSnapshot[id].isActive) { freeId = id; break; }
        }
        if (auto* p = apvts.getParameter("source_id"))
            p->setValueNotifyingHost(apvts.getParameterRange("source_id").convertTo0to1(static_cast<float>(freeId)));
    }

    formatManager.registerBasicFormats();

    for (int i = 0; i < 64; ++i) {
        audioTracks[i] = std::make_unique<AudioTrack>();
        audioTracks[i]->outCh.store(i);
    }

    netThread = std::make_unique<NetworkThread>(*this);
    netThread->startThread(juce::Thread::Priority::normal);

    startTimerHz(60);
    oscReceiver.addListener(this);
}

PanoramixMasterAudioProcessor::~PanoramixMasterAudioProcessor()
{
    stopTimer();
    oscReceiver.removeListener(this);
    oscReceiver.disconnect();
    disconnectNetwork();

    if (netThread != nullptr) {
        netThread->stopThread(1000);
    }

    suspendProcessing(true);

    if (pluginModeParam != nullptr && static_cast<int>(pluginModeParam->load()) == 0)
    {
        int currentId = sourceIdParam != nullptr ? static_cast<int>(sourceIdParam->load()) : 1;
        SpatialData sd;
        if (sharedRegistry->getSource(currentId, sd))
        {
            sd.isActive = false;
            sharedRegistry->updateSource(sd);
        }
    }

    for (int i = 0; i < 64; ++i) {
        if (audioTracks[i]) {
            juce::SpinLock::ScopedLockType sl(audioTracks[i]->processLock);
            if (audioTracks[i]->transportSource != nullptr) {
                audioTracks[i]->transportSource->stop();
                audioTracks[i]->transportSource->setSource(nullptr);
            }
            audioTracks[i]->transportSource.reset();
            audioTracks[i]->readerSource.reset();
        }
    }
    diskReadThread.stopThread(1000);
}

juce::AudioProcessorValueTreeState::ParameterLayout PanoramixMasterAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID("plugin_mode", 1), "Plugin Mode", juce::StringArray{"Node", "Master"}, 0));
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID("source_id", 1), "Source ID", 1, 128, 1));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("azim", 1), "Azimuth", juce::NormalisableRange<float>(-180.0f, 180.0f, 0.1f), 0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("elev", 1), "Elevation", juce::NormalisableRange<float>(-90.0f, 90.0f, 0.1f), 0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("dist", 1), "Distance", juce::NormalisableRange<float>(0.0f, 50.0f, 0.01f), 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("x", 1), "X", juce::NormalisableRange<float>(-50.0f, 50.0f, 0.01f), 0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("y", 1), "Y", juce::NormalisableRange<float>(-50.0f, 50.0f, 0.01f), 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("z", 1), "Z", juce::NormalisableRange<float>(-50.0f, 50.0f, 0.01f), 0.0f));
    
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("morph_time", 1), "Morph Time", juce::NormalisableRange<float>(0.0f, 10.0f, 0.1f), 2.0f));

    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID("motion_enable", 1), "Auto Motion", false));
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID("motion_shape", 1), "Motion Shape",
        juce::StringArray{"Circle", "Figure-8", "Sine X", "Lissajous", "Spiral", "Random", "Perlin Walk", "3D Helix", "Elliptic Orbit", "Pendulum Arc", "Rose Curve", "Flyby", "Box Bounce"}, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("motion_rate", 1), "Motion Rate", juce::NormalisableRange<float>(0.05f, 5.0f, 0.05f), 0.5f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("motion_radius", 1), "Motion Radius", juce::NormalisableRange<float>(0.5f, 15.0f, 0.1f), 3.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("motion_spread", 1), "Motion Spread", juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));

    return layout;
}

void PanoramixMasterAudioProcessor::resetToNewSession()
{
    suspendProcessing(true);
    stopAudioPlayback();
    clearMissingFiles();

    for (int id = 1; id <= 128; ++id)
    {
        SpatialData sd;
        sd.sourceID = id;
        sd.x = 0.0f;
        sd.y = 1.0f;
        sd.z = 0.0f;
        sd.azimuth = 0.0f;
        sd.elevation = 0.0f;
        sd.distance = 1.0f;
        sd.isActive = false;
        sd.isLocked = false;
        sd.showInRadar = false;
        sd.trackName = "";
        sd.isMute = false;
        sd.isSolo = false;
        sharedRegistry->updateSource(sd);
    }

    for (int i = 0; i < 64; ++i)
    {
        auto* track = audioTracks[i].get();
        if (track != nullptr)
        {
            track->hasAudio.store(false);

            std::unique_ptr<juce::AudioFormatReaderSource> oldReader;
            std::unique_ptr<juce::AudioTransportSource> oldTransport;

            {
                juce::SpinLock::ScopedLockType sl(track->processLock);
                if (track->transportSource != nullptr) {
                    track->transportSource->stop();
                    track->transportSource->setSource(nullptr);
                }
                oldTransport = std::move(track->transportSource);
                oldReader = std::move(track->readerSource);

                track->audioData.clear();
                track->audioData.setSize(0, 0);
                track->fileName = "No File";
                track->currentFile = juce::File();
                track->playPosition.store(0);
                track->gain.store(1.0f);
                track->previousGain.store(1.0f);
                track->mute.store(false);
                track->solo.store(false);
                track->outCh.store(i);
                track->currentLevel.store(0.0f);
            }
        }
    }

    auto resetParam = [this](const juce::String& paramID, float defaultValue) {
        if (auto* p = apvts.getParameter(paramID)) {
            p->setValueNotifyingHost(apvts.getParameterRange(paramID).convertTo0to1(defaultValue));
        }
    };

    resetParam("plugin_mode", 0.0f);
    resetParam("source_id", 1.0f);
    resetParam("x", 0.0f);
    resetParam("y", 1.0f);
    resetParam("z", 0.0f);
    resetParam("azim", 0.0f);
    resetParam("elev", 0.0f);
    resetParam("dist", 1.0f);
    resetParam("morph_time", 2.0f);
    resetParam("motion_enable", 0.0f);
    resetParam("motion_shape", 0.0f);
    resetParam("motion_rate", 0.5f);
    resetParam("motion_radius", 3.0f);
    resetParam("motion_spread", 0.0f);

    isGlobalLooping.store(true);
    isMorphing.store(false);
    wasMotionEnabled = false;
    isRemoteGestureActive = false;

    suspendProcessing(false);
}

void PanoramixMasterAudioProcessor::setEngineMode(int newMode)
{
    if (engineMode.load() == newMode) return;
    
    suspendProcessing(true);
    stopAudioPlayback();

    std::array<juce::File, 64> filesToReload;
    std::array<bool, 64> hasFiles;
    for (int i = 0; i < 64; ++i) {
        if (audioTracks[i] && audioTracks[i]->hasAudio.load() && audioTracks[i]->currentFile.existsAsFile()) {
            filesToReload[i] = audioTracks[i]->currentFile;
            hasFiles[i] = true;
        } else {
            hasFiles[i] = false;
        }
    }

    engineMode.store(newMode);

    for (int i = 0; i < 64; ++i) {
        auto* track = audioTracks[i].get();
        if (track) {
            track->hasAudio.store(false);
            
            std::unique_ptr<juce::AudioFormatReaderSource> oldReader;
            std::unique_ptr<juce::AudioTransportSource> oldTransport;

            {
                juce::SpinLock::ScopedLockType sl(track->processLock);
                if (track->transportSource != nullptr) {
                    track->transportSource->stop();
                    track->transportSource->setSource(nullptr);
                }
                oldTransport = std::move(track->transportSource);
                oldReader = std::move(track->readerSource);
                
                track->audioData.clear();
                track->audioData.setSize(0, 0);
                track->fileName = "No File";
                track->currentFile = juce::File();
                track->playPosition.store(0);
                track->currentLevel.store(0.0f);
            }
        }
    }

    for (int i = 0; i < 64; ++i) {
        if (hasFiles[i]) {
            loadAudioFile(i + 1, filesToReload[i]);
        }
    }

    suspendProcessing(false);
}

void PanoramixMasterAudioProcessor::loadAudioFile(int trackId, const juce::File& file)
{
    if (trackId < 1 || trackId > 64) return;
    int idx = trackId - 1;
    auto* track = audioTracks[idx].get();
    if (track == nullptr) return;
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader != nullptr)
    {
        int numCh = (int)reader->numChannels;

        if (engineMode.load() == 0) // RAM Mode Multichannel
        {
            juce::AudioBuffer<float> tempBuf (numCh, (int)reader->lengthInSamples);
            reader->read (&tempBuf, 0, (int)reader->lengthInSamples, 0, true, true);

            double targetRate = (lastSampleRate > 0.0) ? lastSampleRate : 48000.0;
            juce::AudioBuffer<float> resampledBuf;
            
            if (reader->sampleRate != targetRate && reader->sampleRate > 0.0)
            {
                double ratio = reader->sampleRate / targetRate;
                int newLength = juce::roundToInt ((double)reader->lengthInSamples / ratio);
                
                resampledBuf.setSize(numCh, newLength + 16);
                resampledBuf.clear();

                for (int ch = 0; ch < numCh; ++ch)
                {
                    juce::LagrangeInterpolator interpolator;
                    interpolator.process (ratio, tempBuf.getReadPointer(ch), resampledBuf.getWritePointer(ch), newLength);
                }
                
                resampledBuf.setSize(numCh, newLength, true);
            }
            else
            {
                resampledBuf = std::move (tempBuf);
            }

            track->hasAudio.store(false);
            
            std::unique_ptr<juce::AudioFormatReaderSource> oldReader;
            std::unique_ptr<juce::AudioTransportSource> oldTransport;

            {
                juce::SpinLock::ScopedLockType sl(track->processLock);
                if (track->transportSource != nullptr) {
                    track->transportSource->stop();
                    track->transportSource->setSource(nullptr);
                }
                oldTransport = std::move(track->transportSource);
                oldReader = std::move(track->readerSource);

                track->numChannels.store(numCh);
                track->currentFile = file;
                track->audioData = std::move(resampledBuf);
                track->playPosition.store(0);
                track->fileName = file.getFileName();
            }
            track->hasAudio.store(true);
        }
        else // Disk Stream Mode Multichannel
        {
            double readerSampleRate = reader->sampleRate;
            auto newReaderSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
            newReaderSource->setLooping(isGlobalLooping.load());

            auto newTransportSource = std::make_unique<juce::AudioTransportSource>();
            newTransportSource->setSource(newReaderSource.get(), 32768, &diskReadThread, readerSampleRate);
            newTransportSource->prepareToPlay(lastBlockSize, lastSampleRate);

            if (globalPlaying.load()) {
                newTransportSource->start();
            }

            track->hasAudio.store(false);
            
            std::unique_ptr<juce::AudioFormatReaderSource> oldReader;
            std::unique_ptr<juce::AudioTransportSource> oldTransport;

            {
                juce::SpinLock::ScopedLockType sl(track->processLock);
                if (track->transportSource != nullptr) {
                    track->transportSource->stop();
                    track->transportSource->setSource(nullptr);
                }
                oldTransport = std::move(track->transportSource);
                oldReader = std::move(track->readerSource);

                track->numChannels.store(numCh);
                track->currentFile = file;
                track->readerSource = std::move(newReaderSource);
                track->transportSource = std::move(newTransportSource);
                track->fileName = file.getFileName();
            }
            track->hasAudio.store(true);
        }
    }
}

void PanoramixMasterAudioProcessor::setGlobalLooping(bool shouldLoop)
{
    isGlobalLooping.store(shouldLoop);
    if (engineMode.load() == 1) {
        for (int i = 0; i < 64; ++i) {
            if (audioTracks[i]) {
                juce::SpinLock::ScopedLockType sl(audioTracks[i]->processLock);
                if (audioTracks[i]->readerSource != nullptr) {
                    audioTracks[i]->readerSource->setLooping(shouldLoop);
                }
            }
        }
    }
}

void PanoramixMasterAudioProcessor::startAudioPlayback()
{
    globalPlaying.store(true);
    for (int i = 0; i < 64; ++i) {
        if (audioTracks[i] && audioTracks[i]->hasAudio.load()) {
            juce::SpinLock::ScopedLockType sl(audioTracks[i]->processLock);
            if (engineMode.load() == 0) {
                audioTracks[i]->playPosition.store(0);
            } else {
                if (audioTracks[i]->transportSource != nullptr) {
                    audioTracks[i]->transportSource->setPosition(0.0);
                    audioTracks[i]->transportSource->start();
                }
            }
        }
    }
}

void PanoramixMasterAudioProcessor::stopAudioPlayback()
{
    globalPlaying.store(false);
    if (engineMode.load() == 1) {
        for (int i = 0; i < 64; ++i) {
            if (audioTracks[i] && audioTracks[i]->transportSource != nullptr) {
                juce::SpinLock::ScopedLockType sl(audioTracks[i]->processLock);
                if (audioTracks[i]->transportSource != nullptr)
                    audioTracks[i]->transportSource->stop();
            }
        }
    }
}

bool PanoramixMasterAudioProcessor::isGlobalPlaying() const
{
    return globalPlaying.load();
}

double PanoramixMasterAudioProcessor::getGlobalLengthInSeconds() const
{
    double maxLen = 0.0;
    for (int i = 0; i < 64; ++i) {
        auto* track = audioTracks[i].get();
        if (track && track->hasAudio.load()) {
            juce::SpinLock::ScopedLockType sl(track->processLock);
            if (engineMode.load() == 0) {
                double sr = (lastSampleRate > 0.0) ? lastSampleRate : 48000.0;
                double len = (double)track->audioData.getNumSamples() / sr;
                maxLen = std::max(maxLen, len);
            } else {
                if (track->transportSource != nullptr) {
                    double len = track->transportSource->getTotalLength() / (lastSampleRate > 0.0 ? lastSampleRate : 48000.0);
                    maxLen = std::max(maxLen, len);
                }
            }
        }
    }
    return maxLen;
}

double PanoramixMasterAudioProcessor::getGlobalPositionInSeconds() const
{
    double maxPos = 0.0;
    for (int i = 0; i < 64; ++i) {
        auto* track = audioTracks[i].get();
        if (track && track->hasAudio.load()) {
            juce::SpinLock::ScopedLockType sl(track->processLock);
            double pos = 0.0;
            if (engineMode.load() == 0) {
                double sr = (lastSampleRate > 0.0) ? lastSampleRate : 48000.0;
                pos = (double)track->playPosition.load() / sr;
            } else {
                if (track->transportSource != nullptr) {
                    pos = track->transportSource->getCurrentPosition();
                }
            }
            maxPos = std::max(maxPos, pos);
        }
    }
    return maxPos;
}

void PanoramixMasterAudioProcessor::setGlobalPositionInSeconds(double pos)
{
    if (engineMode.load() == 0) {
        double sr = (lastSampleRate > 0.0) ? lastSampleRate : 48000.0;
        int64_t samplePos = (int64_t)(pos * sr);
        for (int i = 0; i < 64; ++i) {
            if (audioTracks[i] && audioTracks[i]->hasAudio.load()) {
                audioTracks[i]->playPosition.store(samplePos);
            }
        }
    } else {
        for (int i = 0; i < 64; ++i) {
            if (audioTracks[i] && audioTracks[i]->hasAudio.load()) {
                juce::SpinLock::ScopedLockType sl(audioTracks[i]->processLock);
                if (audioTracks[i]->transportSource != nullptr) {
                    audioTracks[i]->transportSource->setPosition(pos);
                }
            }
        }
    }
}

void PanoramixMasterAudioProcessor::storePreset(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= 4) return;
    auto sources = sharedRegistry->getSourcesArray();
    for (int id = 1; id <= 128; ++id) {
        presetSlots[slotIndex][id] = { sources[id].x, sources[id].y, sources[id].z };
    }
}

void PanoramixMasterAudioProcessor::triggerPreset(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= 4) return;
    auto sources = sharedRegistry->getSourcesArray();

    for (int id = 1; id <= 128; ++id) {
        morphStartPositions[id] = { sources[id].x, sources[id].y, sources[id].z };
        morphTargetPositions[id] = presetSlots[slotIndex][id];
    }

    float seconds = morphTimeParam != nullptr ? morphTimeParam->load() : 2.0f;
    morphDurationMs.store(seconds * 1000.0f);
    morphStartTime = juce::Time::getMillisecondCounter();
    isMorphing.store(true);
}

void PanoramixMasterAudioProcessor::updateTrackProperties(const TrackProperties& properties)
{
    juce::String newName = properties.name.has_value() ? *properties.name : "";
    int currentId = sourceIdParam != nullptr ? static_cast<int>(sourceIdParam->load()) : 1;
    SpatialData sd;
    
    if (sharedRegistry->getSource(currentId, sd))
    {
        if (sd.trackName != newName && newName.isNotEmpty())
        {
            sd.trackName = newName;
            sharedRegistry->updateSource(sd);
        }
    }
}

void PanoramixMasterAudioProcessor::oscBundleReceived(const juce::OSCBundle& bundle)
{
    for (const auto& element : bundle)
    {
        if (element.isMessage())
            oscMessageReceived(element.getMessage());
        else if (element.isBundle())
            oscBundleReceived(element.getBundle());
    }
}

void PanoramixMasterAudioProcessor::oscMessageReceived(const juce::OSCMessage& message)
{
    lastOscMsgTime.store(juce::Time::getMillisecondCounter());

    juce::String rawAddr = message.getAddressPattern().toString();
    if (rawAddr.equalsIgnoreCase("/pong") || rawAddr.equalsIgnoreCase("/ping") || rawAddr.equalsIgnoreCase("/panoramix/pong"))
    {
        lastHeartbeatRxTime.store(juce::Time::getMillisecondCounter());
        return;
    }

    int id = -1;
    const char* paramPtr = nullptr;
    int paramLen = 0;

    if (rawAddr.startsWith("/track/"))
    {
        int secondSlash = rawAddr.indexOf(7, "/");
        if (secondSlash > 7)
        {
            id = rawAddr.substring(7, secondSlash).getIntValue();
            paramPtr = rawAddr.toRawUTF8() + secondSlash + 1;
            paramLen = rawAddr.length() - (secondSlash + 1);
        }
    }

    if (id > 0 && paramPtr != nullptr && paramLen > 0)
    {
        SpatialData data;
        sharedRegistry->getSource(id, data);
        
        if (data.isLocked) return;

        bool updated = false;

        auto getSafeFloat = [](const juce::OSCArgument& arg) -> float {
            if (arg.isFloat32()) return arg.getFloat32();
            if (arg.isInt32()) return static_cast<float>(arg.getInt32());
            return 0.0f;
        };

        auto equalsIgnoreCase = [paramPtr, paramLen](const char* expected) -> bool {
            int expLen = (int)std::strlen(expected);
            if (paramLen != expLen) return false;
            for (int i = 0; i < expLen; ++i)
            {
                if (std::tolower(static_cast<unsigned char>(paramPtr[i])) != std::tolower(static_cast<unsigned char>(expected[i])))
                    return false;
            }
            return true;
        };

        int mode = pluginModeParam != nullptr ? static_cast<int>(pluginModeParam->load()) : 0;
        bool motionActive = motionEnableParam != nullptr && motionEnableParam->load() > 0.5f;
        bool canUpdatePosition = !data.isDraggedLocally && !(mode == 1 && (motionActive || isMorphing.load()));

        if ((equalsIgnoreCase("xyz") || equalsIgnoreCase("pos")) && message.size() >= 3)
        {
            if (canUpdatePosition)
            {
                data.x = getSafeFloat(message[0]);
                data.y = getSafeFloat(message[1]);
                data.z = getSafeFloat(message[2]);
                data.distance = std::sqrt(data.x*data.x + data.y*data.y + data.z*data.z);
                data.azimuth = std::atan2(data.x, data.y) * (180.0f / juce::MathConstants<float>::pi);
                data.elevation = std::atan2(data.z, std::sqrt(data.x*data.x + data.y*data.y)) * (180.0f / juce::MathConstants<float>::pi);
                updated = true;
            }
        }
        else if ((equalsIgnoreCase("aed") || equalsIgnoreCase("aed_deg")) && message.size() >= 3)
        {
            if (canUpdatePosition)
            {
                data.azimuth = getSafeFloat(message[0]);
                data.elevation = getSafeFloat(message[1]);
                data.distance = getSafeFloat(message[2]);
                float azRad = data.azimuth * (juce::MathConstants<float>::pi / 180.0f);
                float elRad = data.elevation * (juce::MathConstants<float>::pi / 180.0f);
                data.x = data.distance * std::cos(elRad) * std::sin(azRad);
                data.y = data.distance * std::cos(elRad) * std::cos(azRad);
                data.z = data.distance * std::sin(elRad);
                updated = true;
            }
        }
        else if ((equalsIgnoreCase("azim") || equalsIgnoreCase("az")) && message.size() >= 1)
        {
            if (canUpdatePosition)
            {
                data.azimuth = getSafeFloat(message[0]);
                float azRad = data.azimuth * (juce::MathConstants<float>::pi / 180.0f);
                float elRad = data.elevation * (juce::MathConstants<float>::pi / 180.0f);
                data.x = data.distance * std::cos(elRad) * std::sin(azRad);
                data.y = data.distance * std::cos(elRad) * std::cos(azRad);
                data.z = data.distance * std::sin(elRad);
                updated = true;
            }
        }
        else if ((equalsIgnoreCase("elev") || equalsIgnoreCase("el")) && message.size() >= 1)
        {
            if (canUpdatePosition)
            {
                data.elevation = getSafeFloat(message[0]);
                float azRad = data.azimuth * (juce::MathConstants<float>::pi / 180.0f);
                float elRad = data.elevation * (juce::MathConstants<float>::pi / 180.0f);
                data.x = data.distance * std::cos(elRad) * std::sin(azRad);
                data.y = data.distance * std::cos(elRad) * std::cos(azRad);
                data.z = data.distance * std::sin(elRad);
                updated = true;
            }
        }
        else if ((equalsIgnoreCase("dist") || equalsIgnoreCase("d")) && message.size() >= 1)
        {
            if (canUpdatePosition)
            {
                data.distance = getSafeFloat(message[0]);
                float azRad = data.azimuth * (juce::MathConstants<float>::pi / 180.0f);
                float elRad = data.elevation * (juce::MathConstants<float>::pi / 180.0f);
                data.x = data.distance * std::cos(elRad) * std::sin(azRad);
                data.y = data.distance * std::cos(elRad) * std::cos(azRad);
                data.z = data.distance * std::sin(elRad);
                updated = true;
            }
        }
        else if (equalsIgnoreCase("x") && message.size() >= 1)
        {
            if (canUpdatePosition)
            {
                data.x = getSafeFloat(message[0]);
                data.distance = std::sqrt(data.x*data.x + data.y*data.y + data.z*data.z);
                data.azimuth = std::atan2(data.x, data.y) * (180.0f / juce::MathConstants<float>::pi);
                data.elevation = std::atan2(data.z, std::sqrt(data.x*data.x + data.y*data.y)) * (180.0f / juce::MathConstants<float>::pi);
                updated = true;
            }
        }
        else if (equalsIgnoreCase("y") && message.size() >= 1)
        {
            if (canUpdatePosition)
            {
                data.y = getSafeFloat(message[0]);
                data.distance = std::sqrt(data.x*data.x + data.y*data.y + data.z*data.z);
                data.azimuth = std::atan2(data.x, data.y) * (180.0f / juce::MathConstants<float>::pi);
                data.elevation = std::atan2(data.z, std::sqrt(data.x*data.x + data.y*data.y)) * (180.0f / juce::MathConstants<float>::pi);
                updated = true;
            }
        }
        else if (equalsIgnoreCase("z") && message.size() >= 1)
        {
            if (canUpdatePosition)
            {
                data.z = getSafeFloat(message[0]);
                data.distance = std::sqrt(data.x*data.x + data.y*data.y + data.z*data.z);
                data.azimuth = std::atan2(data.x, data.y) * (180.0f / juce::MathConstants<float>::pi);
                data.elevation = std::atan2(data.z, std::sqrt(data.x*data.x + data.y*data.y)) * (180.0f / juce::MathConstants<float>::pi);
                updated = true;
            }
        }
        else if (equalsIgnoreCase("name") && message.size() >= 1 && message[0].isString())
        {
            juce::String incomingName = message[0].getString();
            juce::String lowerName = incomingName.toLowerCase().trim();
            
            bool isGenericName = lowerName.startsWith("all track") ||
                                 lowerName.startsWith("all mono")  ||
                                 lowerName.startsWith("all stereo")||
                                 lowerName.startsWith("track ")    ||
                                 lowerName.startsWith("source ")   ||
                                 lowerName == "master";

            if (!isGenericName && data.trackName != incomingName)
            {
                data.trackName = incomingName;
                updated = true;
            }
        }

        if (updated)
        {
            data.showInRadar = true;
            data.isActive = true;

            int currentId = sourceIdParam != nullptr ? static_cast<int>(sourceIdParam->load()) : 1;

            if (mode == 0 && id == currentId)
            {
                lastX.store(data.x);
                lastY.store(data.y);
                lastZ.store(data.z);
            }

            data.isLocal = false;
            data.hasBeenPushedToAPVTS = false;
            data.lastOscReceiveTime = (juce::int64)juce::Time::getMillisecondCounter();
            sharedRegistry->updateSource(data);
        }
    }
}

void PanoramixMasterAudioProcessor::timerCallback()
{
    {
        int modeNow = pluginModeParam != nullptr ? static_cast<int>(pluginModeParam->load()) : 0;
        if (modeNow != lastKnownPluginMode)
        {
            if (modeNow == 1)
                oscReceiver.connect(receivePort);
            else
                oscReceiver.disconnect();

            lastKnownPluginMode = modeNow;
        }
    }

    int pluginMode = pluginModeParam != nullptr ? static_cast<int>(pluginModeParam->load()) : 0;

    // SENDER NODE MODE: Evaluasi parameter di Message Thread
    if (pluginMode == 0)
    {
        int currentId = sourceIdParam != nullptr ? static_cast<int>(sourceIdParam->load()) : 1;
        SpatialData data;

        if (sharedRegistry->getSource(currentId, data) && !data.isLocked)
        {
            bool isCurrentlyRemoteDragging = data.isBeingDraggedRemotely;

            if (isCurrentlyRemoteDragging && !isRemoteGestureActive)
            {
                if (auto* px = apvts.getParameter("x")) px->beginChangeGesture();
                if (auto* py = apvts.getParameter("y")) py->beginChangeGesture();
                if (auto* pz = apvts.getParameter("z")) pz->beginChangeGesture();
                isRemoteGestureActive = true;
            }

            if (!data.hasBeenPushedToAPVTS)
            {
                auto updateParamWithGesture = [&](const juce::String& paramId, float val) {
                    if (auto* p = apvts.getParameter(paramId)) {
                        bool needsInstantGesture = !isCurrentlyRemoteDragging && !isRemoteGestureActive;

                        if (needsInstantGesture)
                            p->beginChangeGesture();

                        p->setValueNotifyingHost(apvts.getParameterRange(paramId).convertTo0to1(val));

                        if (needsInstantGesture)
                            p->endChangeGesture();
                    }
                };

                updateParamWithGesture("x", data.x);
                updateParamWithGesture("y", data.y);
                updateParamWithGesture("z", data.z);

                lastX.store(data.x);
                lastY.store(data.y);
                lastZ.store(data.z);

                data.hasBeenPushedToAPVTS = true;
                sharedRegistry->updateSource(data);
            }

            if (!isCurrentlyRemoteDragging && isRemoteGestureActive)
            {
                if (auto* px = apvts.getParameter("x")) px->endChangeGesture();
                if (auto* py = apvts.getParameter("y")) py->endChangeGesture();
                if (auto* pz = apvts.getParameter("z")) pz->endChangeGesture();
                isRemoteGestureActive = false;
            }
        }
        return;
    }

    // MASTER HUB MODE
    if (pluginMode == 1)
    {
        bool motionEnabled = motionEnableParam != nullptr && motionEnableParam->load() > 0.5f;

        if (motionEnabled)
        {
            auto sources = sharedRegistry->getSourcesArray();
            if (!wasMotionEnabled)
            {
                wasMotionEnabled = true;
                float sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
                int count = 0;
                for (int id = 1; id <= 128; ++id) {
                    motionOriginPositions[id] = { sources[id].x, sources[id].y, sources[id].z };
                    if (sources[id].isActive) {
                        sumX += sources[id].x;
                        sumY += sources[id].y;
                        sumZ += sources[id].z;
                        count++;
                    }
                }
                motionCenterX = (count > 0) ? (sumX / count) : 0.0f;
                motionCenterY = (count > 0) ? (sumY / count) : 0.0f;
                motionCenterZ = (count > 0) ? (sumZ / count) : 0.0f;
            }

            float rate = motionRateParam != nullptr ? motionRateParam->load() : 0.5f;
            float radius = motionRadiusParam != nullptr ? motionRadiusParam->load() : 3.0f;
            float spread = motionSpreadParam != nullptr ? motionSpreadParam->load() : 0.0f;
            int shape = motionShapeParam != nullptr ? static_cast<int>(motionShapeParam->load()) : 0;

            double t = juce::Time::getMillisecondCounterHiRes() * 0.001;
            float baseTheta = static_cast<float>(t * rate * juce::MathConstants<double>::twoPi);

            int nodeIdx = 0;
            for (int id = 1; id <= 128; ++id)
            {
                auto& data = sources[id];
                if (data.isActive && !data.isLocked && !data.isDraggedLocally && !data.isBeingDraggedRemotely)
                {
                    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
                    float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;

                    float delayStep = spread * 0.8f;
                    float theta = baseTheta - (nodeIdx * delayStep);

                    switch (shape)
                    {
                        case 0: // Circle
                            dx = radius * std::cos(theta);
                            dy = radius * std::sin(theta);
                            break;

                        case 1: // Figure-8
                            dx = radius * std::sin(theta);
                            dy = radius * std::sin(theta) * std::cos(theta);
                            break;

                        case 2: // Sine X
                            dx = radius * std::sin(theta);
                            dy = 0.0f;
                            break;

                        case 3: // Lissajous (3:2)
                            dx = radius * std::sin(3.0f * theta);
                            dy = radius * std::sin(2.0f * theta);
                            break;

                        case 4: // Spiral
                        {
                            float r = radius * std::abs(std::sin(theta * 0.25f));
                            dx = r * std::cos(theta);
                            dy = r * std::sin(theta);
                            break;
                        }

                        case 5: // Random Motion
                        {
                            float speedX = rate * (0.7f + 0.5f * std::sin(id * 2.1f));
                            float speedY = rate * (0.7f + 0.5f * std::cos(id * 3.3f));
                            float phaseX = id * 1.7f;
                            float phaseY = id * 2.9f;

                            dx = radius * std::sin(static_cast<float>(t * speedX * juce::MathConstants<double>::twoPi) + phaseX);
                            dy = radius * std::cos(static_cast<float>(t * speedY * juce::MathConstants<double>::twoPi) + phaseY);
                            break;
                        }

                        case 6: // Perlin / Organic Walk
                        {
                            float pT = static_cast<float>(t * rate);
                            dx = radius * (std::sin(pT * 1.3f + id * 1.1f) + 0.5f * std::sin(pT * 2.7f + id * 2.3f)) * 0.66f;
                            dy = radius * (std::cos(pT * 1.1f + id * 1.7f) + 0.5f * std::cos(pT * 3.1f + id * 0.9f)) * 0.66f;
                            dz = radius * 0.5f * std::sin(pT * 0.8f + id * 1.4f);
                            break;
                        }

                        case 7: // 3D Helix / Corkscrew
                            dx = radius * std::cos(theta);
                            dy = radius * std::sin(theta);
                            dz = (radius * 0.6f) * std::sin(theta * 0.5f);
                            break;

                        case 8: // Elliptic Orbit
                            dx = (radius * 1.4f) * std::cos(theta);
                            dy = (radius * 0.6f) * std::sin(theta);
                            break;

                        case 9: // Pendulum Arc
                            dx = radius * std::sin(theta);
                            dy = radius * (1.0f - std::cos(theta)) * 0.35f;
                            break;

                        case 10: // Rose / Rhodonea Curve (k = 3)
                        {
                            float r = radius * std::cos(3.0f * theta);
                            dx = r * std::cos(theta);
                            dy = r * std::sin(theta);
                            break;
                        }

                        case 11: // Flyby / Pass-Through
                        {
                            float modT = std::fmod(theta, juce::MathConstants<float>::twoPi);
                            if (modT < 0.0f) modT += juce::MathConstants<float>::twoPi;
                            float normT = (modT / juce::MathConstants<float>::twoPi) * 2.0f - 1.0f;
                            dx = normT * radius * 2.0f;
                            dy = normT * radius * 0.5f;
                            break;
                        }

                        case 12: // Box Bounce
                        {
                            auto triangleWave = [](float x) {
                                float m = std::fmod(std::abs(x), 4.0f);
                                return (m < 2.0f) ? (m - 1.0f) : (3.0f - m);
                            };
                            float bTime = static_cast<float>(t * rate);
                            dx = radius * triangleWave(bTime * 0.8f + id * 0.5f);
                            dy = radius * triangleWave(bTime * 0.6f + id * 0.7f);
                            dz = (radius * 0.5f) * triangleWave(bTime * 0.4f + id * 0.3f);
                            break;
                        }

                        default: break;
                    }

                    nodeIdx++;

                    if (spread > 0.01f && shape != 5 && shape != 6 && shape != 12)
                    {
                        targetX = motionCenterX + dx;
                        targetY = motionCenterY + dy;
                        targetZ = motionCenterZ + dz;
                    }
                    else
                    {
                        if (shape == 0 || shape == 1 || shape == 3 || shape == 7 || shape == 8 || shape == 10)
                        {
                            targetX = dx;
                            targetY = dy;
                            targetZ = dz;
                        }
                        else
                        {
                            float ox = motionOriginPositions[id][0];
                            float oy = motionOriginPositions[id][1];
                            float oz = motionOriginPositions[id][2];
                            targetX = ox + dx;
                            targetY = oy + dy;
                            targetZ = oz + dz;
                        }
                    }

                    data.x = juce::jlimit(-50.0f, 50.0f, targetX);
                    data.y = juce::jlimit(-50.0f, 50.0f, targetY);
                    data.z = juce::jlimit(-50.0f, 50.0f, targetZ);

                    data.distance = std::sqrt(data.x*data.x + data.y*data.y + data.z*data.z);
                    data.azimuth = std::atan2(data.x, data.y) * (180.0f / juce::MathConstants<float>::pi);
                    data.elevation = std::atan2(data.z, std::sqrt(data.x*data.x + data.y*data.y)) * (180.0f / juce::MathConstants<float>::pi);

                    data.isLocal = true;
                    data.hasBeenPushedToAPVTS = false;
                    sharedRegistry->updateSource(data);
                }
            }
        }
        else
        {
            wasMotionEnabled = false;
        }

        if (isMorphing.load() && !motionEnabled)
        {
            auto now = juce::Time::getMillisecondCounter();
            float elapsed = static_cast<float>(now - morphStartTime);
            float duration = morphDurationMs.load();
            float progress = (duration > 0.0f) ? (elapsed / duration) : 1.0f;

            if (progress >= 1.0f) {
                progress = 1.0f;
                isMorphing.store(false);
            }

            float smoothProgress = progress * progress * (3.0f - 2.0f * progress);

            auto sources = sharedRegistry->getSourcesArray();
            for (int id = 1; id <= 128; ++id)
            {
                auto& data = sources[id];
                if (data.isActive && !data.isLocked && !data.isDraggedLocally && !data.isBeingDraggedRemotely)
                {
                    float sx = morphStartPositions[id][0];
                    float sy = morphStartPositions[id][1];
                    float sz = morphStartPositions[id][2];

                    float tx = morphTargetPositions[id][0];
                    float ty = morphTargetPositions[id][1];
                    float tz = morphTargetPositions[id][2];

                    data.x = sx + (tx - sx) * smoothProgress;
                    data.y = sy + (ty - sy) * smoothProgress;
                    data.z = sz + (tz - sz) * smoothProgress;

                    data.distance = std::sqrt(data.x*data.x + data.y*data.y + data.z*data.z);
                    data.azimuth = std::atan2(data.x, data.y) * (180.0f / juce::MathConstants<float>::pi);
                    data.elevation = std::atan2(data.z, std::sqrt(data.x*data.x + data.y*data.y)) * (180.0f / juce::MathConstants<float>::pi);
                    
                    data.isLocal = true;
                    data.hasBeenPushedToAPVTS = false;
                    sharedRegistry->updateSource(data);
                }
            }
        }
    }
}

const juce::String PanoramixMasterAudioProcessor::getName() const { return JucePlugin_Name; }
bool PanoramixMasterAudioProcessor::acceptsMidi() const { return false; }
bool PanoramixMasterAudioProcessor::producesMidi() const { return false; }
bool PanoramixMasterAudioProcessor::isMidiEffect() const { return false; }
double PanoramixMasterAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int PanoramixMasterAudioProcessor::getNumPrograms() { return 1; }
int PanoramixMasterAudioProcessor::getCurrentProgram() { return 0; }
void PanoramixMasterAudioProcessor::setCurrentProgram (int index) {}
const juce::String PanoramixMasterAudioProcessor::getProgramName (int index) { return {}; }
void PanoramixMasterAudioProcessor::changeProgramName (int index, const juce::String& newName) {}

void PanoramixMasterAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    lastSampleRate = sampleRate;
    lastBlockSize = samplesPerBlock;

    playbackTempBuffer.setSize (64, juce::jmax (samplesPerBlock * 2, 8192), false, false, true);

    if (engineMode.load() == 1) {
        for (int i = 0; i < 64; ++i) {
            if (audioTracks[i]) {
                juce::SpinLock::ScopedLockType sl(audioTracks[i]->processLock);
                if (audioTracks[i]->transportSource != nullptr) {
                    audioTracks[i]->transportSource->prepareToPlay(samplesPerBlock, sampleRate);
                }
            }
        }
    }
}

void PanoramixMasterAudioProcessor::releaseResources()
{
    if (engineMode.load() == 1) {
        for (int i = 0; i < 64; ++i) {
            if (audioTracks[i]) {
                juce::SpinLock::ScopedLockType sl(audioTracks[i]->processLock);
                if (audioTracks[i]->transportSource != nullptr) {
                    audioTracks[i]->transportSource->releaseResources();
                }
            }
        }
    }
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool PanoramixMasterAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
#else
    auto outSet = layouts.getMainOutputChannelSet();
    auto inSet  = layouts.getMainInputChannelSet();

    if (outSet == juce::AudioChannelSet::disabled())
        return false;

    int outSize = outSet.size();
    if (outSize < 1 || outSize > 64)
        return false;

    if (!inSet.isDisabled() && inSet != juce::AudioChannelSet::mono()
        && inSet != juce::AudioChannelSet::stereo() && inSet != outSet)
        return false;

    return true;
#endif
}
#endif

void PanoramixMasterAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    int currentMode = pluginModeParam != nullptr ? static_cast<int>(pluginModeParam->load()) : 0;
    int numInputChannels = getTotalNumInputChannels();
    int numOutputChannels = getTotalNumOutputChannels();

    if (currentMode == 0) // Sender Node Mode
    {
        int currentId = (sourceIdParam != nullptr) ? static_cast<int>(sourceIdParam->load()) : 1;

        if (pendingActivate.exchange(false))
        {
            SpatialData sd;
            if (sharedRegistry->getSource(currentId, sd))
            {
                if (!sd.isActive)
                {
                    sd.isActive = true;
                    sd.showInRadar = true;
                    sharedRegistry->updateSource(sd);
                }
            }
        }

        if (!isDraggingNode.load())
        {
            SpatialData data;
            if (sharedRegistry->getSource(currentId, data) && !data.isLocked)
            {
                float cx = xParam != nullptr ? xParam->load() : 0.0f;
                float cy = yParam != nullptr ? yParam->load() : 1.0f;
                float cz = zParam != nullptr ? zParam->load() : 0.0f;

                bool xyzChanged = (std::abs(cx - lastX.load()) > 0.001f ||
                                   std::abs(cy - lastY.load()) > 0.001f ||
                                   std::abs(cz - lastZ.load()) > 0.001f);

                if (!data.isBeingDraggedRemotely && (xyzChanged || forceFirstUpdate.exchange(false)))
                {
                    data.x = cx;
                    data.y = cy;
                    data.z = cz;
                    data.distance = std::sqrt(cx*cx + cy*cy + cz*cz);
                    data.azimuth = std::atan2(cx, cy) * (180.0f / juce::MathConstants<float>::pi);
                    data.elevation = std::atan2(cz, std::sqrt(cx*cx + cy*cy)) * (180.0f / juce::MathConstants<float>::pi);
                    data.isLocal = true;
                    data.isActive = true;
                    data.showInRadar = true;
                    sharedRegistry->updateSource(data);

                    lastX.store(cx);
                    lastY.store(cy);
                    lastZ.store(cz);
                }
            }
        }
        
        for (int ch = numInputChannels; ch < numOutputChannels; ++ch)
        {
            buffer.clear (ch, 0, buffer.getNumSamples());
        }
        return;
    }

    // Mode 1: Master Hub
    buffer.clear();

    bool anySolo = false;
    for (int i = 0; i < 64; ++i) {
        if (audioTracks[i] && audioTracks[i]->solo.load()) {
            anySolo = true;
            break;
        }
    }

    const int numSamples = buffer.getNumSamples();
    float decayFactor = std::exp(- (float)numSamples / (float)(lastSampleRate > 0 ? lastSampleRate : 48000.0) * 10.0f);
    int currentEngine = engineMode.load();

    for (int i = 0; i < 64; ++i)
    {
        auto* track = audioTracks[i].get();
        if (track == nullptr || !track->hasAudio.load() || !globalPlaying.load())
        {
            if (track != nullptr) track->currentLevel.store(0.0f, std::memory_order_relaxed);
            continue;
        }
        
        int trackChs = track->numChannels.load();

        if (currentEngine == 0) // RAM Mode Multichannel
        {
            juce::SpinLock::ScopedTryLockType lock (track->processLock);
            if (!lock.isLocked())
            {
                track->currentLevel.store(0.0f, std::memory_order_relaxed);
                continue;
            }

            int trackNumSamples = track->audioData.getNumSamples();
            if (trackNumSamples == 0) continue;

            bool isMuted = track->mute.load();
            bool isSoloed = track->solo.load();
            int targetOutCh = track->outCh.load();

            if (targetOutCh < 0 || targetOutCh >= 64) continue;

            if (!isMuted && (!anySolo || isSoloed))
            {
                int64_t currentPos = track->playPosition.load();
                int samplesToProcess = numSamples;
                int writeOffset = 0;

                for (int ch = 0; ch < trackChs; ++ch)
                    playbackTempBuffer.clear (ch, 0, numSamples);

                while (samplesToProcess > 0)
                {
                    if (currentPos >= trackNumSamples)
                    {
                        if (isGlobalLooping.load())
                            currentPos = 0;
                        else
                            break;
                    }

                    int chunk = (int)std::min((int64_t)samplesToProcess, trackNumSamples - currentPos);

                    for (int ch = 0; ch < trackChs; ++ch)
                    {
                        playbackTempBuffer.copyFrom(ch, writeOffset, track->audioData, ch, (int)currentPos, chunk);
                    }

                    currentPos += chunk;
                    writeOffset += chunk;
                    samplesToProcess -= chunk;
                }

                track->playPosition.store(currentPos);

                float targetGain = track->gain.load();
                float startGain = track->previousGain.load();
                for (int ch = 0; ch < trackChs; ++ch) {
                    playbackTempBuffer.applyGainRamp(ch, 0, numSamples, startGain, targetGain);
                }
                track->previousGain.store(targetGain);

                float maxRmsLvl = 0.0f;
                for (int ch = 0; ch < trackChs; ++ch)
                {
                    maxRmsLvl = std::max(maxRmsLvl, playbackTempBuffer.getRMSLevel(ch, 0, numSamples));
                }
                float prevLvl = track->currentLevel.load(std::memory_order_relaxed);
                float decayedLvl = std::max(maxRmsLvl, prevLvl * decayFactor);
                track->currentLevel.store(decayedLvl, std::memory_order_relaxed);

                for (int ch = 0; ch < trackChs; ++ch)
                {
                    int dstCh = targetOutCh + ch;
                    if (dstCh >= 0 && dstCh < buffer.getNumChannels())
                    {
                        buffer.addFrom(dstCh, 0, playbackTempBuffer, ch, 0, numSamples);
                    }
                }
            }
            else
            {
                track->currentLevel.store(0.0f, std::memory_order_relaxed);
            }
        }
        else // Disk Stream Mode Multichannel
        {
            juce::SpinLock::ScopedTryLockType lock (track->processLock);
            if (!lock.isLocked() || track->transportSource == nullptr)
            {
                track->currentLevel.store(0.0f, std::memory_order_relaxed);
                continue;
            }

            bool isMuted = track->mute.load();
            bool isSoloed = track->solo.load();
            int targetOutCh = track->outCh.load();

            if (targetOutCh < 0 || targetOutCh >= 64) continue;

            if (!isMuted && (!anySolo || isSoloed))
            {
                for (int ch = 0; ch < trackChs; ++ch)
                    playbackTempBuffer.clear (ch, 0, numSamples);

                juce::AudioSourceChannelInfo info(&playbackTempBuffer, 0, numSamples);
                track->transportSource->getNextAudioBlock(info);

                float targetGain = track->gain.load();
                float startGain = track->previousGain.load();
                for (int ch = 0; ch < trackChs; ++ch) {
                    playbackTempBuffer.applyGainRamp(ch, 0, numSamples, startGain, targetGain);
                }
                track->previousGain.store(targetGain);

                float maxRmsLvl = 0.0f;
                for (int ch = 0; ch < trackChs; ++ch)
                {
                    maxRmsLvl = std::max(maxRmsLvl, playbackTempBuffer.getRMSLevel(ch, 0, numSamples));
                }
                float prevLvl = track->currentLevel.load(std::memory_order_relaxed);
                float decayedLvl = std::max(maxRmsLvl, prevLvl * decayFactor);
                track->currentLevel.store(decayedLvl, std::memory_order_relaxed);

                for (int ch = 0; ch < trackChs; ++ch)
                {
                    int dstCh = targetOutCh + ch;
                    if (dstCh >= 0 && dstCh < buffer.getNumChannels())
                    {
                        buffer.addFrom(dstCh, 0, playbackTempBuffer, ch, 0, numSamples);
                    }
                }
            }
            else
            {
                track->currentLevel.store(0.0f, std::memory_order_relaxed);
            }
        }
    }
}

bool PanoramixMasterAudioProcessor::connectToNetwork(const juce::String& ipAddress, int portNumber)
{
    disconnectNetwork();
    if (oscSender.connect(ipAddress, portNumber))
    {
        currentIP = ipAddress;
        currentPort = portNumber;
        isNetworkConnected.store(true);
        return true;
    }
    return false;
}

void PanoramixMasterAudioProcessor::disconnectNetwork()
{
    isNetworkConnected.store(false);
    oscSender.disconnect();
    oscReceiver.disconnect();
}

bool PanoramixMasterAudioProcessor::updateReceivePort(int newPort)
{
    oscReceiver.disconnect();
    receivePort = newPort;
    return oscReceiver.connect(receivePort);
}

bool PanoramixMasterAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* PanoramixMasterAudioProcessor::createEditor() { return new PanoramixMasterAudioProcessorEditor (*this); }

void PanoramixMasterAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    if (xml != nullptr)
    {
        xml->setAttribute("engineMode", engineMode.load());
        xml->setAttribute("isGlobalLooping", isGlobalLooping.load());

        auto* registryXml = xml->createNewChildElement("SPATIAL_REGISTRY");
        auto sources = sharedRegistry->getSourcesArray();
        for (int id = 1; id <= 128; ++id)
        {
            const auto& d = sources[id];
            if (d.isActive || d.showInRadar || d.trackName.isNotEmpty())
            {
                auto* nodeXml = registryXml->createNewChildElement("NODE");
                nodeXml->setAttribute("id", d.sourceID);
                nodeXml->setAttribute("x", (double)d.x);
                nodeXml->setAttribute("y", (double)d.y);
                nodeXml->setAttribute("z", (double)d.z);
                nodeXml->setAttribute("azimuth", (double)d.azimuth);
                nodeXml->setAttribute("elevation", (double)d.elevation);
                nodeXml->setAttribute("distance", (double)d.distance);
                nodeXml->setAttribute("trackName", d.trackName);
                nodeXml->setAttribute("isActive", d.isActive);
                nodeXml->setAttribute("isLocked", d.isLocked);
                nodeXml->setAttribute("showInRadar", d.showInRadar);
                nodeXml->setAttribute("isMute", d.isMute);
                nodeXml->setAttribute("isSolo", d.isSolo);
            }
        }

        auto* tracksXml = xml->createNewChildElement("AUDIO_TRACKS");
        for (int i = 0; i < 64; ++i)
        {
            if (audioTracks[i])
            {
                auto* trkXml = tracksXml->createNewChildElement("TRACK");
                trkXml->setAttribute("id", i + 1);
                trkXml->setAttribute("filePath", audioTracks[i]->currentFile.getFullPathName());
                trkXml->setAttribute("gain", (double)audioTracks[i]->gain.load());
                trkXml->setAttribute("outCh", audioTracks[i]->outCh.load());
                trkXml->setAttribute("mute", audioTracks[i]->mute.load());
                trkXml->setAttribute("solo", audioTracks[i]->solo.load());
            }
        }

        copyXmlToBinary (*xml, destData);
    }
}

void PanoramixMasterAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr)
    {
        suspendProcessing(true);

        if (xmlState->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));

        int savedEngine = xmlState->getIntAttribute("engineMode", 0);
        setEngineMode(savedEngine);
        setGlobalLooping(xmlState->getBoolAttribute("isGlobalLooping", true));

        if (auto* registryXml = xmlState->getChildByName("SPATIAL_REGISTRY"))
        {
            for (auto* nodeXml : registryXml->getChildIterator())
            {
                if (nodeXml->hasTagName("NODE"))
                {
                    int id = nodeXml->getIntAttribute("id");
                    if (id >= 1 && id <= 128)
                    {
                        SpatialData sd;
                        sharedRegistry->getSource(id, sd);
                        sd.sourceID = id;
                        sd.x = (float)nodeXml->getDoubleAttribute("x", sd.x);
                        sd.y = (float)nodeXml->getDoubleAttribute("y", sd.y);
                        sd.z = (float)nodeXml->getDoubleAttribute("z", sd.z);
                        sd.azimuth = (float)nodeXml->getDoubleAttribute("azimuth", sd.azimuth);
                        sd.elevation = (float)nodeXml->getDoubleAttribute("elevation", sd.elevation);
                        sd.distance = (float)nodeXml->getDoubleAttribute("distance", sd.distance);
                        sd.trackName = nodeXml->getStringAttribute("trackName", sd.trackName);
                        sd.isActive = nodeXml->getBoolAttribute("isActive", sd.isActive);
                        sd.isLocked = nodeXml->getBoolAttribute("isLocked", sd.isLocked);
                        sd.showInRadar = nodeXml->getBoolAttribute("showInRadar", sd.showInRadar);
                        sd.isMute = nodeXml->getBoolAttribute("isMute", sd.isMute);
                        sd.isSolo = nodeXml->getBoolAttribute("isSolo", sd.isSolo);
                        sharedRegistry->updateSource(sd);
                    }
                }
            }
        }

        clearMissingFiles();

        if (auto* tracksXml = xmlState->getChildByName("AUDIO_TRACKS"))
        {
            for (auto* trkXml : tracksXml->getChildIterator())
            {
                if (trkXml->hasTagName("TRACK"))
                {
                    int trkId = trkXml->getIntAttribute("id");
                    if (trkId >= 1 && trkId <= 64)
                    {
                        int idx = trkId - 1;
                        auto* track = audioTracks[idx].get();
                        if (track != nullptr)
                        {
                            track->gain.store((float)trkXml->getDoubleAttribute("gain", 1.0));
                            track->previousGain.store(track->gain.load());
                            track->outCh.store(trkXml->getIntAttribute("outCh", idx));
                            track->mute.store(trkXml->getBoolAttribute("mute", false));
                            track->solo.store(trkXml->getBoolAttribute("solo", false));

                            juce::String path = trkXml->getStringAttribute("filePath");
                            if (path.isNotEmpty())
                            {
                                juce::File f(path);
                                if (f.existsAsFile())
                                {
                                    loadAudioFile(trkId, f);
                                }
                                else
                                {
                                    const juce::ScopedLock sl(missingFilesLock);
                                    missingFilesQueue.push_back({ trkId, path });
                                }
                            }
                        }
                    }
                }
            }
        }

        suspendProcessing(false);

        if (!missingFilesQueue.empty() && onMissingFileDetected)
        {
            juce::MessageManager::callAsync([this]() {
                if (onMissingFileDetected) onMissingFileDetected();
            });
        }
    }
}

bool PanoramixMasterAudioProcessor::getNextMissingFile(MissingFileItem& item)
{
    const juce::ScopedLock sl(missingFilesLock);
    if (missingFilesQueue.empty()) return false;
    item = missingFilesQueue.front();
    missingFilesQueue.erase(missingFilesQueue.begin());
    return true;
}

void PanoramixMasterAudioProcessor::clearMissingFiles()
{
    const juce::ScopedLock sl(missingFilesLock);
    missingFilesQueue.clear();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PanoramixMasterAudioProcessor();
}
