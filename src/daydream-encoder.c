#include "daydream-encoder.h"
#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <string.h>

struct daydream_encoder {
	AVCodecContext *codec_ctx;
	AVFrame *frame;
	AVPacket *packet;
	struct SwsContext *sws_ctx;

	uint32_t width;
	uint32_t height;
	uint32_t fps;
	int64_t frame_count;

	bool request_keyframe;

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
	encoder->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	encoder->codec_ctx->gop_size = encoder->fps;
	encoder->codec_ctx->max_b_frames = 0;

	uint32_t bitrate = config->bitrate > 0 ? config->bitrate : 2000000;
	encoder->codec_ctx->bit_rate = bitrate;
	encoder->codec_ctx->rc_max_rate = bitrate;
	encoder->codec_ctx->rc_buffer_size = bitrate / 2;

	configure_encoder_options(encoder->codec_ctx, codec);

	if (avcodec_open2(encoder->codec_ctx, codec, NULL) < 0) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to open codec");
		avcodec_free_context(&encoder->codec_ctx);
		bfree(encoder);
		return NULL;
	}

	encoder->frame = av_frame_alloc();
	if (!encoder->frame) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to allocate frame");
		avcodec_free_context(&encoder->codec_ctx);
		bfree(encoder);
		return NULL;
	}

	encoder->frame->format = AV_PIX_FMT_YUV420P;
	encoder->frame->width = config->width;
	encoder->frame->height = config->height;

	if (av_frame_get_buffer(encoder->frame, 32) < 0) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to allocate frame buffer");
		av_frame_free(&encoder->frame);
		avcodec_free_context(&encoder->codec_ctx);
		bfree(encoder);
		return NULL;
	}

	encoder->packet = av_packet_alloc();
	if (!encoder->packet) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to allocate packet");
		av_frame_free(&encoder->frame);
		avcodec_free_context(&encoder->codec_ctx);
		bfree(encoder);
		return NULL;
	}

	encoder->sws_ctx = sws_getContext(config->width, config->height, AV_PIX_FMT_BGRA, config->width, config->height,
					  AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);

	if (!encoder->sws_ctx) {
		blog(LOG_ERROR, "[Daydream Encoder] Failed to create sws context");
		av_packet_free(&encoder->packet);
		av_frame_free(&encoder->frame);
		avcodec_free_context(&encoder->codec_ctx);
		bfree(encoder);
		return NULL;
	}

	encoder->output_buffer_size = config->width * config->height * 2;
	encoder->output_buffer = bmalloc(encoder->output_buffer_size);

	blog(LOG_INFO, "[Daydream Encoder] Created %dx%d @ %d fps, %d kbps (encoder: %s)", config->width,
	     config->height, encoder->fps, bitrate / 1000, codec->name);

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
	if (encoder->codec_ctx)
		avcodec_free_context(&encoder->codec_ctx);
	if (encoder->output_buffer)
		bfree(encoder->output_buffer);

	bfree(encoder);
}

bool daydream_encoder_encode(struct daydream_encoder *encoder, const uint8_t *bgra_data, uint32_t linesize,
			     struct daydream_encoded_frame *out_frame)
{
	if (!encoder || !bgra_data || !out_frame)
		return false;

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
	if (ret < 0) {
		blog(LOG_ERROR, "[Daydream Encoder] Error sending frame for encoding");
		return false;
	}

	ret = avcodec_receive_packet(encoder->codec_ctx, encoder->packet);
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
