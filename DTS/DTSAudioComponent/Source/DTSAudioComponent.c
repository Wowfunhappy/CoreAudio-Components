#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AudioCodec.h>
#include <AudioUnit/AudioComponent.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// Define DEBUG_LOGGING to enable debug output
#define DEBUG_LOGGING 1

#ifdef DEBUG_LOGGING
// Debug logging
static FILE *gLogFile = NULL;
static void DebugLog(const char *format, ...) {
    if (!gLogFile) {
        gLogFile = fopen("/tmp/dts_component.log", "a");
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

// DTS format constants
#define kDTSDecoderComponentType 'adec'
#define kDTSFormat 'dts '
#define kDTSFormatMP4 'dtsh'
#define kDTSFormatCore 'dtsc'
#define kDTSManufacturer 'dts '

// DTS decoder state
typedef struct DTSDecoder {
    AudioStreamBasicDescription inputFormat;
    AudioStreamBasicDescription outputFormat;
    
    AVCodec *codec;
    AVCodecContext *codecContext;
    AVCodecParserContext *parser;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *swrContext;  // For channel downmixing
    UInt32 lastSystemChannels;  // To detect speaker configuration changes
    
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
} DTSDecoder;

// AudioComponent plugin instance structure
// This matches Apple's layout from ACPlugInDispatch.cpp
typedef struct AudioComponentPlugInInstance {
    AudioComponentPlugInInterface mPlugInInterface;
    void *mPad[4];  // Required padding for binary compatibility
    DTSDecoder mInstanceStorage;
} AudioComponentPlugInInstance;

// Macros to access the instance from self pointer
#define ACPI ((AudioComponentPlugInInstance *)self)
#define DTS_DECODER (&ACPI->mInstanceStorage)

// Channel layout tags for different channel configurations
static const AudioChannelLayoutTag kChannelLayoutTags[8] = {
    kAudioChannelLayoutTag_Mono,         // 1 channel: C
    kAudioChannelLayoutTag_Stereo,       // 2 channels: L R
    kAudioChannelLayoutTag_MPEG_3_0_B,   // 3 channels: C L R
    kAudioChannelLayoutTag_Quadraphonic, // 4 channels: L R Ls Rs
    kAudioChannelLayoutTag_MPEG_5_0_D,   // 5 channels: C L R Ls Rs
    kAudioChannelLayoutTag_MPEG_5_1_D,   // 6 channels: C L R Ls Rs LFE
    kAudioChannelLayoutTag_AAC_6_1,      // 7 channels: C L R Ls Rs Cs LFE
    kAudioChannelLayoutTag_MPEG_7_1_B    // 8 channels: C Lc Rc L R Ls Rs LFE
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
            if (outSize) *outSize = 0; // DTS doesn't use magic cookies
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
    DTSDecoder *decoder = DTS_DECODER;
    
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
                DebugLog("GetProperty: CurrentOutputFormat - channels=%u, bytesPerFrame=%u, sampleRate=%.0f",
                         decoder->outputFormat.mChannelsPerFrame,
                         decoder->outputFormat.mBytesPerFrame,
                         decoder->outputFormat.mSampleRate);
                *(AudioStreamBasicDescription *)outPropertyData = decoder->outputFormat;
            } else {
                return kAudioCodecBadPropertySizeError;
            }
            return noErr;
            
        case kAudioCodecPropertySupportedInputFormats:
        case kAudioCodecPropertyInputFormatsForOutputFormat:
            {
                // DTS supports one input format with variable parameters
                UInt32 theNumberFormats = *ioPropertyDataSize / sizeof(AudioStreamBasicDescription);
                if (theNumberFormats > 0) {
                    AudioStreamBasicDescription *format = (AudioStreamBasicDescription *)outPropertyData;
                    memset(format, 0, sizeof(AudioStreamBasicDescription));
                    
                    format->mFormatID = kDTSFormat;
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
            if (*ioPropertyDataSize != sizeof(AudioCodecPrimeInfo)) {
                return kAudioCodecBadPropertySizeError;
            }
            AudioCodecPrimeInfo *prime = (AudioCodecPrimeInfo *)outPropertyData;
            prime->leadingFrames = 0;
            prime->trailingFrames = 0;
            return noErr;
            
        case kAudioCodecPropertyNameCFString:
            if (*ioPropertyDataSize != sizeof(CFStringRef)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(CFStringRef *)outPropertyData = CFSTR("DTS Audio Decoder");
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
                
                // DTS format with no channel layout
                formatList->mASBD.mFormatID = kDTSFormat;
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
            *(CFStringRef *)outPropertyData = CFSTR("DTS (Digital Theater Systems)");
            CFRetain(*(CFStringRef *)outPropertyData);
            return noErr;
            
        case kAudioCodecPropertyMagicCookie:
            // DTS doesn't use magic cookies
            *ioPropertyDataSize = 0;
            return noErr;
            
        case kAudioCodecPropertyRequiresPacketDescription:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = 1; // DTS has variable packet sizes
            return noErr;
            
        case kAudioCodecPropertyAvailableNumberChannels:
            if (*ioPropertyDataSize < sizeof(UInt32) * 8) {
                return kAudioCodecBadPropertySizeError;
            }
            // Report support for 1-8 channels
            for (UInt32 i = 0; i < 8; i++) {
                ((UInt32 *)outPropertyData)[i] = i + 1;
            }
            *ioPropertyDataSize = sizeof(UInt32) * 8;
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
    DTSDecoder *decoder = DTS_DECODER;
    
    // No property can be set when the codec is initialized
    if (decoder->isInitialized) {
        return kAudioCodecIllegalOperationError;
    }
    
    switch (inPropertyID) {
        case kAudioCodecPropertyCurrentInputFormat:
            if (inPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                const AudioStreamBasicDescription *format = (const AudioStreamBasicDescription *)inPropertyData;
                
                // Validate DTS format
                if (format->mFormatID != kDTSFormat && format->mFormatID != kDTSFormatMP4 && format->mFormatID != kDTSFormatCore) {
                    DebugLog("SetProperty: Invalid format ID: %c%c%c%c",
                             (char)(format->mFormatID >> 24), (char)(format->mFormatID >> 16),
                             (char)(format->mFormatID >> 8), (char)(format->mFormatID));
                    return kAudioCodecUnsupportedFormatError;
                }
                
                // DTS supports 1-8 channels
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

// Helper function to get the system's default output device channel layout
static AudioChannelLayout* GetSystemOutputChannelLayout(UInt32 *outChannelCount) {
    AudioDeviceID deviceID;
    UInt32 propertySize = sizeof(deviceID);
    
    // Get the default output device
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster
    };
    
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress,
                                                0, NULL, &propertySize, &deviceID);
    if (status != noErr || deviceID == kAudioDeviceUnknown) {
        DebugLog("GetSystemOutputChannelLayout: Failed to get default output device");
        if (outChannelCount) *outChannelCount = 2;
        return NULL;
    }
    
    // Try to get preferred channel layout
    propertyAddress.mSelector = kAudioDevicePropertyPreferredChannelLayout;
    propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
    
    status = AudioObjectGetPropertyDataSize(deviceID, &propertyAddress, 0, NULL, &propertySize);
    if (status == noErr && propertySize > 0) {
        AudioChannelLayout *layout = (AudioChannelLayout *)malloc(propertySize);
        if (layout) {
            status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &propertySize, layout);
            if (status == noErr) {
                DebugLog("GetSystemOutputChannelLayout: Got preferred channel layout, tag=%u, channels=%u", 
                         layout->mChannelLayoutTag, layout->mNumberChannelDescriptions);
                
                // Log individual channel descriptions if using channel descriptions
                if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
                    DebugLog("GetSystemOutputChannelLayout: Using channel descriptions, count=%u", 
                             layout->mNumberChannelDescriptions);
                    for (UInt32 i = 0; i < layout->mNumberChannelDescriptions; i++) {
                        AudioChannelDescription *desc = &layout->mChannelDescriptions[i];
                        DebugLog("  Channel %u: label=%u, flags=0x%x, coords=[%f,%f,%f]", 
                                 i, desc->mChannelLabel, desc->mChannelFlags,
                                 desc->mCoordinates[0], desc->mCoordinates[1], desc->mCoordinates[2]);
                    }
                }
                
                // Get channel count from layout
                if (outChannelCount) {
                    if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions) {
                        // Get channel count from layout tag
                        UInt32 channelCount = 0;
                        UInt32 propSize = sizeof(channelCount);
                        AudioFormatGetProperty(kAudioFormatProperty_NumberOfChannelsForLayout,
                                             sizeof(AudioChannelLayoutTag), &layout->mChannelLayoutTag,
                                             &propSize, &channelCount);
                        *outChannelCount = channelCount;
                    } else {
                        *outChannelCount = layout->mNumberChannelDescriptions;
                    }
                }
                return layout;
            }
            free(layout);
        }
    }
    
    DebugLog("GetSystemOutputChannelLayout: No preferred channel layout, falling back to stream config");
    
    // Fall back to getting channel count from stream configuration
    propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;
    propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
    
    status = AudioObjectGetPropertyDataSize(deviceID, &propertyAddress, 0, NULL, &propertySize);
    if (status != noErr) {
        DebugLog("GetSystemOutputChannelLayout: Failed to get stream config size");
        if (outChannelCount) *outChannelCount = 2;
        return NULL;
    }
    
    AudioBufferList *bufferList = (AudioBufferList *)malloc(propertySize);
    if (!bufferList) {
        if (outChannelCount) *outChannelCount = 2;
        return NULL;
    }
    
    status = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &propertySize, bufferList);
    if (status != noErr) {
        free(bufferList);
        DebugLog("GetSystemOutputChannelLayout: Failed to get stream config");
        if (outChannelCount) *outChannelCount = 2;
        return NULL;
    }
    
    // Count total channels
    UInt32 channelCount = 0;
    for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++) {
        channelCount += bufferList->mBuffers[i].mNumberChannels;
    }
    
    free(bufferList);
    DebugLog("GetSystemOutputChannelLayout: System has %u output channels (from stream config)", channelCount);
    if (outChannelCount) *outChannelCount = channelCount;
    return NULL;
}

// Helper function to get the actual channel layout from system descriptions
static int64_t GetActualChannelLayout(AudioChannelLayout *layout, UInt32 *outActualChannels) {
    if (!layout) return 0;
    
    // If we have a specific layout tag, use it
    if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions) {
        if (outActualChannels) {
            UInt32 channelCount = 0;
            UInt32 propSize = sizeof(channelCount);
            AudioFormatGetProperty(kAudioFormatProperty_NumberOfChannelsForLayout,
                                 sizeof(AudioChannelLayoutTag), &layout->mChannelLayoutTag,
                                 &propSize, &channelCount);
            *outActualChannels = channelCount;
        }
        
        // Convert CoreAudio layout tag to FFmpeg layout
        switch (layout->mChannelLayoutTag) {
            case kAudioChannelLayoutTag_Mono:
                return AV_CH_LAYOUT_MONO;
            case kAudioChannelLayoutTag_Stereo:
                return AV_CH_LAYOUT_STEREO;
            case kAudioChannelLayoutTag_MPEG_3_0_B:
                return AV_CH_LAYOUT_SURROUND;  // C L R
            case kAudioChannelLayoutTag_Quadraphonic:
                return AV_CH_LAYOUT_QUAD;
            case kAudioChannelLayoutTag_MPEG_5_0_D:
                return AV_CH_LAYOUT_5POINT0;  // C L R Ls Rs
            case kAudioChannelLayoutTag_MPEG_5_1_D:
                return AV_CH_LAYOUT_5POINT1;
            case kAudioChannelLayoutTag_AAC_6_1:
                return AV_CH_LAYOUT_6POINT1;  // C L R Ls Rs Cs LFE
            case kAudioChannelLayoutTag_MPEG_7_1_A:
            case kAudioChannelLayoutTag_MPEG_7_1_B:
            case kAudioChannelLayoutTag_MPEG_7_1_C:
            case kAudioChannelLayoutTag_Emagic_Default_7_1:
            case kAudioChannelLayoutTag_AAC_7_1_B:
            case kAudioChannelLayoutTag_AAC_7_1_C:
                return AV_CH_LAYOUT_7POINT1;
            default:
                return 0;
        }
    }
    
    // Build layout from channel descriptions
    int64_t channelLayout = 0;
    UInt32 validChannels = 0;
    
    for (UInt32 i = 0; i < layout->mNumberChannelDescriptions; i++) {
        AudioChannelLabel label = layout->mChannelDescriptions[i].mChannelLabel;
        
        // Skip invalid/unknown channels
        if (label == kAudioChannelLabel_Unknown || label == 0xFFFFFFFF) {
            // For invalid labels, assume stereo L/R based on position
            if (layout->mNumberChannelDescriptions == 2) {
                if (i == 0) {
                    channelLayout |= AV_CH_FRONT_LEFT;
                } else if (i == 1) {
                    channelLayout |= AV_CH_FRONT_RIGHT;
                }
                validChannels++;
            }
            continue;
        }
        
        validChannels++;
        
        // Map CoreAudio labels to FFmpeg channel flags
        switch (label) {
            case kAudioChannelLabel_Left:
                channelLayout |= AV_CH_FRONT_LEFT;
                break;
            case kAudioChannelLabel_Right:
                channelLayout |= AV_CH_FRONT_RIGHT;
                break;
            case kAudioChannelLabel_Center:
                channelLayout |= AV_CH_FRONT_CENTER;
                break;
            case kAudioChannelLabel_LFEScreen:
                channelLayout |= AV_CH_LOW_FREQUENCY;
                break;
            case kAudioChannelLabel_LeftSurround:
                channelLayout |= AV_CH_BACK_LEFT;
                break;
            case kAudioChannelLabel_RightSurround:
                channelLayout |= AV_CH_BACK_RIGHT;
                break;
            case kAudioChannelLabel_LeftCenter:
                channelLayout |= AV_CH_FRONT_LEFT_OF_CENTER;
                break;
            case kAudioChannelLabel_RightCenter:
                channelLayout |= AV_CH_FRONT_RIGHT_OF_CENTER;
                break;
            case kAudioChannelLabel_CenterSurround:
                channelLayout |= AV_CH_BACK_CENTER;
                break;
            case kAudioChannelLabel_LeftSurroundDirect:
                channelLayout |= AV_CH_SIDE_LEFT;
                break;
            case kAudioChannelLabel_RightSurroundDirect:
                channelLayout |= AV_CH_SIDE_RIGHT;
                break;
        }
    }
    
    if (outActualChannels) {
        *outActualChannels = validChannels;
    }
    
    DebugLog("GetActualChannelLayout: Built channel layout 0x%llx with %u valid channels", 
             (unsigned long long)channelLayout, validChannels);
    
    return channelLayout;
}

// Helper function to get the system's default output device channel count
static UInt32 GetSystemOutputChannelCount(void) {
    UInt32 channelCount = 2;
    AudioChannelLayout *layout = GetSystemOutputChannelLayout(&channelCount);
    if (layout) free(layout);
    return channelCount;
}

static OSStatus Initialize(void *self,
                          const AudioStreamBasicDescription *inInputFormat,
                          const AudioStreamBasicDescription *inOutputFormat,
                          const void *inMagicCookie,
                          UInt32 inMagicCookieByteSize) {
    DTSDecoder *decoder = DTS_DECODER;
    
    DebugLog("Initialize called! self=%p, decoder=%p", self, decoder);
    DebugLog("Initialize: Input format - channels=%u, Output format - channels=%u",
             decoder->inputFormat.mChannelsPerFrame, decoder->outputFormat.mChannelsPerFrame);
    
    // Check system audio configuration
    UInt32 systemChannels = GetSystemOutputChannelCount();
    DebugLog("Initialize: System audio output has %u channels", systemChannels);
    
    // Save our desired channel count before QuickTime overwrites it
    UInt32 desiredChannels = decoder->inputFormat.mChannelsPerFrame;
    UInt32 requestedOutputChannels = decoder->outputFormat.mChannelsPerFrame;  // Save current output channels
    DebugLog("Initialize: Saving original desired channels = %u", desiredChannels);
    
    if (decoder->isInitialized) {
        DebugLog("Initialize: Already initialized");
        return kAudioCodecStateError;
    }
    
    if (inInputFormat) {
        // Validate input format
        if (inInputFormat->mFormatID != kDTSFormat && inInputFormat->mFormatID != kDTSFormatMP4 && inInputFormat->mFormatID != kDTSFormatCore) {
            DebugLog("Initialize: Invalid input format ID");
            return kAudioCodecUnsupportedFormatError;
        }
        decoder->inputFormat = *inInputFormat;
        DebugLog("Initialize: QuickTime provided format with %u channels", inInputFormat->mChannelsPerFrame);
    }
    
    if (inOutputFormat) {
        // Validate output format
        if (inOutputFormat->mFormatID != kAudioFormatLinearPCM ||
            (inOutputFormat->mFormatFlags & kAudioFormatFlagIsFloat) == 0) {
            DebugLog("Initialize: Invalid output format");
            return kAudioCodecUnsupportedFormatError;
        }
        decoder->outputFormat = *inOutputFormat;
        requestedOutputChannels = inOutputFormat->mChannelsPerFrame;
        DebugLog("Initialize: QuickTime requested %u output channels", inOutputFormat->mChannelsPerFrame);
    }
    
    // Find DTS decoder
    decoder->codec = avcodec_find_decoder(AV_CODEC_ID_DTS);
    if (!decoder->codec) {
        DebugLog("Initialize: Failed to find DTS decoder");
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
    } else {
        // Default to 8 channels if not specified
        decoder->codecContext->channels = 8;
        decoder->inputFormat.mChannelsPerFrame = 8;
        decoder->outputFormat.mChannelsPerFrame = 8;
        DebugLog("Initialize: No channel count provided, defaulting to 8");
    }
    
    if (decoder->inputFormat.mSampleRate > 0) {
        decoder->codecContext->sample_rate = decoder->inputFormat.mSampleRate;
        decoder->outputFormat.mSampleRate = decoder->inputFormat.mSampleRate;
    } else {
        // Default to 48000 Hz if not specified
        decoder->codecContext->sample_rate = 48000;
        decoder->outputFormat.mSampleRate = 48000;
        DebugLog("Initialize: No sample rate provided, defaulting to 48000");
    }
    
    // Always recalculate output format sizes based on current channel count
    decoder->outputFormat.mBytesPerFrame = decoder->outputFormat.mChannelsPerFrame * sizeof(Float32);
    decoder->outputFormat.mBytesPerPacket = decoder->outputFormat.mBytesPerFrame * decoder->outputFormat.mFramesPerPacket;
    
    // Open codec
    if (avcodec_open2(decoder->codecContext, decoder->codec, NULL) < 0) {
        DebugLog("Initialize: Failed to open codec");
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    DebugLog("Initialize: After avcodec_open2 - channels=%d, sample_rate=%d", 
             decoder->codecContext->channels, decoder->codecContext->sample_rate);
    
    // avcodec_open2 may reset the channel count to 2 for DTS
    // We need to preserve our configured channel count for INPUT
    if (desiredChannels > 0) {
        decoder->codecContext->channels = desiredChannels;
        decoder->inputFormat.mChannelsPerFrame = desiredChannels;
        
        // Always force output to 8 channels. QuickTime locks the channel configuration during Initialize,
        // before we know the actual channel count from the DTS bitstream.
        
        decoder->outputFormat.mChannelsPerFrame = 8;
        DebugLog("Initialize: Forcing 8 output channels (system has %u, input has %u)", 
                 systemChannels, desiredChannels);
        
        decoder->outputFormat.mBytesPerFrame = decoder->outputFormat.mChannelsPerFrame * sizeof(Float32);
        decoder->outputFormat.mBytesPerPacket = decoder->outputFormat.mBytesPerFrame * decoder->outputFormat.mFramesPerPacket;
        DebugLog("Initialize: Input channels=%u, Output channels=%u", 
                 decoder->inputFormat.mChannelsPerFrame, decoder->outputFormat.mChannelsPerFrame);
    }
    
    DebugLog("Initialize: Output format after restoration - channels=%u, bytesPerFrame=%u",
             decoder->outputFormat.mChannelsPerFrame, decoder->outputFormat.mBytesPerFrame);
    
    // Create parser
    decoder->parser = av_parser_init(decoder->codec->id);
    if (!decoder->parser) {
        DebugLog("Initialize: Failed to create parser");
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    // Configure parser flags - DTS parser should find frame boundaries itself
    // decoder->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    
    // Log parser info
    DebugLog("Initialize: Parser initialized, flags=0x%x", decoder->parser->flags);
    
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
    
    // DTS frame sizes can vary - common sizes are 512, 1024, 2048, or 4096 samples
    decoder->packetFrameSize = 512;  // Start with minimum DTS frame size
    decoder->maxPacketSize = 8192;   // DTS packets can be larger than EAC3
    decoder->lastSystemChannels = 0;  // Will be set when swresample is initialized
    
    decoder->isInitialized = true;
    
    DebugLog("Initialize: Final output format - channels=%u, bytesPerFrame=%u, sampleRate=%.0f",
             decoder->outputFormat.mChannelsPerFrame, decoder->outputFormat.mBytesPerFrame,
             decoder->outputFormat.mSampleRate);
    
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
    DTSDecoder *decoder = DTS_DECODER;
    
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
    
    if (decoder->swrContext) {
        swr_free(&decoder->swrContext);
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
    DTSDecoder *decoder = DTS_DECODER;
    
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
        if (decoder->inputBufferUsed >= 8) {
            DebugLog("AppendInputData: First 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     decoder->inputBuffer[0], decoder->inputBuffer[1], 
                     decoder->inputBuffer[2], decoder->inputBuffer[3],
                     decoder->inputBuffer[4], decoder->inputBuffer[5],
                     decoder->inputBuffer[6], decoder->inputBuffer[7]);
            
            // Check for DTS sync words
            uint32_t sync = (decoder->inputBuffer[0] << 24) | (decoder->inputBuffer[1] << 16) | 
                           (decoder->inputBuffer[2] << 8) | decoder->inputBuffer[3];
            if (sync == 0x7FFE8001) {
                DebugLog("AppendInputData: Found DTS Core sync word (big endian)");
            } else if (sync == 0xFE7F0180) {
                DebugLog("AppendInputData: Found DTS Core sync word (little endian)");  
            } else if (sync == 0x64582025) {
                DebugLog("AppendInputData: Found DTS-HD sync word");
            }
        }
        
    } else if (ioInputDataByteSize && *ioInputDataByteSize == 0) {
        // End of stream - mark decoder for flushing
        DebugLog("AppendInputData: End of stream signaled (0 bytes)");
        decoder->needsReset = true;
    }
    
    return noErr;
}

static OSStatus ProduceOutputData(void *self,
                                 void *outOutputData,
                                 UInt32 *ioOutputDataByteSize,
                                 UInt32 *ioNumberPackets,
                                 AudioStreamPacketDescription *outPacketDescription,
                                 UInt32 *outStatus) {
    DTSDecoder *decoder = DTS_DECODER;
    
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
    
    // Check if we need to flush the decoder
    if (decoder->needsReset && decoder->outputBufferUsed < requestedFrames) {
        DebugLog("ProduceOutputData: Flushing decoder");
        
        // Send NULL packet to flush decoder
        int ret = avcodec_send_packet(decoder->codecContext, NULL);
        if (ret >= 0) {
            // Receive any remaining frames
            while (1) {
                ret = avcodec_receive_frame(decoder->codecContext, decoder->frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                    break;
                }
                if (ret < 0) {
                    break;
                }
                
                // Process the flushed frame (same processing as normal frames)
                // This code would be duplicated from below, so we should refactor
                // For now, just log that we got a frame
                DebugLog("ProduceOutputData: Got flushed frame with %d samples", 
                        decoder->frame->nb_samples);
            }
        }
        
        decoder->needsReset = false;
    }
    
    // Parse and decode input data
    if (decoder->inputBufferUsed > 0 && decoder->outputBufferUsed < requestedFrames) {
        UInt8 *data = decoder->inputBuffer;
        int data_size = decoder->inputBufferUsed;
        
        while (data_size > 0) {
            uint8_t *out_data = NULL;
            int out_size = 0;
            
            // Parse the input to extract complete frames
            int consumed = av_parser_parse2(decoder->parser, decoder->codecContext,
                                          &out_data, &out_size,
                                          data, data_size,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            
            if (consumed < 0) {
                DebugLog("ProduceOutputData: Parser error, consumed=%d", consumed);
                break;
            }
            
            data += consumed;
            data_size -= consumed;
            
            // If parser produced output, decode it
            if (out_size > 0) {
                decoder->packet->data = out_data;
                decoder->packet->size = out_size;
                
                DebugLog("ProduceOutputData: Parser produced %d bytes, consumed %d bytes from %d total", 
                        out_size, consumed, data_size + (data - decoder->inputBuffer));
                
                // Send packet to decoder
                int ret = avcodec_send_packet(decoder->codecContext, decoder->packet);
                if (ret < 0) {
                    char errbuf[256];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    DebugLog("ProduceOutputData: Error sending packet, ret=%d (%s)", ret, errbuf);
                    if (ret != AVERROR_INVALIDDATA) {
                        continue;
                    }
                }
            
                // Receive frames
                int framesReceived = 0;
                while (ret >= 0) {
                ret = avcodec_receive_frame(decoder->codecContext, decoder->frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    DebugLog("ProduceOutputData: No more frames available after %d frames (ret=%d)", framesReceived, ret);
                    break;
                } else if (ret < 0) {
                    DebugLog("ProduceOutputData: Error receiving frame, ret=%d", ret);
                    break;
                }
                framesReceived++;
                
                DebugLog("ProduceOutputData: Decoded frame with %d samples, channels=%d, sample_rate=%d, format=%d", 
                        decoder->frame->nb_samples, decoder->codecContext->channels, 
                        decoder->codecContext->sample_rate, decoder->codecContext->sample_fmt);
                
                // Additional debug info for troubleshooting
                if (decoder->frame->data[0]) {
                    // Check if the frame actually contains non-zero data
                    int hasAudio = 0;
                    if (decoder->codecContext->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                        // Planar float format - check first channel
                        float *data = (float *)decoder->frame->data[0];
                        for (int i = 0; i < 10 && i < decoder->frame->nb_samples; i++) {
                            if (data[i] != 0.0f) {
                                hasAudio = 1;
                                break;
                            }
                        }
                    } else if (decoder->codecContext->sample_fmt == AV_SAMPLE_FMT_S16P) {
                        // Planar 16-bit format - check all channels
                        int16_t *data = (int16_t *)decoder->frame->data[0];
                        int checkSamples = (decoder->frame->nb_samples < 100) ? decoder->frame->nb_samples : 100;
                        for (int i = 0; i < checkSamples; i++) {
                            if (data[i] != 0) {
                                hasAudio = 1;
                                break;
                            }
                        }
                        // Log first few samples for debugging
                        if (!hasAudio && decoder->frame->nb_samples > 0) {
                            DebugLog("ProduceOutputData: First 10 S16P samples from channel 0: %d %d %d %d %d %d %d %d %d %d",
                                    data[0], data[1], data[2], data[3], data[4], 
                                    data[5], data[6], data[7], data[8], data[9]);
                            // Also check other channels
                            if (decoder->codecContext->channels > 1 && decoder->frame->data[1]) {
                                int16_t *data1 = (int16_t *)decoder->frame->data[1];
                                DebugLog("ProduceOutputData: First 5 S16P samples from channel 1: %d %d %d %d %d",
                                        data1[0], data1[1], data1[2], data1[3], data1[4]);
                            }
                        }
                    }
                    DebugLog("ProduceOutputData: Frame data check - hasAudio=%d", hasAudio);
                }
                
                // Get channel counts
                UInt32 channels = decoder->codecContext->channels;  // Actual channels in the DTS file
                UInt32 outputChannels = decoder->outputFormat.mChannelsPerFrame;  // Always 8
                UInt32 systemChannels = 0;  // Will be set to actual speaker configuration
                
                // 1. 'channels' = what the DTS file actually contains
                // 2. 'systemChannels' = what speakers the user has (e.g. 2 for stereo)
                // 3. 'outputChannels' = what we tell QuickTime (always 8)
                // We downmix from 'channels' to 'systemChannels' for proper audio mixing,
                // then pad from 'systemChannels' to 'outputChannels' with silence.
                
                AudioChannelLayout *systemLayout = GetSystemOutputChannelLayout(&systemChannels);
                
                // Get the actual system channel layout and valid channel count
                UInt32 actualSystemChannels = systemChannels;
                int64_t actualSystemLayout = GetActualChannelLayout(systemLayout, &actualSystemChannels);
                
                // Use actual valid channels for comparison
                if (actualSystemLayout != 0 && actualSystemChannels < systemChannels) {
                    DebugLog("ProduceOutputData: System reports %u channels but only %u are valid", 
                             systemChannels, actualSystemChannels);
                    systemChannels = actualSystemChannels;
                }
                
                // Check if speaker configuration changed
                if (decoder->swrContext && decoder->lastSystemChannels != systemChannels) {
                    DebugLog("ProduceOutputData: Speaker configuration changed from %u to %u channels", 
                             decoder->lastSystemChannels, systemChannels);
                    swr_free(&decoder->swrContext);
                }
                
                // Set up swresample if needed for channel conversion to system speakers
                // Always set up swresample when content channels don't match system channels
                if (!decoder->swrContext && channels != systemChannels) {
                    decoder->swrContext = swr_alloc();
                    if (!decoder->swrContext) {
                        DebugLog("ProduceOutputData: Failed to allocate swresample context");
                        continue;
                    }
                    
                    // Set up channel layouts - downmix to system speaker configuration
                    int64_t in_channel_layout = av_get_default_channel_layout(channels);
                    
                    // Use codec's channel layout if available
                    if (decoder->codecContext->channel_layout != 0) {
                        in_channel_layout = decoder->codecContext->channel_layout;
                        DebugLog("ProduceOutputData: Using codec's channel layout: 0x%llx", (unsigned long long)in_channel_layout);
                    }
                    
                    int64_t out_channel_layout;
                    
                    // Use the actual system channel layout if we got one
                    if (actualSystemLayout != 0) {
                        out_channel_layout = actualSystemLayout;
                        DebugLog("ProduceOutputData: Using actual system channel layout: 0x%llx", (unsigned long long)actualSystemLayout);
                    } else {
                        // Fall back to FFmpeg's default for the channel count
                        out_channel_layout = av_get_default_channel_layout(systemChannels);
                        DebugLog("ProduceOutputData: Using default layout for %u channels: 0x%llx", 
                                systemChannels, (unsigned long long)out_channel_layout);
                        
                        // If still no layout and we have 2 channels, assume stereo
                        if (out_channel_layout == 0 && systemChannels == 2) {
                            out_channel_layout = AV_CH_LAYOUT_STEREO;
                            DebugLog("ProduceOutputData: Forcing stereo layout for 2 channels");
                        }
                    }
                    
                    // Log the channel layouts for debugging
                    char in_layout_str[64], out_layout_str[64];
                    av_get_channel_layout_string(in_layout_str, sizeof(in_layout_str), channels, in_channel_layout);
                    av_get_channel_layout_string(out_layout_str, sizeof(out_layout_str), systemChannels, out_channel_layout);
                    DebugLog("ProduceOutputData: Channel layouts - in: %s, out: %s", in_layout_str, out_layout_str);
                    
                    // Configure swresample to downmix to system channels
                    av_opt_set_int(decoder->swrContext, "in_channel_layout", in_channel_layout, 0);
                    av_opt_set_int(decoder->swrContext, "out_channel_layout", out_channel_layout, 0);
                    av_opt_set_int(decoder->swrContext, "in_sample_rate", decoder->codecContext->sample_rate, 0);
                    av_opt_set_int(decoder->swrContext, "out_sample_rate", decoder->codecContext->sample_rate, 0);
                    av_opt_set_sample_fmt(decoder->swrContext, "in_sample_fmt", decoder->codecContext->sample_fmt, 0);
                    av_opt_set_sample_fmt(decoder->swrContext, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
                    
                    // Set appropriate downmix coefficients for better quality
                    // These work for any downmix scenario
                    av_opt_set_double(decoder->swrContext, "rematrix_volume", 1.0, 0);
                    av_opt_set_double(decoder->swrContext, "center_mix_level", 0.707, 0);
                    av_opt_set_double(decoder->swrContext, "lfe_mix_level", 0.25, 0);
                    av_opt_set_double(decoder->swrContext, "surround_mix_level", 1.0, 0);
                    
                    DebugLog("ProduceOutputData: Set downmix coefficients for %d -> %u channel conversion", 
                             channels, systemChannels);
                    
                    if (swr_init(decoder->swrContext) < 0) {
                        DebugLog("ProduceOutputData: Failed to initialize swresample");
                        swr_free(&decoder->swrContext);
                        continue;
                    }
                    
                    DebugLog("ProduceOutputData: Initialized swresample for %d -> %u channels (will pad to %u)", 
                             channels, systemChannels, outputChannels);
                    
                    // Remember the system channel count
                    decoder->lastSystemChannels = systemChannels;
                }
                
                // Update sample rate if it differs
                if (decoder->codecContext->sample_rate != decoder->outputFormat.mSampleRate) {
                    DebugLog("ProduceOutputData: Updating sample rate from %u to %d",
                             decoder->outputFormat.mSampleRate, decoder->codecContext->sample_rate);
                    decoder->outputFormat.mSampleRate = decoder->codecContext->sample_rate;
                }
                
                // Process audio samples
                UInt32 samples = decoder->frame->nb_samples;
                
                // Ensure we have enough space
                UInt32 requiredFrames = decoder->outputBufferUsed + samples;
                if (requiredFrames > decoder->outputBufferFrames) {
                    decoder->outputBufferFrames = requiredFrames * 2;
                    // Always allocate for maximum possible channels to avoid issues
                    decoder->outputBuffer = realloc(decoder->outputBuffer,
                                               decoder->outputBufferFrames * 8 * sizeof(Float32));
                    if (!decoder->outputBuffer) {
                        return kAudioCodecStateError;
                    }
                }
                
                // Use the actual channel count from the output format
                Float32 *outputPtr = decoder->outputBuffer + (decoder->outputBufferUsed * outputChannels);
                
                if (decoder->swrContext) {
                    // Allocate temporary buffer for downmixed audio
                    Float32 *downmixBuffer = (Float32 *)malloc(samples * systemChannels * sizeof(Float32));
                    if (!downmixBuffer) {
                        DebugLog("ProduceOutputData: Failed to allocate downmix buffer");
                        continue;
                    }
                    
                    // Use swresample to downmix to system channels
                    uint8_t *out_buffers[1] = { (uint8_t *)downmixBuffer };
                    
                    int converted_samples = swr_convert(decoder->swrContext,
                                                      out_buffers, samples,
                                                      (const uint8_t **)decoder->frame->data, samples);
                    
                    if (converted_samples < 0) {
                        DebugLog("ProduceOutputData: swr_convert failed");
                        free(downmixBuffer);
                        continue;
                    }
                    
                    // During initialize, we told QuickTime we had 8 channels, because at the time we had no way to
                    // know the real number. Now QuickTime expects us to provide 8 channels, and will play content at
                    // the wrong speed if we do not. If we have less than 8 real channels (because either the content
                    // or the user's output device has less), pad the difference with silent channels.
                    
                    // Copy downmixed audio to output buffer with padding to reach 8 channels
                    for (UInt32 sample = 0; sample < samples; sample++) {
                        // Copy the actual audio channels from the downmixed output
                        for (UInt32 ch = 0; ch < systemChannels; ch++) {
                            outputPtr[sample * outputChannels + ch] = downmixBuffer[sample * systemChannels + ch];
                        }
                        // Pad remaining channels with silence
                        for (UInt32 ch = systemChannels; ch < outputChannels; ch++) {
                            outputPtr[sample * outputChannels + ch] = 0.0f;
                        }
                    }
                    
                    // Debug: Log which channels we're writing to
                    DebugLog("ProduceOutputData: Writing audio to channels 0-%u, padding channels %u-%u with silence",
                             systemChannels - 1, systemChannels, outputChannels - 1);
                    
                    // Debug: Log first few samples to check audio levels
                    if (samples > 0) {
                        float maxLevel = 0.0f;
                        float maxOutputLevel = 0.0f;
                        for (UInt32 i = 0; i < systemChannels && i < 10; i++) {
                            float level = fabsf(downmixBuffer[i]);
                            if (level > maxLevel) maxLevel = level;
                        }
                        
                        // Also check what we actually wrote to output
                        for (UInt32 i = 0; i < 10 && i < samples; i++) {
                            for (UInt32 ch = 0; ch < systemChannels; ch++) {
                                float level = fabsf(outputPtr[i * outputChannels + ch]);
                                if (level > maxOutputLevel) maxOutputLevel = level;
                            }
                        }
                        
                        if (maxLevel > 0.0001f) {
                            DebugLog("ProduceOutputData: Audio present, downmix level: %f, output level: %f", maxLevel, maxOutputLevel);
                        } else {
                            DebugLog("ProduceOutputData: WARNING - Very low or silent audio detected! downmix: %f, output: %f", maxLevel, maxOutputLevel);
                        }
                    }
                    
                    free(downmixBuffer);
                    
                    DebugLog("ProduceOutputData: Downmixed %d samples from %d to %u channels, padded to %u", 
                             converted_samples, channels, systemChannels, outputChannels);
                } else {
                    // No downmixing needed, but still need to handle format conversion and padding
                    // Create temporary buffer for conversion
                    Float32 *tempBuffer = (Float32 *)malloc(samples * channels * sizeof(Float32));
                    if (!tempBuffer) {
                        DebugLog("ProduceOutputData: Failed to allocate temp buffer");
                        continue;
                    }
                    
                    Float32 *tempPtr = tempBuffer;
                    
                    // Handle different sample formats
                    switch (decoder->codecContext->sample_fmt) {
                        case AV_SAMPLE_FMT_FLTP:
                            // Planar float - convert to interleaved
                            for (UInt32 sample = 0; sample < samples; sample++) {
                                for (UInt32 channel = 0; channel < channels; channel++) {
                                    float *channelData = (float *)decoder->frame->data[channel];
                                    *tempPtr++ = channelData[sample];
                                }
                            }
                            break;
                            
                        case AV_SAMPLE_FMT_FLT:
                            // Interleaved float - just copy
                            memcpy(tempBuffer, decoder->frame->data[0], samples * channels * sizeof(float));
                            break;
                            
                        case AV_SAMPLE_FMT_S16P:
                            // Planar 16-bit - convert to interleaved float
                            for (UInt32 sample = 0; sample < samples; sample++) {
                                for (UInt32 channel = 0; channel < channels; channel++) {
                                    int16_t *channelData = (int16_t *)decoder->frame->data[channel];
                                    *tempPtr++ = channelData[sample] / 32768.0f;
                                }
                            }
                            break;
                            
                        case AV_SAMPLE_FMT_S16:
                            // Interleaved 16-bit - convert to float
                            {
                                int16_t *input = (int16_t *)decoder->frame->data[0];
                                for (UInt32 sample = 0; sample < samples; sample++) {
                                    for (UInt32 channel = 0; channel < channels; channel++) {
                                        *tempPtr++ = input[sample * channels + channel] / 32768.0f;
                                    }
                                }
                            }
                            break;
                            
                        case AV_SAMPLE_FMT_S32P:
                            // Planar 32-bit - convert to interleaved float
                            for (UInt32 sample = 0; sample < samples; sample++) {
                                for (UInt32 channel = 0; channel < channels; channel++) {
                                    int32_t *channelData = (int32_t *)decoder->frame->data[channel];
                                    *tempPtr++ = channelData[sample] / 2147483648.0f;
                                }
                            }
                            break;
                            
                        case AV_SAMPLE_FMT_S32:
                            // Interleaved 32-bit - convert to float
                            {
                                int32_t *input = (int32_t *)decoder->frame->data[0];
                                for (UInt32 sample = 0; sample < samples; sample++) {
                                    for (UInt32 channel = 0; channel < channels; channel++) {
                                        *tempPtr++ = input[sample * channels + channel] / 2147483648.0f;
                                    }
                                }
                            }
                            break;
                            
                        default:
                            DebugLog("ProduceOutputData: Unsupported sample format %d", 
                                    decoder->codecContext->sample_fmt);
                            free(tempBuffer);
                            continue;
                    }
                    
                    // Copy to output buffer with padding to 8 channels
                    for (UInt32 sample = 0; sample < samples; sample++) {
                        // Copy actual audio channels from the source
                        for (UInt32 ch = 0; ch < channels && ch < outputChannels; ch++) {
                            outputPtr[sample * outputChannels + ch] = tempBuffer[sample * channels + ch];
                        }
                        // Pad remaining channels with silence
                        for (UInt32 ch = channels; ch < outputChannels; ch++) {
                            outputPtr[sample * outputChannels + ch] = 0.0f;
                        }
                    }
                    
                    free(tempBuffer);
                    DebugLog("ProduceOutputData: Converted %u samples with %u channels, padded to %u", 
                             samples, channels, outputChannels);
                }
                
                // Clean up system layout
                if (systemLayout) {
                    free(systemLayout);
                }
                
                decoder->outputBufferUsed += samples;
                
                // Update packet frame size based on what decoder produces
                // DTS may use different frame sizes (512, 1024, 2048, 4096, etc.)
                if (samples > 0 && samples != decoder->packetFrameSize) {
                    DebugLog("ProduceOutputData: Adjusting packet frame size from %u to %u based on decoder output", 
                            decoder->packetFrameSize, samples);
                    decoder->packetFrameSize = samples;
                }
                }  // End of frame processing loop
            }      // End of if (out_size > 0)
        }          // End of while (data_size > 0)
        
        // Move any unparsed data to the beginning of the buffer
        if (data_size > 0 && data != decoder->inputBuffer) {
            memmove(decoder->inputBuffer, data, data_size);
        }
        decoder->inputBufferUsed = data_size;
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
        
        // Debug: Check what we're actually outputting
        Float32 *outputCheck = (Float32 *)outOutputData;
        float maxOutput = 0.0f;
        for (UInt32 i = 0; i < framesToCopy * decoder->outputFormat.mChannelsPerFrame && i < 80; i++) {
            float level = fabsf(outputCheck[i]);
            if (level > maxOutput) maxOutput = level;
        }
        DebugLog("ProduceOutputData: Max output level in first 80 samples: %f", maxOutput);
        
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
    
    // For compressed formats, report packets based on actual frame size
    if (framesToCopy > 0) {
        // Use the actual frame size from the decoder
        UInt32 frameSize = decoder->packetFrameSize;
        if (frameSize == 0) frameSize = 512; // Default if not set
        UInt32 packetsProduced = (framesToCopy + frameSize - 1) / frameSize; // Round up
        DebugLog("ProduceOutputData: framesToCopy=%u, frameSize=%u, packets=%u", 
                 framesToCopy, frameSize, packetsProduced);
        *ioNumberPackets = packetsProduced;
    } else {
        *ioNumberPackets = 0;
    }
    
    // Determine the appropriate status
    if (framesToCopy > 0) {
        // We produced output successfully
        // Check if we likely have more data to decode
        if (decoder->outputBufferUsed > 0 || decoder->inputBufferUsed >= 4096) {
            *outStatus = kAudioCodecProduceOutputPacketSuccessHasMore;
        } else {
            *outStatus = kAudioCodecProduceOutputPacketSuccess;
        }
    } else if (decoder->inputBufferUsed > 0) {
        // We have input data but couldn't produce output - might need more data
        *outStatus = kAudioCodecProduceOutputPacketNeedsMoreInputData;
    } else {
        // No input data and no output - we need more input
        *outStatus = kAudioCodecProduceOutputPacketNeedsMoreInputData;
    }
    
    return noErr;
}

static OSStatus Reset(void *self) {
    DTSDecoder *decoder = DTS_DECODER;
    
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
static AudioComponentMethod DTSLookupMethod(SInt16 selector) {
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

static OSStatus DTSOpenProc(void *self, AudioComponentInstance inInstance) {
    DebugLog("OpenProc called: self=%p, instance=%p", self, inInstance);
    
    // Initialize the decoder instance in place
    DTSDecoder *decoder = DTS_DECODER;
    memset(decoder, 0, sizeof(DTSDecoder));
    
    // Initialize defaults
    decoder->inputFormat.mFormatID = kDTSFormat;
    decoder->inputFormat.mFormatFlags = 0;
    decoder->inputFormat.mBytesPerPacket = 0;
    decoder->inputFormat.mFramesPerPacket = 0;
    decoder->inputFormat.mBytesPerFrame = 0;
    decoder->inputFormat.mChannelsPerFrame = 8;  // Default to 8 channels (7.1 surround)
    decoder->inputFormat.mBitsPerChannel = 0;
    decoder->inputFormat.mSampleRate = 48000;
    
    decoder->outputFormat.mFormatID = kAudioFormatLinearPCM;
    decoder->outputFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    decoder->outputFormat.mBitsPerChannel = 32;
    decoder->outputFormat.mFramesPerPacket = 1;
    decoder->outputFormat.mChannelsPerFrame = 8;  // Default to 8 channels for 7.1 support
    decoder->outputFormat.mBytesPerFrame = 8 * sizeof(Float32);
    decoder->outputFormat.mBytesPerPacket = 8 * sizeof(Float32);
    decoder->outputFormat.mSampleRate = 48000;
    decoder->packetFrameSize = 512;  // DTS minimum frame size
    decoder->maxPacketSize = 8192;
    decoder->lastSystemChannels = 0;  // Will be set when swresample is initialized
    
    DebugLog("Initialized decoder at %p - input channels=%u, output channels=%u", 
             decoder, decoder->inputFormat.mChannelsPerFrame, decoder->outputFormat.mChannelsPerFrame);
    
    return noErr;
}

static OSStatus DTSCloseProc(void *self) {
    DebugLog("CloseProc called: self=%p", self);
    
    DTSDecoder *decoder = DTS_DECODER;
    if (decoder->isInitialized) {
        Uninitialize(self);
    }
    
    // The AudioComponentPlugInInstance will be freed by the system
    
    return noErr;
}

// Factory function for AudioComponent
__attribute__((visibility("default")))
void *DTSDecoderEntry(void *params, void *storage) {
    AudioComponentDescription *desc = (AudioComponentDescription *)params;
    
    DebugLog("DTSDecoderEntry called: params=%p, storage=%p", params, storage);
    
    if (desc && desc->componentType == kDTSDecoderComponentType && 
        (desc->componentSubType == kDTSFormat || desc->componentSubType == kDTSFormatMP4 || desc->componentSubType == kDTSFormatCore)) {
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
        acpi->mPlugInInterface.Open = DTSOpenProc;
        acpi->mPlugInInterface.Close = DTSCloseProc;
        acpi->mPlugInInterface.Lookup = DTSLookupMethod;
        acpi->mPlugInInterface.reserved = NULL;
        
        // Clear padding and instance storage
        memset(acpi->mPad, 0, sizeof(acpi->mPad));
        memset(&acpi->mInstanceStorage, 0, sizeof(DTSDecoder));
        
        DebugLog("Returning plugin instance: %p (interface at %p)", acpi, &acpi->mPlugInInterface);
        return &acpi->mPlugInInterface;
    }
    
    DebugLog("Invalid component description!");
    return NULL;
}