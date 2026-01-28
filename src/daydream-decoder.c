#include "daydream-decoder.h"
#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>

struct daydream_decoder {
	AVCodecContext *codec_ctx;
	AVFrame *frame;
	AVFrame *sw_frame;
	AVPacket *packet;
	struct SwsContext *sws_ctx;

	AVBufferRef *hw_device_ctx;
	enum AVPixelFormat hw_pix_fmt;
	bool using_hw;

	uint32_t width;
	uint32_t height;

	uint8_t *output_buffer;
	size_t output_buffer_size;
	uint32_t output_linesize;
};

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	struct daydream_decoder *decoder = ctx->opaque;
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == decoder->hw_pix_fmt)
			return *p;
	}
	blog(LOG_WARNING, "[Daydream Decoder] Failed to get HW pixel format, falling back to SW");
	return pix_fmts[0];
}

static bool init_hw_decoder(struct daydream_decoder *decoder, const AVCodec *codec)
{
#if defined(__APPLE__)
	enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
	const char *hw_name = "videotoolbox";
#elif defined(_WIN32)
	enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_D3D11VA;
	const char *hw_name = "d3d11va";
#elif defined(__linux__)
	enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_VAAPI;
	const char *hw_name = "vaapi";
#else
	return false;
#endif

	for (int i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
		if (!config) {
			blog(LOG_INFO, "[Daydream Decoder] No HW config found for %s", hw_name);
			return false;
		}
		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == hw_type) {
			decoder->hw_pix_fmt = config->pix_fmt;
			break;
		}
	}

	if (av_hwdevice_ctx_create(&decoder->hw_device_ctx, hw_type, NULL, NULL, 0) < 0) {
		blog(LOG_INFO, "[Daydream Decoder] Failed to create HW device context for %s", hw_name);
		return false;
	}

	decoder->codec_ctx->hw_device_ctx = av_buffer_ref(decoder->hw_device_ctx);
	decoder->codec_ctx->opaque = decoder;
	decoder->codec_ctx->get_format = get_hw_format;

	blog(LOG_INFO, "[Daydream Decoder] Using hardware decoder: %s", hw_name);
	return true;
}

struct daydream_decoder *daydream_decoder_create(const struct daydream_decoder_config *config)
{
	if (!config)
		return NULL;

	struct daydream_decoder *decoder = bzalloc(sizeof(struct daydream_decoder));
	decoder->width = config->width;
	decoder->height = config->height;

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		blog(LOG_ERROR, "[Daydream Decoder] H.264 decoder not found");
		bfree(decoder);
		return NULL;
	}

	decoder->codec_ctx = avcodec_alloc_context3(codec);
	if (!decoder->codec_ctx) {
		blog(LOG_ERROR, "[Daydream Decoder] Failed to allocate codec context");
		bfree(decoder);
		return NULL;
	}

	decoder->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	decoder->codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

	decoder->using_hw = init_hw_decoder(decoder, codec);

	if (avcodec_open2(decoder->codec_ctx, codec, NULL) < 0) {
		blog(LOG_ERROR, "[Daydream Decoder] Failed to open codec");
		if (decoder->hw_device_ctx)
			av_buffer_unref(&decoder->hw_device_ctx);
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		return NULL;
	}

	decoder->frame = av_frame_alloc();
	if (!decoder->frame) {
		blog(LOG_ERROR, "[Daydream Decoder] Failed to allocate frame");
		if (decoder->hw_device_ctx)
			av_buffer_unref(&decoder->hw_device_ctx);
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		return NULL;
	}

	if (decoder->using_hw) {
		decoder->sw_frame = av_frame_alloc();
		if (!decoder->sw_frame) {
			blog(LOG_ERROR, "[Daydream Decoder] Failed to allocate sw_frame");
			av_frame_free(&decoder->frame);
			if (decoder->hw_device_ctx)
				av_buffer_unref(&decoder->hw_device_ctx);
			avcodec_free_context(&decoder->codec_ctx);
			bfree(decoder);
			return NULL;
		}
	}

	decoder->packet = av_packet_alloc();
	if (!decoder->packet) {
		blog(LOG_ERROR, "[Daydream Decoder] Failed to allocate packet");
		if (decoder->sw_frame)
			av_frame_free(&decoder->sw_frame);
		av_frame_free(&decoder->frame);
		if (decoder->hw_device_ctx)
			av_buffer_unref(&decoder->hw_device_ctx);
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		return NULL;
	}

	blog(LOG_INFO, "[Daydream Decoder] Created (%s)", decoder->using_hw ? "hardware" : "software");

	return decoder;
}

void daydream_decoder_destroy(struct daydream_decoder *decoder)
{
	if (!decoder)
		return;

	if (decoder->sws_ctx)
		sws_freeContext(decoder->sws_ctx);
	if (decoder->packet)
		av_packet_free(&decoder->packet);
	if (decoder->sw_frame)
		av_frame_free(&decoder->sw_frame);
	if (decoder->frame)
		av_frame_free(&decoder->frame);
	if (decoder->hw_device_ctx)
		av_buffer_unref(&decoder->hw_device_ctx);
	if (decoder->codec_ctx)
		avcodec_free_context(&decoder->codec_ctx);
	if (decoder->output_buffer)
		bfree(decoder->output_buffer);

	bfree(decoder);
}

bool daydream_decoder_decode(struct daydream_decoder *decoder, const uint8_t *h264_data, size_t size,
			     struct daydream_decoded_frame *out_frame)
{
	if (!decoder || !h264_data || size == 0 || !out_frame)
		return false;

	decoder->packet->data = (uint8_t *)h264_data;
	decoder->packet->size = (int)size;

	int ret = avcodec_send_packet(decoder->codec_ctx, decoder->packet);
	if (ret < 0 && ret != AVERROR(EAGAIN))
		return false;

	ret = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
	if (ret < 0)
		return false;

	AVFrame *src_frame = decoder->frame;

	if (decoder->using_hw && decoder->frame->format == decoder->hw_pix_fmt) {
		ret = av_hwframe_transfer_data(decoder->sw_frame, decoder->frame, 0);
		if (ret < 0) {
			blog(LOG_ERROR, "[Daydream Decoder] Failed to transfer HW frame to CPU");
			return false;
		}
		src_frame = decoder->sw_frame;
	}

	uint32_t frame_width = src_frame->width;
	uint32_t frame_height = src_frame->height;

	if (decoder->width != frame_width || decoder->height != frame_height || (decoder->sws_ctx == NULL)) {
		decoder->width = frame_width;
		decoder->height = frame_height;

		if (decoder->sws_ctx) {
			sws_freeContext(decoder->sws_ctx);
			decoder->sws_ctx = NULL;
		}

		decoder->sws_ctx = sws_getContext(frame_width, frame_height, src_frame->format, frame_width,
						  frame_height, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, NULL, NULL, NULL);

		if (!decoder->sws_ctx) {
			blog(LOG_ERROR, "[Daydream Decoder] Failed to create sws context");
			return false;
		}

		decoder->output_linesize = frame_width * 4;
		decoder->output_buffer_size = decoder->output_linesize * frame_height;
		decoder->output_buffer = brealloc(decoder->output_buffer, decoder->output_buffer_size);
	}

	uint8_t *dst_data[1] = {decoder->output_buffer};
	int dst_linesize[1] = {(int)decoder->output_linesize};

	sws_scale(decoder->sws_ctx, (const uint8_t *const *)src_frame->data, src_frame->linesize, 0, frame_height,
		  dst_data, dst_linesize);

	out_frame->data = decoder->output_buffer;
	out_frame->linesize = decoder->output_linesize;
	out_frame->width = frame_width;
	out_frame->height = frame_height;
	out_frame->pts = decoder->frame->pts;

	return true;
}
