#include "daydream-whep.h"
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

struct daydream_whep {
	std::string whep_url;
	std::string api_key;
	std::string resource_url;

	daydream_whep_frame_callback on_frame;
	daydream_whep_state_callback on_state;
	void *userdata;

	int pc_id;
	int track_id;

	std::atomic<bool> connected;
	std::atomic<bool> gathering_done;
	std::mutex mutex;

	std::vector<uint8_t> frame_buffer;
	uint32_t current_timestamp;
	bool waiting_keyframe;
};

struct http_response {
	std::string data;
	std::string location;
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
	if (header.find("Location:") == 0 || header.find("location:") == 0) {
		size_t pos = header.find(':');
		if (pos != std::string::npos) {
			std::string value = header.substr(pos + 1);
			while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
				value.erase(0, 1);
			while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
				value.pop_back();
			resp->location = value;
		}
	}
	return realsize;
}

static void on_state_change(int, rtcState state, void *ptr)
{
	daydream_whep *whep = static_cast<daydream_whep *>(ptr);

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

	blog(LOG_INFO, "[Daydream WHEP] State changed: %s", state_str);

	if (state == RTC_CONNECTED) {
		whep->connected = true;
		if (whep->on_state)
			whep->on_state(true, nullptr, whep->userdata);
	} else if (state == RTC_DISCONNECTED || state == RTC_FAILED || state == RTC_CLOSED) {
		whep->connected = false;
		if (whep->on_state)
			whep->on_state(false, state == RTC_FAILED ? "Connection failed" : nullptr, whep->userdata);
	}
}

static void on_gathering_state_change(int, rtcGatheringState state, void *ptr)
{
	daydream_whep *whep = static_cast<daydream_whep *>(ptr);

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
		whep->gathering_done = true;
		break;
	}
	blog(LOG_INFO, "[Daydream WHEP] Gathering state: %s", state_str);
}

static void on_track(int, int tr, void *ptr)
{
	daydream_whep *whep = static_cast<daydream_whep *>(ptr);

	blog(LOG_INFO, "[Daydream WHEP] Remote track added: %d", tr);
	whep->track_id = tr;
	whep->waiting_keyframe = true;

	rtcSetMessageCallback(tr, on_message);
}

static void on_message(int, const char *message, int size, void *ptr)
{
	daydream_whep *whep = static_cast<daydream_whep *>(ptr);

	if (size <= 0)
		return;

	const uint8_t *data = reinterpret_cast<const uint8_t *>(message);

	bool is_keyframe = false;
	if (size > 12) {
		uint32_t timestamp = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];

		int header_size = 12;
		int cc = data[0] & 0x0F;
		header_size += cc * 4;

		if (data[0] & 0x10) {
			if (size > header_size + 4) {
				int ext_len = (data[header_size + 2] << 8) | data[header_size + 3];
				header_size += 4 + ext_len * 4;
			}
		}

		if (size > header_size) {
			const uint8_t *payload = data + header_size;
			int payload_size = size - header_size;

			if (payload_size > 0) {
				uint8_t nal_type = payload[0] & 0x1F;
				is_keyframe = (nal_type == 5 || nal_type == 7 || nal_type == 8);

				if (whep->waiting_keyframe && !is_keyframe)
					return;

				if (is_keyframe)
					whep->waiting_keyframe = false;

				if (whep->on_frame) {
					whep->on_frame(payload, payload_size, timestamp, is_keyframe, whep->userdata);
				}
			}
		}
	}
}

static bool send_whep_request(daydream_whep *whep, const std::string &sdp_answer)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return false;

	http_response *response = new http_response();

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/sdp");

	std::string auth_header = "Authorization: Bearer " + whep->api_key;
	headers = curl_slist_append(headers, auth_header.c_str());

	curl_easy_setopt(curl, CURLOPT_URL, whep->whep_url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, sdp_answer.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	CURLcode res = curl_easy_perform(curl);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		blog(LOG_ERROR, "[Daydream WHEP] HTTP request failed: %s", curl_easy_strerror(res));
		delete response;
		return false;
	}

	if (http_code != 200 && http_code != 201) {
		blog(LOG_ERROR, "[Daydream WHEP] HTTP error: %ld", http_code);
		delete response;
		return false;
	}

	if (!response->location.empty()) {
		whep->resource_url = response->location;
		blog(LOG_INFO, "[Daydream WHEP] Resource URL: %s", whep->resource_url.c_str());
	}

	if (!response->data.empty()) {
		blog(LOG_INFO, "[Daydream WHEP] Setting remote description (answer)");
		rtcSetRemoteDescription(whep->pc_id, response->data.c_str(), "answer");
	}

	delete response;
	return true;
}

struct daydream_whep *daydream_whep_create(const struct daydream_whep_config *config)
{
	if (!config || !config->whep_url)
		return nullptr;

	daydream_whep *whep = new daydream_whep();
	whep->whep_url = config->whep_url;
	whep->api_key = config->api_key ? config->api_key : "";
	whep->on_frame = config->on_frame;
	whep->on_state = config->on_state;
	whep->userdata = config->userdata;
	whep->pc_id = -1;
	whep->track_id = -1;
	whep->connected = false;
	whep->gathering_done = false;
	whep->waiting_keyframe = true;

	return whep;
}

void daydream_whep_destroy(struct daydream_whep *whep)
{
	if (!whep)
		return;

	daydream_whep_disconnect(whep);
	delete whep;
}

bool daydream_whep_connect(struct daydream_whep *whep)
{
	if (!whep)
		return false;

	blog(LOG_INFO, "[Daydream WHEP] Connecting to %s", whep->whep_url.c_str());

	rtcConfiguration config = {};
	config.iceServers = nullptr;
	config.iceServersCount = 0;

	whep->pc_id = rtcCreatePeerConnection(&config);
	if (whep->pc_id < 0) {
		blog(LOG_ERROR, "[Daydream WHEP] Failed to create peer connection");
		return false;
	}

	rtcSetUserPointer(whep->pc_id, whep);
	rtcSetStateChangeCallback(whep->pc_id, on_state_change);
	rtcSetGatheringStateChangeCallback(whep->pc_id, on_gathering_state_change);
	rtcSetTrackCallback(whep->pc_id, on_track);

	rtcTrackInit track_init = {};
	track_init.direction = RTC_DIRECTION_RECVONLY;
	track_init.codec = RTC_CODEC_H264;
	track_init.payloadType = 96;
	track_init.ssrc = 0;
	track_init.mid = "0";
	track_init.name = "video";
	track_init.msid = "daydream";
	track_init.trackId = "video";

	int recv_track = rtcAddTrackEx(whep->pc_id, &track_init);
	if (recv_track < 0) {
		blog(LOG_ERROR, "[Daydream WHEP] Failed to add video track");
		rtcDeletePeerConnection(whep->pc_id);
		whep->pc_id = -1;
		return false;
	}

	blog(LOG_INFO, "[Daydream WHEP] Local video track added: %d", recv_track);

	rtcSetLocalDescription(whep->pc_id, "offer");

	whep->gathering_done = false;
	int timeout = 100;
	while (!whep->gathering_done && timeout > 0) {
		os_sleep_ms(100);
		timeout--;
	}

	if (!whep->gathering_done) {
		blog(LOG_ERROR, "[Daydream WHEP] ICE gathering timeout");
		rtcDeletePeerConnection(whep->pc_id);
		whep->pc_id = -1;
		return false;
	}

	char sdp_buffer[16384];
	int sdp_size = rtcGetLocalDescription(whep->pc_id, sdp_buffer, sizeof(sdp_buffer));

	if (sdp_size <= 0) {
		blog(LOG_ERROR, "[Daydream WHEP] Failed to get local description");
		rtcDeletePeerConnection(whep->pc_id);
		whep->pc_id = -1;
		return false;
	}

	std::string local_sdp(sdp_buffer, sdp_size);
	blog(LOG_INFO, "[Daydream WHEP] Local SDP created (%d bytes)", sdp_size);

	if (!send_whep_request(whep, local_sdp)) {
		rtcDeletePeerConnection(whep->pc_id);
		whep->pc_id = -1;
		return false;
	}

	return true;
}

void daydream_whep_disconnect(struct daydream_whep *whep)
{
	if (!whep)
		return;

	if (whep->pc_id >= 0) {
		rtcDeletePeerConnection(whep->pc_id);
		whep->pc_id = -1;
	}

	whep->track_id = -1;
	whep->connected = false;
	whep->gathering_done = false;

	if (!whep->resource_url.empty()) {
		CURL *curl = curl_easy_init();
		if (curl) {
			struct curl_slist *headers = nullptr;
			std::string auth_header = "Authorization: Bearer " + whep->api_key;
			headers = curl_slist_append(headers, auth_header.c_str());

			curl_easy_setopt(curl, CURLOPT_URL, whep->resource_url.c_str());
			curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

			curl_easy_perform(curl);

			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		}
		whep->resource_url.clear();
	}

	blog(LOG_INFO, "[Daydream WHEP] Disconnected");
}

bool daydream_whep_is_connected(struct daydream_whep *whep)
{
	if (!whep)
		return false;
	return whep->connected;
}
