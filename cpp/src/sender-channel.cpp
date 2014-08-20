//
//  sender-channel.cpp
//  ndnrtc
//
//  Copyright 2013 Regents of the University of California
//  For licensing details see the LICENSE file.
//
//  Author:  Peter Gusev
//

#include "sender-channel.h"

#define AUDIO_ID 0
#define VIDEO_ID 1

using namespace boost;
using namespace ndnlog;
using namespace ndnlog::new_api;
using namespace ndnrtc;
using namespace ndnrtc::new_api;
using namespace webrtc;

//******************************************************************************
//******************************************************************************
#pragma mark - construction/destruction
NdnMediaChannel::NdnMediaChannel(const ParamsStruct &params,
                                 const ParamsStruct &audioParams):
NdnRtcObject(params),
audioParams_(audioParams)
{
}

NdnMediaChannel::~NdnMediaChannel()
{
}

//******************************************************************************
#pragma mark - static
//******************************************************************************
#pragma mark - public
int
NdnMediaChannel::init()
{
    shared_ptr<std::string> userPrefix = NdnRtcNamespace::getUserPrefix(params_);
    
    ndnKeyChain_ = NdnRtcNamespace::keyChainForUser(*userPrefix);
    bool videoFaceCreated = false, audioFaceCreated = false;
    
    // this is a key chain workaround for face processor:
    // media sender is given a key chain upon initialization (@see init method)
    // this key chain is used for signing individual data segments. this means,
    // sign method will be invoked >100 times per second. to decrease costs of
    // retrieving private key while signing, this key chain should be created
    // using MemoryIdentityStorage. however, the same key chain can not be
    // used for registering a prefix - in this case, NFD will not recognize the
    // key, created in app memory. this is why, we are using default key chain
    // here which talks to OS key chain and provides default certificate for
    // signing control interest - in this case, NFD can recognize this
    // certificate
    videoFaceProcessor_ = FaceProcessor::createFaceProcessor(params_,
                                                             NdnRtcNamespace::defaultKeyChain());
    
    if (!(videoFaceCreated = (videoFaceProcessor_.get() != nullptr)))
        notifyError(RESULT_WARN, "can't initialize NDN networking for video "
                    "channel");
    
    audioFaceProcessor_ = FaceProcessor::createFaceProcessor(params_,
                                                             NdnRtcNamespace::defaultKeyChain());
    
    if (!(audioFaceCreated = (audioFaceProcessor_.get() != nullptr)))
        notifyError(RESULT_WARN, "can't initialize NDN networking for audio "
                    "channel");
    
    if (!(videoFaceCreated || audioFaceCreated))
        return notifyError(RESULT_ERR, "audio and video can not be initialized "
                           "(ndn errors). aborting.");
    
    return RESULT_OK;
}
int
NdnMediaChannel::startTransmission()
{
    if (!isInitialized_)
        return notifyError(RESULT_ERR, "sender channel was not initalized");
    
    if (videoInitialized_)
        videoFaceProcessor_->startProcessing();
    
    if (audioInitialized_)
        audioFaceProcessor_->startProcessing();
    
    return RESULT_OK;
}
int
NdnMediaChannel::stopTransmission()
{
    if (!isTransmitting_)
        return notifyError(RESULT_ERR, "sender channel was not transmitting data");
    
    if (videoTransmitting_)
        videoFaceProcessor_->stopProcessing();
    
    if (audioTransmitting_)
        audioFaceProcessor_->stopProcessing();
    
    return RESULT_OK;
}
//******************************************************************************
#pragma mark - private

//******************************************************************************
#pragma mark - intefaces realization
void
NdnMediaChannel::onInterest(const shared_ptr<const Name>& prefix,
                                 const shared_ptr<const Interest>& interest,
                                 ndn::Transport& transport)
{
    LogInfoC << "incoming interest "<< interest->getName() << std::endl;
}

void
NdnMediaChannel::onRegisterFailed(const shared_ptr<const Name>& prefix)
{
    notifyError(RESULT_ERR, "failed to register prefix %s", prefix->toUri().c_str());
}

//******************************************************************************
//******************************************************************************
#pragma mark - construction/destruction
NdnSenderChannel::NdnSenderChannel(const ParamsStruct &params,
                                   const ParamsStruct &audioParams,
                                   IExternalRenderer *const externalRenderer):
NdnMediaChannel(params, audioParams),
cameraCapturer_(new CameraCapturer(params)),
audioCapturer_(new AudioCapturer(audioParams, NdnRtcUtils::sharedVoiceEngine())),
deliver_cs_(CriticalSectionWrapper::CreateCriticalSection()),
deliverEvent_(*EventWrapper::Create()),
processThread_(*ThreadWrapper::CreateThread(processDeliveredFrame, this,
                                            kHighPriority))
{
    if (externalRenderer)
        localRender_.reset(new ExternalVideoRendererAdaptor(externalRenderer));
    else
        localRender_.reset(new VideoRenderer(0,params));
    
    description_ = "send-channel";
    
    this->setLogger(new Logger(params.loggingLevel,
                               NdnRtcUtils::toString("producer-%s.log", params.producerId)));
    isLoggerCreated_ = true;
        
    cameraCapturer_->setObserver(this);
    localRender_->setObserver(this);
    
    // set connection capturer -> this
    cameraCapturer_->setFrameConsumer(this);

    // set connection audioCapturer -> this
    audioCapturer_->setFrameConsumer(this);
    
    frameFreqMeter_ = NdnRtcUtils::setupFrequencyMeter();
}

NdnSenderChannel::~NdnSenderChannel()
{
    NdnRtcUtils::releaseFrequencyMeter(frameFreqMeter_);
}

//******************************************************************************
#pragma mark - intefaces realization: IRawFrameConsumer
void
NdnSenderChannel::onDeliverFrame(webrtc::I420VideoFrame &frame,
                                      double timestamp)
{
    deliver_cs_->Enter();
    deliverFrame_.SwapFrame(&frame);
    deliveredTimestamp_ = timestamp;
    deliver_cs_->Leave();
    
    deliverEvent_.Set();
};

//******************************************************************************
#pragma mark - intefaces realization: IRawFrameConsumer
void
NdnSenderChannel::onDeliverRtpFrame(unsigned int len, unsigned char *data)
{
    for (int i = 0; i < audioSenders_.size(); i++)
        audioSenders_[i]->onDeliverRtpFrame(len, data);
}

void
NdnSenderChannel::onDeliverRtcpFrame(unsigned int len, unsigned char *data)
{
    for (int i = 0; i < audioSenders_.size(); i++)
        audioSenders_[i]->onDeliverRtcpFrame(len, data);
}

//******************************************************************************
#pragma mark - all static

//******************************************************************************
#pragma mark - public
int
NdnSenderChannel::init()
{
    LogInfoC << "unix timestamp: " << std::fixed << std::setprecision(6)
    << NdnRtcUtils::unixTimestamp() << std::endl;
    
    LogInfoC << "starting initialization " << std::endl;
    
    int res = NdnMediaChannel::init();
    
    if (RESULT_FAIL(res))
        return res;
    
    if (params_.useVideo)
    { // initialize video
        for (int i = 0; i < params_.nStreams; i++)
        {
            shared_ptr<NdnVideoSender> sender(new NdnVideoSender(params_, params_.streamsParams[i]));
            videoSenders_.push_back(sender);
            sender->setObserver(this);
        }
        
        videoInitialized_ = RESULT_NOT_FAIL(cameraCapturer_->init());
        if (!videoInitialized_)
            notifyError(RESULT_WARN, "can't intialize camera capturer");
        
        videoInitialized_ &= RESULT_NOT_FAIL(localRender_->init());
        if (!videoInitialized_)
            notifyError(RESULT_WARN, "can't intialize renderer");
        
        bool streamsInitialized = false;
        for (int i = 0; i < videoSenders_.size(); i++)
        {
            streamsInitialized |= RESULT_NOT_FAIL(videoSenders_[i]->init(videoFaceProcessor_, ndnKeyChain_));
            if (!streamsInitialized)
                notifyError(RESULT_WARN, "can't intialize stream with bitrate %d",
                            params_.streamsParams[i].startBitrate);
        }
        videoInitialized_ &= streamsInitialized;
    }
    
    if (params_.useAudio)
    { // initialize audio
        for (int i = 0; i < audioParams_.nStreams; i++)
        {
            shared_ptr<NdnAudioSender> audioSender(new NdnAudioSender(audioParams_));
            audioSenders_.push_back(audioSender);
            audioSender->setObserver(this);
        }

        audioInitialized_ = RESULT_NOT_FAIL(audioCapturer_->init());
        
        bool streamsInitialized = false;
        for (int i = 0; i < audioSenders_.size(); i++)
        {
            streamsInitialized |= RESULT_NOT_FAIL(audioSenders_[i]->init(audioFaceProcessor_,
                                                                         ndnKeyChain_));
            if (!streamsInitialized)
                notifyError(RESULT_WARN, "can't intialize audio stream with bitrate %d",
                            audioParams_.streamsParams[i].startBitrate);
        }
        
        if (!audioInitialized_)
            notifyError(RESULT_WARN, "can't initialize audio send channel");
        
        audioInitialized_ &= streamsInitialized;
    }
    
    isInitialized_ = audioInitialized_||videoInitialized_;
    
    if (!isInitialized_)
        return notifyError(RESULT_ERR, "audio and video can not be initialized."
                           " aborting.");
    
    LogInfoC
    << "publishing initialized with video: "
    << ((videoInitialized_)?"yes":"no")
    << ", audio: " << ((audioInitialized_)?"yes":"no") << std::endl;
    
    initServiceChannel();
    
    return (videoInitialized_&&audioInitialized_)?RESULT_OK:RESULT_WARN;
}

int
NdnSenderChannel::startTransmission()
{
    LogInfoC << "starting publishing" << std::endl;
    
    int res = NdnMediaChannel::startTransmission();
    
    if (RESULT_FAIL(res))
        return res;
    
    unsigned int tid = 1;
    
    if (videoInitialized_)
    {
        videoTransmitting_ = (processThread_.Start(tid) != 0);
        if (!videoTransmitting_)
            notifyError(RESULT_WARN, "can't start processing thread");
        
        videoTransmitting_ &= RESULT_NOT_FAIL(localRender_->startRendering("Local render"));
        if (!videoTransmitting_)
            notifyError(RESULT_WARN, "can't start render");
        
        videoTransmitting_ &= RESULT_NOT_FAIL(cameraCapturer_->startCapture());
        if (!videoTransmitting_)
            notifyError(RESULT_WARN, "can't start camera capturer");
    }
    
    if (audioInitialized_)
    {
        audioTransmitting_ = RESULT_NOT_FAIL(audioCapturer_->startCapture());
        
        if (!audioTransmitting_)
            notifyError(RESULT_WARN, "can't start audio send channel");
    }
    
    isTransmitting_ = audioTransmitting_ || videoTransmitting_;
    
    if (!isTransmitting_)
        return notifyError(RESULT_ERR, "both audio and video can not be started."
                           " aborting.");
    
    LogInfoC
    << "publishing started with video: "
    << ((videoInitialized_)?"yes":"no")
    << ", audio: " << ((audioInitialized_)?"yes":"no") << std::endl;
    
    return (audioTransmitting_&&videoTransmitting_)?RESULT_OK:RESULT_WARN;
}

int
NdnSenderChannel::stopTransmission()
{
    LogInfoC << "stopping publishing" << std::endl;
    
    int res = NdnMediaChannel::stopTransmission();
    
    if (RESULT_FAIL(res))
        return res;
    
    if (videoTransmitting_)
    {
        videoTransmitting_ = false;
        
        if (cameraCapturer_->isCapturing())
            cameraCapturer_->stopCapture();
        
        processThread_.SetNotAlive();
        deliverEvent_.Set();
        
        if (!processThread_.Stop())
            notifyError(RESULT_WARN, "can't stop processing thread");
        
        localRender_->stopRendering();
        
        for (int i = 0; i < videoSenders_.size(); i++)
            videoSenders_[i]->stop();
    }
    
    if (audioTransmitting_)
    {
        audioTransmitting_ = false;
        audioCapturer_->stopCapture();
        
        for (int i = 0; i < audioSenders_.size(); i++)
            audioSenders_[i]->stop();
    }
    
    LogInfoC << "stopped" << std::endl;
    
    isTransmitting_ = false;
    return RESULT_OK;
}

void
NdnSenderChannel::getChannelStatistics(SenderChannelStatistics &stat)
{
    if (!isTransmitting_)
        return;
    
    if (videoTransmitting_)
    {
        stat.videoStat_.nBytesPerSec_ = videoSenders_[0]->getCurrentOutgoingBitrate();
        stat.videoStat_.nFramesPerSec_ = NdnRtcUtils::currentFrequencyMeterValue(frameFreqMeter_);
        stat.videoStat_.lastFrameNo_ = videoSenders_[0]->getFrameNo();
        stat.videoStat_.encodingRate_ = videoSenders_[0]->getCurrentPacketRate();
        stat.videoStat_.nDroppedByEncoder_ = videoSenders_[0]->getEncoderDroppedNum();
        
        LogStatC << STAT_DIV
        << "br" << STAT_DIV << stat.videoStat_.nBytesPerSec_*8/1000 << STAT_DIV
        << "fps" << STAT_DIV << stat.videoStat_.nFramesPerSec_ << STAT_DIV
        << "fnum" << STAT_DIV << stat.videoStat_.lastFrameNo_ << STAT_DIV
        << "enc" << STAT_DIV << stat.videoStat_.encodingRate_ << STAT_DIV
        << "drop" << STAT_DIV << stat.videoStat_.nDroppedByEncoder_
        << std::endl;
    }
    
    if (audioTransmitting_)
    {
        stat.audioStat_.nBytesPerSec_ = audioSenders_[0]->getCurrentOutgoingBitrate();
        stat.audioStat_.lastFrameNo_ = audioSenders_[0]->getSampleNo();
        stat.audioStat_.nFramesPerSec_ = audioSenders_[0]->getCurrentPacketRate();
        stat.audioStat_.encodingRate_ = audioSenders_[0]->getCurrentPacketRate();
    }
}

void
NdnSenderChannel::setLogger(ndnlog::new_api::Logger *logger)
{
    cameraCapturer_->setLogger(logger);
    localRender_->setLogger(logger);
    
    for (int i = 0; i < videoSenders_.size(); i++)
        videoSenders_[i]->setLogger(logger);
    
    for (int i = 0; i < audioSenders_.size(); i++)
        audioSenders_[i]->setLogger(logger);
    
    ILoggingObject::setLogger(logger);
}

//******************************************************************************
#pragma mark - private
bool
NdnSenderChannel::process()
{
    if (deliverEvent_.Wait(100) == kEventSignaled) {
        NdnRtcUtils::frequencyMeterTick(frameFreqMeter_);
        
        double frameRate = params_.captureFramerate;
        uint64_t now = NdnRtcUtils::millisecondTimestamp();
        
        deliver_cs_->Enter();
        if (!deliverFrame_.IsZeroSize()) {
            
            uint64_t t = NdnRtcUtils::microsecondTimestamp();
            
            localRender_->onDeliverFrame(deliverFrame_, deliveredTimestamp_);
            
            uint64_t t2 = NdnRtcUtils::microsecondTimestamp();
            
            for (int i = 0; i < videoSenders_.size(); i++)
                videoSenders_[i]->onDeliverFrame(deliverFrame_, deliveredTimestamp_);
        }
        deliver_cs_->Leave();
    }

    return true;
}

void
NdnSenderChannel::onSessionInfoBroadcastFailed()
{
    LogErrorC << "failed to publish session info" << std::endl;
}

shared_ptr<SessionInfo>
NdnSenderChannel::onPublishSessionInfo()
{
    ParamsStruct publishingVideoParams = params_,
    publishingAudioParams = audioParams_;
    
    // update params info
    if (videoTransmitting_)
    {
        for (int i = 0; i < params_.nStreams; i++)
            publishingVideoParams.streamsParams[i].codecFrameRate = videoSenders_[i]->getCurrentPacketRate();
    }
    else
    {
        publishingVideoParams.nStreams = 0;
        publishingVideoParams.streamsParams = 0;
    }
    
    if (audioTransmitting_)
    {
        for (int i = 0; i < audioParams_.nStreams; i++)
            audioParams_.streamsParams[i].codecFrameRate = audioSenders_[i]->getCurrentPacketRate();
    }
    else
    {
        publishingAudioParams.nStreams = 0;
        publishingAudioParams.streamsParams = 0;
    }
    
    
    shared_ptr<SessionInfo> sessionInfo(new SessionInfo(publishingVideoParams,
                                                        publishingAudioParams));
    return sessionInfo;
}

void
NdnSenderChannel::initServiceChannel()
{
    boost::shared_ptr<FaceProcessor> serviceFaceProcessor;
    
    if (params_.useAudio)
        serviceFaceProcessor = audioFaceProcessor_;
    else if (params_.useVideo)
        serviceFaceProcessor = videoFaceProcessor_;
    else
        serviceFaceProcessor = FaceProcessor::createFaceProcessor(params_,
                                                                  NdnRtcNamespace::defaultKeyChain());
    
    serviceChannel_.reset(new ServiceChannel(this, serviceFaceProcessor));
    serviceChannel_->startSessionInfoBroadcast(*NdnRtcNamespace::getSessionInfoPrefix(params_),
                                               ndnKeyChain_,
                                               *NdnRtcNamespace::certificateNameForUser(*NdnRtcNamespace::getUserPrefix(params_)));
}
