#include "daydream-encoder.h"
#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <string.h>
#include <pthread.h>

#if defined(__APPLE__)
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_videotoolbox.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#endif

// Forward declarations
#if defined(__APPLE__)
static bool init_zerocopy_encoder(struct daydream_encoder *encoder, uint32_t bitrate);
#endif

struct daydream_encoder {
	AVCodecContext *codec_ctx;
	AVFrame *frame;
	AVPacket *packet;
	struct SwsContext *sws_ctx; // Only used for SW fallback

	uint32_t width;
	uint32_t height;
	uint32_t fps;
	int64_t frame_count;

	bool request_keyframe;
	bool using_hw;
	bool using_zerocopy;

#if defined(__APPLE__)
	AVBufferRef *hw_device_ctx;
	AVBufferRef *hw_frames_ctx;
	CVPixelBufferPoolRef pixel_buffer_pool;

	// Zero-copy path (direct VideoToolbox)
	VTCompressionSessionRef vt_session;
	IOSurfaceRef iosurface;
	CVPixelBufferRef pixel_buffer;

	// Encoded output (from VT callback)
	uint8_t *vt_output_data;
	size_t vt_output_size;
	bool vt_is_keyframe;
	bool vt_frame_ready;
	pthread_mutex_t vt_mutex;
	pthread_cond_t vt_cond;
#endif

	uint8_t *output_buffer;
	size_t output_buffer_size;
};

static const AVCodec *find_best_h264_encoder(void)
{
	const char *encoder_names[] = {
#if defined(__APPLE__)
		"h264_videotoolbox",
#elif defined(_WIN32)
		"h264_nvenc", "h264_amf", "h264_qsv",
#elif defined(__linux__)
		"h264_nvenc", "h264_vaapi", "h264_qsv",
#endif
		"libx264", NULL};

	for (int i = 0; encoder_names[i]; i++) {
		const AVCodec *codec = avcodec_find_encoder_by_name(encoder_names[i]);
		if (codec) {
			blog(LOG_INFO, "[Daydream Encoder] Found encoder: %s", encoder_names[i]);
			return codec;
		}
	}

	return avcodec_find_encoder(AV_CODEC_ID_H264);
}

static void configure_encoder_options(AVCodecContext *ctx, const AVCodec *codec)
{
	const char *name = codec->name;

	if (strcmp(name, "libx264") == 0) {
		av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
		av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
		av_opt_set(ctx->priv_data, "profile", "baseline", 0);
	} else if (strcmp(name, "h264_videotoolbox") == 0) {
		av_opt_set(ctx->priv_data, "realtime", "1", 0);
		av_opt_set(ctx->priv_data, "allow_sw", "0", 0);
	} else if (strcmp(name, "h264_nvenc") == 0) {
		av_opt_set(ctx->priv_data, "preset", "p1", 0);
		av_opt_set(ctx->priv_data, "tune", "ll", 0);
		av_opt_set(ctx->priv_data, "rc", "cbr", 0);
	} else if (strcmp(name, "h264_amf") == 0) {
		av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
		av_opt_set(ctx->priv_data, "quality", "speed", 0);
	} else if (strcmp(name, "h264_qsv") == 0) {
		av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
		av_opt_set(ctx->priv_data, "low_power", "1", 0);
	}
}

#if defined(__APPLE__)
static bool init_videotoolbox_encoder(struct daydream_encoder *encoder, const AVCodec *codec)
{
	UNUSED_PARAMETER(codec);

	// Create hardware device context
	int ret = av_hwdevice_ctx_create(&encoder->hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0);
	if (ret < 0) {
		blog(LOG_INFO, "[Daydream Encoder] Failed to create VideoToolbox device context, using SW path");
		return false;
	}

	encoder->codec_ctx->hw_device_ctx = av_buffer_ref(encoder->hw_device_ctx);
	encoder->codec_ctx->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;

	// Create hardware frames context
	encoder->hw_frames_ctx = av_hwframe_ctx_alloc(encoder->hw_device_ctx);
	if (!encoder->hw_frames_ctx) {
		blog(LOG_INFO, "[Daydream Encoder] Failed to create hw frames context");
		av_buffer_unref(&encoder->hw_device_ctx);
		return false;
	}

	AVHWFramesContext *frames_ctx = (AVHWFramesContext *)encoder->hw_frames_ctx->data;
	frames_ctx->format = AV_PIX_FMT_VIDEOTOOLBOX;
	frames_ctx->sw_format = AV_PIX_FMT_BGRA; // Input format - VideoToolbox will convert
	frames_ctx->width = encoder->width;
	frames_ctx->height = encoder->height;
	frames_ctx->initial_pool_size = 4;

	ret = av_hwframe_ctx_init(encoder->hw_frames_ctx);
	if (ret < 0) {
		blog(LOG_INFO, "[Daydream Encoder] Failed to init hw frames context: %d", ret);
		av_buffer_unref(&encoder->hw_frames_ctx);
		av_buffer_unref(&encoder->hw_device_ctx);
		return false;
	}

	encoder->codec_ctx->hw_frames_ctx = av_buffer_ref(encoder->hw_frames_ctx);

	blog(LOG_INFO, "[Daydream Encoder] VideoToolbox HW encoder initialized (BGRA direct input)");
	return true;
}
#endif

struct daydream_encoder *daydream_encoder_create(const struct daydream_encoder_config *config)
{
	if (!config || config->width == 0 || config->height == 0)
		return NULL;

	struct daydream_encoder *encoder = bzalloc(sizeof(struct daydream_encoder));
	encoder->width = config->width;
	encoder->height = config->height;
	encoder->fps = config->fps > 0 ? config->fps : 30;
	encoder->frame_count = 0;
	encoder->request_keyframe = true;
	encoder->using_hw = false;
	encoder->using_zerocopy = false;

#if defined(__APPLE__)
	// Try zero-copy path first on macOS
	if (config->use_zerocopy) {
		uint32_t bitrate = config->bitrate > 0 ? config->bitrate : 2000000;
		if (init_zerocopy_encoder(encoder, bitrate)) {
			encoder->using_zerocopy = true;
			encoder->output_buffer_size = config->width * config->height * 2;
			encoder->output_buffer = bmalloc(encoder->output_buffer_size);
			blog(LOG_INFO, "[Daydream Encoder] Created %dx%d @ %d fps, %d kbps (zero-copy)", config->width,
			     config->height, encoder->fps, bitrate / 1000);
			return encoder;
		}
		blog(LOG_INFO, "[Daydream Encoder] Zero-copy init failed, falling back to FFmpeg path");
	}
#endif

	const AVCodec *codec = find_best_h264_encoder();
	if (!codec) {
		blog(LOG_ERROR, "[Daydream Encoder] H.264 encoder not found");
		bfree(encoder);
		return NULL;
	}

	encoder->codec_ctx = avcodec_alloc_context3(codec);
	if (!encoder->codec_ctx) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to allocate codec context");
		bfree(encoder);
		return NULL;
	}

	encoder->codec_ctx->width = config->width;
	encoder->codec_ctx->height = config->height;
	encoder->codec_ctx->time_base = (AVRational){1, (int)encoder->fps};
	encoder->codec_ctx->framerate = (AVRational){(int)encoder->fps, 1};
	encoder->codec_ctx->gop_size = encoder->fps;
	encoder->codec_ctx->max_b_frames = 0;

	uint32_t bitrate = config->bitrate > 0 ? config->bitrate : 2000000;
	encoder->codec_ctx->bit_rate = bitrate;
	encoder->codec_ctx->rc_max_rate = bitrate;
	encoder->codec_ctx->rc_buffer_size = bitrate / 2;

	configure_encoder_options(encoder->codec_ctx, codec);

#if defined(__APPLE__)
	// Try hardware encoder with direct BGRA input
	if (strcmp(codec->name, "h264_videotoolbox") == 0) {
		encoder->using_hw = init_videotoolbox_encoder(encoder, codec);
	}
#endif

	// Fallback to software path
	if (!encoder->using_hw) {
		encoder->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	}

	if (avcodec_open2(encoder->codec_ctx, codec, NULL) < 0) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to open codec");
#if defined(__APPLE__)
		if (encoder->hw_frames_ctx)
			av_buffer_unref(&encoder->hw_frames_ctx);
		if (encoder->hw_device_ctx)
			av_buffer_unref(&encoder->hw_device_ctx);
#endif
		avcodec_free_context(&encoder->codec_ctx);
		bfree(encoder);
		return NULL;
	}

	encoder->frame = av_frame_alloc();
	if (!encoder->frame) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to allocate frame");
#if defined(__APPLE__)
		if (encoder->hw_frames_ctx)
			av_buffer_unref(&encoder->hw_frames_ctx);
		if (encoder->hw_device_ctx)
			av_buffer_unref(&encoder->hw_device_ctx);
#endif
		avcodec_free_context(&encoder->codec_ctx);
		bfree(encoder);
		return NULL;
	}

	if (encoder->using_hw) {
#if defined(__APPLE__)
		// For HW path, we'll allocate frames from the hw context when encoding
		encoder->frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
		encoder->frame->width = config->width;
		encoder->frame->height = config->height;
#endif
	} else {
		// Software path - allocate YUV frame
		encoder->frame->format = AV_PIX_FMT_YUV420P;
		encoder->frame->width = config->width;
		encoder->frame->height = config->height;

		if (av_frame_get_buffer(encoder->frame, 32) < 0) {
			blog(LOG_ERROR, "[Daydream Encoder] Failed to allocate frame buffer");
			av_frame_free(&encoder->frame);
#if defined(__APPLE__)
			if (encoder->hw_frames_ctx)
				av_buffer_unref(&encoder->hw_frames_ctx);
			if (encoder->hw_device_ctx)
				av_buffer_unref(&encoder->hw_device_ctx);
#endif
			avcodec_free_context(&encoder->codec_ctx);
			bfree(encoder);
			return NULL;
		}

		// Create sws context for SW path
		encoder->sws_ctx = sws_getContext(config->width, config->height, AV_PIX_FMT_BGRA, config->width,
						  config->height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL,
						  NULL);

		if (!encoder->sws_ctx) {
			blog(LOG_ERROR, "[Daydream Encoder] Failed to create sws context");
			av_frame_free(&encoder->frame);
			avcodec_free_context(&encoder->codec_ctx);
			bfree(encoder);
			return NULL;
		}
	}

	encoder->packet = av_packet_alloc();
	if (!encoder->packet) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to allocate packet");
		if (encoder->sws_ctx)
			sws_freeContext(encoder->sws_ctx);
		av_frame_free(&encoder->frame);
#if defined(__APPLE__)
		if (encoder->hw_frames_ctx)
			av_buffer_unref(&encoder->hw_frames_ctx);
		if (encoder->hw_device_ctx)
			av_buffer_unref(&encoder->hw_device_ctx);
#endif
		avcodec_free_context(&encoder->codec_ctx);
		bfree(encoder);
		return NULL;
	}

	encoder->output_buffer_size = config->width * config->height * 2;
	encoder->output_buffer = bmalloc(encoder->output_buffer_size);

	blog(LOG_INFO, "[Daydream Encoder] Created %dx%d @ %d fps, %d kbps (encoder: %s, hw: %s)", config->width,
	     config->height, encoder->fps, bitrate / 1000, codec->name, encoder->using_hw ? "yes" : "no");

	return encoder;
}

void daydream_encoder_destroy(struct daydream_encoder *encoder)
{
	if (!encoder)
		return;

#if defined(__APPLE__)
	if (encoder->using_zerocopy) {
		if (encoder->vt_session) {
			VTCompressionSessionInvalidate(encoder->vt_session);
			CFRelease(encoder->vt_session);
		}
		if (encoder->pixel_buffer)
			CVPixelBufferRelease(encoder->pixel_buffer);
		if (encoder->iosurface)
			CFRelease(encoder->iosurface);
		pthread_mutex_destroy(&encoder->vt_mutex);
		pthread_cond_destroy(&encoder->vt_cond);
	}
#endif

	if (encoder->sws_ctx)
		sws_freeContext(encoder->sws_ctx);
	if (encoder->packet)
		av_packet_free(&encoder->packet);
	if (encoder->frame)
		av_frame_free(&encoder->frame);
#if defined(__APPLE__)
	if (encoder->hw_frames_ctx)
		av_buffer_unref(&encoder->hw_frames_ctx);
	if (encoder->hw_device_ctx)
		av_buffer_unref(&encoder->hw_device_ctx);
#endif
	if (encoder->codec_ctx)
		avcodec_free_context(&encoder->codec_ctx);
	if (encoder->output_buffer)
		bfree(encoder->output_buffer);

	bfree(encoder);
}

#if defined(__APPLE__)
static bool encode_hw_frame(struct daydream_encoder *encoder, const uint8_t *bgra_data, uint32_t linesize)
{
	// Create software frame pointing to BGRA data
	AVFrame *sw_frame = av_frame_alloc();
	if (!sw_frame)
		return false;

	sw_frame->format = AV_PIX_FMT_BGRA;
	sw_frame->width = encoder->width;
	sw_frame->height = encoder->height;
	sw_frame->data[0] = (uint8_t *)bgra_data;
	sw_frame->linesize[0] = (int)linesize;

	// Get hardware frame from pool
	AVFrame *hw_frame = av_frame_alloc();
	if (!hw_frame) {
		av_frame_free(&sw_frame);
		return false;
	}

	if (av_hwframe_get_buffer(encoder->hw_frames_ctx, hw_frame, 0) < 0) {
		av_frame_free(&sw_frame);
		av_frame_free(&hw_frame);
		return false;
	}

	// Upload software frame to hardware
	if (av_hwframe_transfer_data(hw_frame, sw_frame, 0) < 0) {
		av_frame_free(&sw_frame);
		av_frame_free(&hw_frame);
		return false;
	}

	hw_frame->pts = encoder->frame_count;

	if (encoder->request_keyframe) {
		hw_frame->pict_type = AV_PICTURE_TYPE_I;
		encoder->request_keyframe = false;
	} else {
		hw_frame->pict_type = AV_PICTURE_TYPE_NONE;
	}

	int ret = avcodec_send_frame(encoder->codec_ctx, hw_frame);

	av_frame_free(&sw_frame);
	av_frame_free(&hw_frame);

	return ret >= 0;
}
#endif

bool daydream_encoder_encode(struct daydream_encoder *encoder, const uint8_t *bgra_data, uint32_t linesize,
			     struct daydream_encoded_frame *out_frame)
{
	if (!encoder || !bgra_data || !out_frame)
		return false;

	bool send_success = false;

#if defined(__APPLE__)
	if (encoder->using_hw) {
		send_success = encode_hw_frame(encoder, bgra_data, linesize);
		if (send_success) {
			encoder->frame_count++;
		}
	}
#endif

	if (!encoder->using_hw) {
		// Software path - convert BGRA to YUV420P
		if (av_frame_make_writable(encoder->frame) < 0) {
			blog(LOG_ERROR, "[Daydream Encoder] Frame not writable");
			return false;
		}

		const uint8_t *src_data[1] = {bgra_data};
		int src_linesize[1] = {(int)linesize};

		sws_scale(encoder->sws_ctx, src_data, src_linesize, 0, encoder->height, encoder->frame->data,
			  encoder->frame->linesize);

		encoder->frame->pts = encoder->frame_count++;

		if (encoder->request_keyframe) {
			encoder->frame->pict_type = AV_PICTURE_TYPE_I;
			encoder->request_keyframe = false;
		} else {
			encoder->frame->pict_type = AV_PICTURE_TYPE_NONE;
		}

		int ret = avcodec_send_frame(encoder->codec_ctx, encoder->frame);
		send_success = (ret >= 0);
		if (ret < 0 && ret != AVERROR(EAGAIN)) {
			blog(LOG_ERROR, "[Daydream Encoder] Error sending frame for encoding: %d", ret);
		}
	}

	if (!send_success) {
		return false;
	}

	int ret = avcodec_receive_packet(encoder->codec_ctx, encoder->packet);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
		return false;
	} else if (ret < 0) {
		blog(LOG_ERROR, "[Daydream Encoder] Error receiving encoded packet");
		return false;
	}

	if ((size_t)encoder->packet->size > encoder->output_buffer_size) {
		encoder->output_buffer_size = encoder->packet->size * 2;
		encoder->output_buffer = brealloc(encoder->output_buffer, encoder->output_buffer_size);
	}

	memcpy(encoder->output_buffer, encoder->packet->data, encoder->packet->size);

	out_frame->data = encoder->output_buffer;
	out_frame->size = encoder->packet->size;
	out_frame->is_keyframe = (encoder->packet->flags & AV_PKT_FLAG_KEY) != 0;
	out_frame->pts = encoder->packet->pts;

	av_packet_unref(encoder->packet);

	return true;
}

#if defined(__APPLE__)
// VideoToolbox compression callback
static void vt_compression_callback(void *outputCallbackRefCon, void *sourceFrameRefCon, OSStatus status,
				    VTEncodeInfoFlags infoFlags, CMSampleBufferRef sampleBuffer)
{
	UNUSED_PARAMETER(sourceFrameRefCon);
	UNUSED_PARAMETER(infoFlags);

	struct daydream_encoder *encoder = outputCallbackRefCon;

	pthread_mutex_lock(&encoder->vt_mutex);

	if (status != noErr || !sampleBuffer) {
		encoder->vt_frame_ready = false;
		pthread_cond_signal(&encoder->vt_cond);
		pthread_mutex_unlock(&encoder->vt_mutex);
		return;
	}

	// Check if keyframe
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
	encoder->vt_is_keyframe = false;
	if (attachments && CFArrayGetCount(attachments) > 0) {
		CFDictionaryRef attachment = CFArrayGetValueAtIndex(attachments, 0);
		CFBooleanRef notSync = CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_NotSync);
		encoder->vt_is_keyframe = !notSync || !CFBooleanGetValue(notSync);
	}

	// Get H.264 data
	CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
	size_t totalLength = 0;
	char *dataPointer = NULL;
	CMBlockBufferGetDataPointer(blockBuffer, 0, NULL, &totalLength, &dataPointer);

	// Get format description for SPS/PPS if keyframe
	CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);

	size_t totalSize = totalLength;
	if (encoder->vt_is_keyframe && formatDesc) {
		// Add space for SPS and PPS with start codes
		size_t spsSize, ppsSize;
		const uint8_t *sps, *pps;
		CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 0, &sps, &spsSize, NULL, NULL);
		CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 1, &pps, &ppsSize, NULL, NULL);
		totalSize += 4 + spsSize + 4 + ppsSize; // start codes + data
	}

	// Reallocate if needed
	if (totalSize > encoder->output_buffer_size) {
		encoder->output_buffer_size = totalSize * 2;
		encoder->output_buffer = brealloc(encoder->output_buffer, encoder->output_buffer_size);
	}

	uint8_t *outPtr = encoder->output_buffer;

	// Write SPS/PPS for keyframes
	if (encoder->vt_is_keyframe && formatDesc) {
		size_t spsSize, ppsSize;
		const uint8_t *sps, *pps;
		CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 0, &sps, &spsSize, NULL, NULL);
		CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 1, &pps, &ppsSize, NULL, NULL);

		// SPS with start code
		outPtr[0] = 0x00;
		outPtr[1] = 0x00;
		outPtr[2] = 0x00;
		outPtr[3] = 0x01;
		memcpy(outPtr + 4, sps, spsSize);
		outPtr += 4 + spsSize;

		// PPS with start code
		outPtr[0] = 0x00;
		outPtr[1] = 0x00;
		outPtr[2] = 0x00;
		outPtr[3] = 0x01;
		memcpy(outPtr + 4, pps, ppsSize);
		outPtr += 4 + ppsSize;
	}

	// Convert AVCC format to Annex B (length prefix → start code)
	size_t offset = 0;
	while (offset < totalLength) {
		uint32_t naluLength = 0;
		memcpy(&naluLength, dataPointer + offset, 4);
		naluLength = CFSwapInt32BigToHost(naluLength);
		offset += 4;

		// Write start code
		outPtr[0] = 0x00;
		outPtr[1] = 0x00;
		outPtr[2] = 0x00;
		outPtr[3] = 0x01;
		memcpy(outPtr + 4, dataPointer + offset, naluLength);
		outPtr += 4 + naluLength;
		offset += naluLength;
	}

	encoder->vt_output_data = encoder->output_buffer;
	encoder->vt_output_size = outPtr - encoder->output_buffer;
	encoder->vt_frame_ready = true;

	pthread_cond_signal(&encoder->vt_cond);
	pthread_mutex_unlock(&encoder->vt_mutex);
}

static bool init_zerocopy_encoder(struct daydream_encoder *encoder, uint32_t bitrate)
{
	// Create IOSurface
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	int32_t width = encoder->width;
	int32_t height = encoder->height;
	int32_t bytesPerElement = 4;
	int32_t bytesPerRow = width * bytesPerElement;
	int32_t allocSize = bytesPerRow * height;

	CFNumberRef widthNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &width);
	CFNumberRef heightNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &height);
	CFNumberRef bytesPerElementNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bytesPerElement);
	CFNumberRef bytesPerRowNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bytesPerRow);
	CFNumberRef allocSizeNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &allocSize);

	int32_t pixelFormat = kCVPixelFormatType_32BGRA;
	CFNumberRef pixelFormatNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);

	CFDictionarySetValue(properties, kIOSurfaceWidth, widthNum);
	CFDictionarySetValue(properties, kIOSurfaceHeight, heightNum);
	CFDictionarySetValue(properties, kIOSurfaceBytesPerElement, bytesPerElementNum);
	CFDictionarySetValue(properties, kIOSurfaceBytesPerRow, bytesPerRowNum);
	CFDictionarySetValue(properties, kIOSurfaceAllocSize, allocSizeNum);
	CFDictionarySetValue(properties, kIOSurfacePixelFormat, pixelFormatNum);

	encoder->iosurface = IOSurfaceCreate(properties);

	CFRelease(widthNum);
	CFRelease(heightNum);
	CFRelease(bytesPerElementNum);
	CFRelease(bytesPerRowNum);
	CFRelease(allocSizeNum);
	CFRelease(pixelFormatNum);
	CFRelease(properties);

	if (!encoder->iosurface) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to create IOSurface");
		return false;
	}

	// Create CVPixelBuffer from IOSurface
	CVReturn cvRet =
		CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, encoder->iosurface, NULL, &encoder->pixel_buffer);
	if (cvRet != kCVReturnSuccess) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to create CVPixelBuffer from IOSurface: %d", cvRet);
		CFRelease(encoder->iosurface);
		encoder->iosurface = NULL;
		return false;
	}

	// Create VTCompressionSession
	CFMutableDictionaryRef encoderSpec = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(encoderSpec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
			     kCFBooleanTrue);
	CFDictionarySetValue(encoderSpec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
			     kCFBooleanTrue);

	OSStatus status =
		VTCompressionSessionCreate(kCFAllocatorDefault, width, height, kCMVideoCodecType_H264, encoderSpec,
					   NULL, // sourceImageBufferAttributes
					   kCFAllocatorDefault, vt_compression_callback, encoder, &encoder->vt_session);

	CFRelease(encoderSpec);

	if (status != noErr) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to create VTCompressionSession: %d", (int)status);
		CVPixelBufferRelease(encoder->pixel_buffer);
		CFRelease(encoder->iosurface);
		encoder->pixel_buffer = NULL;
		encoder->iosurface = NULL;
		return false;
	}

	// Configure session for realtime low-latency
	VTSessionSetProperty(encoder->vt_session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
	VTSessionSetProperty(encoder->vt_session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
	VTSessionSetProperty(encoder->vt_session, kVTCompressionPropertyKey_ProfileLevel,
			     kVTProfileLevel_H264_Baseline_AutoLevel);

	// Set bitrate
	CFNumberRef bitrateNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bitrate);
	VTSessionSetProperty(encoder->vt_session, kVTCompressionPropertyKey_AverageBitRate, bitrateNum);
	CFRelease(bitrateNum);

	// Set framerate
	int32_t fps = encoder->fps;
	CFNumberRef fpsNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fps);
	VTSessionSetProperty(encoder->vt_session, kVTCompressionPropertyKey_ExpectedFrameRate, fpsNum);
	CFRelease(fpsNum);

	// Set keyframe interval
	int32_t keyframeInterval = encoder->fps;
	CFNumberRef keyframeNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &keyframeInterval);
	VTSessionSetProperty(encoder->vt_session, kVTCompressionPropertyKey_MaxKeyFrameInterval, keyframeNum);
	CFRelease(keyframeNum);

	// Prepare to encode
	status = VTCompressionSessionPrepareToEncodeFrames(encoder->vt_session);
	if (status != noErr) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to prepare VTCompressionSession: %d", (int)status);
		VTCompressionSessionInvalidate(encoder->vt_session);
		CFRelease(encoder->vt_session);
		CVPixelBufferRelease(encoder->pixel_buffer);
		CFRelease(encoder->iosurface);
		encoder->vt_session = NULL;
		encoder->pixel_buffer = NULL;
		encoder->iosurface = NULL;
		return false;
	}

	pthread_mutex_init(&encoder->vt_mutex, NULL);
	pthread_cond_init(&encoder->vt_cond, NULL);

	blog(LOG_INFO, "[Daydream Encoder] Zero-copy VideoToolbox encoder initialized (%dx%d)", width, height);
	return true;
}

IOSurfaceRef daydream_encoder_get_iosurface(struct daydream_encoder *encoder)
{
	if (!encoder || !encoder->using_zerocopy)
		return NULL;
	return encoder->iosurface;
}

bool daydream_encoder_is_zerocopy(struct daydream_encoder *encoder)
{
	return encoder && encoder->using_zerocopy;
}

bool daydream_encoder_encode_iosurface(struct daydream_encoder *encoder, struct daydream_encoded_frame *out_frame)
{
	if (!encoder || !encoder->using_zerocopy || !encoder->vt_session || !out_frame)
		return false;

	CMTime pts = CMTimeMake(encoder->frame_count, encoder->fps);

	CFMutableDictionaryRef frameProps = NULL;
	if (encoder->request_keyframe) {
		frameProps = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
						       &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(frameProps, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
		encoder->request_keyframe = false;
	}

	pthread_mutex_lock(&encoder->vt_mutex);
	encoder->vt_frame_ready = false;
	pthread_mutex_unlock(&encoder->vt_mutex);

	OSStatus status = VTCompressionSessionEncodeFrame(encoder->vt_session, encoder->pixel_buffer, pts,
							  kCMTimeInvalid, frameProps, NULL, NULL);

	if (frameProps)
		CFRelease(frameProps);

	if (status != noErr) {
		blog(LOG_ERROR, "[Daydream Encoder] VTCompressionSessionEncodeFrame failed: %d", (int)status);
		return false;
	}

	// Force synchronous output
	VTCompressionSessionCompleteFrames(encoder->vt_session, kCMTimeInvalid);

	// Wait for callback
	pthread_mutex_lock(&encoder->vt_mutex);
	while (!encoder->vt_frame_ready) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100000000; // 100ms timeout
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		int ret = pthread_cond_timedwait(&encoder->vt_cond, &encoder->vt_mutex, &ts);
		if (ret != 0)
			break;
	}

	if (!encoder->vt_frame_ready) {
		pthread_mutex_unlock(&encoder->vt_mutex);
		return false;
	}

	out_frame->data = encoder->vt_output_data;
	out_frame->size = encoder->vt_output_size;
	out_frame->is_keyframe = encoder->vt_is_keyframe;
	out_frame->pts = encoder->frame_count;

	pthread_mutex_unlock(&encoder->vt_mutex);

	encoder->frame_count++;
	return true;
}
#endif
