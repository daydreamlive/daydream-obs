#include "daydream-whip.h"
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <curl/curl.h>

#include <rtc/rtc.h>

#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>

struct daydream_whip {
	std::string whip_url;
	std::string api_key;
	std::string resource_url;
	std::string whep_url;

	uint32_t width;
	uint32_t height;
	uint32_t fps;

	daydream_whip_state_callback on_state;
	void *userdata;

	int pc_id;
	int track_id;

	std::atomic<bool> connected;
	std::atomic<bool> gathering_done;
	std::mutex mutex;

	uint32_t ssrc;
	uint16_t seq_num;
	uint32_t timestamp;
	uint32_t timestamp_increment;
};

struct http_response {
	std::string data;
	std::string location;
	std::string whep_url;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	http_response *resp = static_cast<http_response *>(userp);
	resp->data.append(static_cast<char *>(contents), realsize);
	return realsize;
}

static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	size_t realsize = size * nitems;
	http_response *resp = static_cast<http_response *>(userdata);

	std::string header(buffer, realsize);

	auto extract_header_value = [&header](size_t colon_pos) -> std::string {
		std::string value = header.substr(colon_pos + 1);
		while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
			value.erase(0, 1);
		while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
			value.pop_back();
		return value;
	};

	std::string header_lower = header;
	for (char &c : header_lower)
		c = (char)tolower((unsigned char)c);

	if (header_lower.find("location:") == 0) {
		size_t pos = header.find(':');
		if (pos != std::string::npos)
			resp->location = extract_header_value(pos);
	} else if (header_lower.find("livepeer-playback-url:") == 0) {
		size_t pos = header.find(':');
		if (pos != std::string::npos) {
			resp->whep_url = extract_header_value(pos);
			blog(LOG_INFO, "[Daydream WHIP] Got WHEP URL from header: %s", resp->whep_url.c_str());
		}
	}
	return realsize;
}

static void on_state_change(int, rtcState state, void *ptr)
{
	daydream_whip *whip = static_cast<daydream_whip *>(ptr);

	const char *state_str = "unknown";
	switch (state) {
	case RTC_NEW:
		state_str = "new";
		break;
	case RTC_CONNECTING:
		state_str = "connecting";
		break;
	case RTC_CONNECTED:
		state_str = "connected";
		break;
	case RTC_DISCONNECTED:
		state_str = "disconnected";
		break;
	case RTC_FAILED:
		state_str = "failed";
		break;
	case RTC_CLOSED:
		state_str = "closed";
		break;
	}

	blog(LOG_INFO, "[Daydream WHIP] State changed: %s", state_str);

	if (state == RTC_CONNECTED) {
		whip->connected = true;
		if (whip->on_state)
			whip->on_state(true, nullptr, whip->userdata);
	} else if (state == RTC_DISCONNECTED || state == RTC_FAILED || state == RTC_CLOSED) {
		whip->connected = false;
		if (whip->on_state)
			whip->on_state(false, state == RTC_FAILED ? "Connection failed" : nullptr, whip->userdata);
	}
}

static void on_gathering_state_change(int, rtcGatheringState state, void *ptr)
{
	daydream_whip *whip = static_cast<daydream_whip *>(ptr);

	const char *state_str = "unknown";
	switch (state) {
	case RTC_GATHERING_NEW:
		state_str = "new";
		break;
	case RTC_GATHERING_INPROGRESS:
		state_str = "in-progress";
		break;
	case RTC_GATHERING_COMPLETE:
		state_str = "complete";
		whip->gathering_done = true;
		break;
	}
	blog(LOG_INFO, "[Daydream WHIP] Gathering state: %s", state_str);
}

static bool send_whip_offer(daydream_whip *whip, const std::string &sdp_offer)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return false;

	http_response *response = new http_response();

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/sdp");

	std::string auth_header = "Authorization: Bearer " + whip->api_key;
	headers = curl_slist_append(headers, auth_header.c_str());

	curl_easy_setopt(curl, CURLOPT_URL, whip->whip_url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, sdp_offer.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	CURLcode res = curl_easy_perform(curl);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		blog(LOG_ERROR, "[Daydream WHIP] HTTP request failed: %s", curl_easy_strerror(res));
		delete response;
		return false;
	}

	if (http_code != 200 && http_code != 201) {
		blog(LOG_ERROR, "[Daydream WHIP] HTTP error: %ld", http_code);
		delete response;
		return false;
	}

	if (!response->location.empty()) {
		whip->resource_url = response->location;
		blog(LOG_INFO, "[Daydream WHIP] Resource URL: %s", whip->resource_url.c_str());
	}

	if (!response->whep_url.empty()) {
		whip->whep_url = response->whep_url;
		blog(LOG_INFO, "[Daydream WHIP] WHEP URL: %s", whip->whep_url.c_str());
	}

	if (!response->data.empty()) {
		blog(LOG_INFO, "[Daydream WHIP] Setting remote description");
		rtcSetRemoteDescription(whip->pc_id, response->data.c_str(), "answer");
	}

	delete response;
	return true;
}

static std::string create_video_sdp(uint32_t width, uint32_t height, uint32_t fps, uint32_t ssrc)
{
	char sdp[4096];
	snprintf(
		sdp, sizeof(sdp),
		"v=0\r\n"
		"o=- 0 0 IN IP4 127.0.0.1\r\n"
		"s=-\r\n"
		"t=0 0\r\n"
		"m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
		"c=IN IP4 0.0.0.0\r\n"
		"a=rtcp:9 IN IP4 0.0.0.0\r\n"
		"a=ice-ufrag:whip\r\n"
		"a=ice-pwd:whipwhipwhipwhip\r\n"
		"a=ice-options:trickle\r\n"
		"a=fingerprint:sha-256 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00\r\n"
		"a=setup:actpass\r\n"
		"a=mid:0\r\n"
		"a=sendonly\r\n"
		"a=rtcp-mux\r\n"
		"a=rtpmap:96 H264/90000\r\n"
		"a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
		"a=ssrc:%u cname:daydream\r\n"
		"a=ssrc:%u msid:daydream video\r\n"
		"a=ssrc:%u mslabel:daydream\r\n"
		"a=ssrc:%u label:video\r\n"
		"a=framerate:%u\r\n"
		"a=imageattr:96 send [x=%u,y=%u]\r\n",
		ssrc, ssrc, ssrc, ssrc, fps, width, height);
	return std::string(sdp);
}

struct daydream_whip *daydream_whip_create(const struct daydream_whip_config *config)
{
	if (!config || !config->whip_url)
		return nullptr;

	daydream_whip *whip = new daydream_whip();
	whip->whip_url = config->whip_url;
	whip->api_key = config->api_key ? config->api_key : "";
	whip->width = config->width > 0 ? config->width : 512;
	whip->height = config->height > 0 ? config->height : 512;
	whip->fps = config->fps > 0 ? config->fps : 30;
	whip->on_state = config->on_state;
	whip->userdata = config->userdata;
	whip->pc_id = -1;
	whip->track_id = -1;
	whip->connected = false;
	whip->gathering_done = false;

	whip->ssrc = 12345678;
	whip->seq_num = 0;
	whip->timestamp = 0;
	whip->timestamp_increment = 90000 / whip->fps;

	return whip;
}

void daydream_whip_destroy(struct daydream_whip *whip)
{
	if (!whip)
		return;

	daydream_whip_disconnect(whip);
	delete whip;
}

bool daydream_whip_connect(struct daydream_whip *whip)
{
	if (!whip)
		return false;

	blog(LOG_INFO, "[Daydream WHIP] Connecting to %s", whip->whip_url.c_str());

	rtcConfiguration config = {};
	config.iceServers = nullptr;
	config.iceServersCount = 0;

	whip->pc_id = rtcCreatePeerConnection(&config);
	if (whip->pc_id < 0) {
		blog(LOG_ERROR, "[Daydream WHIP] Failed to create peer connection");
		return false;
	}

	rtcSetUserPointer(whip->pc_id, whip);
	rtcSetStateChangeCallback(whip->pc_id, on_state_change);
	rtcSetGatheringStateChangeCallback(whip->pc_id, on_gathering_state_change);

	rtcTrackInit track_init = {};
	track_init.direction = RTC_DIRECTION_SENDONLY;
	track_init.codec = RTC_CODEC_H264;
	track_init.payloadType = 96;
	track_init.ssrc = whip->ssrc;
	track_init.mid = "0";
	track_init.name = "video";
	track_init.msid = "daydream";
	track_init.trackId = "video";

	whip->track_id = rtcAddTrackEx(whip->pc_id, &track_init);
	if (whip->track_id < 0) {
		blog(LOG_ERROR, "[Daydream WHIP] Failed to add video track");
		rtcDeletePeerConnection(whip->pc_id);
		whip->pc_id = -1;
		return false;
	}

	blog(LOG_INFO, "[Daydream WHIP] Video track added: %d", whip->track_id);

	rtcSetLocalDescription(whip->pc_id, "offer");

	int timeout = 100;
	while (!whip->gathering_done && timeout > 0) {
		os_sleep_ms(100);
		timeout--;
	}

	if (!whip->gathering_done) {
		blog(LOG_ERROR, "[Daydream WHIP] ICE gathering timeout");
		rtcDeletePeerConnection(whip->pc_id);
		whip->pc_id = -1;
		return false;
	}

	char sdp_buffer[16384];
	int sdp_size = rtcGetLocalDescription(whip->pc_id, sdp_buffer, sizeof(sdp_buffer));

	if (sdp_size <= 0) {
		blog(LOG_ERROR, "[Daydream WHIP] Failed to get local description");
		rtcDeletePeerConnection(whip->pc_id);
		whip->pc_id = -1;
		return false;
	}

	std::string local_sdp(sdp_buffer, sdp_size);
	blog(LOG_INFO, "[Daydream WHIP] Local SDP created (%d bytes)", sdp_size);

	if (!send_whip_offer(whip, local_sdp)) {
		rtcDeletePeerConnection(whip->pc_id);
		whip->pc_id = -1;
		return false;
	}

	return true;
}

void daydream_whip_disconnect(struct daydream_whip *whip)
{
	if (!whip)
		return;

	if (whip->pc_id >= 0) {
		rtcDeletePeerConnection(whip->pc_id);
		whip->pc_id = -1;
	}

	whip->track_id = -1;
	whip->connected = false;
	whip->gathering_done = false;
	whip->resource_url.clear();

	blog(LOG_INFO, "[Daydream WHIP] Disconnected");
}

bool daydream_whip_is_connected(struct daydream_whip *whip)
{
	if (!whip)
		return false;
	return whip->connected;
}

bool daydream_whip_send_frame(struct daydream_whip *whip, const uint8_t *h264_data, size_t size, uint32_t timestamp_ms,
			      bool is_keyframe)
{
	UNUSED_PARAMETER(is_keyframe);

	if (!whip || !whip->connected || whip->track_id < 0)
		return false;

	if (!h264_data || size == 0)
		return false;

	uint32_t rtp_timestamp = (timestamp_ms * 90);

	const size_t MAX_RTP_PAYLOAD = 1200;
	const uint8_t *data = h264_data;
	size_t remaining = size;

	while (remaining > 0) {
		uint8_t rtp_packet[1500];
		size_t rtp_size = 0;

		rtp_packet[0] = 0x80;
		rtp_packet[1] = (remaining <= MAX_RTP_PAYLOAD) ? (0x80 | 96) : 96;
		rtp_packet[2] = (whip->seq_num >> 8) & 0xFF;
		rtp_packet[3] = whip->seq_num & 0xFF;
		rtp_packet[4] = (rtp_timestamp >> 24) & 0xFF;
		rtp_packet[5] = (rtp_timestamp >> 16) & 0xFF;
		rtp_packet[6] = (rtp_timestamp >> 8) & 0xFF;
		rtp_packet[7] = rtp_timestamp & 0xFF;
		rtp_packet[8] = (whip->ssrc >> 24) & 0xFF;
		rtp_packet[9] = (whip->ssrc >> 16) & 0xFF;
		rtp_packet[10] = (whip->ssrc >> 8) & 0xFF;
		rtp_packet[11] = whip->ssrc & 0xFF;

		rtp_size = 12;

		size_t payload_size = (remaining > MAX_RTP_PAYLOAD) ? MAX_RTP_PAYLOAD : remaining;
		memcpy(rtp_packet + rtp_size, data, payload_size);
		rtp_size += payload_size;

		int result = rtcSendMessage(whip->track_id, reinterpret_cast<const char *>(rtp_packet),
					    static_cast<int>(rtp_size));
		if (result < 0) {
			blog(LOG_WARNING, "[Daydream WHIP] Failed to send RTP packet");
			return false;
		}

		whip->seq_num++;
		data += payload_size;
		remaining -= payload_size;
	}

	return true;
}

const char *daydream_whip_get_whep_url(struct daydream_whip *whip)
{
	if (!whip || whip->whep_url.empty())
		return nullptr;
	return whip->whep_url.c_str();
}
