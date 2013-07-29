//framework
#include <binder/ProcessState.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MPEG4Writer.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
//#include <media/MediaPlayerInterface.h>

//libmix
#include <va/va_tpi.h>
#include <va/va_android.h>
#include <VideoEncoderHost.h>
#include <stdio.h>
#include <getopt.h>
#include <IntelMetadataBuffer.h>

#include <private/gui/ComposerService.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/IGraphicBufferAlloc.h>

#include <ui/PixelFormat.h>
#include <hardware/gralloc.h>
#include <hal/hal_public.h>
#include "loadsurface.h"

#include <binder/IMemory.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <binder/MemoryDealer.h>

using namespace android;

#define ENABLE_LOG   true

#if ENABLE_LOG
//#define LOG(x, ...) printf("%s:"x, __FUNCTION__, ##__VA_ARGS__)
#define LOG  printf
#else
#define LOG(...)
#endif

enum {
    kYUV420SP = 0,
    kYUV420P  = 1,
};

static const int BOX_WIDTH = 64;
static const int PRELOAD_FRAME_NUM = 16;
uint32_t gNumFramesOutput = 0;
static unsigned int pts;


#define CHECK_ENC_STATUS(FUNC)\
    if (ret < ENCODE_SUCCESS) { \
        printf(FUNC" Failed. ret = 0x%08x\n", ret); \
        return UNKNOWN_ERROR; \
    }

#define CHECK_STATUS(err)\
    if (err != OK) { \
        printf("%s:%d: Failed. ret = 0x%08x\n", __FUNCTION__, __LINE__, err); \
        return err; \
    }

class DummySource : public MediaSource {

public:
    DummySource(int width, int height, int stride, int nFrames, int fps,
						bool metadata, const char* yuv)
        : mWidth(width),
          mHeight(height),
          mStride(stride),
          mMaxNumFrames(nFrames),
          mFrameRate(fps),
          mMetadata(metadata),
          mYuvfile(yuv),
          mYuvhandle(NULL){

        if (mMetadata)
            mSize = 128;
        else
            mSize = mStride * mHeight * 3 /2;

        for(int i=0; i<PRELOAD_FRAME_NUM; i++)
            mGroup.add_buffer(new MediaBuffer(mSize));

    }

    virtual sp<MetaData> getFormat() {
        sp<MetaData> meta = new MetaData;
        meta->setInt32(kKeyWidth, mWidth);
        meta->setInt32(kKeyHeight, mHeight);
        meta->setInt32(kKeyStride, mStride);
        meta->setInt32(kKeyColorFormat, OMX_COLOR_FormatYUV420SemiPlanar);
        meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);

        return meta;
    }

    virtual status_t start(MetaData *params) {
//        LOG("begin\n");
        gNumFramesOutput = 0;
        createResource ();

        if (mYuvfile == NULL) {
            //upload src data
            LOG("Fill src picture width=%d, Height=%d\n", mStride, mHeight);
            for(int i=0; i<PRELOAD_FRAME_NUM; i++)
                YUV_generator_planar(mStride, mHeight, mUsrptr[i], mStride, mUsrptr[i] + mStride*mHeight, mStride, 0, 0, 1);
        }else{
            mYuvhandle = fopen(mYuvfile, "rb");
            if (mYuvhandle == NULL)
                return errno;
        }
   //     LOG("end ...\n");
        return OK;
    }

    virtual status_t stop() {
        gNumFramesOutput = 0;
        return OK;
    }

    virtual status_t read(MediaBuffer **buffer, const MediaSource::ReadOptions *options) {

        if (gNumFramesOutput == mMaxNumFrames) {
            return ERROR_END_OF_STREAM;
        }

        status_t err = mGroup.acquire_buffer(buffer);
        if (err != OK) {
            return err;
        }

        if (mYuvhandle) {
            //load from yuv file
            size_t readsize = mStride*mHeight*3/2;
            size_t ret = 0;

            while (readsize > 0) {
                ret = fread(mUsrptr[gNumFramesOutput % PRELOAD_FRAME_NUM] + mStride*mHeight*3/2 - readsize,  1, readsize, mYuvhandle);

                if (ret <= 0) {
                    (*buffer)->release();
                    if (feof(mYuvhandle)) {
                        printf("Read from YUV file EOS");
                        return ERROR_END_OF_STREAM;
                    }else
                        return ferror(mYuvhandle);
                }
                readsize -= ret;

               // LOG("loading from file, ret=%d, readsize=%d\n", ret, readsize);
            }
        }

        uint8_t * data;
        uint32_t size;

        if (mMetadata) {
            mIMB[gNumFramesOutput % PRELOAD_FRAME_NUM]->Serialize(data, size);
            //LOG("use metadata mode\n");
        }else {
            data = mUsrptr[gNumFramesOutput % PRELOAD_FRAME_NUM];
            size = mSize;
        }

        memcpy ((*buffer)->data(), data, size);
        (*buffer)->set_range(0, size);
        (*buffer)->meta_data()->clear();
        (*buffer)->meta_data()->setInt64(
                kKeyTime, (gNumFramesOutput * 1000000) / mFrameRate);

        ++gNumFramesOutput;
        if (gNumFramesOutput % 10 ==0)
            fprintf(stderr, ".");
        return OK;
    }

    virtual status_t createResource() {
        return OK;
    }

protected:
    virtual ~DummySource() {
        for(int i = 0; i < PRELOAD_FRAME_NUM; i ++)
            delete mIMB[i];
    }

private:

    int YUV_generator_planar(int width, int height,
                      unsigned char *Y_start, int Y_pitch,
                      unsigned char *U_start, int U_pitch,
                      unsigned char *V_start, int V_pitch,
                      int UV_interleave)
    {
        static int row_shift = 0;
        int row;

        /* copy Y plane */
        for (row=0;row<height;row++) {
            unsigned char *Y_row = Y_start + row * Y_pitch;
            int jj, xpos, ypos;

            ypos = (row / BOX_WIDTH) & 0x1;

            for (jj=0; jj<width; jj++) {
                xpos = ((row_shift + jj) / BOX_WIDTH) & 0x1;

                if ((xpos == 0) && (ypos == 0))
                    Y_row[jj] = 0xeb;
                if ((xpos == 1) && (ypos == 1))
                    Y_row[jj] = 0xeb;

                if ((xpos == 1) && (ypos == 0))
                    Y_row[jj] = 0x10;
                if ((xpos == 0) && (ypos == 1))
                    Y_row[jj] = 0x10;
            }
        }

        /* copy UV data */
        for( row =0; row < height/2; row++) {
            if (UV_interleave) {
                unsigned char *UV_row = U_start + row * U_pitch;
                memset (UV_row,0x80,width);
            } else {
                unsigned char *U_row = U_start + row * U_pitch;
                unsigned char *V_row = V_start + row * V_pitch;

                memset (U_row,0x80,width/2);
                memset (V_row,0x80,width/2);
            }
        }

        row_shift += 8;
//        if (row_shift==BOX_WIDTH) row_shift = 0;

        YUV_blend_with_pic(width,height,
                           Y_start, Y_pitch,
                           U_start, U_pitch,
                           V_start, V_pitch,
                           1, 70);

        return 0;
    }

protected:

    bool mMetadata;
    const char* mYuvfile;
    FILE* mYuvhandle;
    MediaBufferGroup mGroup;
    int mWidth, mHeight, mStride;
    int mMaxNumFrames;
    int mFrameRate;
    int mColorFormat;
    size_t mSize;
//    int64_t mNumFramesOutput;

    DummySource(const DummySource &);
    DummySource &operator=(const DummySource &);

    //for uploading src pictures, also for Camera malloc, WiDi clone, raw mode usrptr storage
    uint8_t* mUsrptr[PRELOAD_FRAME_NUM];

    //for Metadatabuffer transfer
    IntelMetadataBuffer* mIMB[PRELOAD_FRAME_NUM];

};

class MallocSource : public DummySource {
public:

    MallocSource(int width, int height, int stride, int nFrames, int fps, bool mdata, const char* yuv) :
			DummySource (width, height, stride, nFrames, fps, mdata, yuv) {
    }

    ~MallocSource() {
        for(int i = 0; i < PRELOAD_FRAME_NUM; i ++)
            delete mMallocPtr[i];
    }

    //malloc external memory, and not need to set into encoder before start()
    status_t createResource()
    {
        uint32_t size = mStride * mHeight * 3 /2;

        ValueInfo vinfo;
        vinfo.mode = MEM_MODE_MALLOC;
        vinfo.handle = 0;
        vinfo.size = size;
        vinfo.width = mWidth;
        vinfo.height = mHeight;
        vinfo.lumaStride = mStride;
        vinfo.chromStride = mStride;
        vinfo.format = STRING_TO_FOURCC("NV12");
        vinfo.s3dformat = 0xFFFFFFFF;

        for(int i = 0; i < PRELOAD_FRAME_NUM; i ++)
        {
            mMallocPtr[i] = (uint8_t*) malloc(size + 4095);

            //keep address 4K aligned
            mUsrptr[i] = (uint8_t*)((((uint32_t )mMallocPtr[i] + 4095) / 4096 ) * 4096);

            mIMB[i] = new IntelMetadataBuffer(MetadataBufferTypeCameraSource, (int32_t) mUsrptr[i]);
            mIMB[i]->SetValueInfo(&vinfo);
//            LOG("Malloc address=%x\n", mUsrptr[i]);
        }

        return OK;
    }

private:
    uint8_t* mMallocPtr[PRELOAD_FRAME_NUM];

};


class MemHeapSource : public DummySource {
public:

    MemHeapSource(int width, int height, int stride, int nFrames, int fps, bool mdata, const char* yuv) :
			DummySource (width, height, stride, nFrames, fps, mdata, yuv) {
    }

    ~MemHeapSource() {
//        for(int i = 0; i < PRELOAD_FRAME_NUM; i ++)
  //          delete mMallocPtr[i];
    }

    //malloc external memory, and not need to set into encoder before start()
    status_t createResource()
    {
        uint32_t size = mStride * mHeight * 3 /2;
        size += 0x0FFF;

        ValueInfo vinfo;
        vinfo.mode = MEM_MODE_MALLOC;
        vinfo.handle = 0;
        vinfo.size = size;
        vinfo.width = mWidth;
        vinfo.height = mHeight;
        vinfo.lumaStride = mStride;
        vinfo.chromStride = mStride;
        vinfo.format = STRING_TO_FOURCC("NV12");
        vinfo.s3dformat = 0xFFFFFFFF;

        mHeap = new MemoryHeapBase(PRELOAD_FRAME_NUM * size);

        for(int i = 0; i < PRELOAD_FRAME_NUM; i ++)
        {
            mBuffers[i] = new MemoryBase(mHeap, i * size, size);

            mUsrptr[i] = (uint8_t*) ((int) (mBuffers[i]->pointer() + 0x0FFF) & ~0x0FFF);

            mIMB[i] = new IntelMetadataBuffer(MetadataBufferTypeCameraSource, (int32_t) mUsrptr[i]);
            mIMB[i]->SetValueInfo(&vinfo);
            LOG("MemHeap address=%x\n", mUsrptr[i]);
        }

        return OK;
    }

private:
    sp<MemoryHeapBase> mHeap;
    sp<MemoryBase> mBuffers[PRELOAD_FRAME_NUM];

};

extern "C" {
    VAStatus vaLockSurface(VADisplay dpy,
        VASurfaceID surface,
        unsigned int *fourcc,
        unsigned int *luma_stride,
        unsigned int *chroma_u_stride,
        unsigned int *chroma_v_stride,
        unsigned int *luma_offset,
        unsigned int *chroma_u_offset,
        unsigned int *chroma_v_offset,
        unsigned int *buffer_name,
        void **buffer
    );

    VAStatus vaUnlockSurface(VADisplay dpy,
        VASurfaceID surface
    );
}

class VASurfaceSource : public DummySource {
public:
    VASurfaceSource(int width, int height, int stride, int nFrames, int fps, bool mdata, const char* yuv, int mode) :
			DummySource (width, height, stride, nFrames, fps, mdata, yuv) {
        mMode = mode;
    }

    virtual ~VASurfaceSource() {
        vaDestroySurfaces(mVADisplay, mSurfaces, PRELOAD_FRAME_NUM);
    }

    status_t createResource()
    {
        unsigned int display = 0;
        int majorVersion = -1;
        int minorVersion = -1;
        VAStatus vaStatus;

        mVADisplay = vaGetDisplay(&display);

        if (mVADisplay == NULL) {
            LOG("vaGetDisplay failed.");
        }

        vaStatus = vaInitialize(mVADisplay, &majorVersion, &minorVersion);
        if (vaStatus != VA_STATUS_SUCCESS) {
            LOG( "Failed vaInitialize, vaStatus = %d\n", vaStatus);
        }

        VASurfaceAttributeTPI attribute_tpi;

        attribute_tpi.size = mWidth * mHeight * 3 /2;
        attribute_tpi.luma_stride = mWidth;
        attribute_tpi.chroma_u_stride = mWidth;
        attribute_tpi.chroma_v_stride = mWidth;
        attribute_tpi.luma_offset = 0;
        attribute_tpi.chroma_u_offset = mWidth * mHeight;
        attribute_tpi.chroma_v_offset = mWidth * mHeight;
        attribute_tpi.pixel_format = VA_FOURCC_NV12;
        attribute_tpi.type = VAExternalMemoryNULL;

        vaStatus = vaCreateSurfacesWithAttribute(mVADisplay, mWidth, mHeight, VA_RT_FORMAT_YUV420,
                PRELOAD_FRAME_NUM, mSurfaces, &attribute_tpi);

        if (vaStatus != VA_STATUS_SUCCESS) {
            LOG( "Failed vaCreateSurfaces, vaStatus = %d\n", vaStatus);
        }

        if (mMode == 1){
            uint32_t fourCC = 0;
            uint32_t lumaStride = 0;
            uint32_t chromaUStride = 0;
            uint32_t chromaVStride = 0;
            uint32_t lumaOffset = 0;
            uint32_t chromaUOffset = 0;
            uint32_t chromaVOffset = 0;

            for(int i = 0; i < PRELOAD_FRAME_NUM; i++) {
                vaStatus = vaLockSurface(
                        mVADisplay, (VASurfaceID)mSurfaces[i],
                        &fourCC, &lumaStride, &chromaUStride, &chromaVStride,
                        &lumaOffset, &chromaUOffset, &chromaVOffset, &mKBufHandle[i], NULL);
                if (vaStatus != VA_STATUS_SUCCESS) {
                     LOG( "Failed vaLockSurface, vaStatus = %d\n", vaStatus);
                }
#if 0
                LOG("lumaStride = %d", lumaStride);
                LOG("chromaUStride = %d\n", chromaUStride);
                LOG("chromaVStride = %d\n", chromaVStride);
                LOG("lumaOffset = %d\n", lumaOffset);
                LOG("chromaUOffset = %d\n", chromaUOffset);
                LOG("chromaVOffset = %d\n", chromaVOffset);
                LOG("kBufHandle = 0x%08x\n", mKBufHandle[i]);
                LOG("fourCC = %d\n", fourCC);
#endif
                vaStatus = vaUnlockSurface(mVADisplay, (VASurfaceID)mSurfaces[i]);

            }
        }

        //get usrptr for uploading src pictures
        VAImage surface_image;
        ValueInfo vinfo;
        memset(&vinfo, 0, sizeof(ValueInfo));
        vinfo.mode = MEM_MODE_SURFACE;
        vinfo.handle = (uint32_t) mVADisplay;
        vinfo.size = 0;
        vinfo.width = mWidth;
        vinfo.height = mHeight;
        vinfo.lumaStride = mStride;
        vinfo.chromStride = mStride;
        vinfo.format = STRING_TO_FOURCC("NV12");
        vinfo.s3dformat = 0xFFFFFFFF;

        for (int i = 0; i < PRELOAD_FRAME_NUM; i++) {
            vaStatus = vaDeriveImage(mVADisplay, mSurfaces[i], &surface_image);
            if (vaStatus != VA_STATUS_SUCCESS) {
                LOG("Failed vaDeriveImage, vaStatus = %d\n", vaStatus);
            }

            vaMapBuffer(mVADisplay, surface_image.buf, (void**)&mUsrptr[i]);
            if (vaStatus != VA_STATUS_SUCCESS) {
                LOG("Failed vaMapBuffer, vaStatus = %d\n", vaStatus);
            }

            vaUnmapBuffer(mVADisplay, surface_image.buf);
            vaDestroyImage(mVADisplay, surface_image.image_id);



            if (mMode == 0)
                mIMB[i] = new IntelMetadataBuffer(MetadataBufferTypeUser, mSurfaces[i]);
            else {
                mIMB[i] = new IntelMetadataBuffer(MetadataBufferTypeUser, mKBufHandle[i]);
                vinfo.mode = MEM_MODE_KBUFHANDLE;
                vinfo.handle = 0;
            }

            mIMB[i]->SetValueInfo(&vinfo);
        }

        return OK;
    }

private:
    //for WiDi user mode
    VADisplay mVADisplay;
    VASurfaceID mSurfaces[PRELOAD_FRAME_NUM];

    //for WiDi ext mode
    uint32_t mKBufHandle[PRELOAD_FRAME_NUM];
    int mMode;
};


class GfxSource : public DummySource {

public:
    GfxSource(int width, int height, int stride, int nFrames, int fps, bool mdata, const char* yuv) :
			DummySource (width, height, stride, nFrames, fps, mdata, yuv) {
        mColor = 0;
        mWidth = ((mWidth + 15 ) / 16 ) * 16;
        mHeight = ((mHeight + 15 ) / 16 ) * 16;
        mStride = mWidth;
    }

    virtual ~GfxSource() {
  //      for(int i=0; i<PRELOAD_FRAME_NUM; i++)
      //      delete mGraphicBuffer[i];
    }

    status_t createResource()
    {
        sp<ISurfaceComposer> composer(ComposerService::getComposerService());
        mGraphicBufferAlloc = composer->createGraphicBufferAlloc();

        uint32_t usage = GraphicBuffer::USAGE_HW_TEXTURE;// | GraphicBuffer::USAGE_HW_COMPOSER;
        int format = HAL_PIXEL_FORMAT_NV12;
        if (mColor == 1)
            format = 0x7FA00E00; // = OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar in OMX_IVCommon.h

        int32_t error;

        for(int i = 0; i < PRELOAD_FRAME_NUM; i ++)
        {
            sp<GraphicBuffer> graphicBuffer(
                    mGraphicBufferAlloc->createGraphicBuffer(
                                    mWidth, mHeight, format, usage, &error));

            if (graphicBuffer.get() == NULL) {
                printf("GFX createGraphicBuffer failed\n");
                return UNKNOWN_ERROR;
            }
            mGraphicBuffer[i] = graphicBuffer;
            graphicBuffer->lock(usage | GraphicBuffer::USAGE_SW_WRITE_OFTEN |GraphicBuffer::USAGE_SW_READ_OFTEN, (void**)(&mUsrptr[i]));

            mIMB[i] = new IntelMetadataBuffer(MetadataBufferTypeGrallocSource, (int32_t)mGraphicBuffer[i]->handle);
            graphicBuffer->unlock();

            IMG_native_handle_t* h = (IMG_native_handle_t*) mGraphicBuffer[i]->handle;
            mStride = h->iWidth;
            mHeight = h->iHeight;

//printf("mStride=%d, height=%d, format=%x", mStride, mHeight, h->iFormat);
        }

        return OK;
    }

private:
    //for gfxhandle
    sp<IGraphicBufferAlloc> mGraphicBufferAlloc;
    sp<GraphicBuffer> mGraphicBuffer[PRELOAD_FRAME_NUM];

    int mColor;
};

class GrallocSource : public DummySource {

public:
    GrallocSource(int width, int height, int stride, int nFrames, int fps, bool mdata, const char* yuv) :
			DummySource (width, height, stride, nFrames, fps, mdata, yuv) {
    }

    virtual ~GrallocSource () {
        for(int i = 0; i < PRELOAD_FRAME_NUM; i ++)
            gfx_free(mHandle[i]);
    }

    status_t createResource()
    {
        int usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;
        int format = HAL_PIXEL_FORMAT_NV12;
        if (mColor == 1)
            format = 0x7FA00E00; // = OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar in OMX_IVCommon.h

        gfx_init();

        for(int i = 0; i < PRELOAD_FRAME_NUM; i ++)
        {
            if (gfx_alloc(mWidth, mHeight, format, usage, &mHandle[i], (int32_t*)&mStride) != 0)
                return UNKNOWN_ERROR;
            if (gfx_lock(mHandle[i], usage | GRALLOC_USAGE_SW_WRITE_OFTEN, 0, 0, mWidth, mHeight, (void**)(&mUsrptr[i])) != 0)
                return UNKNOWN_ERROR;
            mIMB[i] = new IntelMetadataBuffer(MetadataBufferTypeGrallocSource, (int32_t)mHandle[i]);
            gfx_unlock(mHandle[i]);
            IMG_native_handle_t* h = (IMG_native_handle_t*) mHandle[i];
            mHeight = h->iHeight;
        }

        return OK;
    }

private:
    void gfx_init()
    {
        int err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &mModule);
        if (err) {
            LOG("FATAL: can't find the %s module", GRALLOC_HARDWARE_MODULE_ID);
            return;
        }

        mAllocMod = (gralloc_module_t const *)mModule;

        err = gralloc_open(mModule, &mAllocDev);
        if (err) {
            LOG("FATAL: gralloc open failed\n");
        }

    }

    int gfx_alloc(uint32_t w, uint32_t h, int format,
              int usage, buffer_handle_t* handle, int32_t* stride)
    {
        int err;

        err = mAllocDev->alloc(mAllocDev, w, h, format, usage, handle, stride);
        if (err) {
            LOG("alloc(%u, %u, %d, %08x, ...) failed %d (%s)\n",
                   w, h, format, usage, err, strerror(-err));
        }

        return err;
    }

    int gfx_free(buffer_handle_t handle)
    {
        int err;

        err = mAllocDev->free(mAllocDev, handle);
        if (err) {
            LOG("free(...) failed %d (%s)\n", err, strerror(-err));
        }

        return err;
    }

    int gfx_lock(buffer_handle_t handle,
                int usage, int left, int top, int width, int height,
                void** vaddr)
    {
        int err;

        err = mAllocMod->lock(mAllocMod, handle, usage,
                              left, top, width, height, vaddr);

        if (err){
            LOG("lock(...) failed %d (%s)", err, strerror(-err));
        }

        return err;
    }


    int gfx_unlock(buffer_handle_t handle)
    {
        int err;

        err = mAllocMod->unlock(mAllocMod, handle);
        if (err) {
            LOG("unlock(...) failed %d (%s)\n", err, strerror(-err));
        }

        return err;
    }

private:
    hw_module_t const *mModule;
    gralloc_module_t const *mAllocMod; /* get by force hw_module_t */
    alloc_device_t  *mAllocDev; /* get by gralloc_open */

    buffer_handle_t mHandle[PRELOAD_FRAME_NUM];
    int mColor;
};

static const char *AVC_MIME_TYPE = "video/h264";
static const char *MPEG4_MIME_TYPE = "video/mpeg4";
static const char *H263_MIME_TYPE = "video/h263";
static const char *VP8_MIME_TYPE = "video/x-webm";


class MixEncoder : public MediaSource {

public:
    MixEncoder(const sp<MediaSource> &source, const sp<MetaData> &meta, int rcmode, uint32_t flag) {
        mFirstFrame = false;
        mSrcEOS = false;
        mEncoderFlag = flag;
        mEncodeFrameCount = 0;
        mSource = source;

        const char *mime;
        bool success = meta->findCString(kKeyMIMEType, &mime);
        CHECK(success);

        mCodec = mime;
        if (strcmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG4) == 0) {
            mMixCodec = (char*) MPEG4_MIME_TYPE;
        } else if (strcmp(mime, MEDIA_MIMETYPE_VIDEO_H263) == 0) {
            mMixCodec = (char*) H263_MIME_TYPE;
        } else if (strcmp(mime, MEDIA_MIMETYPE_VIDEO_VPX) == 0) {
            mMixCodec = (char*) VP8_MIME_TYPE;
        } else {
            mMixCodec = (char*) AVC_MIME_TYPE;
        }

        success = meta->findInt32(kKeyWidth, &mWidth);
        CHECK(success);

        success = meta->findInt32(kKeyHeight, &mHeight);
        CHECK(success);

        success = meta->findInt32(kKeyFrameRate, &mFPS);
        CHECK(success);

        success = meta->findInt32(kKeyBitRate, &mBitrate);
        CHECK(success);

        success = meta->findInt32('itqp', &mInitQP);
        CHECK(success);

        success = meta->findInt32('mnqp', &mMinQP);
        CHECK(success);

        success = meta->findInt32('iapd', &mIntraPeriod);
        CHECK(success);

        success = meta->findInt32('wsiz', &mWinSize);
        CHECK(success);

        success = meta->findInt32('idri', &mIdrInt);
        CHECK(success);

        success = meta->findInt32('difs', &mDisableFrameSkip);
        CHECK(success);

//    const char *RCMODE[] = {"VBR", "CBR", "VCM", "NO_RC", NULL};
        VideoRateControl RC_MODES[] = {RATE_CONTROL_VBR,
        							RATE_CONTROL_CBR,
        							RATE_CONTROL_VCM,
        							RATE_CONTROL_NONE};

        mRCMode = RC_MODES[rcmode];
    }

    virtual sp<MetaData> getFormat() {
        sp<MetaData> meta = new MetaData;
        meta->setInt32(kKeyWidth, mWidth);
        meta->setInt32(kKeyHeight, mHeight);
//        meta->setInt32(kKeyColorFormat, mColorFormat);
        meta->setCString(kKeyMIMEType, mCodec);
        return meta;
    }

    virtual status_t start(MetaData *params) {
        Encode_Status ret;
        status_t err;

        //create video encoder
        mVideoEncoder = createVideoEncoder(mMixCodec);
        if (!mVideoEncoder) {
            printf("MIX::createVideoEncoder failed\n");
            return UNKNOWN_ERROR;
        }

        //set parameter
        err = SetVideoEncoderParam();
        CHECK_STATUS(err);

        //start
        ret = mVideoEncoder->start();
        CHECK_ENC_STATUS("MIX::Start");

        uint32_t maxsize;
        mVideoEncoder->getMaxOutSize(&maxsize);
        mGroup.add_buffer(new MediaBuffer(maxsize));
        mGroup.add_buffer(new MediaBuffer(maxsize));
        mGroup.add_buffer(new MediaBuffer(maxsize));

        return mSource->start();
    }

    virtual status_t stop() {
        Encode_Status ret;

        ret = mVideoEncoder->stop();
        CHECK_ENC_STATUS("MIX::stop");

        return OK;
    }

    status_t encode(MediaBuffer* in) {
        status_t  err = OK;
        Encode_Status ret;

        VideoEncRawBuffer InBuf;
        InBuf.data = (uint8_t *) in->data() + in->range_offset();
        InBuf.size = in->range_length();
        InBuf.bufAvailable = true;
        InBuf.type = FTYPE_UNKNOWN;
        InBuf.flag = 0;
        in->meta_data()->findInt64(kKeyTime, &InBuf.timeStamp);
        InBuf.priv = (void*)in;

#if 0
        if (mEncodeFrameCount > 1 && mEncodeFrameCount % 60 == 0){
            VideoParamConfigSet configIDRRequest;
            configIDRRequest.type = VideoConfigTypeIDRRequest;
            mVideoEncoder->setConfig(&configIDRRequest);
            printf("MIX::encode request IDR\n");
        }
#endif
        ret = mVideoEncoder->encode(&InBuf);
        if (ret < ENCODE_SUCCESS) {
            printf("MIX::encode failed, ret=%d\n", ret);
            in->release();
            return UNKNOWN_ERROR;
        }

        mEncodeFrameCount ++;
        return err;
    }

    status_t getoutput(MediaBuffer* out, VideoOutputFormat format) {
        Encode_Status ret;

        VideoEncOutputBuffer OutBuf;
        OutBuf.bufferSize = out->size() ;
        OutBuf.dataSize = 0;
        OutBuf.data = (uint8_t *) out->data() + out->range_offset();
        OutBuf.format = format;
        OutBuf.flag = 0;
        OutBuf.timeStamp = 0;

        ret = mVideoEncoder->getOutput(&OutBuf);
        if (ret < ENCODE_SUCCESS) {
            if ((ret == ENCODE_NO_REQUEST_DATA) && (strcmp(mMixCodec, H263_MIME_TYPE) == 0)) {
                printf("H263 FrameSkip happens at Frame #%d\n", mEncodeFrameCount);
                OutBuf.dataSize = 0;
            } else {
                printf("MIX::getOutput failed, ret=%d\n", ret);
                out->release();
                return UNKNOWN_ERROR;
            }
        }

        out->set_range(0, OutBuf.dataSize);
        out->meta_data()->clear();
        out->meta_data()->setInt64(kKeyTime, OutBuf.timeStamp);
        out->meta_data()->setInt64(kKeyDecodingTime, OutBuf.timeStamp);

        bool isSync = (OutBuf.flag & ENCODE_BUFFERFLAG_SYNCFRAME);
        bool isCsd = (OutBuf.flag & ENCODE_BUFFERFLAG_CODECCONFIG);
        out->meta_data()->setInt32(kKeyIsSyncFrame, isSync);
        out->meta_data()->setInt32(kKeyIsCodecConfig, isCsd);

        //for h263 frame skip case, ENCODE_NO_REQUEST_DATA is returned, but priv is not set.
        //to handle properly, need to change libmix to set priv in outbuf even in wrong case
        if (format != OUTPUT_CODEC_DATA && (ret != ENCODE_NO_REQUEST_DATA)) {
            MediaBuffer* in = (MediaBuffer*) OutBuf.priv;
            in->release();
        }

        return OK;
    }

    virtual status_t read(MediaBuffer **buffer, const MediaSource::ReadOptions *options) {

        status_t  err;
        Encode_Status ret;

        if (mSrcEOS)
            return ERROR_END_OF_STREAM;

        //write rest data of first frame after outputting csd, only for H264/MPEG4
        if (mFirstFrame) {
            err = mGroup.acquire_buffer(buffer);
            CHECK_STATUS(err);

            err = getoutput(*buffer, OUTPUT_EVERYTHING);
            CHECK_STATUS(err);

            mFirstFrame = false;
            return OK;
        }

        //input buffers
        int loop=1;
        if (mEncodeFrameCount == 0)
            loop = 2;

        for(int i=0; i<loop; i++) {
            MediaBuffer *src;

            err = mSource->read (&src);
            if (err == ERROR_END_OF_STREAM) {
                LOG ("\nReach Resource EOS, still need to get final frame encoded data\n");
                mSrcEOS = true;
            }else {
                CHECK_STATUS(err);

                err = encode(src);
                CHECK_STATUS(err);
            }
        }

        //output buffers
        err = mGroup.acquire_buffer(buffer);
        CHECK_STATUS(err);

        VideoOutputFormat format;
        if ((mEncodeFrameCount == 2 && (strcasecmp(mMixCodec, H263_MIME_TYPE) != 0))&&
			(mEncodeFrameCount == 2 && (strcasecmp(mMixCodec, VP8_MIME_TYPE) != 0))){
            format = OUTPUT_CODEC_DATA;
            mFirstFrame = true;
		}else
            format = OUTPUT_EVERYTHING;

        err = getoutput(*buffer, format);
        CHECK_STATUS(err);

        return OK;
    }

    virtual ~MixEncoder() {
        releaseVideoEncoder(mVideoEncoder);
    }

private:

    MixEncoder(const MixEncoder &);
    MixEncoder &operator=(const MixEncoder &);

    status_t SetVideoEncoderParam() {

        Encode_Status ret = ENCODE_SUCCESS;

        ret = mVideoEncoder->getParameters(&mEncoderParams);
        CHECK_ENC_STATUS("MIX::getParameters");

        LOG("Set Encoding Width=%d, Height=%d\n", mWidth, mHeight);
        mEncoderParams.resolution.height = mHeight;
        mEncoderParams.resolution.width = mWidth;
        mEncoderParams.frameRate.frameRateDenom = 1;
        mEncoderParams.frameRate.frameRateNum = mFPS;
        mEncoderParams.rcMode = mRCMode;

        if (strcmp(mMixCodec, MPEG4_MIME_TYPE) == 0) {
            mEncoderParams.profile = (VAProfile)VAProfileMPEG4Simple;
        } else if (strcmp(mMixCodec, H263_MIME_TYPE) == 0) {
            mEncoderParams.profile = (VAProfile)VAProfileH263Baseline;
        } else if (strcmp(mMixCodec, VP8_MIME_TYPE) == 0) {
            mEncoderParams.profile = (VAProfile)VAProfileVP8Version0_3;
		} else {
            mEncoderParams.profile = (VAProfile)VAProfileH264Baseline;
        }

        mEncoderParams.rcParams.bitRate = mBitrate;
        mEncoderParams.rcParams.initQP = mInitQP;
        mEncoderParams.rcParams.minQP = mMinQP;
        mEncoderParams.rcParams.windowSize = mWinSize;
        mEncoderParams.intraPeriod = mIntraPeriod;

        ret = mVideoEncoder->setParameters(&mEncoderParams);
        CHECK_ENC_STATUS("MIX::setParameters VideoParamsCommon");

        mStoreMetaDataInBuffers.isEnabled = (mEncoderFlag & OMXCodec::kStoreMetaDataInVideoBuffers);

        ret = mVideoEncoder->setParameters(&mStoreMetaDataInBuffers);
        CHECK_ENC_STATUS("MIX::setParameters StoreMetaDataInBuffers");

        if (strcmp(mMixCodec, AVC_MIME_TYPE) == 0) {
            VideoParamsAVC AVCParam;
            mVideoEncoder->getParameters(&AVCParam);
            AVCParam.idrInterval = mIdrInt;
            mVideoEncoder->setParameters(&AVCParam);
        }

#if 1
        VideoConfigBitRate configBitrate;
        mVideoEncoder->getConfig(&configBitrate);
        configBitrate.rcParams.disableBitsStuffing = 0;
        configBitrate.rcParams.disableFrameSkip = mDisableFrameSkip;
        mVideoEncoder->setConfig(&configBitrate);
#endif
        return OK;
    }

private:

    const char* mMixCodec;
    const char* mCodec;
    int mBitrate;
    int mWidth;
    int mHeight;
    int mFPS;
    int mIntraPeriod;
    int mEncodeFrameCount;
    uint32_t mEncoderFlag;
    int mInitQP;
    int mMinQP;
    int mWinSize;
    int mIdrInt;
    int mDisableFrameSkip;

//    int mSyncMode;
    bool mFirstFrame;
    bool mSrcEOS;

    IVideoEncoder *mVideoEncoder;
    VideoParamsCommon mEncoderParams;
    VideoParamsAVC mVideoParamsAVC;
    VideoParamsStoreMetaDataInBuffers mStoreMetaDataInBuffers;
    VideoRateControl mRCMode;

    sp<MediaSource> mSource;
    MediaBufferGroup mGroup;

};

class IVFWriter : public MediaWriter {

public:
    const char* mFile;
    FILE* mFilehandle;
    sp<MediaSource> mSource;
    pthread_t mThread;
    bool mRunning;
    bool mEOS;
	char vp8_file_header[32];
	
public:
    IVFWriter(char* file) {
        mFile = file;
        mRunning = false;
    }

    status_t addSource(const sp<MediaSource> &source) {
        mSource = source;
        return OK;
    }

    bool reachedEOS() {
        return mEOS;
    }

    status_t start(MetaData *params = NULL) {

        mSource->start();

        mRunning = true;
        mEOS = false;

        mFilehandle = fopen(mFile, "w+");
        if (mFilehandle == NULL)
            return errno;

		write_ivf_file_header(params);

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        pthread_create(&mThread, &attr, IVFWriter::ThreadFunc, this);
        pthread_attr_destroy(&attr);

        return OK;
    }

    status_t stop() {
        mRunning = false;
        void *dummy;
        pthread_join(mThread, &dummy);
        fclose(mFilehandle);
        return OK;
    }

    status_t pause() {
        return OK;
    }

	
	static void mem_put_le16(char *mem, unsigned int val)
	{
		mem[0] = val;
		mem[1] = val>>8;
	}
	
	static void mem_put_le32(char *mem, unsigned int val)
	{
		mem[0] = val;
		mem[1] = val>>8;
		mem[2] = val>>16;
		mem[3] = val>>24;
	}

	void write_ivf_file_header(MetaData *params)	{

    int width,height,framerate; 
	params->findInt32(kKeyWidth, &width);
    params->findInt32(kKeyHeight, &height);
    params->findInt32(kKeyFrameRate, &framerate);
     /* write ivf header */
    vp8_file_header[0] = 'D';
    vp8_file_header[1] = 'K';
    vp8_file_header[2] = 'I';
    vp8_file_header[3] = 'F';
    mem_put_le16(vp8_file_header+4,  0);                     /* version */
    mem_put_le16(vp8_file_header+6,  32);                    /* headersize */
    mem_put_le32(vp8_file_header+8,  0x30385056);            /* headersize */
    mem_put_le16(vp8_file_header+12, width);           /* width */
    mem_put_le16(vp8_file_header+14, height);          /* height */
    mem_put_le32(vp8_file_header+16, framerate);       /* rate default at 30 */
    mem_put_le32(vp8_file_header+20, 1);                     /* scale */
    mem_put_le32(vp8_file_header+24, 50);           /* length just hardcode to 50*/
    mem_put_le32(vp8_file_header+28, 0);                     /* unused */
    fwrite(vp8_file_header, 1, 32, mFilehandle);

	}

private:

    static void *ThreadFunc(void *me) {
        IVFWriter *writer = static_cast<IVFWriter *>(me);

        status_t err = OK;
		char vp8_frame_header[12];
		unsigned int vp8_frame_length;


        while (writer->mRunning) {
            MediaBuffer* buffer;
            err = writer->mSource->read(&buffer, NULL);

            if (err == OK) {
				vp8_frame_length = buffer->range_length();
				mem_put_le32(vp8_frame_header, vp8_frame_length);
				mem_put_le32(vp8_frame_header+4, pts&0xFFFFFFFF);
				mem_put_le32(vp8_frame_header+8, pts >> 32);
				fwrite(vp8_frame_header, 1, 12, writer->mFilehandle);
				pts++;
                fwrite(buffer->data()+buffer->range_offset(), 1, buffer->range_length(), writer->mFilehandle);
				buffer->release();
                continue;
            }else {
                if (err != ERROR_END_OF_STREAM)
                    LOG("RawWriter::threadfunc err=%d\n", err);
                writer->mEOS = true;
                writer->mRunning = false;
                fflush(writer->mFilehandle);
                return NULL;
            }
        }

        return NULL;
    }

};


class RawWriter : public MediaWriter {
public:
    RawWriter(char* file) {
        mFile = file;
        mRunning = false;
    }

    status_t addSource(const sp<MediaSource> &source) {
        mSource = source;
        return OK;
    }

    bool reachedEOS() {
        return mEOS;
    }

    status_t start(MetaData *params = NULL) {

        mSource->start();

        mRunning = true;
        mEOS = false;

        mFilehandle = fopen(mFile, "w+");
        if (mFilehandle == NULL)
            return errno;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        pthread_create(&mThread, &attr, RawWriter::ThreadFunc, this);
        pthread_attr_destroy(&attr);

        return OK;
    }

    status_t stop() {
        mRunning = false;
        void *dummy;
        pthread_join(mThread, &dummy);
        fclose(mFilehandle);
        return OK;
    }

    status_t pause() {
        return OK;
    }

private:
    static void *ThreadFunc(void *me) {
        RawWriter *writer = static_cast<RawWriter *>(me);

        status_t err = OK;

        while (writer->mRunning) {
            MediaBuffer* buffer;
            err = writer->mSource->read(&buffer, NULL);

            if (err == OK) {
                fwrite(buffer->data()+buffer->range_offset(), 1, buffer->range_length(), writer->mFilehandle);
//                LOG("RawWriter::threadfunc fwrite size=%d\n", buffer->range_length());
                buffer->release();
                continue;
            }else {
                if (err != ERROR_END_OF_STREAM)
                    LOG("RawWriter::threadfunc err=%d\n", err);
                writer->mEOS = true;
                writer->mRunning = false;
                fflush(writer->mFilehandle);
                return NULL;
            }
        }

        return NULL;
    }

public:

    const char* mFile;
    FILE* mFilehandle;
    sp<MediaSource> mSource;
    pthread_t mThread;
    bool mRunning;
    bool mEOS;
};


void usage() {
    printf("2nd generation Mix_encoder\n");
    printf("Usage: mix_encoder2 [options]\n\n");
    printf(" -a/--initQP <qp>					set initQP, default 0\n");
    printf(" -b/--bitrate <Bitrate>				set bitrate bps, default 10M\n");
    printf(" -c/--codec <Codec>				select codec, like H264(default), MPEG4, H263, VP8\n");
    printf(" -d/--intraPeriod <period>			set IntraPeriod, default 30\n");
    printf(" -e/--encoder	 <encoder>			select encoder, like MIX(default), OMXCODEC\n");
    printf(" -f <output file>				set output file name\n");
    printf(" -i/--yuv <yuvfile>				select yuv generate method, AUTO(default) or from yuv file\n");
    printf(" -j/--winSize						set window size, default 1000\n");
    printf(" -k/--encWidth <Width> -g/--encHeight <Hight>	set encoder width/height, default 1280*720\n");
    printf(" -l/--idrInterval					set IdrInterval, default 1\n");
    printf(" -m/--disableMetadata				disable Metadata Mode(default enabled)\n");
    printf(" -n/--count <number>				set source frame number, default 30\n");
    printf(" -o/--outputformat <format>			set output file format, like MP4(default), RAW, IVF(only for VP8)\n");
    printf(" -p/--fps <Bitrate>				set frame rate, default 30\n");
    printf(" -q/--minQP <qp>					set minQP, default 0\n");
    printf(" -r/--rcMode <Mode>				set rc mode, like VBR(default), CBR, VCM, NO_RC\n");
    printf(" -s/--src <source>				select source, like MALLOC(default), VASURFACE, KBUFHANDLE, GFX, GRALLOC, MEMHEAP (CAMERASOURCE, not support yet) \n");
    printf(" -w <Width> -h <Height>				set source width /height, default 1280*720\n");
    printf(" -t/--disableFrameSkip				disable frame skip, default is false\n");
    printf("\n");

}

int main(int argc, char* argv[])
{
    int SrcType = 0;
    int SrcWidth = 1280;
    int SrcHeight = 720;
    int SrcStride = 1280;
    int SrcFrameNum = 30;
    bool MetadataMode = true;
    bool SoftCodec = false;
    int SrcFps = 30;
    int EncType = 0;
    int EncCodec = 0;
    int EncRCMode = 0;
    int EncBitrate = 10000000;
    int EncWidth = 1280;
    int EncHeight = 720;
    int InitQP = 0;
    int MinQP = 0;
    int WinSize = 1000;
    int IdrInt = 1;
    int IntraPeriod = 30;
    int DisableFrameSkip = 0;
    int OutFormat = 0;
//    bool SyncMode = false;
    char* OutFileName = "out.264";
    const char* Yuvfile = NULL;

    android::ProcessState::self()->startThreadPool();

    const char *short_opts = "a:b:c:d:e:f:g:h:i:j:k:l:m:n:o:p:q:r:s:t:q:u:v:w:x:y:z:?";
    const struct option long_opts[] = {
						{"help", no_argument, NULL, '?'},
						{"file", required_argument, NULL, 'f'},
						{"src", required_argument, NULL, 's'},
						{"yuv", required_argument, NULL, 'i'},
						{"srcWidth", required_argument, NULL, 'w'},
						{"srcHeight", required_argument, NULL, 'h'},
						{"disableMetadata", no_argument, NULL, 'm'},
						{"count", required_argument, NULL, 'n'},
						{"encoder", required_argument, NULL, 'e'},
						{"codec", required_argument, NULL, 'c'},
						{"bitrate", required_argument, NULL, 'b'},
						{"output", required_argument, NULL, 'o'},
						{"fps", required_argument, NULL, 'p'},
						{"rcMode", required_argument, NULL, 'r'},
						{"encWidth", required_argument, NULL, 'k'},
						{"encHeight", required_argument, NULL, 'g'},
						{"initQP", required_argument, NULL, 'a'},
						{"minQP", required_argument, NULL, 'q'},
						{"intraPeriod", required_argument, NULL, 'd'},
						{"winSize", required_argument, NULL, 'j'},
						{"idrInt", required_argument, NULL, 'l'},
						{"disableFrameSkip", no_argument, NULL, 't'},
						{0, 0, 0, 0}
    };

    char c;

    const char *SRCTYPE[] = {"MALLOC", "VASURFACE", "KBUFHANDLE", "GFX", "GRALLOC", "MEMHEAP", "CAMERASOURCE", "SURFACEMEDIASOURCE", NULL};
    const char *ENCTYPE[] = {"MIX", "OMXCODEC", NULL};
    const char *CODEC[] = {"H264", "MPEG4", "H263", "VP8",NULL};
    const char *RCMODE[] = {"VBR", "CBR", "VCM", "NO_RC", NULL};
    const char *OUTFORMAT[] = {"MP4", "RAW", "IVF", NULL};

    while ((c = getopt_long(argc, argv, short_opts, long_opts, NULL) ) != EOF) {
        switch (c) {
                case 'a':
                    InitQP = atoi(optarg);
                    break;

                case 'b':
                    EncBitrate = atoi(optarg);
                    break;

                case 'c':
                    for (int i = 0; CODEC[i] != NULL; i++) {
                        if (strcasecmp (optarg, CODEC[i]) == 0) {
                            EncCodec = i;
                            break;
                        }else
                            continue;
                    }

                    break;

                case 'd':
                    IntraPeriod = atoi(optarg);
                    break;

                case 'e':
                    for (int i = 0; ENCTYPE[i] != NULL; i++) {
                        if (strcasecmp (optarg, ENCTYPE[i]) == 0) {
                            EncType = i;
                            break;
                        }else
                            continue;
                    }

                    break;

                case 'f':
                    OutFileName = optarg;
                    break;

                case 'g':
                    EncHeight = atoi(optarg);
                    break;

                case 'h':
                    SrcHeight = atoi(optarg);
                    EncHeight = SrcHeight;
                    break;

                case 'i':
                    if (strcasecmp (optarg, "AUTO") == 0)
                        Yuvfile = NULL;
                    else
                        Yuvfile = optarg;

                    break;

                case 'j':
                    WinSize = atoi(optarg);
                    break;

                case 'k':
                    EncWidth = atoi(optarg);
                    break;

                case 'l':
                    IdrInt = atoi(optarg);
                    break;

                case 'm':
                    MetadataMode = false;
                    break;

                case 'n':
                    SrcFrameNum = atoi(optarg);
                    break;

                case 'o':
                    for (int i = 0; OUTFORMAT[i] != NULL; i++) {
                        if (strcasecmp (optarg, OUTFORMAT[i]) == 0) {
                            OutFormat = i;
                            break;
                        }else
                            continue;
                    }

                    break;

                case 'p':
                    SrcFps = atoi(optarg);
                    break;

                case 'q':
                    MinQP = atoi(optarg);
                    break;

                case 'r':
                    for (int i = 0; RCMODE[i] != NULL; i++) {
                        if (strcasecmp (optarg, RCMODE[i]) == 0) {
                            EncRCMode = i;
                            break;
                        }else
                            continue;
                    }

                    break;

                case 's':
                    for (int i = 0; SRCTYPE[i] != NULL; i++) {
                        if (strcasecmp (optarg, SRCTYPE[i]) == 0) {
                            SrcType = i;
                            break;
                        }else
                            continue;
                    }

                    break;

                case 't':
                    DisableFrameSkip = 1;
                    break;

                case 'w':
                    SrcWidth = atoi(optarg);
                    SrcStride = SrcWidth;
                    EncWidth = SrcWidth;
                    break;

                case '?':
                default:
                    usage();
                    exit(0);
        }
    }

    //export encoding parameters summary
    printf("=========================================\n");
    printf("Source:\n");
    printf("Type: %s, Width: %d, Height: %d, Stride: %d\n", SRCTYPE[SrcType], SrcWidth, SrcHeight, SrcStride);
    printf("FPS: %d, YUV: %s, Metadata: %d\n", SrcFps, Yuvfile, MetadataMode);

    printf("\nEncoder:\n");
    printf("Type: %s, Codec: %s, Width: %d, Height: %d\n", ENCTYPE[EncType], CODEC[EncCodec], EncWidth, EncHeight);
    printf("RC: %s, Bitrate: %d bps, initQP: %d, minQP: %d\n", RCMODE[EncRCMode], EncBitrate, InitQP, MinQP);
    printf("winSize: %d, IdrInterval: %d, IntraPeriod: %d, FPS: %d \n", WinSize, IdrInt, IntraPeriod, SrcFps);
    printf("Frameskip: %d\n", !DisableFrameSkip);

    printf("\nOut:\n");
    printf("Type: %s, File: %s\n", OUTFORMAT[OutFormat], OutFileName);
    printf("=========================================\n");

    sp<MediaSource> source;
    sp<MediaSource> encoder;
    sp<MediaWriter> writer;

    //setup source
    if (SrcType == 0) {
        source = new MallocSource(SrcWidth, SrcHeight, SrcStride, SrcFrameNum,
			SrcFps, MetadataMode, Yuvfile);
    } else if (SrcType == 1) {
        source = new VASurfaceSource(SrcWidth, SrcHeight, SrcStride, SrcFrameNum,
			SrcFps, MetadataMode, Yuvfile, 0);
    } else if (SrcType == 2) {
        source = new VASurfaceSource(SrcWidth, SrcHeight, SrcStride, SrcFrameNum,
			SrcFps, MetadataMode, Yuvfile, 1);
    } else if (SrcType == 3) {
        source = new GfxSource(SrcWidth, SrcHeight, SrcStride, SrcFrameNum,
			SrcFps, MetadataMode, Yuvfile);
    } else if (SrcType == 4) {
        source = new GrallocSource(SrcWidth, SrcHeight, SrcStride, SrcFrameNum,
			SrcFps, MetadataMode, Yuvfile);
    } else if (SrcType == 5) {
        source = new MemHeapSource(SrcWidth, SrcHeight, SrcStride, SrcFrameNum,
			SrcFps, MetadataMode, Yuvfile);
    } else{
        printf("Source Type is not supported\n");
        return 0;
    }

    printf("Setup Encoder EncCodec is %d\n",EncCodec);
    //setup encoder
    sp<MetaData> enc_meta = new MetaData;
    switch (EncCodec) {
        case 1:
            enc_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);
            break;
        case 2:
            enc_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_H263);
            break;
		case 3: 
			enc_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_VPX);
			break;
        default:
            enc_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
            break;
    }

    enc_meta->setInt32(kKeyWidth, EncWidth);
    enc_meta->setInt32(kKeyHeight, EncHeight);
    enc_meta->setInt32(kKeyFrameRate, SrcFps);
    enc_meta->setInt32(kKeyBitRate, EncBitrate);
    enc_meta->setInt32(kKeyStride, EncWidth);
    enc_meta->setInt32(kKeySliceHeight, EncHeight);
    enc_meta->setInt32(kKeyIFramesInterval, 1);
    enc_meta->setInt32(kKeyColorFormat, OMX_COLOR_FormatYUV420SemiPlanar);

    //only valid for MIX encoder
    enc_meta->setInt32('itqp', InitQP);
    enc_meta->setInt32('mnqp', MinQP);
    enc_meta->setInt32('iapd', IntraPeriod);
    enc_meta->setInt32('wsiz', WinSize);
    enc_meta->setInt32('idri', WinSize);
    enc_meta->setInt32('difs', DisableFrameSkip);

    uint32_t encoder_flags = 0;
    if (MetadataMode)
        encoder_flags |= OMXCodec::kStoreMetaDataInVideoBuffers;
    if (SoftCodec)
        encoder_flags |= OMXCodec::kPreferSoftwareCodecs;

    OMXClient client;

    if (EncType == 1) {
        CHECK_EQ(client.connect(), (status_t)OK);

        encoder = OMXCodec::Create(
                client.interface(), enc_meta, true /* createEncoder */, source,
                0, encoder_flags);
    } else {
        encoder = new MixEncoder(source, enc_meta, EncRCMode, encoder_flags);
    }

    //setup output
    printf("Setup Writer\n");

    if (OutFormat == 0)
        writer = new MPEG4Writer(OutFileName);
    else if (OutFormat == 1)
        writer = new RawWriter(OutFileName);
	else 
		writer = new IVFWriter(OutFileName);

    writer->addSource(encoder);

    printf("Start encoding\n");

    int64_t start = systemTime();
    CHECK_EQ((status_t)OK, writer->start(enc_meta.get()));
    while (!writer->reachedEOS()) {
        usleep(100000);
    }
    status_t err = writer->stop();
    int64_t end = systemTime();

    if (EncType == 1) {
        client.disconnect();
    }

    printf("Stop Encoding\n");

    if (err != OK && err != ERROR_END_OF_STREAM) {
        LOG("record failed: %d\n", err);
        return 1;
    }

    enc_meta.clear();

    printf("encoding %d frames in %lld us\n", gNumFramesOutput, (end-start)/1000);
    printf("encoding speed is: %.2f fps\n", (gNumFramesOutput * 1E9) / (end-start));

    return 1;
}

