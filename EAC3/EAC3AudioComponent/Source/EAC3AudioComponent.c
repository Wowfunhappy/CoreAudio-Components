#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AudioCodec.h>
#include <AudioUnit/AudioComponent.h>
#include <AudioToolbox/AudioToolbox.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// Define DEBUG_LOGGING to enable debug output
#define DEBUG_LOGGING 1

#ifdef DEBUG_LOGGING
// Debug logging
static FILE *gLogFile = NULL;
static void DebugLog(const char *format, ...) {
    if (!gLogFile) {
        gLogFile = fopen("/tmp/eac3_component.log", "a");
        if (!gLogFile) return;
    }
    
    va_list args;
    va_start(args, format);
    vfprintf(gLogFile, format, args);
    va_end(args);
    fprintf(gLogFile, "\n");
    fflush(gLogFile);
}
#else
#define DebugLog(...) do {} while(0)
#endif

// EAC3 format constants
#define kEAC3DecoderComponentType 'adec'
#define kEAC3Format 'ec-3'
#define kEAC3FormatMP4 'ec+3'
#define kEAC3Manufacturer 'Ac-3'

// EAC3 decoder state
typedef struct EAC3Decoder {
    AudioStreamBasicDescription inputFormat;
    AudioStreamBasicDescription outputFormat;
    
    AVCodec *codec;
    AVCodecContext *codecContext;
    AVCodecParserContext *parser;
    AVFrame *frame;
    AVPacket *packet;
    
    UInt8 *inputBuffer;
    UInt32 inputBufferSize;
    UInt32 inputBufferUsed;
    
    Float32 *outputBuffer;
    UInt32 outputBufferFrames;
    UInt32 outputBufferUsed;
    
    Boolean isInitialized;
    UInt32 packetFrameSize;
    UInt32 maxPacketSize;
    Boolean needsReset;
} EAC3Decoder;

// AudioComponent plugin instance structure
// This matches Apple's layout from ACPlugInDispatch.cpp
typedef struct AudioComponentPlugInInstance {
    AudioComponentPlugInInterface mPlugInInterface;
    void *mPad[4];  // Required padding for binary compatibility
    EAC3Decoder mInstanceStorage;
} AudioComponentPlugInInstance;

// Macros to access the instance from self pointer
#define ACPI ((AudioComponentPlugInInstance *)self)
#define EAC3_DECODER (&ACPI->mInstanceStorage)

// Channel layout tags for different channel configurations
static const AudioChannelLayoutTag kChannelLayoutTags[8] = {
    kAudioChannelLayoutTag_Mono,        // 1 channel: C
    kAudioChannelLayoutTag_Stereo,      // 2 channels: L R
    kAudioChannelLayoutTag_MPEG_3_0_B,  // 3 channels: C L R
    kAudioChannelLayoutTag_MPEG_4_0_B,  // 4 channels: C L R Cs
    kAudioChannelLayoutTag_MPEG_5_0_D,  // 5 channels: C L R Ls Rs
    kAudioChannelLayoutTag_MPEG_5_1_D,  // 6 channels: C L R Ls Rs LFE
    kAudioChannelLayoutTag_AAC_6_1,     // 7 channels: C L R Ls Rs Cs LFE
    kAudioChannelLayoutTag_MPEG_7_1_B   // 8 channels: C Lc Rc L R Ls Rs LFE
};

// AudioCodec implementation
static OSStatus GetPropertyInfo(void *self,
                               AudioCodecPropertyID inPropertyID,
                               UInt32 *outSize,
                               Boolean *outWritable) {
    DebugLog("GetPropertyInfo called: self=%p, propertyID=%08x (%c%c%c%c)", 
             self, (unsigned int)inPropertyID,
             (char)(inPropertyID >> 24), (char)(inPropertyID >> 16),
             (char)(inPropertyID >> 8), (char)inPropertyID);
    
    switch (inPropertyID) {
        case kAudioCodecPropertyCurrentInputFormat:
        case kAudioCodecPropertyCurrentOutputFormat:
        case kAudioCodecPropertySupportedInputFormats:
        case kAudioCodecPropertySupportedOutputFormats:
        case kAudioCodecPropertyInputFormatsForOutputFormat:
        case kAudioCodecPropertyOutputFormatsForInputFormat:
            if (outSize) *outSize = sizeof(AudioStreamBasicDescription);
            if (outWritable) *outWritable = (inPropertyID == kAudioCodecPropertyCurrentInputFormat ||
                                            inPropertyID == kAudioCodecPropertyCurrentOutputFormat);
            return noErr;
            
        case kAudioCodecPropertyPacketFrameSize:
        case kAudioCodecPropertyMaximumPacketByteSize:
        case kAudioCodecPropertyHasVariablePacketByteSizes:
        case kAudioCodecPropertyIsInitialized:
        case kAudioCodecPropertyMinimumNumberOutputPackets:
        case kAudioCodecPropertyInputBufferSize:
        case kAudioCodecPropertyUsedInputBufferSize:
            if (outSize) *outSize = sizeof(UInt32);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyFormatList:
            if (outSize) *outSize = sizeof(AudioFormatListItem);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyFormatCFString:
            if (outSize) *outSize = sizeof(CFStringRef);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyInputChannelLayout:
        case kAudioCodecPropertyOutputChannelLayout:
            if (outSize) *outSize = sizeof(AudioChannelLayout);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyPrimeInfo:
            if (outSize) *outSize = sizeof(AudioCodecPrimeInfo);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyNameCFString:
        case kAudioCodecPropertyManufacturerCFString:
            if (outSize) *outSize = sizeof(CFStringRef);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyMagicCookie:
            if (outSize) *outSize = 0; // EAC3 doesn't use magic cookies
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyRequiresPacketDescription:
        case kAudioCodecPropertyPrimeMethod:
        case kAudioCodecPropertyDoesSampleRateConversion:
            if (outSize) *outSize = sizeof(UInt32);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyAvailableNumberChannels:
            if (outSize) *outSize = sizeof(UInt32) * 8; // Support 1-8 channels
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyAvailableInputChannelLayouts:
        case kAudioCodecPropertyAvailableOutputChannelLayouts:
            if (outSize) *outSize = 8 * sizeof(AudioChannelLayoutTag);
            if (outWritable) *outWritable = false;
            return noErr;
            
        default:
            DebugLog("Unknown property ID: %08x", (unsigned int)inPropertyID);
            return kAudioCodecUnknownPropertyError;
    }
}

static OSStatus GetProperty(void *self,
                           AudioCodecPropertyID inPropertyID,
                           UInt32 *ioPropertyDataSize,
                           void *outPropertyData) {
    EAC3Decoder *decoder = EAC3_DECODER;
    
    switch (inPropertyID) {
        case kAudioCodecPropertyCurrentInputFormat:
            if (*ioPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                *(AudioStreamBasicDescription *)outPropertyData = decoder->inputFormat;
            } else {
                return kAudioCodecBadPropertySizeError;
            }
            return noErr;
            
        case kAudioCodecPropertyCurrentOutputFormat:
            if (*ioPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                *(AudioStreamBasicDescription *)outPropertyData = decoder->outputFormat;
            } else {
                return kAudioCodecBadPropertySizeError;
            }
            return noErr;
            
        case kAudioCodecPropertySupportedInputFormats:
        case kAudioCodecPropertyInputFormatsForOutputFormat:
            {
                // EAC3 supports one input format with variable parameters
                UInt32 theNumberFormats = *ioPropertyDataSize / sizeof(AudioStreamBasicDescription);
                if (theNumberFormats > 0) {
                    AudioStreamBasicDescription *format = (AudioStreamBasicDescription *)outPropertyData;
                    memset(format, 0, sizeof(AudioStreamBasicDescription));
                    
                    format->mFormatID = kEAC3Format;
                    format->mFormatFlags = 0;
                    format->mBytesPerPacket = 0; // Variable
                    format->mFramesPerPacket = 0; // Variable
                    format->mBytesPerFrame = 0;
                    format->mChannelsPerFrame = 0; // Any number of channels
                    format->mBitsPerChannel = 0; // N/A for compressed
                    format->mSampleRate = 0; // Any sample rate
                }
                *ioPropertyDataSize = sizeof(AudioStreamBasicDescription);
            }
            return noErr;
            
        case kAudioCodecPropertySupportedOutputFormats:
        case kAudioCodecPropertyOutputFormatsForInputFormat:
            {
                // We support one output format: 32-bit float PCM
                UInt32 theNumberFormats = *ioPropertyDataSize / sizeof(AudioStreamBasicDescription);
                if (theNumberFormats > 0) {
                    AudioStreamBasicDescription *format = (AudioStreamBasicDescription *)outPropertyData;
                    memset(format, 0, sizeof(AudioStreamBasicDescription));
                    
                    format->mFormatID = kAudioFormatLinearPCM;
                    format->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
                    format->mBitsPerChannel = 32;
                    format->mFramesPerPacket = 1;
                    
                    // These will be set based on input format
                    format->mChannelsPerFrame = 0; // Any number of channels
                    format->mSampleRate = 0; // Any sample rate
                    format->mBytesPerFrame = 0; // Will be calculated
                    format->mBytesPerPacket = 0; // Will be calculated
                }
                *ioPropertyDataSize = sizeof(AudioStreamBasicDescription);
            }
            return noErr;
            
        case kAudioCodecPropertyPacketFrameSize:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = decoder->packetFrameSize;
            return noErr;
            
        case kAudioCodecPropertyMaximumPacketByteSize:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = decoder->maxPacketSize;
            return noErr;
            
        case kAudioCodecPropertyHasVariablePacketByteSizes:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = 1;
            return noErr;
            
        case kAudioCodecPropertyIsInitialized:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = decoder->isInitialized ? 1 : 0;
            return noErr;
            
        case kAudioCodecPropertyInputBufferSize:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = decoder->inputBufferSize;
            return noErr;
            
        case kAudioCodecPropertyMinimumNumberOutputPackets:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = 1;
            return noErr;
            
        case kAudioCodecPropertyInputChannelLayout:
        case kAudioCodecPropertyOutputChannelLayout:
            if (outPropertyData && *ioPropertyDataSize >= sizeof(AudioChannelLayout)) {
                AudioChannelLayout *layout = (AudioChannelLayout *)outPropertyData;
                UInt32 channels = decoder->outputFormat.mChannelsPerFrame;
                
                // Use appropriate channel layout based on channel count
                if (channels >= 1 && channels <= 8) {
                    layout->mChannelLayoutTag = kChannelLayoutTags[channels - 1];
                } else {
                    layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                }
                
                layout->mChannelBitmap = 0;
                layout->mNumberChannelDescriptions = 0;
            }
            *ioPropertyDataSize = sizeof(AudioChannelLayout);
            return noErr;
            
        case kAudioCodecPropertyPrimeInfo:
            if (*ioPropertyDataSize == sizeof(AudioCodecPrimeInfo)) {
                AudioCodecPrimeInfo *prime = (AudioCodecPrimeInfo *)outPropertyData;
                prime->leadingFrames = 0;
                prime->trailingFrames = 0;
            } else {
                return kAudioCodecBadPropertySizeError;
            }
            return noErr;
            
        case kAudioCodecPropertyNameCFString:
            if (*ioPropertyDataSize != sizeof(CFStringRef)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(CFStringRef *)outPropertyData = CFSTR("EAC3 Audio Decoder");
            CFRetain(*(CFStringRef *)outPropertyData);
            return noErr;
            
        case kAudioCodecPropertyManufacturerCFString:
            if (*ioPropertyDataSize != sizeof(CFStringRef)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(CFStringRef *)outPropertyData = CFSTR("FFmpeg Project");
            CFRetain(*(CFStringRef *)outPropertyData);
            return noErr;
            
        case kAudioCodecPropertyFormatList:
            {
                if (*ioPropertyDataSize < sizeof(AudioFormatListItem)) {
                    return kAudioCodecBadPropertySizeError;
                }
                AudioFormatListItem *formatList = (AudioFormatListItem *)outPropertyData;
                
                // EAC3 format with no channel layout
                formatList->mASBD.mFormatID = kEAC3Format;
                formatList->mASBD.mFormatFlags = 0;
                formatList->mASBD.mBytesPerPacket = 0; // Variable
                formatList->mASBD.mFramesPerPacket = 0; // Variable
                formatList->mASBD.mBytesPerFrame = 0;
                formatList->mASBD.mChannelsPerFrame = 0; // Any
                formatList->mASBD.mBitsPerChannel = 0; // N/A
                formatList->mASBD.mSampleRate = 0; // Any
                formatList->mChannelLayoutTag = 0; // No specific layout
                
                *ioPropertyDataSize = sizeof(AudioFormatListItem);
            }
            return noErr;
            
        case kAudioCodecPropertyFormatCFString:
            if (*ioPropertyDataSize != sizeof(CFStringRef)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(CFStringRef *)outPropertyData = CFSTR("E-AC-3 (Enhanced AC-3)");
            CFRetain(*(CFStringRef *)outPropertyData);
            return noErr;
            
        case kAudioCodecPropertyMagicCookie:
            // EAC3 doesn't use magic cookies
            *ioPropertyDataSize = 0;
            return noErr;
            
        case kAudioCodecPropertyRequiresPacketDescription:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = 1; // EAC3 has variable packet sizes
            return noErr;
            
        case kAudioCodecPropertyAvailableNumberChannels:
            if (*ioPropertyDataSize >= sizeof(UInt32) * 8) {
                // Report support for 1-8 channels
                for (UInt32 i = 0; i < 8; i++) {
                    ((UInt32 *)outPropertyData)[i] = i + 1;
                }
                *ioPropertyDataSize = sizeof(UInt32) * 8;
            } else {
                return kAudioCodecBadPropertySizeError;
            }
            return noErr;
            
        case kAudioCodecPropertyPrimeMethod:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = kAudioCodecPrimeMethod_None;
            return noErr;
            
        case kAudioCodecPropertyDoesSampleRateConversion:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = 0; // No sample rate conversion
            return noErr;
            
        case kAudioCodecPropertyUsedInputBufferSize:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = decoder->inputBufferUsed;
            return noErr;
            
        case kAudioCodecPropertyAvailableInputChannelLayouts:
        case kAudioCodecPropertyAvailableOutputChannelLayouts:
            if (*ioPropertyDataSize >= 8 * sizeof(AudioChannelLayoutTag)) {
                AudioChannelLayoutTag *tags = (AudioChannelLayoutTag *)outPropertyData;
                
                if (decoder->isInitialized) {
                    // When initialized, only report the current channel layout
                    UInt32 channels = decoder->outputFormat.mChannelsPerFrame;
                    if (channels >= 1 && channels <= 8) {
                        tags[0] = kChannelLayoutTags[channels - 1];
                        *ioPropertyDataSize = sizeof(AudioChannelLayoutTag);
                    } else {
                        return kAudioCodecUnknownPropertyError;
                    }
                } else {
                    // When not initialized, report all supported layouts
                    for (UInt32 i = 0; i < 8; i++) {
                        tags[i] = kChannelLayoutTags[i];
                    }
                    *ioPropertyDataSize = 8 * sizeof(AudioChannelLayoutTag);
                }
            } else {
                return kAudioCodecBadPropertySizeError;
            }
            return noErr;
            
        default:
            DebugLog("GetProperty: Unknown property ID: %08x (%c%c%c%c)", 
                     (unsigned int)inPropertyID,
                     (char)(inPropertyID >> 24), (char)(inPropertyID >> 16),
                     (char)(inPropertyID >> 8), (char)inPropertyID);
            return kAudioCodecUnknownPropertyError;
    }
}

static OSStatus SetProperty(void *self,
                           AudioCodecPropertyID inPropertyID,
                           UInt32 inPropertyDataSize,
                           const void *inPropertyData) {
    EAC3Decoder *decoder = EAC3_DECODER;
    
    // No property can be set when the codec is initialized
    if (decoder->isInitialized) {
        return kAudioCodecIllegalOperationError;
    }
    
    switch (inPropertyID) {
        case kAudioCodecPropertyCurrentInputFormat:
            if (inPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                const AudioStreamBasicDescription *format = (const AudioStreamBasicDescription *)inPropertyData;
                
                // Validate EAC3 format
                if (format->mFormatID != kEAC3Format && format->mFormatID != kEAC3FormatMP4) {
                    DebugLog("SetProperty: Invalid format ID: %c%c%c%c",
                             (char)(format->mFormatID >> 24), (char)(format->mFormatID >> 16),
                             (char)(format->mFormatID >> 8), (char)(format->mFormatID));
                    return kAudioCodecUnsupportedFormatError;
                }
                
                // EAC3 supports 1-8 channels
                if (format->mChannelsPerFrame > 0 && (format->mChannelsPerFrame < 1 || format->mChannelsPerFrame > 8)) {
                    DebugLog("SetProperty: Invalid channel count: %u", format->mChannelsPerFrame);
                    return kAudioCodecUnsupportedFormatError;
                }
                
                // Sample rate must be positive if specified
                if (format->mSampleRate != 0 && format->mSampleRate < 0) {
                    DebugLog("SetProperty: Invalid sample rate: %f", format->mSampleRate);
                    return kAudioCodecUnsupportedFormatError;
                }
                
                decoder->inputFormat = *format;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyCurrentOutputFormat:
            if (inPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                const AudioStreamBasicDescription *format = (const AudioStreamBasicDescription *)inPropertyData;
                
                // We only support float PCM output
                if (format->mFormatID != kAudioFormatLinearPCM) {
                    return kAudioCodecUnsupportedFormatError;
                }
                
                if ((format->mFormatFlags & kAudioFormatFlagIsFloat) == 0) {
                    return kAudioCodecUnsupportedFormatError;
                }
                
                decoder->outputFormat = *format;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        default:
            return kAudioCodecUnknownPropertyError;
    }
}

static OSStatus Initialize(void *self,
                          const AudioStreamBasicDescription *inInputFormat,
                          const AudioStreamBasicDescription *inOutputFormat,
                          const void *inMagicCookie,
                          UInt32 inMagicCookieByteSize) {
    EAC3Decoder *decoder = EAC3_DECODER;
    
    DebugLog("Initialize called! self=%p, decoder=%p", self, decoder);
    
    if (decoder->isInitialized) {
        DebugLog("Initialize: Already initialized");
        return kAudioCodecStateError;
    }
    
    if (inInputFormat) {
        // Validate input format
        if (inInputFormat->mFormatID != kEAC3Format && inInputFormat->mFormatID != kEAC3FormatMP4) {
            DebugLog("Initialize: Invalid input format ID");
            return kAudioCodecUnsupportedFormatError;
        }
        decoder->inputFormat = *inInputFormat;
    }
    
    if (inOutputFormat) {
        // Validate output format
        if (inOutputFormat->mFormatID != kAudioFormatLinearPCM ||
            (inOutputFormat->mFormatFlags & kAudioFormatFlagIsFloat) == 0) {
            DebugLog("Initialize: Invalid output format");
            return kAudioCodecUnsupportedFormatError;
        }
        decoder->outputFormat = *inOutputFormat;
    }
    
    // Find EAC3 decoder
    decoder->codec = avcodec_find_decoder(AV_CODEC_ID_EAC3);
    if (!decoder->codec) {
        DebugLog("Initialize: Failed to find EAC3 decoder");
        return kAudioCodecStateError;
    }
    
    // Allocate codec context
    decoder->codecContext = avcodec_alloc_context3(decoder->codec);
    if (!decoder->codecContext) {
        DebugLog("Initialize: Failed to allocate codec context");
        return kAudioCodecStateError;
    }
    
    // Set codec parameters if we know them
    if (decoder->inputFormat.mChannelsPerFrame > 0) {
        decoder->codecContext->channels = decoder->inputFormat.mChannelsPerFrame;
        decoder->outputFormat.mChannelsPerFrame = decoder->inputFormat.mChannelsPerFrame;
    }
    if (decoder->inputFormat.mSampleRate > 0) {
        decoder->codecContext->sample_rate = decoder->inputFormat.mSampleRate;
        decoder->outputFormat.mSampleRate = decoder->inputFormat.mSampleRate;
    }
    
    // Calculate output format sizes
    decoder->outputFormat.mBytesPerFrame = decoder->outputFormat.mChannelsPerFrame * sizeof(Float32);
    decoder->outputFormat.mBytesPerPacket = decoder->outputFormat.mBytesPerFrame * decoder->outputFormat.mFramesPerPacket;
    
    // Open codec
    if (avcodec_open2(decoder->codecContext, decoder->codec, NULL) < 0) {
        DebugLog("Initialize: Failed to open codec");
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    // Create parser
    decoder->parser = av_parser_init(decoder->codec->id);
    if (!decoder->parser) {
        DebugLog("Initialize: Failed to create parser");
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    // Configure parser flags
    decoder->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    
    // Allocate frame and packet
    decoder->frame = av_frame_alloc();
    if (!decoder->frame) {
        DebugLog("Initialize: Failed to allocate frame");
        av_parser_close(decoder->parser);
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    decoder->packet = av_packet_alloc();
    if (!decoder->packet) {
        DebugLog("Initialize: Failed to allocate packet");
        av_frame_free(&decoder->frame);
        av_parser_close(decoder->parser);
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    // Allocate buffers
    decoder->inputBufferSize = 65536;
    decoder->inputBuffer = malloc(decoder->inputBufferSize);
    if (!decoder->inputBuffer) {
        DebugLog("Initialize: Failed to allocate input buffer");
        av_packet_free(&decoder->packet);
        av_frame_free(&decoder->frame);
        av_parser_close(decoder->parser);
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    decoder->outputBufferFrames = 16384;
    decoder->outputBuffer = malloc(decoder->outputBufferFrames * 8 * sizeof(Float32));
    if (!decoder->outputBuffer) {
        DebugLog("Initialize: Failed to allocate output buffer");
        free(decoder->inputBuffer);
        decoder->inputBuffer = NULL;
        av_packet_free(&decoder->packet);
        av_frame_free(&decoder->frame);
        av_parser_close(decoder->parser);
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    // EAC3 frame size is 1536 samples
    decoder->packetFrameSize = 1536;
    decoder->maxPacketSize = 4096;
    
    decoder->isInitialized = true;
    
    DebugLog("Initialize complete: input=%c%c%c%c output=%c%c%c%c", 
             (char)(decoder->inputFormat.mFormatID >> 24), 
             (char)(decoder->inputFormat.mFormatID >> 16),
             (char)(decoder->inputFormat.mFormatID >> 8), 
             (char)(decoder->inputFormat.mFormatID),
             (char)(decoder->outputFormat.mFormatID >> 24), 
             (char)(decoder->outputFormat.mFormatID >> 16),
             (char)(decoder->outputFormat.mFormatID >> 8), 
             (char)(decoder->outputFormat.mFormatID));
    
    return noErr;
}

static OSStatus Uninitialize(void *self) {
    EAC3Decoder *decoder = EAC3_DECODER;
    
    DebugLog("Uninitialize called: self=%p, decoder=%p", self, decoder);
    
    if (!decoder->isInitialized) {
        return kAudioCodecStateError;
    }
    
    if (decoder->packet) {
        av_packet_free(&decoder->packet);
    }
    
    if (decoder->frame) {
        av_frame_free(&decoder->frame);
    }
    
    if (decoder->parser) {
        av_parser_close(decoder->parser);
        decoder->parser = NULL;
    }
    
    if (decoder->codecContext) {
        avcodec_free_context(&decoder->codecContext);
    }
    
    free(decoder->inputBuffer);
    decoder->inputBuffer = NULL;
    
    free(decoder->outputBuffer);
    decoder->outputBuffer = NULL;
    
    decoder->isInitialized = false;
    return noErr;
}

static OSStatus AppendInputData(void *self,
                               const void *inInputData,
                               UInt32 *ioInputDataByteSize,
                               UInt32 *ioNumberPackets,
                               const AudioStreamPacketDescription *inPacketDescription) {
    EAC3Decoder *decoder = EAC3_DECODER;
    
    DebugLog("AppendInputData called! self=%p, decoder=%p, bytes=%u packets=%u", 
             self, decoder,
             ioInputDataByteSize ? *ioInputDataByteSize : 0,
             ioNumberPackets ? *ioNumberPackets : 0);
    
    if (!decoder->isInitialized) {
        return kAudioCodecStateError;
    }
    
    if (inInputData && ioInputDataByteSize && *ioInputDataByteSize > 0) {
        if (decoder->inputBufferUsed + *ioInputDataByteSize > decoder->inputBufferSize) {
            decoder->inputBufferSize = (decoder->inputBufferUsed + *ioInputDataByteSize) * 2;
            decoder->inputBuffer = realloc(decoder->inputBuffer, decoder->inputBufferSize);
        }
        
        memcpy(decoder->inputBuffer + decoder->inputBufferUsed, inInputData, *ioInputDataByteSize);
        decoder->inputBufferUsed += *ioInputDataByteSize;
        
        // Log first few bytes to check format
        if (decoder->inputBufferUsed >= 4) {
            DebugLog("AppendInputData: First 4 bytes: %02x %02x %02x %02x", 
                     decoder->inputBuffer[0], decoder->inputBuffer[1], 
                     decoder->inputBuffer[2], decoder->inputBuffer[3]);
        }
    }
    
    return noErr;
}

static OSStatus ProduceOutputData(void *self,
                                 void *outOutputData,
                                 UInt32 *ioOutputDataByteSize,
                                 UInt32 *ioNumberPackets,
                                 AudioStreamPacketDescription *outPacketDescription,
                                 UInt32 *outStatus) {
    EAC3Decoder *decoder = EAC3_DECODER;
    
    DebugLog("ProduceOutputData called! self=%p, decoder=%p, bytes=%u packets=%u", 
             self, decoder,
             ioOutputDataByteSize ? *ioOutputDataByteSize : 0,
             ioNumberPackets ? *ioNumberPackets : 0);
    
    if (!decoder->isInitialized) {
        return kAudioCodecStateError;
    }
    
    // Decode frames if we need more
    // Calculate requested frames from byte size
    UInt32 requestedFrames = *ioOutputDataByteSize / decoder->outputFormat.mBytesPerFrame;
    
    DebugLog("ProduceOutputData: Decoding... requestedPackets=%u, requestedBytes=%u (frames=%u), bufferedFrames=%u, inputBytes=%u", 
             *ioNumberPackets, *ioOutputDataByteSize, requestedFrames, decoder->outputBufferUsed, decoder->inputBufferUsed);
    
    // If we have input data, try to decode it directly as a complete frame
    if (decoder->inputBufferUsed > 0 && decoder->outputBufferUsed < requestedFrames) {
        // Set packet data directly from input buffer
        decoder->packet->data = decoder->inputBuffer;
        decoder->packet->size = decoder->inputBufferUsed;
        
        DebugLog("ProduceOutputData: Attempting to decode %d bytes directly", decoder->packet->size);
        
        if (decoder->packet->size > 0) {
            // Send packet to decoder
            int ret = avcodec_send_packet(decoder->codecContext, decoder->packet);
            if (ret < 0 && ret != AVERROR_INVALIDDATA) {
                DebugLog("ProduceOutputData: Error sending packet, ret=%d", ret);
            } else {
            
            // Receive frames
            while (ret >= 0) {
                ret = avcodec_receive_frame(decoder->codecContext, decoder->frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    DebugLog("ProduceOutputData: Error receiving frame, ret=%d", ret);
                    break;
                }
                
                DebugLog("ProduceOutputData: Decoded frame with %d samples", decoder->frame->nb_samples);
                
                // Update output format info from decoder if needed
                if (decoder->codecContext->channels != decoder->outputFormat.mChannelsPerFrame ||
                    decoder->codecContext->sample_rate != decoder->outputFormat.mSampleRate) {
                    decoder->outputFormat.mChannelsPerFrame = decoder->codecContext->channels;
                    decoder->outputFormat.mSampleRate = decoder->codecContext->sample_rate;
                    decoder->outputFormat.mBytesPerFrame = decoder->outputFormat.mChannelsPerFrame * sizeof(Float32);
                    decoder->outputFormat.mBytesPerPacket = decoder->outputFormat.mBytesPerFrame;
                }
                
                // Convert to float
                UInt32 samples = decoder->frame->nb_samples;
                UInt32 channels = decoder->codecContext->channels;
                
                // Ensure we have enough space
                UInt32 requiredFrames = decoder->outputBufferUsed + samples;
                if (requiredFrames > decoder->outputBufferFrames) {
                    decoder->outputBufferFrames = requiredFrames * 2;
                    decoder->outputBuffer = realloc(decoder->outputBuffer,
                                               decoder->outputBufferFrames * channels * sizeof(Float32));
                    if (!decoder->outputBuffer) {
                        return kAudioCodecStateError;
                    }
                }
                
                Float32 *output = decoder->outputBuffer + (decoder->outputBufferUsed * channels);
                
                // Handle different sample formats
                switch (decoder->codecContext->sample_fmt) {
                    case AV_SAMPLE_FMT_FLTP:
                        // Planar float
                        for (UInt32 sample = 0; sample < samples; sample++) {
                            for (UInt32 channel = 0; channel < channels; channel++) {
                                float *channelData = (float *)decoder->frame->data[channel];
                                *output++ = channelData[sample];
                            }
                        }
                        break;
                        
                    case AV_SAMPLE_FMT_FLT:
                        // Interleaved float
                        memcpy(output, decoder->frame->data[0], samples * channels * sizeof(float));
                        break;
                        
                    case AV_SAMPLE_FMT_S16P:
                        // Planar 16-bit
                        for (UInt32 sample = 0; sample < samples; sample++) {
                            for (UInt32 channel = 0; channel < channels; channel++) {
                                int16_t *channelData = (int16_t *)decoder->frame->data[channel];
                                *output++ = channelData[sample] / 32768.0f;
                            }
                        }
                        break;
                        
                    case AV_SAMPLE_FMT_S16:
                        // Interleaved 16-bit
                        {
                            int16_t *input = (int16_t *)decoder->frame->data[0];
                            for (UInt32 i = 0; i < samples * channels; i++) {
                                *output++ = input[i] / 32768.0f;
                            }
                        }
                        break;
                        
                    default:
                        DebugLog("ProduceOutputData: Unsupported sample format %d", 
                                decoder->codecContext->sample_fmt);
                        break;
                }
                
                decoder->outputBufferUsed += samples;
                decoder->packetFrameSize = samples;
            }
            }
            
            // Clear input buffer after successful decode
            if (decoder->outputBufferUsed > 0) {
                decoder->inputBufferUsed = 0;
            }
        }
    }
    
    DebugLog("ProduceOutputData: After decode, bufferedFrames=%u", 
             decoder->outputBufferUsed);
    
    // Copy output
    UInt32 framesToCopy = decoder->outputBufferUsed;
    UInt32 maxFrames = *ioOutputDataByteSize / decoder->outputFormat.mBytesPerFrame;
    if (framesToCopy > maxFrames) {
        framesToCopy = maxFrames;
    }
    
    if (framesToCopy > 0) {
        UInt32 bytesToCopy = framesToCopy * decoder->outputFormat.mBytesPerFrame;
        if (bytesToCopy > *ioOutputDataByteSize) {
            bytesToCopy = *ioOutputDataByteSize;
            framesToCopy = bytesToCopy / decoder->outputFormat.mBytesPerFrame;
        }
        
        memcpy(outOutputData, decoder->outputBuffer, bytesToCopy);
        *ioOutputDataByteSize = bytesToCopy;
        
        DebugLog("ProduceOutputData: Copied %u bytes (%u frames)", bytesToCopy, framesToCopy);
        
        // Move remaining frames to the beginning of the buffer
        UInt32 remainingFrames = decoder->outputBufferUsed - framesToCopy;
        if (remainingFrames > 0) {
            UInt32 channelCount = decoder->outputFormat.mChannelsPerFrame;
            memmove(decoder->outputBuffer, 
                   decoder->outputBuffer + (framesToCopy * channelCount),
                   remainingFrames * channelCount * sizeof(Float32));
        }
        decoder->outputBufferUsed = remainingFrames;
        
        if (outPacketDescription) {
            for (UInt32 i = 0; i < framesToCopy; i++) {
                outPacketDescription[i].mStartOffset = i * decoder->outputFormat.mBytesPerFrame;
                outPacketDescription[i].mVariableFramesInPacket = 0;
                outPacketDescription[i].mDataByteSize = decoder->outputFormat.mBytesPerFrame;
            }
        }
    } else {
        *ioOutputDataByteSize = 0;
    }
    
    // For compressed formats, report packets (1 packet = 1536 frames for EAC3)
    if (framesToCopy > 0) {
        *ioNumberPackets = (framesToCopy + 1535) / 1536; // Round up
    } else {
        *ioNumberPackets = 0;
    }
    
    *outStatus = (decoder->inputBufferUsed == 0 && framesToCopy == 0) ?
                 kAudioCodecProduceOutputPacketAtEOF :
                 kAudioCodecProduceOutputPacketSuccess;
    
    return noErr;
}

static OSStatus Reset(void *self) {
    EAC3Decoder *decoder = EAC3_DECODER;
    
    DebugLog("Reset called: self=%p, decoder=%p", self, decoder);
    
    if (!decoder->isInitialized) {
        return noErr;
    }
    
    // Clear buffers completely
    decoder->inputBufferUsed = 0;
    decoder->outputBufferUsed = 0;
    if (decoder->inputBuffer) {
        memset(decoder->inputBuffer, 0, decoder->inputBufferSize);
    }
    if (decoder->outputBuffer) {
        memset(decoder->outputBuffer, 0, decoder->outputBufferFrames * 8 * sizeof(Float32));
    }
    
    // Flush decoder
    if (decoder->codecContext) {
        avcodec_flush_buffers(decoder->codecContext);
    }
    
    // Reset parser - recreate it to ensure clean state
    if (decoder->parser) {
        av_parser_close(decoder->parser);
        decoder->parser = av_parser_init(decoder->codec->id);
        if (!decoder->parser) {
            DebugLog("Reset: Failed to recreate parser");
        }
    }
    
    // Clear packet
    if (decoder->packet) {
        av_packet_unref(decoder->packet);
    }
    
    // Clear frame
    if (decoder->frame) {
        av_frame_unref(decoder->frame);
    }
    
    DebugLog("Reset complete");
    
    return noErr;
}

// Modern AudioComponent support
static AudioComponentMethod EAC3LookupMethod(SInt16 selector) {
    DebugLog("LookupMethod called: selector=%d", selector);
    switch (selector) {
        case kAudioCodecGetPropertyInfoSelect:
            return (AudioComponentMethod)GetPropertyInfo;
        case kAudioCodecGetPropertySelect:
            return (AudioComponentMethod)GetProperty;
        case kAudioCodecSetPropertySelect:
            return (AudioComponentMethod)SetProperty;
        case kAudioCodecInitializeSelect:
            return (AudioComponentMethod)Initialize;
        case kAudioCodecUninitializeSelect:
            return (AudioComponentMethod)Uninitialize;
        case kAudioCodecAppendInputDataSelect:
            return (AudioComponentMethod)AppendInputData;
        case kAudioCodecProduceOutputDataSelect:
            return (AudioComponentMethod)ProduceOutputData;
        case kAudioCodecResetSelect:
            return (AudioComponentMethod)Reset;
        default:
            DebugLog("Unknown selector: %d", selector);
            return NULL;
    }
}

static OSStatus EAC3OpenProc(void *self, AudioComponentInstance inInstance) {
    DebugLog("OpenProc called: self=%p, instance=%p", self, inInstance);
    
    // Initialize the decoder instance in place
    EAC3Decoder *decoder = EAC3_DECODER;
    memset(decoder, 0, sizeof(EAC3Decoder));
    
    // Initialize defaults
    decoder->inputFormat.mFormatID = kEAC3Format;
    decoder->inputFormat.mFormatFlags = 0;
    decoder->inputFormat.mBytesPerPacket = 0;
    decoder->inputFormat.mFramesPerPacket = 0;
    decoder->inputFormat.mBytesPerFrame = 0;
    decoder->inputFormat.mChannelsPerFrame = 2;
    decoder->inputFormat.mBitsPerChannel = 0;
    decoder->inputFormat.mSampleRate = 48000;
    
    decoder->outputFormat.mFormatID = kAudioFormatLinearPCM;
    decoder->outputFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    decoder->outputFormat.mBitsPerChannel = 32;
    decoder->outputFormat.mFramesPerPacket = 1;
    decoder->outputFormat.mChannelsPerFrame = 2;
    decoder->outputFormat.mBytesPerFrame = 8;
    decoder->outputFormat.mBytesPerPacket = 8;
    decoder->outputFormat.mSampleRate = 48000;
    decoder->packetFrameSize = 1536;
    decoder->maxPacketSize = 4096;
    
    DebugLog("Initialized decoder at %p", decoder);
    
    return noErr;
}

static OSStatus EAC3CloseProc(void *self) {
    DebugLog("CloseProc called: self=%p", self);
    
    EAC3Decoder *decoder = EAC3_DECODER;
    if (decoder->isInitialized) {
        Uninitialize(self);
    }
    
    // The AudioComponentPlugInInstance will be freed by the system
    
    return noErr;
}

// Factory function for AudioComponent
__attribute__((visibility("default")))
void *EAC3DecoderEntry(void *params, void *storage) {
    AudioComponentDescription *desc = (AudioComponentDescription *)params;
    
    DebugLog("EAC3DecoderEntry called: params=%p, storage=%p", params, storage);
    
    if (desc && desc->componentType == kEAC3DecoderComponentType && 
        (desc->componentSubType == kEAC3Format || desc->componentSubType == kEAC3FormatMP4)) {
        DebugLog("Called as factory function! type=%c%c%c%c, subtype=%c%c%c%c",
                 (char)(desc->componentType >> 24), (char)(desc->componentType >> 16),
                 (char)(desc->componentType >> 8), (char)(desc->componentType),
                 (char)(desc->componentSubType >> 24), (char)(desc->componentSubType >> 16),
                 (char)(desc->componentSubType >> 8), (char)(desc->componentSubType));
        
        // Allocate the plugin instance structure including space for the decoder
        size_t instanceSize = sizeof(AudioComponentPlugInInstance);
        AudioComponentPlugInInstance *acpi = (AudioComponentPlugInInstance *)malloc(instanceSize);
        if (!acpi) {
            DebugLog("Failed to allocate plugin instance");
            return NULL;
        }
        
        // Initialize the plugin interface
        acpi->mPlugInInterface.Open = EAC3OpenProc;
        acpi->mPlugInInterface.Close = EAC3CloseProc;
        acpi->mPlugInInterface.Lookup = EAC3LookupMethod;
        acpi->mPlugInInterface.reserved = NULL;
        
        // Clear padding and instance storage
        memset(acpi->mPad, 0, sizeof(acpi->mPad));
        memset(&acpi->mInstanceStorage, 0, sizeof(EAC3Decoder));
        
        DebugLog("Returning plugin instance: %p (interface at %p)", acpi, &acpi->mPlugInInterface);
        return &acpi->mPlugInInterface;
    }
    
    DebugLog("Invalid component description!");
    return NULL;
}