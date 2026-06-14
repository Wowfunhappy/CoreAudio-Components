#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AudioCodec.h>
#include <AudioUnit/AudioComponent.h>
#include <AudioToolbox/AudioToolbox.h>
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
    
    OpusDecoder *decoder;        // Used for mono/stereo (mapping family 0)
    OpusMSDecoder *msDecoder;    // Used for multi-channel (>2 channels)

    UInt8 *inputBuffer;
    UInt32 inputBufferSize;
    UInt32 inputBufferUsed;
    
    Float32 *outputBuffer;
    UInt32 outputBufferFrames;
    UInt32 outputBufferUsed;
    
    Boolean isInitialized;
    UInt32 packetFrameSize;
    UInt32 maxPacketSize;
    
    // Opus specific
    int channels;
    int preskip;
    float gain;

    // Multi-channel (multistream) configuration, derived from the OpusHead.
    int mappingFamily;
    int streams;
    int coupledStreams;
    unsigned char mapping[8];
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

// Standard CoreAudio layout tag for each channel count. We decode each multi-channel
// stream into the canonical interleaved order these tags imply (the "...A/C" SMPTE
// order, e.g. 5.1 = L R C LFE Ls Rs) and report the matching tag, so the PCM data and
// the declared layout agree. The host uses the declared layout to route each channel
// to the correct speaker.
static const AudioChannelLayoutTag kCoreAudioLayoutTags[8] = {
    kAudioChannelLayoutTag_Mono,        // 1: C
    kAudioChannelLayoutTag_Stereo,      // 2: L R
    kAudioChannelLayoutTag_MPEG_3_0_A,  // 3: L R C
    kAudioChannelLayoutTag_Quadraphonic,// 4: L R Ls Rs
    kAudioChannelLayoutTag_MPEG_5_0_A,  // 5: L R C Ls Rs
    kAudioChannelLayoutTag_MPEG_5_1_A,  // 6: L R C LFE Ls Rs
    kAudioChannelLayoutTag_MPEG_6_1_A,  // 7: L R C LFE Ls Rs Cs
    kAudioChannelLayoutTag_MPEG_7_1_C,  // 8: L R C LFE Ls Rs Rls Rrs
};

// For each channel count, the Opus (Vorbis, RFC 7845) source position that supplies
// each CoreAudio output position above. i.e. coreAudio[k] gets its audio from
// vorbis position kVorbisToCoreAudio[channels-1][k]. Composing this with the
// stream's own mapping lets the multistream decoder write channels straight into
// CoreAudio order for free.
//
//   Vorbis order            CoreAudio order (tag above)
//   1: C                    C
//   2: L R                  L R
//   3: L C R                L R C
//   4: FL FR RL RR          L R Ls Rs        (already matches)
//   5: FL C FR RL RR        L R C Ls Rs
//   6: FL C FR RL RR LFE    L R C LFE Ls Rs
//   7: FL C FR SL SR RC LFE L R C LFE Ls Rs Cs
//   8: FL C FR SL SR RL RR LFE  L R C LFE Ls Rs Rls Rrs
static const unsigned char kVorbisToCoreAudio[8][8] = {
    /* 1 */ { 0 },
    /* 2 */ { 0, 1 },
    /* 3 */ { 0, 2, 1 },
    /* 4 */ { 0, 1, 2, 3 },
    /* 5 */ { 0, 2, 1, 3, 4 },
    /* 6 */ { 0, 2, 1, 5, 3, 4 },
    /* 7 */ { 0, 2, 1, 6, 3, 4, 5 },
    /* 8 */ { 0, 2, 1, 7, 3, 4, 5, 6 },
};

// libopus default multistream configuration for mapping family 1, encoded as
// { streamCount, coupledStreamCount, mapping[channelCount]... }, indexed by
// channelCount-1. Used as a fallback when no OpusHead cookie is supplied. The
// mapping is in Vorbis order; we re-order it to CoreAudio order in Initialize.
static const unsigned char kVorbisDefaultMapping[8][10] = {
    /* 1 */ { 1, 0,  0 },
    /* 2 */ { 1, 1,  0, 1 },
    /* 3 */ { 2, 1,  0, 2, 1 },
    /* 4 */ { 2, 2,  0, 1, 2, 3 },
    /* 5 */ { 3, 2,  0, 4, 1, 2, 3 },
    /* 6 */ { 4, 2,  0, 4, 1, 2, 3, 5 },
    /* 7 */ { 4, 3,  0, 4, 1, 2, 3, 5, 6 },
    /* 8 */ { 5, 3,  0, 6, 1, 2, 3, 4, 5, 7 },
};

// The layout tag we report (and emit channels for) for a given channel count.
static AudioChannelLayoutTag ChannelLayoutTagForChannels(int channels) {
    if (channels >= 1 && channels <= 8) {
        return kCoreAudioLayoutTags[channels - 1];
    }
    return kAudioChannelLayoutTag_UseChannelDescriptions;
}

// Size in bytes of the AudioChannelLayout we report. We always use a layout tag,
// so the bare struct is sufficient.
static UInt32 ChannelLayoutSizeForChannels(int channels) {
    (void)channels;
    return (UInt32)sizeof(AudioChannelLayout);
}

// Fill an AudioChannelLayout describing the decoder's output channels.
static void FillChannelLayout(AudioChannelLayout *layout, int channels) {
    memset(layout, 0, sizeof(AudioChannelLayout));
    layout->mChannelLayoutTag = ChannelLayoutTagForChannels(channels);
    layout->mNumberChannelDescriptions = 0;
}

// Compose a Vorbis-order Opus channel mapping into CoreAudio output order:
// out[k] = vmap[ kVorbisToCoreAudio[channels-1][k] ]. Passing the result to the
// multistream decoder makes it write each channel straight into the speaker slot
// our reported layout tag expects.
static void ReorderVorbisMappingToCoreAudio(const unsigned char *vmap, int channels,
                                            unsigned char *out) {
    const unsigned char *perm = kVorbisToCoreAudio[channels - 1];
    for (int k = 0; k < channels; k++) {
        out[k] = vmap[perm[k]];
    }
}

// Parse an Opus identification header to recover the channel count, mapping
// family and (for multi-channel streams) the multistream configuration. Handles
// both the Ogg "OpusHead" header (little-endian, 8-byte magic prefix) and the
// MP4 "dOps" OpusSpecificBox (big-endian, no magic prefix). Returns true on a
// usable parse. The mapping/stream fields are single bytes, so they are
// endian-independent; only pre-skip differs between the two encapsulations.
static Boolean ParseOpusCookie(const UInt8 *cookie, UInt32 size,
                               int *outChannels, int *outFamily,
                               int *outStreams, int *outCoupled,
                               unsigned char *outMapping /* [8] */,
                               int *outPreskip) {
    if (!cookie) return false;

    Boolean ogg = (size >= 8 && memcmp(cookie, "OpusHead", 8) == 0);
    UInt32 base = ogg ? 8 : 0;

    // Need through the mapping-family byte at base+10.
    if (size < base + 11) return false;

    int channels = cookie[base + 1];
    int family   = cookie[base + 10];
    if (channels < 1 || channels > 8) return false;

    if (outPreskip) {
        *outPreskip = ogg ? (cookie[base + 2] | (cookie[base + 3] << 8))   // LE (Ogg)
                          : ((cookie[base + 2] << 8) | cookie[base + 3]);  // BE (dOps)
    }
    *outChannels = channels;
    *outFamily   = family;

    if (family == 0) {
        // RFC 7845: family 0 is mono or stereo with an implicit mapping.
        *outStreams = 1;
        *outCoupled = (channels == 2) ? 1 : 0;
        outMapping[0] = 0;
        if (channels == 2) outMapping[1] = 1;
        return true;
    }

    // Families 1 and 255 carry an explicit stream count, coupled count and
    // per-channel mapping table immediately after the family byte.
    UInt32 tableOff = base + 11;
    if (size < tableOff + 2 + (UInt32)channels) return false;
    *outStreams = cookie[tableOff];
    *outCoupled = cookie[tableOff + 1];
    for (int i = 0; i < channels; i++) {
        outMapping[i] = cookie[tableOff + 2 + i];
    }
    return true;
}

// Forward declarations
static OSStatus GetPropertyInfo(void *self, AudioCodecPropertyID inPropertyID, UInt32 *outSize, Boolean *outWritable);
static OSStatus GetProperty(void *self, AudioCodecPropertyID inPropertyID, UInt32 *ioPropertyDataSize, void *outPropertyData);
static OSStatus SetProperty(void *self, AudioCodecPropertyID inPropertyID, UInt32 inPropertyDataSize, const void *inPropertyData);
static OSStatus Initialize(void *self, const AudioStreamBasicDescription *inInputFormat, const AudioStreamBasicDescription *inOutputFormat, const void *inMagicCookie, UInt32 inMagicCookieByteSize);
static OSStatus Uninitialize(void *self);
static OSStatus AppendInputData(void *self, const void *inInputData, UInt32 *ioInputDataByteSize, UInt32 *ioNumberPackets, const AudioStreamPacketDescription *inPacketDescription);
static OSStatus ProduceOutputData(void *self, void *outOutputData, UInt32 *ioOutputDataByteSize, UInt32 *ioNumberPackets, AudioStreamPacketDescription *outPacketDescription, UInt32 *outStatus);
static OSStatus Reset(void *self);

// AudioCodec implementation
static OSStatus GetPropertyInfo(void *self,
                               AudioCodecPropertyID inPropertyID,
                               UInt32 *outSize,
                               Boolean *outWritable) {
    OPUSDecoder *decoder = OPUS_DECODER;
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
            if (outSize) *outSize = ChannelLayoutSizeForChannels(decoder->outputFormat.mChannelsPerFrame);
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
                // For OutputFormatsForInputFormat the caller supplies the input ASBD
                // in the buffer; our PCM output has the same channel count as that
                // input. Fall back to the decoder's channel count (or stereo).
                UInt32 outChannels = (inPropertyID == kAudioCodecPropertyOutputFormatsForInputFormat)
                                     ? format->mChannelsPerFrame : (UInt32)decoder->channels;
                if (outChannels < 1 || outChannels > 8) {
                    outChannels = decoder->channels ? (UInt32)decoder->channels : 2;
                }
                memset(format, 0, sizeof(AudioStreamBasicDescription));
                format->mFormatID = kAudioFormatLinearPCM;
                format->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
                format->mBitsPerChannel = 32;
                format->mFramesPerPacket = 1;
                format->mChannelsPerFrame = outChannels;
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
            {
                int channels = (int)decoder->outputFormat.mChannelsPerFrame;
                UInt32 needed = ChannelLayoutSizeForChannels(channels);
                if (*ioPropertyDataSize < needed) {
                    return kAudioCodecBadPropertySizeError;
                }
                FillChannelLayout((AudioChannelLayout *)outPropertyData, channels);
                *ioPropertyDataSize = needed;
            }
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
                        tags[0] = kCoreAudioLayoutTags[channels - 1];
                        *ioPropertyDataSize = sizeof(AudioChannelLayoutTag);
                    } else {
                        return kAudioCodecUnknownPropertyError;
                    }
                } else {
                    // When not initialized, report all supported layouts
                    for (UInt32 i = 0; i < 8; i++) {
                        tags[i] = kCoreAudioLayoutTags[i];
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
    
    // Parse the OpusHead/dOps magic cookie if present. This is the authoritative
    // source for the channel count and, crucially, the multistream layout used by
    // multi-channel streams. (Hard-coding that layout is what produced garbled
    // surround output in earlier attempts.)
    int cookieChannels = 0, cookieFamily = -1, cookieStreams = 0, cookieCoupled = 0, cookiePreskip = 0;
    unsigned char cookieMapping[8] = {0};
    Boolean haveCookie = ParseOpusCookie((const UInt8 *)inMagicCookie, inMagicCookieByteSize,
                                         &cookieChannels, &cookieFamily,
                                         &cookieStreams, &cookieCoupled,
                                         cookieMapping, &cookiePreskip);
    if (haveCookie) {
        DebugLog("Initialize: OpusHead parsed - channels=%d, family=%d, streams=%d, coupled=%d, preskip=%d",
                 cookieChannels, cookieFamily, cookieStreams, cookieCoupled, cookiePreskip);
    } else {
        DebugLog("Initialize: No usable OpusHead cookie (size=%u); using stream format defaults",
                 inMagicCookieByteSize);
    }

    // Determine channel count: trust the cookie, then the input format, then stereo.
    decoder->channels = haveCookie ? cookieChannels : (int)decoder->inputFormat.mChannelsPerFrame;
    if (decoder->channels < 1) {
        decoder->channels = 2; // Default to stereo
    }
    if (decoder->channels > 8) {
        DebugLog("Initialize: Unsupported channel count: %d", decoder->channels);
        return kAudioCodecUnsupportedFormatError;
    }

    int error = OPUS_OK;
    decoder->decoder = NULL;
    decoder->msDecoder = NULL;

    if (decoder->channels <= 2) {
        // Mono/stereo: the plain decoder is sufficient and matches mapping family 0.
        decoder->mappingFamily = 0;
        DebugLog("Initialize: Creating Opus decoder for %d channels", decoder->channels);
        decoder->decoder = opus_decoder_create(48000, decoder->channels, &error);
        if (error != OPUS_OK || !decoder->decoder) {
            DebugLog("Initialize: Failed to create Opus decoder: %d", error);
            return kAudioCodecUnspecifiedError;
        }
    } else {
        // Multi-channel: use the multistream decoder. Prefer the stream configuration
        // from the cookie; fall back to libopus's standard mapping-family-1 config for
        // the channel count. Both give a mapping in Vorbis order, which we re-order to
        // the canonical CoreAudio speaker order that matches our reported layout tag.
        const unsigned char *def = kVorbisDefaultMapping[decoder->channels - 1];
        Boolean usingCookieLayout = (haveCookie && cookieFamily != 0);
        unsigned char vmap[8];
        if (usingCookieLayout) {
            decoder->mappingFamily = cookieFamily;
            decoder->streams = cookieStreams;
            decoder->coupledStreams = cookieCoupled;
            memcpy(vmap, cookieMapping, decoder->channels);
        } else {
            decoder->mappingFamily = 1;
            decoder->streams = def[0];
            decoder->coupledStreams = def[1];
            memcpy(vmap, def + 2, decoder->channels);
        }
        ReorderVorbisMappingToCoreAudio(vmap, decoder->channels, decoder->mapping);

        DebugLog("Initialize: Creating Opus multistream decoder for %d channels (streams=%d, coupled=%d)",
                 decoder->channels, decoder->streams, decoder->coupledStreams);
        decoder->msDecoder = opus_multistream_decoder_create(48000, decoder->channels,
                                                             decoder->streams, decoder->coupledStreams,
                                                             decoder->mapping, &error);

        // If a cookie-supplied layout was rejected, retry with the standard default.
        if ((error != OPUS_OK || !decoder->msDecoder) && usingCookieLayout) {
            DebugLog("Initialize: Cookie layout rejected (%d); retrying with default mapping", error);
            decoder->mappingFamily = 1;
            decoder->streams = def[0];
            decoder->coupledStreams = def[1];
            memcpy(vmap, def + 2, decoder->channels);
            ReorderVorbisMappingToCoreAudio(vmap, decoder->channels, decoder->mapping);
            decoder->msDecoder = opus_multistream_decoder_create(48000, decoder->channels,
                                                                 decoder->streams, decoder->coupledStreams,
                                                                 decoder->mapping, &error);
        }

        if (error != OPUS_OK || !decoder->msDecoder) {
            DebugLog("Initialize: Failed to create multistream decoder: %d", error);
            return kAudioCodecUnspecifiedError;
        }
    }

    // Initialize buffers
    decoder->inputBufferSize = 65536;
    decoder->inputBuffer = (UInt8 *)calloc(decoder->inputBufferSize, 1);
    decoder->inputBufferUsed = 0;
    
    decoder->outputBufferFrames = 48000; // 1 second at 48kHz
    decoder->outputBuffer = (Float32 *)calloc(decoder->outputBufferFrames * decoder->channels, sizeof(Float32));
    decoder->outputBufferUsed = 0;
    
    decoder->maxPacketSize = 4000;
    decoder->packetFrameSize = 960; // 20ms at 48kHz
    decoder->preskip = 0;
    decoder->gain = 1.0f;
    
    // Update output format. Our output always has decoder->channels channels; the
    // host learns this up front via kAudioCodecPropertyOutputFormatsForInputFormat
    // and configures itself (and any downmix to the actual speakers) accordingly.
    decoder->outputFormat.mSampleRate = 48000;
    decoder->outputFormat.mChannelsPerFrame = decoder->channels;
    decoder->outputFormat.mBytesPerFrame = decoder->channels * sizeof(Float32);
    decoder->outputFormat.mBytesPerPacket = decoder->outputFormat.mBytesPerFrame;
    decoder->outputFormat.mFramesPerPacket = 1;
    
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
        if (decoder->outputBufferUsed + maxFrames > decoder->outputBufferFrames) {
            decoder->outputBufferFrames = (decoder->outputBufferUsed + maxFrames) * 2;
            decoder->outputBuffer = realloc(decoder->outputBuffer,
                                          decoder->outputBufferFrames * decoder->channels * sizeof(Float32));
        }
        
        Float32 *decodeTarget = decoder->outputBuffer + (decoder->outputBufferUsed * decoder->channels);
        int frameSize;
        if (decoder->msDecoder) {
            frameSize = opus_multistream_decode_float(decoder->msDecoder,
                                                     decoder->inputBuffer,
                                                     decoder->inputBufferUsed,
                                                     decodeTarget,
                                                     maxFrames,
                                                     0);
        } else {
            frameSize = opus_decode_float(decoder->decoder,
                                         decoder->inputBuffer,
                                         decoder->inputBufferUsed,
                                         decodeTarget,
                                         maxFrames,
                                         0);
        }

        if (frameSize < 0) {
            DebugLog("ProduceOutputData: opus decode failed: %d (%s)",
                     frameSize, opus_strerror(frameSize));
            // Clear the input buffer on error
            decoder->inputBufferUsed = 0;
        } else {
            decoder->outputBufferUsed += frameSize;
            decoder->inputBufferUsed = 0; // Consumed all input
            DebugLog("ProduceOutputData: decoded %d frames", frameSize);
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
            memmove(decoder->outputBuffer, 
                   decoder->outputBuffer + (framesToCopy * decoder->channels),
                   remainingFrames * decoder->channels * sizeof(Float32));
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
    if (decoder->msDecoder) {
        opus_multistream_decoder_ctl(decoder->msDecoder, OPUS_RESET_STATE);
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