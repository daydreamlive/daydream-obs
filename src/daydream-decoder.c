#include "daydream-decoder.h"
#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

struct daydream_decoder {
	AVCodecContext *codec_ctx;
	AVFrame *frame;
	AVPacket *packet;
	struct SwsContext *sws_ctx;

	uint32_t width;
	uint32_t height;

	uint8_t *output_buffer;
	size_t output_buffer_size;
	uint32_t output_linesize;
};

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

	if (avcodec_open2(decoder->codec_ctx, codec, NULL) < 0) {
		blog(LOG_ERROR, "[Daydream Decoder] Failed to open codec");
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		return NULL;
	}

	decoder->frame = av_frame_alloc();
	if (!decoder->frame) {
		blog(LOG_ERROR, "[Daydream Decoder] Failed to allocate frame");
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		return NULL;
	}

	decoder->packet = av_packet_alloc();
	if (!decoder->packet) {
		blog(LOG_ERROR, "[Daydream Decoder] Failed to allocate packet");
		av_frame_free(&decoder->frame);
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		return NULL;
	}

	blog(LOG_INFO, "[Daydream Decoder] Created");

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
	if (decoder->frame)
		av_frame_free(&decoder->frame);
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

	uint32_t frame_width = decoder->frame->width;
	uint32_t frame_height = decoder->frame->height;

	if (decoder->width != frame_width || decoder->height != frame_height) {
		decoder->width = frame_width;
		decoder->height = frame_height;

		if (decoder->sws_ctx) {
			sws_freeContext(decoder->sws_ctx);
			decoder->sws_ctx = NULL;
		}
	}

	if (!decoder->sws_ctx) {
		decoder->sws_ctx = sws_getContext(frame_width, frame_height, decoder->frame->format, frame_width,
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

	sws_scale(decoder->sws_ctx, (const uint8_t *const *)decoder->frame->data, decoder->frame->linesize, 0,
		  frame_height, dst_data, dst_linesize);

	out_frame->data = decoder->output_buffer;
	out_frame->linesize = decoder->output_linesize;
	out_frame->width = frame_width;
	out_frame->height = frame_height;
	out_frame->pts = decoder->frame->pts;

	return true;
}
