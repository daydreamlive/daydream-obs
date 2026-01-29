#include "daydream-encoder.h"
#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <string.h>

#if defined(__APPLE__)
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_videotoolbox.h>
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

#if defined(__APPLE__)
	AVBufferRef *hw_device_ctx;
	AVBufferRef *hw_frames_ctx;
	CVPixelBufferPoolRef pixel_buffer_pool;
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
