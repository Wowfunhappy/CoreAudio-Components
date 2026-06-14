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

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// Define DEBUG_LOGGING to enable debug output
//#define DEBUG_LOGGING 1

#define MAX_CHANNEL_COUNT 8

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
    UInt32 cookieChannels;  // True channel count parsed from the dec3 magic cookie (0 = unknown).
                            // Persists across Initialize/Uninitialize/Reset cycles for one instance.
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
    kAudioChannelLayoutTag_Mono,         // 1 channel: C
    kAudioChannelLayoutTag_Stereo,       // 2 channels: L R
    kAudioChannelLayoutTag_MPEG_3_0_B,   // 3 channels: C L R
    kAudioChannelLayoutTag_Quadraphonic, // 4 channels: L R Ls Rs
    kAudioChannelLayoutTag_MPEG_5_0_D,   // 5 channels: C L R Ls Rs
    kAudioChannelLayoutTag_MPEG_5_1_D,   // 6 channels: C L R Ls Rs LFE
    kAudioChannelLayoutTag_AAC_6_1,      // 7 channels: C L R Ls Rs Cs LFE
    kAudioChannelLayoutTag_MPEG_7_1_B    // 8 channels: C Lc Rc L R Ls Rs LFE
    // Note: We actually support up to MAX_CHANNEL_COUNT channels
};

// --- E-AC-3 channel count from the 'dec3' magic cookie ----------------------------
// QuickTime hands us the elementary-stream config as the magic cookie. For E-AC-3 in
// MP4/MOV this carries a 'dec3' box (EC3SpecificBox, ETSI TS 102 366 Annex F) that
// describes each independent substream. Parsing acmod + lfeon (+ chan_loc for any
// dependent substreams) tells us the true channel count up front, which we otherwise
// could not know until after decoding a frame.

// Full-bandwidth channels implied by each acmod value (before adding LFE).
static const int kAcmodChannels[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };

// Big-endian bit reader over a byte buffer. Reads past the end return zero bits.
typedef struct {
    const UInt8 *data;
    UInt32 size;     // bytes available
    UInt32 bitPos;   // current bit offset
} EAC3BitReader;

static UInt32 EAC3BitsRead(EAC3BitReader *br, UInt32 nbits) {
    UInt32 value = 0;
    for (UInt32 i = 0; i < nbits; i++) {
        UInt32 byteIndex = br->bitPos >> 3;
        UInt32 bit = 0;
        if (byteIndex < br->size) {
            bit = (br->data[byteIndex] >> (7 - (br->bitPos & 7))) & 1;
        }
        value = (value << 1) | bit;
        br->bitPos++;
    }
    return value;
}

// Channels contributed by each chan_loc bit of a dependent substream
// (pairs contribute 2, single speakers contribute 1).
static int EAC3ChanLocChannels(UInt32 chanLoc) {
    static const int perBit[9] = {
        2, // bit 0: Lc/Rc
        2, // bit 1: Lrs/Rrs
        1, // bit 2: Cs
        1, // bit 3: Ts
        2, // bit 4: Lsd/Rsd
        2, // bit 5: Lw/Rw
        2, // bit 6: Lvh/Rvh
        1, // bit 7: Cvh
        1  // bit 8: LFE2
    };
    int extra = 0;
    for (int b = 0; b < 9; b++) {
        if (chanLoc & (1u << b)) extra += perBit[b];
    }
    return extra;
}

// Parse a raw EC3SpecificBox payload (bytes following the 'dec3' fourcc).
// Returns the channel count of the main program, or 0 if it can't be determined.
static UInt32 EAC3ParseDec3Payload(const UInt8 *payload, UInt32 len) {
    if (!payload || len < 2) return 0;

    EAC3BitReader br = { payload, len, 0 };
    EAC3BitsRead(&br, 13);                          // data_rate
    UInt32 numIndSub = EAC3BitsRead(&br, 3) + 1;    // num_ind_sub (stored as count - 1)

    UInt32 mainProgramChannels = 0;
    for (UInt32 s = 0; s < numIndSub; s++) {
        EAC3BitsRead(&br, 2);                       // fscod
        EAC3BitsRead(&br, 5);                       // bsid
        EAC3BitsRead(&br, 1);                       // reserved
        EAC3BitsRead(&br, 1);                       // asvc
        EAC3BitsRead(&br, 3);                       // bsmod
        UInt32 acmod = EAC3BitsRead(&br, 3);
        UInt32 lfeon = EAC3BitsRead(&br, 1);
        EAC3BitsRead(&br, 3);                       // reserved
        UInt32 numDepSub = EAC3BitsRead(&br, 4);

        UInt32 subChannels = (UInt32)kAcmodChannels[acmod & 7] + (lfeon ? 1u : 0u);
        if (numDepSub > 0) {
            UInt32 chanLoc = EAC3BitsRead(&br, 9);
            subChannels += (UInt32)EAC3ChanLocChannels(chanLoc);
        } else {
            EAC3BitsRead(&br, 1);                   // reserved
        }

        // The decoder renders the first independent substream (the main program);
        // any further independent substreams are alternate programs.
        if (s == 0) mainProgramChannels = subChannels;
    }
    return mainProgramChannels;
}

// Scan a magic cookie for a 'dec3' box and return its channel count, or 0 if none.
// The cookie may wrap the box (e.g. an 'frma'/'ec-3' box followed by 'dec3'), so we
// search for the fourcc rather than assuming a fixed offset.
static UInt32 EAC3ChannelCountFromCookie(const void *cookie, UInt32 cookieSize) {
    if (!cookie || cookieSize < 5) return 0;
    const UInt8 *bytes = (const UInt8 *)cookie;
    for (UInt32 i = 0; i + 4 <= cookieSize; i++) {
        if (bytes[i] == 'd' && bytes[i+1] == 'e' && bytes[i+2] == 'c' && bytes[i+3] == '3') {
            return EAC3ParseDec3Payload(bytes + i + 4, cookieSize - (i + 4));
        }
    }
    return 0;
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
            if (outSize) *outSize = sizeof(UInt32) * MAX_CHANNEL_COUNT; // Support 1-MAX_CHANNEL_COUNT channels
            if (outWritable) *outWritable = false;
            return noErr;
            
        case kAudioCodecPropertyAvailableInputChannelLayouts:
        case kAudioCodecPropertyAvailableOutputChannelLayouts:
            if (outSize) *outSize = MAX_CHANNEL_COUNT * sizeof(AudioChannelLayoutTag);
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
                DebugLog("GetProperty: CurrentOutputFormat - channels=%u, bytesPerFrame=%u, sampleRate=%.0f, formatID=%c%c%c%c",
                         decoder->outputFormat.mChannelsPerFrame,
                         decoder->outputFormat.mBytesPerFrame,
                         decoder->outputFormat.mSampleRate,
                         (char)(decoder->outputFormat.mFormatID >> 24), (char)(decoder->outputFormat.mFormatID >> 16),
                         (char)(decoder->outputFormat.mFormatID >> 8), (char)decoder->outputFormat.mFormatID);
                DebugLog("GetProperty: CurrentOutputFormat - inputChannels=%u, isInitialized=%d",
                         decoder->inputFormat.mChannelsPerFrame, decoder->isInitialized);
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
                    // We have predefined layouts for 1-8 channels
                    layout->mChannelLayoutTag = kChannelLayoutTags[channels - 1];
                } else if (channels > 8 && channels <= MAX_CHANNEL_COUNT) {
                    // For more than 8 channels, use generic descriptions
                    layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
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
            if (*ioPropertyDataSize < sizeof(UInt32) * 8) {
                return kAudioCodecBadPropertySizeError;
            }
            // Report support for 1-MAX_CHANNEL_COUNT channels
            for (UInt32 i = 0; i < MAX_CHANNEL_COUNT; i++) {
                ((UInt32 *)outPropertyData)[i] = i + 1;
            }
            *ioPropertyDataSize = sizeof(UInt32) * MAX_CHANNEL_COUNT;
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
            if (*ioPropertyDataSize >= MAX_CHANNEL_COUNT * sizeof(AudioChannelLayoutTag)) {
                AudioChannelLayoutTag *tags = (AudioChannelLayoutTag *)outPropertyData;
                
                if (decoder->isInitialized) {
                    // When initialized, only report the current channel layout
                    UInt32 channels = decoder->outputFormat.mChannelsPerFrame;
                    if (channels >= 1 && channels <= MAX_CHANNEL_COUNT) {
                        tags[0] = kChannelLayoutTags[channels - 1];
                        *ioPropertyDataSize = sizeof(AudioChannelLayoutTag);
                    } else {
                        return kAudioCodecUnknownPropertyError;
                    }
                } else {
                    // When not initialized, report all supported layouts
                    // Note: We only have defined layouts for up to 8 channels
                    UInt32 layoutCount = (MAX_CHANNEL_COUNT > 8) ? 8 : MAX_CHANNEL_COUNT;
                    for (UInt32 i = 0; i < layoutCount; i++) {
                        tags[i] = kChannelLayoutTags[i];
                    }
                    *ioPropertyDataSize = MAX_CHANNEL_COUNT * sizeof(AudioChannelLayoutTag);
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
                
                // EAC3 supports 1-MAX_CHANNEL_COUNT channels
                if (format->mChannelsPerFrame > 0 && (format->mChannelsPerFrame < 1 || format->mChannelsPerFrame > MAX_CHANNEL_COUNT)) {
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

        case kAudioCodecPropertyMagicCookie:
            // We don't need the cookie to decode, but it carries the dec3 box with the
            // true channel count. Capture it here in case the host sets it before
            // Initialize (so the very first Initialize already knows the real count).
            {
                UInt32 cookieChannels = EAC3ChannelCountFromCookie(inPropertyData, inPropertyDataSize);
                if (cookieChannels > 0 && cookieChannels <= MAX_CHANNEL_COUNT) {
                    decoder->cookieChannels = cookieChannels;
                    DebugLog("SetProperty: Parsed %u channels from dec3 magic cookie", cookieChannels);
                }
            }
            return noErr;

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

static OSStatus Initialize(void *self,
                          const AudioStreamBasicDescription *inInputFormat,
                          const AudioStreamBasicDescription *inOutputFormat,
                          const void *inMagicCookie,
                          UInt32 inMagicCookieByteSize) {
    EAC3Decoder *decoder = EAC3_DECODER;
    
    DebugLog("Initialize called! self=%p, decoder=%p", self, decoder);

    // DIAGNOSTIC: log the magic cookie QuickTime passes us. For EAC3 in MP4/MOV this
    // should be a 'dec3' (EC3SpecificBox) carrying the real channel configuration.
    DebugLog("Initialize: magic cookie ptr=%p size=%u", inMagicCookie, (unsigned int)inMagicCookieByteSize);
    if (inMagicCookie && inMagicCookieByteSize > 0) {
        const unsigned char *cookieBytes = (const unsigned char *)inMagicCookie;
        UInt32 dumpLen = inMagicCookieByteSize < 64 ? inMagicCookieByteSize : 64;
        char hex[64 * 3 + 1];
        UInt32 pos = 0;
        for (UInt32 i = 0; i < dumpLen; i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", cookieBytes[i]);
        }
        DebugLog("Initialize: magic cookie bytes: %s", hex);
    }

    UInt32 requestedOutputChannels = decoder->outputFormat.mChannelsPerFrame;
    
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
        DebugLog("Initialize: QuickTime requested %u output channels", requestedOutputChannels);
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
    
    // Set sample rate if known
    if (decoder->inputFormat.mSampleRate > 0) {
        decoder->codecContext->sample_rate = decoder->inputFormat.mSampleRate;
        decoder->outputFormat.mSampleRate = decoder->inputFormat.mSampleRate;
    }
    
    // Open codec
    if (avcodec_open2(decoder->codecContext, decoder->codec, NULL) < 0) {
        DebugLog("Initialize: Failed to open codec");
        avcodec_free_context(&decoder->codecContext);
        return kAudioCodecStateError;
    }
    
    DebugLog("Initialize: After avcodec_open2 - channels=%d, sample_rate=%d", 
             decoder->codecContext->channels, decoder->codecContext->sample_rate);
    
    // The container's 'dec3' magic cookie tells us the true channel count. The host
    // hands it to us inconsistently (the first Initialize of a converter's setup dance
    // often has no cookie, a later one does), so remember it for this instance and let
    // it win on every subsequent Initialize/Reset cycle.
    UInt32 cookieChannels = EAC3ChannelCountFromCookie(inMagicCookie, inMagicCookieByteSize);
    if (cookieChannels > 0 && cookieChannels <= MAX_CHANNEL_COUNT) {
        decoder->cookieChannels = cookieChannels;
        DebugLog("Initialize: Parsed %u channels from dec3 magic cookie", cookieChannels);
    }

    if (decoder->cookieChannels > 0 && decoder->cookieChannels <= MAX_CHANNEL_COUNT) {
        // Authoritative: we know the real channel count from the container's dec3 box.
        decoder->outputFormat.mChannelsPerFrame = decoder->cookieChannels;
        decoder->inputFormat.mChannelsPerFrame = decoder->cookieChannels;
        decoder->codecContext->channels = decoder->cookieChannels;
        DebugLog("Initialize: Using dec3 cookie channels: %u", decoder->cookieChannels);
    } else if (requestedOutputChannels != 2 && requestedOutputChannels <= MAX_CHANNEL_COUNT) {
        // No cookie, but QuickTime seems to provide a channel count > 2 only when it
        // actually knows the real count (e.g. from MKV), so trust it.
        decoder->outputFormat.mChannelsPerFrame = requestedOutputChannels;
        decoder->inputFormat.mChannelsPerFrame = requestedOutputChannels;
        decoder->codecContext->channels = requestedOutputChannels;
        DebugLog("Initialize: Using QuickTime-requested channels: %u", requestedOutputChannels);
    } else {
        // As far as we can tell, QuickTime doesn't know the channel count. Unfortunately, neither do we;
        // we'll get that later from the EAC3 bitstream. QuickTime locks the channel configuration during Initialize,
        // so we'll tell it we have MAX_CHANNEL_COUNT channels. If we have less in reality, we'll add padding.
        decoder->outputFormat.mChannelsPerFrame = MAX_CHANNEL_COUNT;
        decoder->inputFormat.mChannelsPerFrame = MAX_CHANNEL_COUNT;
        decoder->codecContext->channels = MAX_CHANNEL_COUNT;
        DebugLog("Initialize: Using %u channel workaround (QuickTime provided %u)", MAX_CHANNEL_COUNT, requestedOutputChannels);
    }
    
    decoder->outputFormat.mBytesPerFrame = decoder->outputFormat.mChannelsPerFrame * sizeof(Float32);
    decoder->outputFormat.mBytesPerPacket = decoder->outputFormat.mBytesPerFrame * decoder->outputFormat.mFramesPerPacket;
    DebugLog("Initialize: Final - Input channels=%u, Output channels=%u", 
             decoder->inputFormat.mChannelsPerFrame, decoder->outputFormat.mChannelsPerFrame);
    
    
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
    decoder->outputBuffer = malloc(decoder->outputBufferFrames * MAX_CHANNEL_COUNT * sizeof(Float32));
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
                        out_size, consumed, decoder->inputBufferUsed);
                
                // Send packet to decoder
                int ret = avcodec_send_packet(decoder->codecContext, decoder->packet);
                if (ret < 0 && ret != AVERROR_INVALIDDATA) {
                    DebugLog("ProduceOutputData: Error sending packet, ret=%d", ret);
                    continue;
                }
            
            // Receive frames
            while (ret >= 0) {
                ret = avcodec_receive_frame(decoder->codecContext, decoder->frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    DebugLog("ProduceOutputData: Error receiving frame, ret=%d", ret);
                    break;
                }
                
                DebugLog("ProduceOutputData: Decoded frame with %d samples, channels=%d, sample_rate=%d", 
                        decoder->frame->nb_samples, decoder->codecContext->channels, 
                        decoder->codecContext->sample_rate);
                
                // Get channel counts
                UInt32 channels = decoder->codecContext->channels;  // Actual channels in the EAC3 file
                UInt32 outputChannels = decoder->outputFormat.mChannelsPerFrame;  // What we told QuickTime
                UInt32 systemChannels = 0;  // Will be set to actual speaker configuration
                
                DebugLog("ProduceOutputData: channels=%u, outputChannels=%u (from format)", channels, outputChannels);
                
                // 1. 'channels' = what the EAC3 file actually contains
                // 2. 'systemChannels' = what speakers the user has (e.g. 2 for stereo)
                // 3. 'outputChannels' = what we tell QuickTime
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
                        DebugLog("ProduceOutputData: Using actual system channel layout");
                    } else {
                        // Fall back to FFmpeg's default for the channel count
                        out_channel_layout = av_get_default_channel_layout(systemChannels);
                        DebugLog("ProduceOutputData: Using default layout for %u channels", systemChannels);
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
                                               decoder->outputBufferFrames * MAX_CHANNEL_COUNT * sizeof(Float32));
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
                    
                    // During initialize, QuickTime asked us how many channels we had, but it was too early for us to
                    // know the answer. If QuickTime seemed to already know the correct channel count, we trusted it;
                    // otherwise, we said we had MAX_CHANNEL_COUNT channels.

                    // Now we know the true number of channels, but QuickTime won't let us change our previous answer.
                    // We must provide the same number of channels we said we had, or QuickTime will play content at
                    // the wrong speed. If we have fewer real channels (including after downmixing if applicable),
                    // pad the difference with silent channels.
                    
                    // Copy downmixed audio to output buffer with padding to reach outputChannels
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
                            
                        default:
                            DebugLog("ProduceOutputData: Unsupported sample format %d", 
                                    decoder->codecContext->sample_fmt);
                            free(tempBuffer);
                            continue;
                    }
                    
                    // Copy to output buffer with padding to outputChannels
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
                // EAC3 may use different frame sizes (256, 768, 1536, etc.)
                if (samples > 0 && samples != decoder->packetFrameSize) {
                    DebugLog("ProduceOutputData: Adjusting packet frame size from %u to %u based on decoder output", 
                            decoder->packetFrameSize, samples);
                    decoder->packetFrameSize = samples;
                }
                }
            }
        }
        
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
        if (frameSize == 0) frameSize = 1536; // Default if not set
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
        memset(decoder->outputBuffer, 0, decoder->outputBufferFrames * MAX_CHANNEL_COUNT * sizeof(Float32));
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
    decoder->outputFormat.mBytesPerFrame = 2 * sizeof(Float32);
    decoder->outputFormat.mBytesPerPacket = 2 * sizeof(Float32);
    decoder->outputFormat.mSampleRate = 48000;
    decoder->packetFrameSize = 1536;
    decoder->maxPacketSize = 4096;
    decoder->lastSystemChannels = 0;  // Will be set when swresample is initialized
    
    DebugLog("Initialized decoder at %p - input channels=%u, output channels=%u", 
             decoder, decoder->inputFormat.mChannelsPerFrame, decoder->outputFormat.mChannelsPerFrame);
    
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