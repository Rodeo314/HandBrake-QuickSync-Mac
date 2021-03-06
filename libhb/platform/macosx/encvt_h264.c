/* encvt_h264.c

   Copyright (c) 2003-2013 HandBrake Team
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

struct hb_work_private_s
{
    hb_job_t    * job;

    VTCompressionSessionRef session;
    CMSimpleQueueRef queue;

    int h264_profile;
    int averageBitRate;
    int expectedFrameRate;
    CFStringRef profileLevel;
    struct
    {
        int maxFrameDelayCount;
        int maxKeyFrameInterval;
        CFBooleanRef allowFrameReordering;
    }
    settings;
    
    int            chap_mark;   // saved chap mark when we're propagating it
    int64_t        next_chap;
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
    { "auto", { CFSTR("H264_Baseline_4_1"), CFSTR("H264_Main_4_1"    ), CFSTR("H264_High_5_0"    ), }, },
    // support all levels returned by hb_h264_levels()
    { "1.0",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "1b",   { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "1.1",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "1.2",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "1.3",  { CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), CFSTR("H264_Baseline_1_3"), }, },
    { "2.0",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_Main_3_0"    ), }, },
    { "2.1",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_Main_3_0"    ), }, },
    { "2.2",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_Main_3_0"    ), }, },
    { "3.0",  { CFSTR("H264_Baseline_3_0"), CFSTR("H264_Main_3_0"    ), CFSTR("H264_Main_3_0"    ), }, },
    { "3.1",  { CFSTR("H264_Baseline_3_1"), CFSTR("H264_Main_3_1"    ), CFSTR("H264_Main_3_1"    ), }, },
    { "3.2",  { CFSTR("H264_Baseline_3_2"), CFSTR("H264_Main_3_2"    ), CFSTR("H264_Main_3_2"    ), }, },
    { "4.0",  { CFSTR("H264_Baseline_4_1"), CFSTR("H264_Main_4_0"    ), CFSTR("H264_Main_4_0"    ), }, },
    { "4.1",  { CFSTR("H264_Baseline_4_1"), CFSTR("H264_Main_4_1"    ), CFSTR("H264_Main_4_1"    ), }, },
    { "4.2",  { CFSTR("H264_Main_5_0"    ), CFSTR("H264_Main_5_0"    ), CFSTR("H264_High_5_0"    ), }, },
    { "5.0",  { CFSTR("H264_Main_5_0"    ), CFSTR("H264_Main_5_0"    ), CFSTR("H264_High_5_0"    ), }, },
    { "5.1",  { CFSTR("H264_Main_5_0"    ), CFSTR("H264_Main_5_0"    ), CFSTR("H264_High_5_0"    ), }, },
    { "5.2",  { CFSTR("H264_Main_5_0"    ), CFSTR("H264_Main_5_0"    ), CFSTR("H264_High_5_0"    ), }, },
    { NULL,   { NULL,                       NULL,                       NULL,                       }, },
};

void pixelBufferReleasePlanarBytesCallback( void *releaseRefCon, const void *dataPtr, size_t dataSize, size_t numberOfPlanes, const void *planeAddresses[] )
{
    hb_buffer_t * buf = (hb_buffer_t *) releaseRefCon;
    hb_buffer_close( &buf );
}

void pixelBufferReleaseBytesCallback( void *releaseRefCon, const void *baseAddress )
{
    free( (void *) baseAddress );
}

void myVTCompressionOutputCallback(
void *outputCallbackRefCon,
void *sourceFrameRefCon,
OSStatus status,
VTEncodeInfoFlags infoFlags,
CMSampleBufferRef sampleBuffer )
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
            hb_log("VTCompressionSession: myVTCompressionOutputCallback queue full");
    }
}

OSStatus initVTSession(hb_work_object_t * w, hb_job_t * job, hb_work_private_t * pv)
{
    OSStatus err = noErr;
    
    CFStringRef key = kVTVideoEncoderSpecification_EncoderID;
    CFStringRef value = CFSTR("com.apple.videotoolbox.videoencoder.h264.gva");
    
    CFStringRef bkey = CFSTR("EnableHardwareAcceleratedVideoEncoder");
    CFBooleanRef bvalue = kCFBooleanTrue;
    
    CFStringRef ckey = CFSTR("RequireHardwareAcceleratedVideoEncoder");
    CFBooleanRef cvalue = kCFBooleanTrue;

    CFMutableDictionaryRef encoderSpecifications = CFDictionaryCreateMutable(
                                                                             kCFAllocatorDefault,
                                                                             3,
                                                                             &kCFTypeDictionaryKeyCallBacks,
                                                                             &kCFTypeDictionaryValueCallBacks);
    
    // Comment out to disable QuickSync
    CFDictionaryAddValue(encoderSpecifications, bkey, bvalue);
    CFDictionaryAddValue(encoderSpecifications, ckey, cvalue);
    CFDictionaryAddValue(encoderSpecifications, key, value);

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
    
    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                             &pv->expectedFrameRate);
    err = VTSessionSetProperty(pv->session,
                               kVTCompressionPropertyKey_ExpectedFrameRate,
                               cfValue);
    CFRelease(cfValue);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_ExpectedFrameRate failed");
    }

    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                             &pv->averageBitRate);
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
                               pv->profileLevel);
    if (err != noErr)
    {
        hb_log("VTSessionSetProperty: kVTCompressionPropertyKey_ProfileLevel failed");
    }

    CFRelease(encoderSpecifications);
    
    return err;

}

OSStatus setupMagicCookie(hb_work_object_t * w, hb_job_t * job, hb_work_private_t * pv)
{
    OSStatus err;
    CMFormatDescriptionRef format = NULL;

    err = initVTSession(w, job, pv);
    if (err != noErr)
        return err;

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
        hb_log("VTCompressionSession: CVPixelBuffer error");

    CMTime pts = CMTimeMake(0, 90000);
    err = VTCompressionSessionEncodeFrame(
                                 		  pv->session,
                                 	      pxbuffer, 
                                 	      pts,
                                          kCMTimeInvalid,
                                          NULL,
                                 	      pxbuffer,
                                 	      NULL);
    VTCompressionSessionCompleteFrames(pv->session, CMTimeMake(0,90000));
                                
    CMSampleBufferRef sampleBuffer = (CMSampleBufferRef) CMSimpleQueueDequeue(pv->queue);

    if (!sampleBuffer)
    {
        hb_log("VTCompressionSession: sampleBuffer == NULL");
        VTCompressionSessionInvalidate(pv->session);
        CFRelease(pv->session);
        return -1;
    }
    else
    {
        format = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (!format)
            hb_log("VTCompressionSession: Format Description error");
        else
        {
            CFDictionaryRef extentions = CMFormatDescriptionGetExtensions(format);
            if (!extentions)
                hb_log("VTCompressionSession: Format Description Extensions error");
            else
            {
                
                CFDictionaryRef atoms = CFDictionaryGetValue(extentions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
                CFDataRef magicCookie = CFDictionaryGetValue(atoms, CFSTR("avcC"));
                if (!magicCookie)
                    hb_log("VTCompressionSession: Magic Cookie error");
                
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
                for (i = 0; i < ppsCount; i++) {
                    uint16_t ppsSize = (avcCAtom[ptrPos++] << 8) & 0xff00;
                    ppsSize += avcCAtom[ptrPos++] & 0xff;
                    memcpy(w->config->h264.pps + ppsPos, avcCAtom+ptrPos, ppsSize);;
                    
                    ptrPos += ppsSize;
                    ppsPos += ppsSize;
                }
                w->config->h264.pps_length = ppsPos;
            }
        }

        CFRelease(sampleBuffer);
    }

    VTCompressionSessionInvalidate(pv->session);
    CFRelease(pv->session);
    
    return err;
}

int encvt_h264Init( hb_work_object_t * w, hb_job_t * job )
{
    OSStatus err;
    hb_work_private_t * pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data = pv;

    pv->job = job;

    // set the profile and level before initializing the session
    if (job->h264_profile != NULL && *job->h264_profile != '\0')
    {
        if (!strcasecmp(job->h264_profile, "baseline"))
        {
            pv->h264_profile = HB_VT_H264_PROFILE_BASELINE;
        }
        else if (!strcasecmp(job->h264_profile, "main") ||
                 !strcasecmp(job->h264_profile, "auto"))
        {
            pv->h264_profile = HB_VT_H264_PROFILE_MAIN;
        }
        else if (!strcasecmp(job->h264_profile, "high"))
        {
            pv->h264_profile = HB_VT_H264_PROFILE_HIGH;
        }
        else
        {
            hb_error("encvt_h264Init: invalid profile '%s'", job->h264_profile);
            *job->die = 1;
            return -1;
        }
    }
    else
    {
        pv->h264_profile = HB_VT_H264_PROFILE_MAIN;
    }
    if (job->h264_level != NULL && *job->h264_level != '\0')
    {
        int i;
        for (i = 0; hb_vt_h264_levels[i].name != NULL; i++)
        {
            if (!strcasecmp(job->h264_level, hb_vt_h264_levels[i].name))
            {
                pv->profileLevel = hb_vt_h264_levels[i].level[pv->h264_profile];
                break;
            }
        }
        if (hb_vt_h264_levels[i].name == NULL)
        {
            hb_error("encvt_h264Init: invalid level '%s'", job->h264_level);
            *job->die = 1;
            return -1;
        }
    }
    else
    {
        pv->profileLevel = hb_vt_h264_levels[0].level[pv->h264_profile];
    }

    /*
     * Set default settings.
     * TODO: implement advanced options parsing.
     *
     * Compute the frame rate, keyframe interval and output bit rate.
     *
     * Note: Quick Sync Video usually works fine with keyframes every 5 seconds.
     */
    pv->expectedFrameRate             = ((double)job->vrate /
                                         (double)job->vrate_base + 0.5);
    pv->settings.maxFrameDelayCount   = 24;
    pv->settings.allowFrameReordering = kCFBooleanTrue;
    pv->settings.maxKeyFrameInterval  = pv->expectedFrameRate * 5;
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
        double properties  = job->height * sqrt(job->width * pv->expectedFrameRate);
        pv->averageBitRate = properties * quality + offset;
    }
    else if (job->vbitrate > 0)
    {
        pv->averageBitRate = job->vbitrate * 1000;
    }
    else
    {
        hb_error("encvt_h264Init: invalid rate control (bitrate %d, quality %f)",
                 job->vbitrate, job->vquality);
    }
    hb_log("encvt_h264Init: encoding with output bitrate %d Kbps",
           pv->averageBitRate / 1000);
    
    CMSimpleQueueCreate(
                        kCFAllocatorDefault,
                        200,
                        &pv->queue);

    err = setupMagicCookie(w, job, pv);
    if (err != noErr)
    {
        hb_log("VTCompressionSession: Magic Cookie Error err=%"PRId64"", (int64_t)err);
        *job->die = 1;
        return -1;
    }

    err = initVTSession(w, job, pv);    
    if (err != noErr)
    {
        hb_log("VTCompressionSession: Error creating a VTCompressionSession err=%"PRId64"", (int64_t)err);
        *job->die = 1;
        return -1;
    }

    return 0;
}

/***********************************************************************
 * Close
 ***********************************************************************
 *
 **********************************************************************/
void encvt_h264Close( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;

    VTCompressionSessionInvalidate(pv->session);
    CFRelease(pv->session);
    CFRelease(pv->queue);

    free( pv );
    w->private_data = NULL;
}

/***********************************************************************
 * Work
 ***********************************************************************
 *
 **********************************************************************/

hb_buffer_t* extractData(CMSampleBufferRef sampleBuffer, hb_work_object_t * w)
{
    OSStatus err;
    hb_work_private_t * pv = w->private_data;
    hb_job_t * job = pv->job;
    hb_buffer_t *buf = NULL;
    
    CMItemCount samplesNum = CMSampleBufferGetNumSamples(sampleBuffer);
    if (samplesNum > 1)
        hb_log("VTCompressionSession: more than 1 sample in sampleBuffer = %ld", samplesNum);

    CMBlockBufferRef buffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (buffer)
    {
        size_t sampleSize = CMBlockBufferGetDataLength(buffer);
        buf = hb_buffer_init( sampleSize );
        
        err = CMBlockBufferCopyDataBytes(buffer, 0, sampleSize, buf->data);

        if (err != kCMBlockBufferNoErr)
            hb_log("VTCompressionSession: CMBlockBufferCopyDataBytes error");

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
                        buf->s.frametype = HB_FRAME_P;
                    else
                    {
                        buf->s.frametype = HB_FRAME_B;
                        buf->s.flags &= ~HB_FRAME_REF;
                    }
                }
                else {
                    buf->s.frametype = HB_FRAME_P;
                }
            }
        }

        CMTime decodeTimeStamp = CMSampleBufferGetDecodeTimeStamp(sampleBuffer);
        CMTime presentationTimeStamp = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);

        if ( !w->config->h264.init_delay && presentationTimeStamp.value )
        {
            w->config->h264.init_delay = presentationTimeStamp.value;
        }

        buf->f.width = job->width;
        buf->f.height = job->height;
        buf->s.start = presentationTimeStamp.value + w->config->h264.init_delay;
        buf->s.stop  = presentationTimeStamp.value + w->config->h264.init_delay;
        buf->s.renderOffset = decodeTimeStamp.value;
        
        /* if we have a chapter marker pending and this
        frame's presentation time stamp is at or after
        the marker's time stamp, use this as the
        chapter start. */
         if( buf->s.frametype == HB_FRAME_IDR && pv->next_chap != 0 && pv->next_chap <= presentationTimeStamp.value )
         {
             pv->next_chap = 0;
             buf->s.new_chap = pv->chap_mark;
         }
    }
    
    return buf;
}

int encvt_h264Work( hb_work_object_t * w, hb_buffer_t ** buf_in,
                 hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t * job = pv->job;
    hb_buffer_t * in = *buf_in, * buf = NULL;
    CMSampleBufferRef sampleBuffer = NULL;
    
    OSStatus err;
    
    if ( in->size <= 0 )
    {
        // EOF on input. Flush any frames still in the decoder then
        // send the eof downstream to tell the muxer we're done.
        VTCompressionSessionCompleteFrames(pv->session, kCMTimeInvalid);
        
        hb_buffer_t *last_buf = NULL;

        while ( ( sampleBuffer = (CMSampleBufferRef) CMSimpleQueueDequeue(pv->queue) ) )
        {
            buf = extractData(sampleBuffer, w);
            CFRelease(sampleBuffer);
            
            if ( buf )
            {
                if ( last_buf == NULL )
                    *buf_out = buf;
                else
                    last_buf->next = buf;
                last_buf = buf;
            }
        }

        // Flushed everything - add the eof to the end of the chain.
        if ( last_buf == NULL )
            *buf_out = in;
        else
            last_buf->next = in;

        *buf_in = NULL;
        return HB_WORK_DONE;
    }
    
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
        hb_log("VTCompressionSession: CVPixelBuffer error");
    
    CFDictionaryRef frameProperties = NULL;
    if( in->s.new_chap && job->chapter_markers )
    {
        /* chapters have to start with an IDR frame so request that this
           frame be coded as IDR. Since there may be up to 16 frames
           currently buffered in the encoder remember the timestamp so
           when this frame finally pops out of the encoder we'll mark
           its buffer as the start of a chapter. */
        const void *keys[1] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
        const void *values[1] = { kCFBooleanTrue };

        frameProperties = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        
        if( pv->next_chap == 0 )
        {
            pv->next_chap = in->s.start;
            pv->chap_mark = in->s.new_chap;
        }
        /* don't let 'work_loop' put a chapter mark on the wrong buffer */
        in->s.new_chap = 0;
    }

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
        hb_log("VTCompressionSession: VTCompressionSessionEncodeFrame error");
    
    if (frameProperties)
        CFRelease(frameProperties);

    // Return a frame if ready
    sampleBuffer = (CMSampleBufferRef) CMSimpleQueueDequeue(pv->queue);

    if (sampleBuffer)
    {
        buf = extractData(sampleBuffer, w);
        CFRelease(sampleBuffer);
    }

    // Take ownership of the input buffer
    // Avoid a memcpy
    *buf_in = NULL;

    *buf_out = buf;

    return HB_WORK_OK;
}

