#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AudioCodec.h>
#include <AudioUnit/AudioComponent.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <opus.h>
#include <opus_multistream.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// Define DEBUG_LOGGING to enable debug output
// #define DEBUG_LOGGING 1

#ifdef DEBUG_LOGGING
// Debug logging
static FILE *gLogFile = NULL;
static void DebugLog(const char *format, ...) {
    if (!gLogFile) {
        gLogFile = fopen("/tmp/opus_component.log", "a");
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

#define kOPUSDecoderComponentType 'adec'
#define kOPUSFormat 'opus'
#define kOPUSFormatMP4 'Opus'
#define kOPUSManufacturer 'Opus'

// OPUS decoder state
typedef struct OPUSDecoder {
    AudioStreamBasicDescription inputFormat;
    AudioStreamBasicDescription outputFormat;
    
    OpusDecoder *decoder;
    OpusMSDecoder *msDecoder;  // For multi-stream/surround content
    
    UInt8 *inputBuffer;
    UInt32 inputBufferSize;
    UInt32 inputBufferUsed;
    
    Float32 *outputBuffer;
    UInt32 outputBufferFrames;
    UInt32 outputBufferUsed;
    
    Float32 *downmixBuffer;     // Buffer for downmixed audio
    UInt32 downmixBufferFrames;
    
    Boolean isInitialized;
    UInt32 packetFrameSize;
    UInt32 maxPacketSize;
    
    // Opus specific
    int channels;
    int preskip;
    float gain;
    
    // Downmixing support
    UInt32 lastSystemChannels;  // To detect speaker configuration changes
    Boolean needsDownmix;       // Whether downmixing is required
} OPUSDecoder;

// AudioComponent plugin instance structure
typedef struct AudioComponentPlugInInstance {
    AudioComponentPlugInInterface mPlugInInterface;
    void *mPad[4];  // Required padding for binary compatibility
    OPUSDecoder mInstanceStorage;
} AudioComponentPlugInInstance;

// Macros to access the instance from self pointer
#define ACPI ((AudioComponentPlugInInstance *)self)
#define OPUS_DECODER (&ACPI->mInstanceStorage)

// Channel layout tags for 1-8 channels
static const AudioChannelLayoutTag kChannelLayoutTags[8] = {
    kAudioChannelLayoutTag_Mono,
    kAudioChannelLayoutTag_Stereo,
    kAudioChannelLayoutTag_MPEG_3_0_A,
    kAudioChannelLayoutTag_MPEG_4_0_A,
    kAudioChannelLayoutTag_MPEG_5_0_A,
    kAudioChannelLayoutTag_MPEG_5_1_A,
    kAudioChannelLayoutTag_MPEG_7_1_A,
    kAudioChannelLayoutTag_Octagonal
};

// Forward declarations
static OSStatus GetPropertyInfo(void *self, AudioCodecPropertyID inPropertyID, UInt32 *outSize, Boolean *outWritable);
static OSStatus GetProperty(void *self, AudioCodecPropertyID inPropertyID, UInt32 *ioPropertyDataSize, void *outPropertyData);
static OSStatus SetProperty(void *self, AudioCodecPropertyID inPropertyID, UInt32 inPropertyDataSize, const void *inPropertyData);
static OSStatus Initialize(void *self, const AudioStreamBasicDescription *inInputFormat, const AudioStreamBasicDescription *inOutputFormat, const void *inMagicCookie, UInt32 inMagicCookieByteSize);
static OSStatus Uninitialize(void *self);
static OSStatus AppendInputData(void *self, const void *inInputData, UInt32 *ioInputDataByteSize, UInt32 *ioNumberPackets, const AudioStreamPacketDescription *inPacketDescription);
static OSStatus ProduceOutputData(void *self, void *outOutputData, UInt32 *ioOutputDataByteSize, UInt32 *ioNumberPackets, AudioStreamPacketDescription *outPacketDescription, UInt32 *outStatus);
static OSStatus Reset(void *self);

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
                DebugLog("GetSystemOutputChannelLayout: Got preferred channel layout");
                
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
    DebugLog("GetSystemOutputChannelLayout: System has %u output channels", channelCount);
    if (outChannelCount) *outChannelCount = channelCount;
    return NULL;
}

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
        case kAudioCodecPropertyRequiresPacketDescription:
        case kAudioCodecPropertyAvailableNumberChannels:
        case kAudioCodecPropertyPrimeMethod:
        case kAudioCodecPropertyDoesSampleRateConversion:
        case kAudioCodecPropertyUsedInputBufferSize:
            if (outSize) *outSize = sizeof(UInt32);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyCurrentInputChannelLayout:
        case kAudioCodecPropertyCurrentOutputChannelLayout:
            if (outSize) *outSize = sizeof(AudioChannelLayout);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyAvailableInputChannelLayouts:
        case kAudioCodecPropertyAvailableOutputChannelLayouts:
            if (outSize) *outSize = 8 * sizeof(AudioChannelLayoutTag);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyNameCFString:
        case kAudioCodecPropertyManufacturerCFString:
        case kAudioCodecPropertyFormatCFString:
            if (outSize) *outSize = sizeof(CFStringRef);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyFormatList:
            if (outSize) *outSize = sizeof(AudioFormatListItem);
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyMagicCookie:
            if (outSize) *outSize = 0; // Opus doesn't use magic cookies in the same way
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyPrimeInfo:
            if (outSize) *outSize = sizeof(AudioCodecPrimeInfo);
            if (outWritable) *outWritable = false;
            return noErr;
            
        default:
            DebugLog("GetPropertyInfo: Unknown property ID: %08x", (unsigned int)inPropertyID);
            return kAudioCodecUnknownPropertyError;
    }
}

static OSStatus GetProperty(void *self,
                           AudioCodecPropertyID inPropertyID,
                           UInt32 *ioPropertyDataSize,
                           void *outPropertyData) {
    OPUSDecoder *decoder = OPUS_DECODER;
    
    DebugLog("GetProperty called: self=%p, propertyID=%08x (%c%c%c%c)", 
             self, (unsigned int)inPropertyID,
             (char)(inPropertyID >> 24), (char)(inPropertyID >> 16),
             (char)(inPropertyID >> 8), (char)inPropertyID);
    
    switch (inPropertyID) {
        case kAudioCodecPropertyCurrentInputFormat:
            if (*ioPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                *(AudioStreamBasicDescription *)outPropertyData = decoder->inputFormat;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyCurrentOutputFormat:
            if (*ioPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                *(AudioStreamBasicDescription *)outPropertyData = decoder->outputFormat;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertySupportedInputFormats:
        case kAudioCodecPropertyInputFormatsForOutputFormat:
            if (*ioPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                AudioStreamBasicDescription *format = (AudioStreamBasicDescription *)outPropertyData;
                memset(format, 0, sizeof(AudioStreamBasicDescription));
                format->mFormatID = kOPUSFormat;
                format->mFormatFlags = 0;
                format->mBytesPerPacket = 0;
                format->mFramesPerPacket = 0;
                format->mBytesPerFrame = 0;
                format->mChannelsPerFrame = 0; // Any
                format->mBitsPerChannel = 0;
                format->mSampleRate = 48000; // Opus always decodes at 48kHz
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertySupportedOutputFormats:
        case kAudioCodecPropertyOutputFormatsForInputFormat:
            if (*ioPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                AudioStreamBasicDescription *format = (AudioStreamBasicDescription *)outPropertyData;
                memset(format, 0, sizeof(AudioStreamBasicDescription));
                format->mFormatID = kAudioFormatLinearPCM;
                format->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
                format->mBitsPerChannel = 32;
                format->mFramesPerPacket = 1;
                format->mChannelsPerFrame = decoder->channels ? decoder->channels : 2;
                format->mBytesPerFrame = format->mChannelsPerFrame * sizeof(Float32);
                format->mBytesPerPacket = format->mBytesPerFrame;
                format->mSampleRate = 48000; // Opus always outputs at 48kHz
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyPacketFrameSize:
            if (*ioPropertyDataSize == sizeof(UInt32)) {
                // Opus uses 20ms frames by default (960 samples at 48kHz)
                *(UInt32 *)outPropertyData = 960;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyMaximumPacketByteSize:
            if (*ioPropertyDataSize == sizeof(UInt32)) {
                *(UInt32 *)outPropertyData = decoder->maxPacketSize;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyHasVariablePacketByteSizes:
            if (*ioPropertyDataSize == sizeof(UInt32)) {
                *(UInt32 *)outPropertyData = 1; // Opus has variable packet sizes
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyIsInitialized:
            if (*ioPropertyDataSize == sizeof(UInt32)) {
                *(UInt32 *)outPropertyData = decoder->isInitialized;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyMinimumNumberOutputPackets:
            if (*ioPropertyDataSize == sizeof(UInt32)) {
                *(UInt32 *)outPropertyData = 1;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyInputBufferSize:
            if (*ioPropertyDataSize == sizeof(UInt32)) {
                *(UInt32 *)outPropertyData = decoder->maxPacketSize;
                return noErr;
            }
            return kAudioCodecBadPropertySizeError;
            
        case kAudioCodecPropertyCurrentInputChannelLayout:
        case kAudioCodecPropertyCurrentOutputChannelLayout:
            if (*ioPropertyDataSize == sizeof(AudioChannelLayout)) {
                AudioChannelLayout *layout = (AudioChannelLayout *)outPropertyData;
                memset(layout, 0, sizeof(AudioChannelLayout));
                
                UInt32 channels = decoder->outputFormat.mChannelsPerFrame;
                if (channels >= 1 && channels <= 8) {
                    layout->mChannelLayoutTag = kChannelLayoutTags[channels - 1];
                } else {
                    layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
                }
                
                layout->mChannelBitmap = 0;
                layout->mNumberChannelDescriptions = 0;
            }
            *ioPropertyDataSize = sizeof(AudioChannelLayout);
            return noErr;
            
        case kAudioCodecPropertyPrimeInfo:
            if (*ioPropertyDataSize == sizeof(AudioCodecPrimeInfo)) {
                AudioCodecPrimeInfo *prime = (AudioCodecPrimeInfo *)outPropertyData;
                prime->leadingFrames = decoder->preskip;
                prime->trailingFrames = 0;
            } else {
                return kAudioCodecBadPropertySizeError;
            }
            return noErr;
            
        case kAudioCodecPropertyNameCFString:
            if (*ioPropertyDataSize != sizeof(CFStringRef)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(CFStringRef *)outPropertyData = CFSTR("OPUS Audio Decoder");
            CFRetain(*(CFStringRef *)outPropertyData);
            return noErr;
            
        case kAudioCodecPropertyManufacturerCFString:
            if (*ioPropertyDataSize != sizeof(CFStringRef)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(CFStringRef *)outPropertyData = CFSTR("Xiph.Org Foundation");
            CFRetain(*(CFStringRef *)outPropertyData);
            return noErr;
            
        case kAudioCodecPropertyFormatList:
            {
                if (*ioPropertyDataSize < sizeof(AudioFormatListItem)) {
                    return kAudioCodecBadPropertySizeError;
                }
                AudioFormatListItem *formatList = (AudioFormatListItem *)outPropertyData;
                
                formatList->mASBD.mFormatID = kOPUSFormat;
                formatList->mASBD.mFormatFlags = 0;
                formatList->mASBD.mBytesPerPacket = 0; // Variable
                formatList->mASBD.mFramesPerPacket = 0; // Variable
                formatList->mASBD.mBytesPerFrame = 0;
                formatList->mASBD.mChannelsPerFrame = 0; // Any
                formatList->mASBD.mBitsPerChannel = 0;
                formatList->mASBD.mSampleRate = 48000;
                formatList->mChannelLayoutTag = 0; // No specific layout
                
                *ioPropertyDataSize = sizeof(AudioFormatListItem);
            }
            return noErr;
            
        case kAudioCodecPropertyFormatCFString:
            if (*ioPropertyDataSize != sizeof(CFStringRef)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(CFStringRef *)outPropertyData = CFSTR("Opus Interactive Audio Codec");
            CFRetain(*(CFStringRef *)outPropertyData);
            return noErr;
            
        case kAudioCodecPropertyMagicCookie:
            // Opus doesn't use magic cookies
            *ioPropertyDataSize = 0;
            return noErr;
            
        case kAudioCodecPropertyRequiresPacketDescription:
            if (*ioPropertyDataSize != sizeof(UInt32)) {
                return kAudioCodecBadPropertySizeError;
            }
            *(UInt32 *)outPropertyData = 1; // Opus has variable packet sizes
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
            *(UInt32 *)outPropertyData = kAudioCodecPrimeMethod_Pre;
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
    OPUSDecoder *decoder = OPUS_DECODER;
    
    // No property can be set when the codec is initialized
    if (decoder->isInitialized) {
        return kAudioCodecIllegalOperationError;
    }
    
    switch (inPropertyID) {
        case kAudioCodecPropertyCurrentInputFormat:
            if (inPropertyDataSize == sizeof(AudioStreamBasicDescription)) {
                const AudioStreamBasicDescription *format = (const AudioStreamBasicDescription *)inPropertyData;
                
                // Validate OPUS format
                if (format->mFormatID != kOPUSFormat && format->mFormatID != kOPUSFormatMP4) {
                    DebugLog("SetProperty: Invalid format ID: %c%c%c%c",
                             (char)(format->mFormatID >> 24), (char)(format->mFormatID >> 16),
                             (char)(format->mFormatID >> 8), (char)(format->mFormatID));
                    return kAudioCodecUnsupportedFormatError;
                }
                
                // OPUS supports 1-8 channels
                if (format->mChannelsPerFrame > 0 && (format->mChannelsPerFrame < 1 || format->mChannelsPerFrame > 8)) {
                    DebugLog("SetProperty: Invalid channel count: %u", format->mChannelsPerFrame);
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
                
                if (!(format->mFormatFlags & kAudioFormatFlagIsFloat)) {
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
    OPUSDecoder *decoder = OPUS_DECODER;
    
    DebugLog("Initialize called: self=%p, inputFormat=%p, outputFormat=%p, magicCookieSize=%u", 
             self, inInputFormat, inOutputFormat, inMagicCookieByteSize);
    
    if (decoder->isInitialized) {
        return kAudioCodecStateError;
    }
    
    // Update formats if provided
    if (inInputFormat) {
        decoder->inputFormat = *inInputFormat;
        DebugLog("Initialize: Input format - channels=%u, sampleRate=%f", 
                 inInputFormat->mChannelsPerFrame, inInputFormat->mSampleRate);
    }
    if (inOutputFormat) {
        decoder->outputFormat = *inOutputFormat;
    }
    
    // Parse magic cookie if present (MP4 OPUS may have configuration)
    if (inMagicCookie && inMagicCookieByteSize >= 8) {
        const UInt8 *cookie = (const UInt8 *)inMagicCookie;
        DebugLog("Initialize: Magic cookie present, size=%u", inMagicCookieByteSize);
        // Log first few bytes
        if (inMagicCookieByteSize >= 8) {
            DebugLog("Initialize: Cookie bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                     cookie[0], cookie[1], cookie[2], cookie[3],
                     cookie[4], cookie[5], cookie[6], cookie[7]);
        }
    }
    
    // Determine channel count from input
    decoder->channels = decoder->inputFormat.mChannelsPerFrame;
    if (decoder->channels == 0) {
        decoder->channels = 2; // Default to stereo
    }
    
    DebugLog("Initialize: Creating Opus decoder for %d channels", decoder->channels);
    
    // Create Opus decoder
    int error;
    decoder->decoder = opus_decoder_create(48000, decoder->channels, &error);
    if (error != OPUS_OK || !decoder->decoder) {
        DebugLog("Initialize: Failed to create Opus decoder: %d", error);
        return kAudioCodecUnspecifiedError;
    }
    
    // Get system output channel configuration
    UInt32 systemChannels = 0;
    AudioChannelLayout *systemLayout = GetSystemOutputChannelLayout(&systemChannels);
    if (systemLayout) {
        free(systemLayout);
    }
    
    // Determine if we need downmixing
    decoder->needsDownmix = (decoder->channels > systemChannels) && (systemChannels > 0);
    if (decoder->needsDownmix) {
        DebugLog("Initialize: Will downmix from %d to %u channels", decoder->channels, systemChannels);
    }
    
    // Initialize buffers
    decoder->inputBufferSize = 65536;
    decoder->inputBuffer = (UInt8 *)calloc(decoder->inputBufferSize, 1);
    decoder->inputBufferUsed = 0;
    
    // Allocate output buffer for the actual output channel count
    UInt32 outputChannels = decoder->needsDownmix ? systemChannels : decoder->channels;
    decoder->outputBufferFrames = 48000; // 1 second at 48kHz
    decoder->outputBuffer = (Float32 *)calloc(decoder->outputBufferFrames * outputChannels, sizeof(Float32));
    decoder->outputBufferUsed = 0;
    
    // Allocate downmix buffer if needed (for intermediate decoded audio)
    if (decoder->needsDownmix) {
        decoder->downmixBufferFrames = 5760; // Maximum Opus frame size (120ms at 48kHz)
        decoder->downmixBuffer = (Float32 *)calloc(decoder->downmixBufferFrames * decoder->channels, sizeof(Float32));
    } else {
        decoder->downmixBuffer = NULL;
        decoder->downmixBufferFrames = 0;
    }
    
    decoder->maxPacketSize = 4000;
    decoder->packetFrameSize = 960; // 20ms at 48kHz
    decoder->preskip = 0;
    decoder->gain = 1.0f;
    decoder->lastSystemChannels = systemChannels;
    
    // Update output format based on actual output channels
    decoder->outputFormat.mSampleRate = 48000;
    decoder->outputFormat.mChannelsPerFrame = outputChannels;
    decoder->outputFormat.mBytesPerFrame = outputChannels * sizeof(Float32);
    decoder->outputFormat.mBytesPerPacket = decoder->outputFormat.mBytesPerFrame;
    decoder->outputFormat.mFramesPerPacket = 1;
    
    DebugLog("Initialize: Input channels=%d, Output channels=%u, Downmix=%s", 
             decoder->channels, outputChannels, decoder->needsDownmix ? "YES" : "NO");
    
    decoder->isInitialized = true;
    
    DebugLog("Initialize complete: channels=%d", decoder->channels);
    
    return noErr;
}

static OSStatus Uninitialize(void *self) {
    OPUSDecoder *decoder = OPUS_DECODER;
    
    DebugLog("Uninitialize called: self=%p", self);
    
    if (!decoder->isInitialized) {
        return noErr;
    }
    
    if (decoder->decoder) {
        opus_decoder_destroy(decoder->decoder);
        decoder->decoder = NULL;
    }
    
    if (decoder->msDecoder) {
        opus_multistream_decoder_destroy(decoder->msDecoder);
        decoder->msDecoder = NULL;
    }
    
    if (decoder->inputBuffer) {
        free(decoder->inputBuffer);
        decoder->inputBuffer = NULL;
    }
    
    if (decoder->outputBuffer) {
        free(decoder->outputBuffer);
        decoder->outputBuffer = NULL;
    }
    
    if (decoder->downmixBuffer) {
        free(decoder->downmixBuffer);
        decoder->downmixBuffer = NULL;
    }
    
    decoder->isInitialized = false;
    
    return noErr;
}

static OSStatus AppendInputData(void *self,
                               const void *inInputData,
                               UInt32 *ioInputDataByteSize,
                               UInt32 *ioNumberPackets,
                               const AudioStreamPacketDescription *inPacketDescription) {
    OPUSDecoder *decoder = OPUS_DECODER;
    
    DebugLog("AppendInputData called: inputBytes=%u, numberPackets=%u", 
             *ioInputDataByteSize, *ioNumberPackets);
    
    if (!decoder->isInitialized) {
        return kAudioCodecStateError;
    }
    
    // We can only handle one packet at a time with Opus
    if (*ioNumberPackets == 0) {
        return noErr;
    }
    
    // Clear any previous data since we process one packet at a time
    decoder->inputBufferUsed = 0;
    
    // Get the first packet only
    UInt32 bytesToAppend;
    if (inPacketDescription) {
        bytesToAppend = inPacketDescription[0].mDataByteSize;
        if (bytesToAppend > *ioInputDataByteSize) {
            bytesToAppend = *ioInputDataByteSize;
        }
    } else {
        // If no packet description, assume it's all one packet
        bytesToAppend = *ioInputDataByteSize;
    }
    
    if (bytesToAppend > decoder->inputBufferSize) {
        // Grow buffer if needed
        decoder->inputBufferSize = bytesToAppend * 2;
        decoder->inputBuffer = realloc(decoder->inputBuffer, decoder->inputBufferSize);
    }
    
    if (bytesToAppend > 0) {
        const UInt8 *inputData = (const UInt8 *)inInputData;
        if (inPacketDescription) {
            inputData += inPacketDescription[0].mStartOffset;
        }
        memcpy(decoder->inputBuffer, inputData, bytesToAppend);
        decoder->inputBufferUsed = bytesToAppend;
    }
    
    // We only consumed one packet
    *ioInputDataByteSize = bytesToAppend;
    *ioNumberPackets = 1;
    
    // Log packet details if available
    if (inPacketDescription && *ioNumberPackets > 0) {
        DebugLog("AppendInputData: packet[0] offset=%lld, size=%u, frames=%u", 
                 inPacketDescription[0].mStartOffset,
                 (unsigned int)inPacketDescription[0].mDataByteSize,
                 (unsigned int)inPacketDescription[0].mVariableFramesInPacket);
    }
    
    DebugLog("AppendInputData: appended %u bytes (one packet), buffer now has %u bytes", 
             bytesToAppend, decoder->inputBufferUsed);
    
    return noErr;
}

static OSStatus ProduceOutputData(void *self,
                                 void *outOutputData,
                                 UInt32 *ioOutputDataByteSize,
                                 UInt32 *ioNumberPackets,
                                 AudioStreamPacketDescription *outPacketDescription,
                                 UInt32 *outStatus) {
    OPUSDecoder *decoder = OPUS_DECODER;
    
    if (!decoder->isInitialized) {
        return kAudioCodecStateError;
    }
    
    UInt32 requestedFrames = *ioNumberPackets;
    if (requestedFrames == 0) {
        requestedFrames = *ioOutputDataByteSize / decoder->outputFormat.mBytesPerFrame;
    }
    
    DebugLog("ProduceOutputData: requestedFrames=%u, bufferedFrames=%u, inputBytes=%u", 
             requestedFrames, decoder->outputBufferUsed, decoder->inputBufferUsed);
    
    // Decode Opus packets
    // Note: In MP4, OPUS packets are delivered one at a time with proper boundaries
    if (decoder->inputBufferUsed > 0) {
        // Ensure we have space for output
        UInt32 maxFrames = 5760; // Maximum Opus frame size (120ms at 48kHz)
        UInt32 outputChannels = decoder->outputFormat.mChannelsPerFrame;
        
        if (decoder->outputBufferUsed + maxFrames > decoder->outputBufferFrames) {
            decoder->outputBufferFrames = (decoder->outputBufferUsed + maxFrames) * 2;
            decoder->outputBuffer = realloc(decoder->outputBuffer,
                                          decoder->outputBufferFrames * outputChannels * sizeof(Float32));
        }
        
        // Decode to temporary buffer if downmixing, otherwise directly to output
        Float32 *decodeTarget;
        if (decoder->needsDownmix) {
            decodeTarget = decoder->downmixBuffer;
        } else {
            decodeTarget = decoder->outputBuffer + (decoder->outputBufferUsed * outputChannels);
        }
        
        int frameSize = opus_decode_float(decoder->decoder, 
                                         decoder->inputBuffer, 
                                         decoder->inputBufferUsed,
                                         decodeTarget,
                                         maxFrames,
                                         0);
        
        if (frameSize < 0) {
            DebugLog("ProduceOutputData: opus_decode_float failed: %d (%s)", 
                     frameSize, opus_strerror(frameSize));
            // Clear the input buffer on error
            decoder->inputBufferUsed = 0;
        } else {
            // Check if speaker configuration changed
            UInt32 systemChannels = 0;
            AudioChannelLayout *systemLayout = GetSystemOutputChannelLayout(&systemChannels);
            if (systemLayout) {
                free(systemLayout);
            }
            
            if (systemChannels != decoder->lastSystemChannels && systemChannels > 0) {
                DebugLog("ProduceOutputData: Speaker configuration changed from %u to %u channels", 
                         decoder->lastSystemChannels, systemChannels);
                decoder->lastSystemChannels = systemChannels;
                decoder->needsDownmix = (decoder->channels > systemChannels);
                
                // Reallocate output buffer if needed
                if (systemChannels != outputChannels) {
                    outputChannels = systemChannels;
                    decoder->outputFormat.mChannelsPerFrame = outputChannels;
                    decoder->outputFormat.mBytesPerFrame = outputChannels * sizeof(Float32);
                    decoder->outputFormat.mBytesPerPacket = decoder->outputFormat.mBytesPerFrame;
                    
                    // Reallocate output buffer for new channel count
                    free(decoder->outputBuffer);
                    decoder->outputBuffer = (Float32 *)calloc(decoder->outputBufferFrames * outputChannels, sizeof(Float32));
                    decoder->outputBufferUsed = 0;
                }
            }
            
            // If we need to downmix, do it now
            if (decoder->needsDownmix) {
                // Simple downmixing algorithm
                Float32 *outputPtr = decoder->outputBuffer + (decoder->outputBufferUsed * outputChannels);
                
                for (int i = 0; i < frameSize; i++) {
                    Float32 *inputFrame = decodeTarget + (i * decoder->channels);
                    Float32 *outputFrame = outputPtr + (i * outputChannels);
                    
                    if (decoder->channels == 6 && outputChannels == 2) {
                        // 5.1 to stereo downmix
                        // L' = L + 0.707*C + 0.707*Ls + 0.707*LFE
                        // R' = R + 0.707*C + 0.707*Rs + 0.707*LFE
                        Float32 L = inputFrame[0];
                        Float32 R = inputFrame[1];
                        Float32 C = inputFrame[2];
                        Float32 LFE = inputFrame[3];
                        Float32 Ls = inputFrame[4];
                        Float32 Rs = inputFrame[5];
                        
                        outputFrame[0] = L + 0.707f * C + 0.707f * Ls + 0.707f * LFE;
                        outputFrame[1] = R + 0.707f * C + 0.707f * Rs + 0.707f * LFE;
                    } else if (decoder->channels == 8 && outputChannels == 2) {
                        // 7.1 to stereo downmix
                        // Opus 7.1 channel order: L R C LFE BL BR SL SR
                        Float32 L = inputFrame[0];
                        Float32 R = inputFrame[1];
                        Float32 C = inputFrame[2];
                        Float32 LFE = inputFrame[3];
                        Float32 BL = inputFrame[4];  // Back Left
                        Float32 BR = inputFrame[5];  // Back Right
                        Float32 SL = inputFrame[6];  // Side Left
                        Float32 SR = inputFrame[7];  // Side Right
                        
                        outputFrame[0] = L + 0.707f * C + 0.5f * SL + 0.5f * BL + 0.707f * LFE;
                        outputFrame[1] = R + 0.707f * C + 0.5f * SR + 0.5f * BR + 0.707f * LFE;
                    } else if (decoder->channels == 8 && outputChannels == 6) {
                        // 7.1 to 5.1 downmix
                        Float32 L = inputFrame[0];
                        Float32 R = inputFrame[1];
                        Float32 C = inputFrame[2];
                        Float32 LFE = inputFrame[3];
                        Float32 BL = inputFrame[4];
                        Float32 BR = inputFrame[5];
                        Float32 SL = inputFrame[6];
                        Float32 SR = inputFrame[7];
                        
                        outputFrame[0] = L + 0.707f * SL;  // L + Side Left
                        outputFrame[1] = R + 0.707f * SR;  // R + Side Right
                        outputFrame[2] = C;
                        outputFrame[3] = LFE;
                        outputFrame[4] = BL;  // Back Left becomes Left Surround
                        outputFrame[5] = BR;  // Back Right becomes Right Surround
                    } else if (decoder->channels > 2 && outputChannels == 2) {
                        // Generic multichannel to stereo
                        // Mix center channel equally to L/R, mix surrounds to their respective sides
                        Float32 L = inputFrame[0];
                        Float32 R = inputFrame[1];
                        
                        if (decoder->channels >= 3) {
                            // Add center channel
                            Float32 C = inputFrame[2];
                            L += 0.707f * C;
                            R += 0.707f * C;
                        }
                        
                        if (decoder->channels >= 4) {
                            // Check if we have LFE (4th channel in 5.1/7.1)
                            if (decoder->channels >= 6) {
                                // Has LFE, add it
                                L += 0.707f * inputFrame[3];
                                R += 0.707f * inputFrame[3];
                            }
                        }
                        
                        if (decoder->channels >= 5) {
                            // Add surround channels
                            L += 0.707f * inputFrame[4]; // Left surround
                            R += 0.707f * inputFrame[decoder->channels > 5 ? 5 : 4]; // Right surround
                        }
                        
                        if (decoder->channels >= 7) {
                            // Add side channels if present (7.1)
                            L += 0.5f * inputFrame[6]; // Side left
                            R += 0.5f * inputFrame[7]; // Side right
                        }
                        
                        outputFrame[0] = L;
                        outputFrame[1] = R;
                    } else if (decoder->channels == 1 && outputChannels == 2) {
                        // Mono to stereo
                        outputFrame[0] = inputFrame[0];
                        outputFrame[1] = inputFrame[0];
                    } else {
                        // For other configurations, just copy what we can
                        UInt32 channelsToCopy = decoder->channels < outputChannels ? decoder->channels : outputChannels;
                        for (UInt32 ch = 0; ch < channelsToCopy; ch++) {
                            outputFrame[ch] = inputFrame[ch];
                        }
                        // Zero out any remaining channels
                        for (UInt32 ch = channelsToCopy; ch < outputChannels; ch++) {
                            outputFrame[ch] = 0.0f;
                        }
                    }
                }
            }
            
            decoder->outputBufferUsed += frameSize;
            decoder->inputBufferUsed = 0; // Consumed all input
            DebugLog("ProduceOutputData: decoded %d frames, downmixed=%s", frameSize, decoder->needsDownmix ? "YES" : "NO");
        }
    }
    
    // Copy output
    UInt32 framesToCopy = decoder->outputBufferUsed;
    UInt32 maxFrames = *ioOutputDataByteSize / decoder->outputFormat.mBytesPerFrame;
    if (framesToCopy > maxFrames) {
        framesToCopy = maxFrames;
    }
    
    if (framesToCopy > 0) {
        UInt32 bytesToCopy = framesToCopy * decoder->outputFormat.mBytesPerFrame;
        memcpy(outOutputData, decoder->outputBuffer, bytesToCopy);
        *ioOutputDataByteSize = bytesToCopy;
        
        // Move remaining frames to the beginning
        UInt32 remainingFrames = decoder->outputBufferUsed - framesToCopy;
        if (remainingFrames > 0) {
            UInt32 outputChannels = decoder->outputFormat.mChannelsPerFrame;
            memmove(decoder->outputBuffer, 
                   decoder->outputBuffer + (framesToCopy * outputChannels),
                   remainingFrames * outputChannels * sizeof(Float32));
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
    
    *ioNumberPackets = framesToCopy;
    
    // Determine status
    if (framesToCopy > 0) {
        if (decoder->outputBufferUsed > 0 || decoder->inputBufferUsed > 0) {
            *outStatus = kAudioCodecProduceOutputPacketSuccessHasMore;
        } else {
            *outStatus = kAudioCodecProduceOutputPacketSuccess;
        }
    } else {
        *outStatus = kAudioCodecProduceOutputPacketNeedsMoreInputData;
    }
    
    return noErr;
}

static OSStatus Reset(void *self) {
    OPUSDecoder *decoder = OPUS_DECODER;
    
    DebugLog("Reset called: self=%p", self);
    
    if (!decoder->isInitialized) {
        return noErr;
    }
    
    // Clear buffers
    decoder->inputBufferUsed = 0;
    decoder->outputBufferUsed = 0;
    
    // Reset Opus decoder
    if (decoder->decoder) {
        opus_decoder_ctl(decoder->decoder, OPUS_RESET_STATE);
    }
    
    return noErr;
}

// Modern AudioComponent support
static AudioComponentMethod OPUSLookupMethod(SInt16 selector) {
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

static OSStatus OPUSOpenProc(void *self, AudioComponentInstance inInstance) {
    DebugLog("OpenProc called: self=%p, instance=%p", self, inInstance);
    
    // Initialize the decoder instance in place
    OPUSDecoder *decoder = OPUS_DECODER;
    memset(decoder, 0, sizeof(OPUSDecoder));
    
    // Initialize defaults
    decoder->inputFormat.mFormatID = kOPUSFormat;
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
    decoder->packetFrameSize = 960;
    decoder->maxPacketSize = 4000;
    
    DebugLog("Initialized decoder at %p", decoder);
    
    return noErr;
}

static OSStatus OPUSCloseProc(void *self) {
    DebugLog("CloseProc called: self=%p", self);
    
    OPUSDecoder *decoder = OPUS_DECODER;
    if (decoder->isInitialized) {
        Uninitialize(self);
    }
    
    return noErr;
}

// Factory function for AudioComponent
__attribute__((visibility("default")))
void *OPUSDecoderEntry(void *params, void *storage) {
    AudioComponentDescription *desc = (AudioComponentDescription *)params;
    
    DebugLog("OPUSDecoderEntry called: params=%p, storage=%p", params, storage);
    
    if (desc && desc->componentType == kOPUSDecoderComponentType && 
        (desc->componentSubType == kOPUSFormat || desc->componentSubType == kOPUSFormatMP4)) {
        DebugLog("Called as factory function! type=%c%c%c%c, subtype=%c%c%c%c",
                 (char)(desc->componentType >> 24), (char)(desc->componentType >> 16),
                 (char)(desc->componentType >> 8), (char)(desc->componentType),
                 (char)(desc->componentSubType >> 24), (char)(desc->componentSubType >> 16),
                 (char)(desc->componentSubType >> 8), (char)(desc->componentSubType));
        
        // Allocate the plugin instance structure
        size_t instanceSize = sizeof(AudioComponentPlugInInstance);
        AudioComponentPlugInInstance *acpi = (AudioComponentPlugInInstance *)malloc(instanceSize);
        if (!acpi) {
            DebugLog("Failed to allocate plugin instance");
            return NULL;
        }
        
        // Initialize the plugin interface
        acpi->mPlugInInterface.Open = OPUSOpenProc;
        acpi->mPlugInInterface.Close = OPUSCloseProc;
        acpi->mPlugInInterface.Lookup = OPUSLookupMethod;
        acpi->mPlugInInterface.reserved = NULL;
        
        // Clear padding and instance storage
        memset(acpi->mPad, 0, sizeof(acpi->mPad));
        memset(&acpi->mInstanceStorage, 0, sizeof(OPUSDecoder));
        
        DebugLog("Returning plugin instance: %p (interface at %p)", acpi, &acpi->mPlugInInterface);
        return &acpi->mPlugInInterface;
    }
    
    DebugLog("Invalid component description!");
    return NULL;
}