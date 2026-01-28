#include "daydream-whep.h"
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

struct daydream_whep {
	std::string whep_url;
	std::string api_key;
	std::string resource_url;

	daydream_whep_frame_callback on_frame;
	daydream_whep_state_callback on_state;
	void *userdata;

	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> track;

	std::atomic<bool> connected;
	std::atomic<bool> gathering_done;

	std::vector<uint8_t> frame_buffer;
	std::vector<uint8_t> fua_buffer;
	uint32_t current_timestamp;
	bool waiting_keyframe;
	bool fua_in_progress;
	uint8_t fua_nal_header;
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

	std::string header_lower = header;
	for (char &c : header_lower)
		c = (char)tolower((unsigned char)c);

	if (header_lower.find("location:") == 0) {
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

static bool send_whep_request_once(daydream_whep *whep, const std::string &sdp_offer, long *out_http_code)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return false;

	http_response *response = new http_response();

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/sdp");

	curl_easy_setopt(curl, CURLOPT_URL, whep->whep_url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, sdp_offer.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	CURLcode res = curl_easy_perform(curl);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (out_http_code)
		*out_http_code = http_code;

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		blog(LOG_INFO, "[Daydream WHEP] HTTP request failed: %s (url=%s)", curl_easy_strerror(res),
		     whep->whep_url.c_str());
		delete response;
		return false;
	}

	if (http_code != 200 && http_code != 201) {
		blog(LOG_INFO, "[Daydream WHEP] HTTP error: %ld (url=%s)", http_code, whep->whep_url.c_str());
		delete response;
		return false;
	}

	if (!response->location.empty()) {
		whep->resource_url = response->location;
		blog(LOG_INFO, "[Daydream WHEP] Resource URL: %s", whep->resource_url.c_str());
	}

	if (!response->data.empty()) {
		blog(LOG_INFO, "[Daydream WHEP] Setting remote description (answer)");
		whep->pc->setRemoteDescription(rtc::Description(response->data, rtc::Description::Type::Answer));
	}

	delete response;
	return true;
}

static bool send_whep_request(daydream_whep *whep, const std::string &sdp_offer)
{
	const int max_retries = 60;
	const int retry_delay_ms = 500;
	const int rate_limit_delay_ms = 2000;

	for (int retry = 0; retry < max_retries; retry++) {
		if (retry > 0) {
			blog(LOG_INFO, "[Daydream WHEP] Retry %d/%d...", retry, max_retries);
		}

		long http_code = 0;
		if (send_whep_request_once(whep, sdp_offer, &http_code)) {
			return true;
		}

		if (http_code == 429) {
			blog(LOG_INFO, "[Daydream WHEP] Rate limited, waiting %dms...", rate_limit_delay_ms);
			os_sleep_ms(rate_limit_delay_ms);
			continue;
		}

		if (http_code == 404 || http_code == 503 || http_code == 0) {
			os_sleep_ms(retry_delay_ms);
			continue;
		}

		blog(LOG_ERROR, "[Daydream WHEP] HTTP error %ld, not retrying", http_code);
		return false;
	}

	blog(LOG_ERROR, "[Daydream WHEP] Failed after %d retries", max_retries);
	return false;
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
	whep->connected = false;
	whep->gathering_done = false;
	whep->current_timestamp = 0;
	whep->waiting_keyframe = true;
	whep->fua_in_progress = false;
	whep->fua_nal_header = 0;

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

	rtc::Configuration config;

	whep->pc = std::make_shared<rtc::PeerConnection>(config);

	whep->pc->onStateChange([whep](rtc::PeerConnection::State state) {
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
		blog(LOG_INFO, "[Daydream WHEP] State changed: %s", state_str);

		if (state == rtc::PeerConnection::State::Connected) {
			whep->connected = true;
			if (whep->on_state)
				whep->on_state(true, nullptr, whep->userdata);
		} else if (state == rtc::PeerConnection::State::Disconnected ||
			   state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed) {
			whep->connected = false;
			if (whep->on_state)
				whep->on_state(false,
					       state == rtc::PeerConnection::State::Failed ? "Connection failed"
											   : nullptr,
					       whep->userdata);
		}
	});

	whep->pc->onGatheringStateChange([whep](rtc::PeerConnection::GatheringState state) {
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
			whep->gathering_done = true;
			break;
		}
		blog(LOG_INFO, "[Daydream WHEP] Gathering state: %s", state_str);
	});

	rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
	media.addH264Codec(96);

	whep->track = whep->pc->addTrack(media);

	blog(LOG_INFO, "[Daydream WHEP] Video track added (recvonly)");

	whep->track->onMessage([whep](rtc::message_variant data) {
		if (!std::holds_alternative<rtc::binary>(data))
			return;

		const auto &bin = std::get<rtc::binary>(data);
		int size = static_cast<int>(bin.size());

		static uint64_t msg_count = 0;
		static uint64_t last_log_time = 0;
		static uint64_t frames_decoded = 0;
		msg_count++;

		if (size <= 12)
			return;

		const uint8_t *rtp_data = reinterpret_cast<const uint8_t *>(bin.data());

		uint8_t pt = rtp_data[1] & 0x7F;
		bool marker = (rtp_data[1] & 0x80) != 0;

		if (pt < 96 || pt > 127)
			return;

		uint32_t timestamp = (rtp_data[4] << 24) | (rtp_data[5] << 16) | (rtp_data[6] << 8) | rtp_data[7];

		int header_size = 12;
		int cc = rtp_data[0] & 0x0F;
		header_size += cc * 4;

		if (rtp_data[0] & 0x10) {
			if (size > header_size + 4) {
				int ext_len = (rtp_data[header_size + 2] << 8) | rtp_data[header_size + 3];
				header_size += 4 + ext_len * 4;
			}
		}

		if (size <= header_size)
			return;

		const uint8_t *payload = rtp_data + header_size;
		int payload_size = size - header_size;

		if (payload_size <= 0)
			return;

		if (whep->current_timestamp != timestamp) {
			whep->frame_buffer.clear();
			whep->current_timestamp = timestamp;
		}

		uint8_t nal_type = payload[0] & 0x1F;
		uint8_t nri = payload[0] & 0x60;

		auto append_nal = [whep](const uint8_t *nal_data, int nal_size) {
			uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
			whep->frame_buffer.insert(whep->frame_buffer.end(), start_code, start_code + 4);
			whep->frame_buffer.insert(whep->frame_buffer.end(), nal_data, nal_data + nal_size);
		};

		if (nal_type >= 1 && nal_type <= 23) {
			append_nal(payload, payload_size);
		} else if (nal_type == 24) {
			int offset = 1;
			while (offset + 2 <= payload_size) {
				int nal_len = (payload[offset] << 8) | payload[offset + 1];
				offset += 2;
				if (offset + nal_len > payload_size)
					break;
				append_nal(payload + offset, nal_len);
				offset += nal_len;
			}
		} else if (nal_type == 28) {
			if (payload_size < 2)
				return;

			uint8_t fu_header = payload[1];
			bool start_bit = (fu_header & 0x80) != 0;
			bool end_bit = (fu_header & 0x40) != 0;
			uint8_t fu_nal_type = fu_header & 0x1F;

			if (start_bit) {
				whep->fua_buffer.clear();
				whep->fua_nal_header = (nri | fu_nal_type);
				whep->fua_buffer.push_back(whep->fua_nal_header);
				whep->fua_in_progress = true;
			}

			if (whep->fua_in_progress && payload_size > 2) {
				whep->fua_buffer.insert(whep->fua_buffer.end(), payload + 2, payload + payload_size);
			}

			if (end_bit && whep->fua_in_progress) {
				append_nal(whep->fua_buffer.data(), (int)whep->fua_buffer.size());
				whep->fua_buffer.clear();
				whep->fua_in_progress = false;
			}
		}

		if (marker && !whep->frame_buffer.empty()) {
			bool has_keyframe = false;
			const uint8_t *buf = whep->frame_buffer.data();
			size_t buf_size = whep->frame_buffer.size();

			for (size_t i = 0; i + 4 < buf_size; i++) {
				if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
					uint8_t nt = buf[i + 4] & 0x1F;
					if (nt == 5 || nt == 7 || nt == 8) {
						has_keyframe = true;
						break;
					}
				}
			}

			if (whep->waiting_keyframe && !has_keyframe) {
				whep->frame_buffer.clear();
				return;
			}

			if (has_keyframe)
				whep->waiting_keyframe = false;

			frames_decoded++;
			uint64_t now = os_gettime_ns();
			if (now - last_log_time > 1000000000ULL) {
				blog(LOG_INFO,
				     "[Daydream WHEP] Received %llu pkts, decoded %llu frames, last=%zu bytes",
				     (unsigned long long)msg_count, (unsigned long long)frames_decoded,
				     whep->frame_buffer.size());
				last_log_time = now;
			}

			if (whep->on_frame) {
				whep->on_frame(whep->frame_buffer.data(), (int)whep->frame_buffer.size(), timestamp,
					       has_keyframe, whep->userdata);
			}

			whep->frame_buffer.clear();
		}
	});

	blog(LOG_INFO, "[Daydream WHEP] Message callback set on track");

	whep->pc->setLocalDescription();

	whep->gathering_done = false;
	int timeout = 100;
	while (!whep->gathering_done && timeout > 0) {
		os_sleep_ms(100);
		timeout--;
	}

	if (!whep->gathering_done) {
		blog(LOG_ERROR, "[Daydream WHEP] ICE gathering timeout");
		whep->pc.reset();
		return false;
	}

	auto localDesc = whep->pc->localDescription();
	if (!localDesc) {
		blog(LOG_ERROR, "[Daydream WHEP] Failed to get local description");
		whep->pc.reset();
		return false;
	}

	std::string sdp = std::string(*localDesc);
	blog(LOG_INFO, "[Daydream WHEP] Local SDP created (%zu bytes):\n%s", sdp.size(), sdp.c_str());

	if (!send_whep_request(whep, sdp)) {
		whep->pc.reset();
		return false;
	}

	return true;
}

void daydream_whep_disconnect(struct daydream_whep *whep)
{
	if (!whep)
		return;

	if (whep->pc) {
		whep->pc->close();
		whep->pc.reset();
	}

	whep->track.reset();
	whep->connected = false;
	whep->gathering_done = false;
	whep->resource_url.clear();

	blog(LOG_INFO, "[Daydream WHEP] Disconnected");
}

bool daydream_whep_is_connected(struct daydream_whep *whep)
{
	if (!whep)
		return false;
	return whep->connected;
}
