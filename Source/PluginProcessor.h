#pragma once
#include <JuceHeader.h>
#include <array>
#include <memory>
#include <atomic>
#include <shared_mutex>
#include <vector>
#include <functional>

struct SpatialData
{
    int sourceID {1};
    juce::String trackName {""};
    float azimuth {0.0f};
    float elevation {0.0f};
    float distance {1.0f};
    float x {0.0f};
    float y {1.0f};
    float z {0.0f};
    
    bool isLocal {true};
    bool hasBeenPushedToAPVTS {true};
    bool isBeingDraggedRemotely {false};
    bool remoteDragStarted {false};
    bool remoteDragEnded {false};
    bool isDraggedLocally {false};
    
    bool isActive {false};
    bool isLocked {false};
    bool showInRadar {false};
    bool isMute {false};
    bool isSolo {false};
    
    int groupLeader {0};
    float relX {0.0f};
    float relY {0.0f};
    float relZ {0.0f};

    juce::int64 lastOscReceiveTime {-1000};
    juce::int64 lastApvtsUpdateTime {-1000};
    juce::int64 lastLocalDragTime {-1000};
};

class SpatialRegistry
{
public:
    SpatialRegistry()
    {
        for (int i = 1; i <= 128; ++i) {
            sources[i].sourceID = i;
            sources[i].x = 0.0f;
            sources[i].y = 1.0f;
            sources[i].z = 0.0f;
            sources[i].isActive = false;
            sources[i].isLocked = false;
            sources[i].showInRadar = false;
            sources[i].groupLeader = 0;
        }
    }
    ~SpatialRegistry() = default;

    void updateSource(const SpatialData& data)
    {
        if (data.sourceID < 1 || data.sourceID > 128) return;
        const std::unique_lock<std::shared_mutex> lock(mutex);
        sources[data.sourceID] = data;
    }

    bool getSource(int id, SpatialData& outData) const
    {
        if (id < 1 || id > 128) return false;
        const std::shared_lock<std::shared_mutex> lock(mutex);
        outData = sources[id];
        return true;
    }

    std::array<SpatialData, 129> getSourcesArray() const
    {
        const std::shared_lock<std::shared_mutex> lock(mutex);
        return sources;
    }

private:
    mutable std::shared_mutex mutex;
    std::array<SpatialData, 129> sources {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpatialRegistry)
};

struct AudioTrack
{
    juce::AudioBuffer<float> audioData;

    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::AudioTransportSource> transportSource;

    juce::File currentFile;
    std::atomic<int> numChannels {1};
    std::atomic<bool> hasAudio {false};
    std::atomic<juce::int64> playPosition {0};
    juce::String fileName {"No File"};
    std::atomic<bool> mute {false};
    std::atomic<bool> solo {false};
    std::atomic<float> gain {1.0f};
    std::atomic<float> previousGain {1.0f};
    std::atomic<int> outCh {0};
    std::atomic<float> currentLevel {0.0f};
    juce::SpinLock processLock;
};

struct MissingFileItem
{
    int trackId;
    juce::String missingPath;
};

class PanoramixMasterAudioProcessor : public juce::AudioProcessor,
                                      public juce::Timer,
                                      public juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
{
public:
    PanoramixMasterAudioProcessor();
    ~PanoramixMasterAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void updateTrackProperties (const TrackProperties& properties) override;

    bool connectToNetwork(const juce::String& ipAddress, int portNumber);
    void disconnectNetwork();
    bool updateReceivePort(int newPort);
    
    void timerCallback() override;
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void oscBundleReceived(const juce::OSCBundle& bundle) override;

    void storePreset(int slotIndex);
    void triggerPreset(int slotIndex);

    void loadAudioFile (int trackId, const juce::File& file);
    void startAudioPlayback();
    void stopAudioPlayback();
    bool isGlobalPlaying() const;
    double getGlobalLengthInSeconds() const;
    double getGlobalPositionInSeconds() const;
    void setGlobalPositionInSeconds(double pos);
    void setGlobalLooping(bool shouldLoop);

    void setEngineMode(int newMode);
    void resetToNewSession();

    bool getNextMissingFile(MissingFileItem& item);
    void clearMissingFiles();
    bool getIsMorphingState() const { return isMorphing.load(); }

    std::function<void()> onMissingFileDetected;

    juce::AudioProcessorValueTreeState apvts;
    juce::SharedResourcePointer<SpatialRegistry> sharedRegistry;
    
    juce::String currentIP {"127.0.0.1"};
    int currentPort {4000};
    int receivePort {4001};
    std::atomic<bool> isNetworkConnected {false};
    std::atomic<bool> forceFirstUpdate {true};
    std::atomic<bool> isDraggingNode {false};
    std::atomic<bool> pendingActivate {false};
    std::atomic<bool> isGlobalLooping {true};

    std::atomic<juce::uint32> lastHeartbeatRxTime {0};
    std::atomic<juce::uint32> lastOscMsgTime {0};
    std::atomic<bool> isHeartbeatAlive {false};
    std::atomic<int> engineMode {0};

    juce::OSCSender oscSender;
    juce::OSCReceiver oscReceiver;

    std::atomic<float> lastX {0.0f};
    std::atomic<float> lastY {1.0f};
    std::atomic<float> lastZ {0.0f};

    std::array<std::unique_ptr<AudioTrack>, 64> audioTracks;
    
private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    class NetworkThread : public juce::Thread
    {
    public:
        NetworkThread(PanoramixMasterAudioProcessor& p) : juce::Thread("OscNetworkThread"), processor(p) {}
        void run() override;
    private:
        PanoramixMasterAudioProcessor& processor;
    };

    std::unique_ptr<NetworkThread> netThread;
    juce::TimeSliceThread diskReadThread;

    std::atomic<float>* pluginModeParam {nullptr};
    std::atomic<float>* sourceIdParam {nullptr};
    std::atomic<float>* azimParam {nullptr};
    std::atomic<float>* elevParam {nullptr};
    std::atomic<float>* distParam {nullptr};
    std::atomic<float>* xParam {nullptr};
    std::atomic<float>* yParam {nullptr};
    std::atomic<float>* zParam {nullptr};
    std::atomic<float>* morphTimeParam {nullptr};

    std::atomic<float>* motionEnableParam {nullptr};
    std::atomic<float>* motionShapeParam {nullptr};
    std::atomic<float>* motionRateParam {nullptr};
    std::atomic<float>* motionRadiusParam {nullptr};
    std::atomic<float>* motionSpreadParam {nullptr};

    std::atomic<int> lastSourceId {-1};

    std::array<std::array<std::array<float, 3>, 129>, 4> presetSlots {};
    std::array<std::array<float, 3>, 129> morphStartPositions {};
    std::array<std::array<float, 3>, 129> morphTargetPositions {};
    std::atomic<bool> isMorphing {false};
    juce::uint32 morphStartTime {0};
    std::atomic<float> morphDurationMs {2000.0f};

    bool wasMotionEnabled {false};
    bool isRemoteGestureActive {false};
    std::atomic<uint64_t> processBlockCounter {0};

    float motionCenterX {0.0f};
    float motionCenterY {0.0f};
    float motionCenterZ {0.0f};
    std::array<std::array<float, 3>, 129> motionOriginPositions {};

    juce::AudioFormatManager formatManager;
    std::atomic<bool> globalPlaying {false};

    juce::AudioBuffer<float> playbackTempBuffer;
    int lastKnownPluginMode {-1};

    double lastSampleRate {48000.0};
    int lastBlockSize {512};

    juce::CriticalSection missingFilesLock;
    std::vector<MissingFileItem> missingFilesQueue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanoramixMasterAudioProcessor)
};
