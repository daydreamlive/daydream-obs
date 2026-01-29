#include "daydream-whip.h"
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <curl/curl.h>

#include <rtc/rtc.hpp>

#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>
#include <memory>
#include <chrono>

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

	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> track;
	std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;

	std::atomic<bool> connected;
	std::atomic<bool> gathering_done;

	uint32_t ssrc;
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
		whip->pc->setRemoteDescription(rtc::Description(response->data, rtc::Description::Type::Answer));
	}

	delete response;
	return true;
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
	whip->connected = false;
	whip->gathering_done = false;
	whip->ssrc = 12345678;

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

	rtc::Configuration config;

	whip->pc = std::make_shared<rtc::PeerConnection>(config);

	whip->pc->onStateChange([whip](rtc::PeerConnection::State state) {
		const char *state_str = "unknown";
		switch (state) {
		case rtc::PeerConnection::State::New:
			state_str = "new";
			break;
		case rtc::PeerConnection::State::Connecting:
			state_str = "connecting";
			break;
		case rtc::PeerConnection::State::Connected:
			state_str = "connected";
			break;
		case rtc::PeerConnection::State::Disconnected:
			state_str = "disconnected";
			break;
		case rtc::PeerConnection::State::Failed:
			state_str = "failed";
			break;
		case rtc::PeerConnection::State::Closed:
			state_str = "closed";
			break;
		}
		blog(LOG_INFO, "[Daydream WHIP] State changed: %s", state_str);

		if (state == rtc::PeerConnection::State::Connected) {
			whip->connected = true;
			if (whip->on_state)
				whip->on_state(true, nullptr, whip->userdata);
		} else if (state == rtc::PeerConnection::State::Disconnected ||
			   state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed) {
			whip->connected = false;
			if (whip->on_state)
				whip->on_state(false,
					       state == rtc::PeerConnection::State::Failed ? "Connection failed"
											   : nullptr,
					       whip->userdata);
		}
	});

	whip->pc->onGatheringStateChange([whip](rtc::PeerConnection::GatheringState state) {
		const char *state_str = "unknown";
		switch (state) {
		case rtc::PeerConnection::GatheringState::New:
			state_str = "new";
			break;
		case rtc::PeerConnection::GatheringState::InProgress:
			state_str = "in-progress";
			break;
		case rtc::PeerConnection::GatheringState::Complete:
			state_str = "complete";
			whip->gathering_done = true;
			break;
		}
		blog(LOG_INFO, "[Daydream WHIP] Gathering state: %s", state_str);
	});

	rtc::Description::Video videoMedia("video", rtc::Description::Direction::SendOnly);
	videoMedia.addH264Codec(96);
	videoMedia.addSSRC(whip->ssrc, "daydream");

	whip->track = whip->pc->addTrack(videoMedia);

	whip->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(whip->ssrc, "daydream", 96,
									rtc::H264RtpPacketizer::defaultClockRate);

	auto packetizer =
		std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, whip->rtpConfig);

	whip->track->setMediaHandler(packetizer);

	whip->track->onOpen([whip]() { blog(LOG_INFO, "[Daydream WHIP] Video track opened"); });

	blog(LOG_INFO, "[Daydream WHIP] Video track added");

	whip->pc->setLocalDescription();

	whip->gathering_done = false;
	int timeout = 100;
	while (!whip->gathering_done && timeout > 0) {
		os_sleep_ms(100);
		timeout--;
	}

	if (!whip->gathering_done) {
		blog(LOG_ERROR, "[Daydream WHIP] ICE gathering timeout");
		whip->pc.reset();
		return false;
	}

	auto localDesc = whip->pc->localDescription();
	if (!localDesc) {
		blog(LOG_ERROR, "[Daydream WHIP] Failed to get local description");
		whip->pc.reset();
		return false;
	}

	std::string sdp = std::string(*localDesc);
	blog(LOG_INFO, "[Daydream WHIP] Local SDP created (%zu bytes):\n%s", sdp.size(), sdp.c_str());

	if (!send_whip_offer(whip, sdp)) {
		whip->pc.reset();
		return false;
	}

	return true;
}

void daydream_whip_disconnect(struct daydream_whip *whip)
{
	if (!whip)
		return;

	if (whip->pc) {
		whip->pc->close();
		whip->pc.reset();
	}

	whip->track.reset();
	whip->rtpConfig.reset();
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

	if (!whip || !whip->connected || !whip->track) {
		static int log_count = 0;
		if (log_count++ < 5) {
			blog(LOG_WARNING, "[Daydream WHIP] send_frame skip: whip=%p connected=%d track=%p",
			     (void *)whip, whip ? (int)whip->connected.load() : -1, whip ? whip->track.get() : nullptr);
		}
		return false;
	}

	if (!h264_data || size == 0)
		return false;

	try {
		uint32_t rtp_timestamp = static_cast<uint32_t>((timestamp_ms * 90ULL) % UINT32_MAX);
		whip->rtpConfig->timestamp = rtp_timestamp;
		whip->track->send(reinterpret_cast<const std::byte *>(h264_data), size);
		return true;
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[Daydream WHIP] Failed to send frame: %s", e.what());
		return false;
	}
}

const char *daydream_whip_get_whep_url(struct daydream_whip *whip)
{
	if (!whip || whip->whep_url.empty())
		return nullptr;
	return whip->whep_url.c_str();
}
