/* encvt_h264.c

   Copyright (c) 2003-2016 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "hb.h"
#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

int  encvt_h264Init( hb_work_object_t *, hb_job_t * );
int  encvt_h264Work( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void encvt_h264Close( hb_work_object_t * );

hb_work_object_t hb_encvt_h264 =
{
    WORK_ENCVT_H264,
    "H.264 encoder (VideoToolbox)",
    encvt_h264Init,
    encvt_h264Work,
    encvt_h264Close
};

/*
 * The frame info struct remembers information about each frame across calls
 * to x264_encoder_encode. Since frames are uniquely identified by their
 * timestamp, we use some bits of the timestamp as an index. The LSB is
 * chosen so that two successive frames will have different values in the
 * bits over any plausible range of frame rates. (Starting with bit 8 allows
 * any frame rate slower than 352fps.) The MSB determines the size of the array.
 * It is chosen so that two frames can't use the same slot during the
 * encoder's max frame delay (set by the standard as 16 frames) and so
 * that, up to some minimum frame rate, frames are guaranteed to map to
 * different slots. (An MSB of 17 which is 2^(17-8+1) = 1024 slots guarantees
 * no collisions down to a rate of .7 fps).
 */
#define FRAME_INFO_MAX2 (8)     // 2^8 = 256; 90000/256 = 352 frames/sec
#define FRAME_INFO_MIN2 (17)    // 2^17 = 128K; 90000/131072 = 1.4 frames/sec
#define FRAME_INFO_SIZE (1 << (FRAME_INFO_MIN2 - FRAME_INFO_MAX2 + 1))
#define FRAME_INFO_MASK (FRAME_INFO_SIZE - 1)

struct hb_work_private_s
{
    hb_job_t    * job;

    CMFormatDescriptionRef  format;
    VTCompressionSessionRef session;

    CMSimpleQueueRef queue;

    // Multipass
    VTMultiPassStorageRef passStorage;
    CMItemCount           timeRangeCount;
    const CMTimeRange   * timeRangeArray;
    int                   remainingPasses;

    struct hb_vt_h264_param
    {
        int averageBitRate;
        double expectedFrameRate;

        int h264_profile;
        CFStringRef profileLevel;

        int maxFrameDelayCount;
        int maxKeyFrameInterval;
        CFBooleanRef allowFrameReordering;
        CFBooleanRef allowTemporalCompression;
        struct
        {
            int maxrate;
            int bufsize;
        }
        vbv;
        struct
        {
            int prim;
            int matrix;
            int transfer;
        }
        color;
        struct
        {
            SInt32 width;
            SInt32 height;
        }
        par;
        enum
        {
            HB_VT_FIELDORDER_PROGRESSIVE = 0,
            HB_VT_FIELDORDER_TFF,
            HB_VT_FIELDORDER_BFF,
        }
        fieldDetail;
        /*
         * Intel HD Graphics 3000, Mac OS X 10.8.4, supported properties:
         *
         * "MaxFrameDelayCount"                    // implemented (GopRefDist???)
         * "NumberOfParallelCores"
         * "ConvergenceDurationForAverageDataRate"
         * "AverageDataRate"
         * "AllowTemporalCompression"              // implemented
         * "ThrottleForBackground"
         * "ExpectedDuration"                      // not useful
         * "MaxKeyFrameInterval"                   // implemented
         * "TransferFunction"                      // needs fixing
         * "InputQueueMaxCount"
         * "AllowFrameReordering"                  // implemented
         * "PixelAspectRatio"                      // needs fixing
         * "SourceFrameCount"                      // not useful
         * "ExpectedFrameRate"                     // needs fixing
         * "ExpectedInputBufferDimensions"
         * "ICCProfile"
         * "UsingHardwareAcceleratedVideoEncoder"  // implemented???
         * "Depth"
         * "DataRateLimits"                        // needs fixing???
         * "YCbCrMatrix"                           // needs fixing
         * "PixelBufferPoolIsShared"
         * "VideoEncoderPixelBufferAttributes"
         * "FieldCount"                            // needs fixing
         * "NegotiationDetails"
         * "Priority"                              // implemented
         * "ColorPrimaries"                        // needs fixing
         * "AverageBitRate"                        // implemented
         * "NumberOfPendingFrames"                 // not useful
         * "FieldDetail"                           // needs fixing
         * "EncoderUsage"
         * "ProfileLevel"                          // implemented
         * "MaxKeyFrameIntervalDuration"           // use MaxKeyFrameInterval
         * "CleanAperture"
         * "PixelTransferProperties"
         * "NumberOfSlices"
         */
    }
    settings;

    // Sync
    int init_delay;

    struct {
        int64_t        duration;
    } frame_info[FRAME_INFO_SIZE];

    hb_list_t        * delayed_chapters;
    int64_t            next_chapter_pts;
};

void hb_vt_param_default(struct hb_vt_h264_param *param)
{
    param->vbv.maxrate              = 0;
    param->vbv.bufsize              = 0;
    param->maxFrameDelayCount       = 24;
    param->allowFrameReordering     = kCFBooleanTrue;
    param->allowTemporalCompression = kCFBooleanTrue;
    param->fieldDetail              = HB_VT_FIELDORDER_PROGRESSIVE;
}

// used to pass the compression session
// to the next job
typedef struct vt_interjob_s
{
    VTCompressionSessionRef session;
    VTMultiPassStorageRef   passStorage;
    CMSimpleQueueRef        queue;
    CMFormatDescriptionRef  format;
} vt_interjob_t;

// used in delayed_chapters list
struct chapter_s
{
    int     index;
    int64_t start;
};

enum
{
    HB_VT_H264_PROFILE_BASELINE = 0,
    HB_VT_H264_PROFILE_MAIN,
    HB_VT_H264_PROFILE_HIGH,
    HB_VT_H264_PROFILE_NB,
};

struct
{
    const char *name;
    const CFStringRef level[HB_VT_H264_PROFILE_NB];
}
hb_vt_h264_levels[] =
{
    // TODO: implement automatic level detection
    { "auto", { CFSTR("H264_Baseline_AutoLevel"), CFSTR("H264_Main_AutoLevel"), CFSTR("H264_High_AutoLevel"), }, },
    // support all levels returned by hb_h264_levels()
    { "1.0",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "1b",   { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "1.1",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "1.2",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "1.3",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "2.0",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_Main_3_0"    ), }, },
    { "2.1",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_Main_3_0"    ), }, },
    { "2.2",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_Main_3_0"    ), }, },
    { "3.0",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_High_3_0"    ), }, },
    { "3.1",  { CFSTR("H264_Baseline_3_1"), CFSTR("H264_Main_3_1"    ), CFSTR("H264_High_3_1"    ), }, },
    { "3.2",  { CFSTR("H264_Baseline_3_2"), CFSTR("H264_Main_3_2"    ), CFSTR("H264_High_3_2"    ), }, },
    { "4.0",  { CFSTR("H264_Baseline_4_1"), CFSTR("H264_Main_4_0"    ), CFSTR("H264_High_4_0"    ), }, },
    { "4.1",  { CFSTR("H264_Baseline_4_1"), CFSTR("H264_Main_4_1"    ), CFSTR("H264_High_4_1"    ), }, },
    { "4.2",  { CFSTR("H264_Baseline_4_2"), CFSTR("H264_Main_4_2"    ), CFSTR("H264_High_4_2"    ), }, },
    { "5.0",  { CFSTR("H264_Baseline_5_0"), CFSTR("H264_Main_5_0"    ), CFSTR("H264_High_5_0"    ), }, },
    { "5.1",  { CFSTR("H264_Baseline_5_1"), CFSTR("H264_Main_5_1"    ), CFSTR("H264_High_5_1"    ), }, },
    { "5.2",  { CFSTR("H264_Baseline_5_2"), CFSTR("H264_Main_5_2"    ), CFSTR("H264_High_5_2"    ), }, },
    { NULL,   { NULL,                       NULL,                       NULL,                       }, },
};

static CFStringRef hb_vt_colr_pri_xlat(int color_prim)
{
    switch (color_prim)
    {
        case HB_COLR_PRI_BT709:
            return kCMFormatDescriptionColorPrimaries_ITU_R_709_2;
        case HB_COLR_PRI_EBUTECH:
            return kCMFormatDescriptionColorPrimaries_EBU_3213;
        case HB_COLR_PRI_SMPTEC:
            return kCMFormatDescriptionColorPrimaries_SMPTE_C;
        default:
            return NULL;
    }
}

static CFStringRef hb_vt_colr_tra_xlat(int color_transfer)
{
    switch (color_transfer)
    {
        case HB_COLR_TRA_BT709:
            return kCMFormatDescriptionTransferFunction_ITU_R_709_2;
        case HB_COLR_TRA_SMPTE240M:
            return kCMFormatDescriptionTransferFunction_SMPTE_240M_1995;
        default:
            return NULL;
    }
}

static CFStringRef hb_vt_colr_mat_xlat(int color_matrix)
{
    switch (color_matrix)
    {
        case HB_COLR_MAT_BT709:
            return kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2;
        case HB_COLR_MAT_SMPTE170M:
            return kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4;
        case HB_COLR_MAT_SMPTE240M:
            return kCMFormatDescriptionYCbCrMatrix_SMPTE_240M_1995;
        default:
            return NULL;
    }
}

static void hb_vt_add_color_tag(CVPixelBufferRef pxbuffer, hb_job_t *job)
{
    CFStringRef prim = hb_vt_colr_pri_xlat(job->title->color_prim);
    CFStringRef trasfer = hb_vt_colr_tra_xlat(job->title->color_transfer);
    CFStringRef matrix = hb_vt_colr_mat_xlat(job->title->color_matrix);

    CVBufferSetAttachment(pxbuffer, kCVImageBufferColorPrimariesKey, prim, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pxbuffer, kCVImageBufferTransferFunctionKey, trasfer, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pxbuffer, kCVImageBufferYCbCrMatrixKey, matrix, kCVAttachmentMode_ShouldPropagate);
}

void pixelBufferReleasePlanarBytesCallback(
                                           void *releaseRefCon,
                                           const void *dataPtr,
                                           size_t dataSize,
                                           size_t numberOfPlanes,
                                           const void *planeAddresses[])
{
    hb_buffer_t * buf = (hb_buffer_t *) releaseRefCon;
    hb_buffer_close(&buf);
}

void pixelBufferReleaseBytesCallback(void *releaseRefCon, const void *baseAddress)
{
    free((void *) baseAddress);
}

void myVTCompressionOutputCallback(
                                   void *outputCallbackRefCon,
                                   void *sourceFrameRefCon,
                                   OSStatus status,
                                   VTEncodeInfoFlags infoFlags,
                                   CMSampleBufferRef sampleBuffer)
{
    OSStatus err;

    if (sourceFrameRefCon)
    {
        CVPixelBufferRef pixelbuffer = sourceFrameRefCon;
        CVPixelBufferRelease(pixelbuffer);
    }

    if (status != noErr)
    {
        hb_log("VTCompressionSession: myVTCompressionOutputCallback called error");
    }
    else
    {
        CFRetain(sampleBuffer);
        CMSimpleQueueRef queue = outputCallbackRefCon;
        err = CMSimpleQueueEnqueue(queue, sampleBuffer);
        if (err)
        {
            hb_log("VTCompressionSession: myVTCompressionOutputCallback queue full");
        }
    }
}

//#define VT_STATS

#ifdef VT_STATS
static void toggle_vt_gva_stats(bool state)
{
    CFPropertyListRef cf_state = state ? kCFBooleanTrue : kCFBooleanFalse;
    CFPreferencesSetValue(CFSTR("gvaEncoderPerf"), cf_state, CFSTR("com.apple.GVAEncoder"), kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
    CFPreferencesSetValue(CFSTR("gvaEncoderPSNR"), cf_state, CFSTR("com.apple.GVAEncoder"), kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
    CFPreferencesSetValue(CFSTR("gvaEncoderSSIM"), cf_state, CFSTR("com.apple.GVAEncoder"), kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
}
#endif

static OSStatus init_vtsession(hb_work_object_t * w, hb_job_t * job, hb_work_private_t * pv, OSType pixelFormatType, int cookieOnly)
{
    OSStatus err = noErr;

    CFStringRef bkey = kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder;
    CFBooleanRef bvalue = kCFBooleanTrue;

    CFStringRef ckey = kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder;
    CFBooleanRef cvalue = kCFBooleanTrue;

    CFMutableDictionaryRef encoderSpecifications = CFDictionaryCreateMutable(
                                                                             kCFAllocatorDefault,
                                                                             3,
                                                                             &kCFTypeDictionaryKeyCallBacks,
                                                                             &kCFTypeDictionaryValueCallBacks);

    // Comment out to disable QuickSync
    CFDictionaryAddValue(encoderSpecifications, bkey, bvalue);
    CFDictionaryAddValue(encoderSpecifications, ckey, cvalue);

    err = VTCompressionSessionCreate(
                               kCFAllocatorDefault,
                               job->width,
                               job->height,
                               kCMVideoCodecType_H264,
                               encoderSpecifications,
                               NULL,
                               NULL,
                               &myVTCompressionOutputCallback,
                               pv->queue,
                               &pv->session);

    CFRelease(encoderSpecifications);

    if (err != noErr)
    {
        hb_log("Error creating a VTCompressionSession err=%"PRId64"", (int64_t)err);
        return err;
    }
    CFNumberRef cfValue = NULL;

    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_AllowFrameReordering,
                               pv->settings.allowFrameReordering);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_AllowFrameReordering failed");
    }

    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                             &pv->settings.maxKeyFrameInterval);
    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_MaxKeyFrameInterval,
                               cfValue);
    CFRelease(cfValue);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_MaxKeyFrameInterval failed");
    }

    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                             &pv->settings.maxFrameDelayCount);
    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_MaxFrameDelayCount,
                               cfValue);
    CFRelease(cfValue);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_MaxFrameDelayCount failed");
    }

    if (pv->settings.vbv.maxrate > 0 &&
        pv->settings.vbv.bufsize > 0)
    {
        float seconds = ((float)pv->settings.vbv.bufsize /
                         (float)pv->settings.vbv.maxrate);
        int bytes = pv->settings.vbv.maxrate * 125 * seconds;
        CFNumberRef size = CFNumberCreate(kCFAllocatorDefault,
                                          kCFNumberIntType, &bytes);
        CFNumberRef duration = CFNumberCreate(kCFAllocatorDefault,
                                              kCFNumberFloatType, &seconds);
        CFMutableArrayRef dataRateLimits = CFArrayCreateMutable(kCFAllocatorDefault, 2,
                                                                &kCFTypeArrayCallBacks);
        CFArrayAppendValue(dataRateLimits, size);
        CFArrayAppendValue(dataRateLimits, duration);
        err = VTSessionSetProperty(pv->session,
                                   kVTCompressionPropertyKey_DataRateLimits,
                                   dataRateLimits);
        CFRelease(size);
        CFRelease(duration);
        CFRelease(dataRateLimits);
        if (err != noErr)
        {
            hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_DataRateLimits failed");
        }
    }

    if (pv->settings.fieldDetail != HB_VT_FIELDORDER_PROGRESSIVE)
    {
        int count = 2;
        cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &count);
        err = VTSessionSetProperty(pv->session,
                                   kVTCompressionPropertyKey_FieldCount,
                                   cfValue);
        CFRelease(cfValue);
        if (err != noErr)
        {
            hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_FieldCount failed");
        }

        CFStringRef cfStringValue = NULL;

        switch (pv->settings.fieldDetail)
        {
            case HB_VT_FIELDORDER_BFF:
                cfStringValue = kCMFormatDescriptionFieldDetail_TemporalBottomFirst;
                break;
            case HB_VT_FIELDORDER_TFF:
            default:
                cfStringValue = kCMFormatDescriptionFieldDetail_TemporalTopFirst;
                break;
        }
        err = VTSessionSetProperty(pv->session,
                                   kVTCompressionPropertyKey_FieldDetail,
                                   cfStringValue);
        if (err != noErr)
        {
            hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_FieldDetail failed");
        }
    }

    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_ColorPrimaries,
                               hb_vt_colr_pri_xlat(pv->settings.color.prim));
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_ColorPrimaries failed");
    }
    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_TransferFunction,
                               hb_vt_colr_tra_xlat(pv->settings.color.transfer));
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_TransferFunction failed");
    }
    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_YCbCrMatrix,
                               hb_vt_colr_mat_xlat(pv->settings.color.matrix));
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_YCbCrMatrix failed");
    }

    CFNumberRef parWidth = CFNumberCreate(kCFAllocatorDefault,
                                          kCFNumberSInt32Type,
                                          &pv->settings.par.width);
    CFNumberRef parHeight = CFNumberCreate(kCFAllocatorDefault,
                                           kCFNumberSInt32Type,
                                           &pv->settings.par.height);
    CFMutableDictionaryRef pixelAspectRatio = CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
                                                                        &kCFTypeDictionaryKeyCallBacks,
                                                                        &kCFTypeDictionaryValueCallBacks);
    CFDictionaryAddValue(pixelAspectRatio,
                         kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing,
                         parWidth);
    CFDictionaryAddValue(pixelAspectRatio,
                         kCMFormatDescriptionKey_PixelAspectRatioVerticalSpacing,
                         parHeight);
    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_PixelAspectRatio,
                               pixelAspectRatio);
    CFRelease(parWidth);
    CFRelease(parHeight);
    CFRelease(pixelAspectRatio);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_PixelAspectRatio failed");
    }

    // FIXME: seems to get rounded, e.g. 23.976 -> 24
    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType,
                             &pv->settings.expectedFrameRate);
    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_ExpectedFrameRate,
                               cfValue);
    CFRelease(cfValue);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_ExpectedFrameRate failed");
    }

    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                             &pv->settings.averageBitRate);
    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_AverageBitRate,
                               cfValue);
    CFRelease(cfValue);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_AverageBitRate failed");
    }

    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_ProfileLevel,
                               pv->settings.profileLevel);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_ProfileLevel failed");
    }

    // Multi-pass
    if (job->pass_id == HB_PASS_ENCODE_1ST && cookieOnly == 0)
    {
        char filename[1024];
        memset(filename, 0, 1024);
        hb_get_tempory_filename(job->h, filename, "videotoolbox.log");

        CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, filename, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, FALSE);
        err = VTMultiPassStorageCreate(kCFAllocatorDefault, url, kCMTimeRangeInvalid, NULL, &pv->passStorage);

        if (err != noErr)
        {
            return err;
        }
        else
        {
            err = VTSessionSetProperty(pv->session,
                                       kVTCompressionPropertyKey_MultiPassStorage,
                                       pv->passStorage);
            if (err != noErr)
            {
                hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_MultiPassStorage failed");
            }

            err =  VTCompressionSessionBeginPass(pv->session, 0, 0);
            if (err != noErr)
            {
                hb_log("VTCompressionSessionBeginPass failed");
            }
        }

        CFRelease(path);
        CFRelease(url);
    }

    err = VTCompressionSessionPrepareToEncodeFrames(pv->session);
    if (err != noErr)
    {
        hb_log("VTCompressionSessionPrepareToEncodeFrames failed");
        return err;
    }

    return err;
}

static void set_cookie(hb_work_object_t * w, CMFormatDescriptionRef format)
{
    CFDictionaryRef extentions = CMFormatDescriptionGetExtensions(format);
    if (!extentions)
    {
        hb_log("VTCompressionSession: Format Description Extensions error");
    }
    else
    {

        CFDictionaryRef atoms = CFDictionaryGetValue(extentions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
        CFDataRef magicCookie = CFDictionaryGetValue(atoms, CFSTR("avcC"));

        const uint8_t *avcCAtom = CFDataGetBytePtr(magicCookie);

        SInt64 i;
        int8_t spsCount = (avcCAtom[5] & 0x1f);
        uint8_t ptrPos = 6;
        uint8_t spsPos = 0;
        for (i = 0; i < spsCount; i++) {
            uint16_t spsSize = (avcCAtom[ptrPos++] << 8) & 0xff00;
            spsSize += avcCAtom[ptrPos++] & 0xff;
            memcpy(w->config->h264.sps + spsPos, avcCAtom+ptrPos, spsSize);;
            ptrPos += spsSize;
            spsPos += spsSize;
        }
        w->config->h264.sps_length = spsPos;

        int8_t ppsCount = avcCAtom[ptrPos++];
        uint8_t ppsPos = 0;
        for (i = 0; i < ppsCount; i++)
        {
            uint16_t ppsSize = (avcCAtom[ptrPos++] << 8) & 0xff00;
            ppsSize += avcCAtom[ptrPos++] & 0xff;
            memcpy(w->config->h264.pps + ppsPos, avcCAtom+ptrPos, ppsSize);;

            ptrPos += ppsSize;
            ppsPos += ppsSize;
        }
        w->config->h264.pps_length = ppsPos;

    }
}

static OSStatus create_cookie(hb_work_object_t * w, hb_job_t * job, hb_work_private_t * pv)
{
    OSStatus err;

    err = init_vtsession(w, job, pv, kCVPixelFormatType_420YpCbCr8Planar, 1);
    if (err != noErr)
    {
        return err;
    }

    size_t rgbBufSize = sizeof(uint8) * 3 * job->width * job->height;
    uint8 *rgbBuf = malloc(rgbBufSize);

    // Compress a random frame to get the magicCookie
    CVPixelBufferRef pxbuffer = NULL;
    CVPixelBufferCreateWithBytes(kCFAllocatorDefault,
                                 job->width,
                                 job->height,
                                 kCVPixelFormatType_24RGB,
                                 rgbBuf,
                                 job->width * 3,
                                 &pixelBufferReleaseBytesCallback,
                                 NULL,
                                 NULL,
                                 &pxbuffer);

    if (kCVReturnSuccess != err)
    {
        hb_log("VTCompressionSession: CVPixelBuffer error");
    }

    hb_vt_add_color_tag(pxbuffer, job);

    CMTime pts = CMTimeMake(0, 90000);
    err = VTCompressionSessionEncodeFrame(
                                          pv->session,
                                          pxbuffer,
                                          pts,
                                          kCMTimeInvalid,
                                          NULL,
                                          pxbuffer,
                                          NULL);
    err = VTCompressionSessionCompleteFrames(pv->session, CMTimeMake(0,90000));
    if (noErr != err)
    {
        hb_log("VTCompressionSession: VTCompressionSessionCompleteFrames error");
    }
    CMSampleBufferRef sampleBuffer = (CMSampleBufferRef) CMSimpleQueueDequeue(pv->queue);

    if (!sampleBuffer)
    {
        hb_log("VTCompressionSession: sampleBuffer == NULL");
        VTCompressionSessionInvalidate(pv->session);
        CFRelease(pv->session);
        pv->session = NULL;
        return -1;
    }
    else
    {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (!format)
        {
            hb_log("VTCompressionSession: Format Description error");
        }
        else
        {
            pv->format = format;
            CFRetain(pv->format);

            set_cookie(w, format);
        }

        CFRelease(sampleBuffer);
    }
    
    VTCompressionSessionInvalidate(pv->session);
    CFRelease(pv->session);
    
    return err;
}

static OSStatus reuse_vtsession(hb_work_object_t * w, hb_job_t * job, hb_work_private_t * pv)
{
    OSStatus err = noErr;

    hb_interjob_t * interjob = hb_interjob_get(job->h);
    vt_interjob_t * context = interjob->vt_context;

    set_cookie(w, context->format);

    CFRelease(pv->queue);

    pv->queue = context->queue;
    pv->session = context->session;
    pv->passStorage = context->passStorage;

    if (err != noErr)
    {
        hb_log("Error reusing a VTCompressionSession err=%"PRId64"", (int64_t)err);
        return err;
    }

    err = VTCompressionSessionGetTimeRangesForNextPass(pv->session, &pv->timeRangeCount, &pv->timeRangeArray);

    if (err != noErr)
    {
        hb_log("Error begining a VTCompressionSession final pass err=%"PRId64"", (int64_t)err);
        return err;
    }

    err = VTCompressionSessionBeginPass(pv->session, kVTCompressionSessionBeginFinalPass, 0);

    if (err != noErr)
    {
        hb_log("Error begining a VTCompressionSession final pass err=%"PRId64"", (int64_t)err);
        return err;
    }

    interjob->vt_context = NULL;
    free(context);

    return err;
}


int encvt_h264Init(hb_work_object_t * w, hb_job_t * job)
{
#ifdef VT_STATS
    toggle_vt_gva_stats(true);
#endif

    OSStatus err;
    hb_work_private_t * pv = calloc(1, sizeof(hb_work_private_t));
    w->private_data = pv;

    pv->job = job;
    pv->next_chapter_pts = AV_NOPTS_VALUE;
    pv->delayed_chapters = hb_list_init();

    // set the profile and level before initializing the session
    if (job->encoder_profile != NULL && *job->encoder_profile != '\0')
    {
        if (!strcasecmp(job->encoder_profile, "baseline"))
        {
            pv->settings.h264_profile = HB_VT_H264_PROFILE_BASELINE;
        }
        else if (!strcasecmp(job->encoder_profile, "main") ||
                 !strcasecmp(job->encoder_profile, "auto"))
        {
            pv->settings.h264_profile = HB_VT_H264_PROFILE_MAIN;
        }
        else if (!strcasecmp(job->encoder_profile, "high"))
        {
            pv->settings.h264_profile = HB_VT_H264_PROFILE_HIGH;
        }
        else
        {
            hb_error("encvt_h264Init: invalid profile '%s'", job->encoder_profile);
            *job->die = 1;
            return -1;
        }
    }
    else
    {
        pv->settings.h264_profile = HB_VT_H264_PROFILE_HIGH;
    }

    if (job->encoder_level != NULL && *job->encoder_level != '\0')
    {
        int i;
        for (i = 0; hb_vt_h264_levels[i].name != NULL; i++)
        {
            if (!strcasecmp(job->encoder_level, hb_vt_h264_levels[i].name))
            {
                pv->settings.profileLevel = hb_vt_h264_levels[i].level[pv->settings.h264_profile];
                break;
            }
        }
        if (hb_vt_h264_levels[i].name == NULL)
        {
            hb_error("encvt_h264Init: invalid level '%s'", job->encoder_level);
            *job->die = 1;
            return -1;
        }
    }
    else
    {
        pv->settings.profileLevel = hb_vt_h264_levels[0].level[pv->settings.h264_profile];
    }

    pv->settings.maxKeyFrameInterval  = pv->settings.expectedFrameRate * 5;

    /* Compute the frame rate and output bit rate. */
    pv->settings.expectedFrameRate = (double)job->vrate.num / (double)job->vrate.den;
    pv->init_delay = 90000 / pv->settings.expectedFrameRate * 2;

    if (job->vquality >= 0.0)
    {
        /*
         * XXX: CQP not supported, so let's come up with a "good" bitrate value
         *
         * Offset by the width to that vquality == 0.0 doesn't result in 0 Kbps.
         *
         * Compression efficiency can be pretty low, so let's be generous:
         *  720 x  480, 30 fps -> ~2800 Kbps (50%),  ~5800 Kbps (75%)
         * 1280 x  720, 25 fps -> ~5000 Kbps (50%), ~10400 Kbps (75%)
         * 1920 x 1080, 24 fps -> ~8800 Kbps (50%), ~18500 Kbps (75%)
         */
        double offset      = job->height * 1000.;
        double quality     = job->vquality * job->vquality / 75.;
        double properties  = job->height * sqrt(job->width * pv->settings.expectedFrameRate);
        pv->settings.averageBitRate = properties * quality + offset;
    }
    else if (job->vbitrate > 0)
    {
        pv->settings.averageBitRate = job->vbitrate * 1000;

    }
    else
    {
        hb_error("encvt_h264Init: invalid rate control (bitrate %d, quality %f)",
                 job->vbitrate, job->vquality);
    }
    hb_log("encvt_h264Init: encoding with output bitrate %d Kbps",
           pv->settings.averageBitRate / 1000);


    /* Set global default values. */
    hb_vt_param_default(&pv->settings);

    /* Initialize input-specific default settings. */
    switch (job->color_matrix_code)
    {
        case 4: // custom
            pv->settings.color.prim     = job->color_prim;
            pv->settings.color.transfer = job->color_transfer;
            pv->settings.color.matrix   = job->color_matrix;
            break;

        case 3: // ITU BT.709 HD content
            pv->settings.color.prim     = HB_COLR_PRI_BT709;
            pv->settings.color.transfer = HB_COLR_TRA_BT709;
            pv->settings.color.matrix   = HB_COLR_MAT_BT709;
            break;

        case 2: // ITU BT.601 DVD or SD TV content (PAL)
            pv->settings.color.prim     = HB_COLR_PRI_EBUTECH;
            pv->settings.color.transfer = HB_COLR_TRA_BT709;
            pv->settings.color.matrix   = HB_COLR_MAT_SMPTE170M;
            break;

        case 1: // ITU BT.601 DVD or SD TV content (NTSC)
            pv->settings.color.prim     = HB_COLR_PRI_SMPTEC;
            pv->settings.color.transfer = HB_COLR_TRA_BT709;
            pv->settings.color.matrix   = HB_COLR_MAT_SMPTE170M;
            break;

        default: // detected during scan
            pv->settings.color.prim     = job->title->color_prim;
            pv->settings.color.transfer = job->title->color_transfer;
            pv->settings.color.matrix   = job->title->color_matrix;
            break;
    }
    // Note: Quick Sync Video usually works fine with keyframes every 5 seconds.
    int fps                          = pv->settings.expectedFrameRate + 0.5;
    pv->settings.par.height          = job->par.num;
    pv->settings.par.width           = job->par.den;
    pv->settings.maxKeyFrameInterval = fps * 5 + 1;

    /* TODO: implement advanced options parsing. */

    /*
     * Reload colorimetry settings in case custom values were set in the
     * advanced options string
     */
    job->color_matrix_code = 4;
    job->color_prim        = pv->settings.color.prim;
    job->color_transfer    = pv->settings.color.transfer;
    job->color_matrix      = pv->settings.color.matrix;

    /* Sanitize interframe settings */
    switch (pv->settings.maxKeyFrameInterval)
    {
        case 1:
            pv->settings.allowTemporalCompression = kCFBooleanFalse;
        case 2:
            pv->settings.allowFrameReordering     = kCFBooleanFalse;
        default:
            break;
    }
    switch (pv->settings.maxFrameDelayCount)
    {
            // FIXME: is this GopRefDist???
        case 0:
            pv->settings.allowTemporalCompression = kCFBooleanFalse;
        case 1:
            pv->settings.allowFrameReordering     = kCFBooleanFalse;
        default:
            break;
    }

    CMSimpleQueueCreate(
                        kCFAllocatorDefault,
                        200,
                        &pv->queue);

    if (job->pass_id == HB_PASS_ENCODE_1ST)
    {
        pv->remainingPasses = 1;
    }
    else
    {
        pv->remainingPasses = 0;
    }

    if (job->pass_id != HB_PASS_ENCODE_2ND)
    {
        err = create_cookie(w, job, pv);
        if (err != noErr)
        {
            hb_log("VTCompressionSession: Magic Cookie Error err=%"PRId64"", (int64_t)err);
            *job->die = 1;
            return -1;
        }

        err = init_vtsession(w, job, pv, kCVPixelFormatType_420YpCbCr8Planar, 0);
        if (err != noErr)
        {
            hb_log("VTCompressionSession: Error creating a VTCompressionSession err=%"PRId64"", (int64_t)err);
            *job->die = 1;
            return -1;
        }
    }
    else
    {
        err = reuse_vtsession(w, job, pv);
        if (err != noErr)
        {
            hb_log("VTCompressionSession: Error reusing a VTCompressionSession err=%"PRId64"", (int64_t)err);
            *job->die = 1;
            return -1;
        }
    }

    return 0;
}

void encvt_h264Close( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;

    if (pv == NULL)
    {
        // Not initialized
        return;
    }
    if (pv->delayed_chapters != NULL)
    {
        struct chapter_s *item;
        while ((item = hb_list_item(pv->delayed_chapters, 0)) != NULL)
        {
            hb_list_rem(pv->delayed_chapters, item);
            free(item);
        }
        hb_list_close(&pv->delayed_chapters);
    }

    if (pv->remainingPasses == 0)
    {
        if (pv->session)
        {
            VTCompressionSessionInvalidate(pv->session);
            CFRelease(pv->session);
        }
        if (pv->passStorage)
        {
            VTMultiPassStorageClose(pv->passStorage);
            CFRelease(pv->passStorage);
        }
        if (pv->queue)
        {
            CFRelease(pv->queue);
        }
        if (pv->format)
        {
            CFRelease(pv->format);
        }
    }

    free(pv);
    w->private_data = NULL;

#ifdef VT_STATS
    toggle_vt_gva_stats(false);
#endif
}

/*
 * see comments in definition of 'frame_info' in pv struct for description
 * of what these routines are doing.
 */
static void save_frame_info( hb_work_private_t * pv, hb_buffer_t * in )
{
    int i = (in->s.start >> FRAME_INFO_MAX2) & FRAME_INFO_MASK;
    pv->frame_info[i].duration = in->s.stop - in->s.start;
}

static int64_t get_frame_duration( hb_work_private_t * pv, int64_t pts )
{
    int i = (pts >> FRAME_INFO_MAX2) & FRAME_INFO_MASK;
    return pv->frame_info[i].duration;
}

static hb_buffer_t * extract_buf(CMSampleBufferRef sampleBuffer, hb_work_object_t * w)
{
    OSStatus err;
    hb_work_private_t * pv = w->private_data;
    hb_job_t * job = pv->job;
    hb_buffer_t *buf = NULL;

    CMItemCount samplesNum = CMSampleBufferGetNumSamples(sampleBuffer);
    if (samplesNum > 1)
    {
        hb_log("VTCompressionSession: more than 1 sample in sampleBuffer = %ld", samplesNum);
    }

    CMBlockBufferRef buffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (buffer)
    {
        size_t sampleSize = CMBlockBufferGetDataLength(buffer);
        buf = hb_buffer_init(sampleSize);

        err = CMBlockBufferCopyDataBytes(buffer, 0, sampleSize, buf->data);

        if (err != kCMBlockBufferNoErr)
        {
            hb_log("VTCompressionSession: CMBlockBufferCopyDataBytes error");
        }

        buf->s.frametype = HB_FRAME_IDR;
        buf->s.flags |= HB_FRAME_REF;

        CFArrayRef attachmentsArray = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, 0);
        if (CFArrayGetCount(attachmentsArray))
        {
            CFDictionaryRef dict = CFArrayGetValueAtIndex(attachmentsArray, 0);
            if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync, NULL))
            {
                CFBooleanRef b;
                if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_PartialSync, NULL))
                {
                    buf->s.frametype = HB_FRAME_I;
                }
                else if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_IsDependedOnByOthers,(const void **) &b))
                {
                    Boolean bv = CFBooleanGetValue(b);
                    if (bv)
                    {
                        buf->s.frametype = HB_FRAME_P;
                    }
                    else
                    {
                        buf->s.frametype = HB_FRAME_B;
                        buf->s.flags &= ~HB_FRAME_REF;
                    }
                }
                else
                {
                    buf->s.frametype = HB_FRAME_P;
                }
            }
        }

        CMTime decodeTimeStamp = CMSampleBufferGetDecodeTimeStamp(sampleBuffer);
        CMTime presentationTimeStamp = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        int64_t duration = get_frame_duration(pv, presentationTimeStamp.value);

        // FIXME: ??
        if (!w->config->h264.init_delay)
        {
            if (pv->init_delay < buf->s.duration)
            {
                pv->init_delay = buf->s.duration;
            }
            w->config->h264.init_delay = pv->init_delay;
        }

        buf->f.width = job->width;
        buf->f.height = job->height;
        buf->s.duration = duration;
        buf->s.start = presentationTimeStamp.value;
        buf->s.stop  = presentationTimeStamp.value + buf->s.duration;
        buf->s.renderOffset = decodeTimeStamp.value - pv->init_delay;

        if (buf->s.frametype == HB_FRAME_IDR)
        {
            /* if we have a chapter marker pending and this
             frame's presentation time stamp is at or after
             the marker's time stamp, use this as the
             chapter start. */
            if (pv->next_chapter_pts != AV_NOPTS_VALUE &&
                pv->next_chapter_pts <= presentationTimeStamp.value)
            {
                // we're no longer looking for this chapter
                pv->next_chapter_pts = AV_NOPTS_VALUE;

                // get the chapter index from the list
                struct chapter_s *item = hb_list_item(pv->delayed_chapters, 0);
                if (item != NULL)
                {
                    // we're done with this chapter
                    buf->s.new_chap = item->index;
                    hb_list_rem(pv->delayed_chapters, item);
                    free(item);

                    // we may still have another pending chapter
                    item = hb_list_item(pv->delayed_chapters, 0);
                    if (item != NULL)
                    {
                        // we're looking for this one now
                        // we still need it, don't remove it
                        pv->next_chapter_pts = item->start;
                    }
                }
            }
        }
    }

    return buf;
}

static hb_buffer_t *vt_encode(hb_work_object_t *w, hb_buffer_t *in)
{
    OSStatus err;
    hb_work_private_t *pv = w->private_data;
    hb_job_t *job = pv->job;

    // Create a CVPixelBuffer to wrap the frame data
    CVPixelBufferRef pxbuffer = NULL;

    void *planeBaseAddress[3] = {in->plane[0].data, in->plane[1].data, in->plane[2].data};
    size_t planeWidth[3] = {in->plane[0].width, in->plane[1].width, in->plane[2].width};
    size_t planeHeight[3] = {in->plane[0].height, in->plane[1].height, in->plane[2].height};
    size_t planeBytesPerRow[3] = {in->plane[0].stride, in->plane[1].stride, in->plane[2].stride};

    err = CVPixelBufferCreateWithPlanarBytes(
                                             kCFAllocatorDefault,
                                             job->width,
                                             job->height,
                                             kCVPixelFormatType_420YpCbCr8Planar,
                                             in->data,
                                             0,
                                             3,
                                             planeBaseAddress,
                                             planeWidth,
                                             planeHeight,
                                             planeBytesPerRow,
                                             &pixelBufferReleasePlanarBytesCallback,
                                             in,
                                             NULL,
                                             &pxbuffer);

    if (kCVReturnSuccess != err)
    {
        hb_log("VTCompressionSession: CVPixelBuffer error");
    }
    else
    {
        hb_vt_add_color_tag(pxbuffer, w->private_data->job);

        CFDictionaryRef frameProperties = NULL;
        if (in->s.new_chap && job->chapter_markers)
        {
            /* chapters have to start with an IDR frame so request that this
             frame be coded as IDR. Since there may be up to 16 frames
             currently buffered in the encoder remember the timestamp so
             when this frame finally pops out of the encoder we'll mark
             its buffer as the start of a chapter. */
            const void *keys[1] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
            const void *values[1] = { kCFBooleanTrue };

            frameProperties = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

            if (pv->next_chapter_pts == AV_NOPTS_VALUE)
            {
                pv->next_chapter_pts = in->s.start;
            }
            /*
             * Chapter markers are sometimes so close we can get a new one before the
             * previous marker has been through the encoding queue.
             *
             * Dropping markers can cause weird side-effects downstream, including but
             * not limited to missing chapters in the output, so we need to save it
             * somehow.
             */
            struct chapter_s *item = malloc(sizeof(struct chapter_s));
            if (item != NULL)
            {
                item->start = in->s.start;
                item->index = in->s.new_chap;
                hb_list_add(pv->delayed_chapters, item);
            }
            /* don't let 'work_loop' put a chapter mark on the wrong buffer */
            in->s.new_chap = 0;

        }

        // Remember info about this frame that we need to pass across
        // the vt_encode call (since it reorders frames).
        save_frame_info(pv, in);

        // Send the frame to be encoded
        err = VTCompressionSessionEncodeFrame(
                                              pv->session,
                                              pxbuffer,
                                              CMTimeMake(in->s.start, 90000),
                                              kCMTimeInvalid,
                                              frameProperties,
                                              pxbuffer,
                                              NULL);
        
        if (err)
        {
            hb_log("VTCompressionSession: VTCompressionSessionEncodeFrame error");
        }
        
        if (frameProperties)
        {
            CFRelease(frameProperties);
        }
    }
    
    // Return a frame if ready
    CMSampleBufferRef sampleBuffer = (CMSampleBufferRef) CMSimpleQueueDequeue(pv->queue);
    hb_buffer_t       *buf_out = NULL;

    if (sampleBuffer)
    {
        buf_out = extract_buf(sampleBuffer, w);
        CFRelease(sampleBuffer);
    }

    return buf_out;
}

int encvt_h264Work(hb_work_object_t * w, hb_buffer_t ** buf_in,
                 hb_buffer_t ** buf_out)
{
    hb_work_private_t * pv = w->private_data;
    hb_buffer_t * in = *buf_in;

    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        // EOF on input. Flush any frames still in the decoder then
        // send the eof downstream to tell the muxer we're done.
        CMSampleBufferRef sampleBuffer = NULL;
        hb_buffer_list_t list;

        hb_buffer_list_clear(&list);
        VTCompressionSessionCompleteFrames(pv->session, kCMTimeInvalid);

        while ((sampleBuffer = (CMSampleBufferRef) CMSimpleQueueDequeue(pv->queue)))
        {
            hb_buffer_t *buf = extract_buf(sampleBuffer, w);
            CFRelease(sampleBuffer);
            
            if (buf)
            {
                hb_buffer_list_append(&list, buf);
            }
            else
            {
                break;
            }
        }

        // add the EOF to the end of the chain
        hb_buffer_list_append(&list, in);

        *buf_out = hb_buffer_list_clear(&list);
        *buf_in = NULL;

        hb_work_private_t *pv = w->private_data;
        hb_job_t *job = pv->job;

        Boolean furtherPassesRequestedOut;
        if (job->pass_id == HB_PASS_ENCODE_1ST)
        {
            OSStatus err = noErr;
            err = VTCompressionSessionEndPass(pv->session,
                                              &furtherPassesRequestedOut,
                                              0);
            if (err != noErr)
            {
                hb_log("VTCompressionSessionEndPass error");
            }

            // Save the sessions and the related context
            // for the next pass.
            vt_interjob_t * context = (vt_interjob_t *)malloc(sizeof(vt_interjob_t));
            context->session = pv->session;
            context->passStorage = pv->passStorage;
            context->queue = pv->queue;
            context->format = pv->format;

            hb_interjob_t * interjob = hb_interjob_get(job->h);
            interjob->vt_context = context;
        }
        else if (job->pass_id == HB_PASS_ENCODE_2ND)
        {
            VTCompressionSessionEndPass(pv->session,
                                        NULL,
                                        0);
        }

        return HB_WORK_DONE;
    }

    // Not EOF - encode the packet
    *buf_out = vt_encode(w, in);
    // Take ownership of the input buffer, avoid a memcpy
    *buf_in = NULL;
    return HB_WORK_OK;
}
