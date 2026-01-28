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
#include <set>

class NackSender : public rtc::MediaHandler {
public:
	NackSender() : mSsrc(0), mInitialized(false), mLastSeq(0), mPacketCount(0), mLastLogTime(0) {}

	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override
	{
		std::vector<uint16_t> missing;

		for (const auto &message : messages) {
			if (message->type == rtc::Message::Control)
				continue;

			if (message->size() < sizeof(rtc::RtpHeader))
				continue;

			auto rtp = reinterpret_cast<const rtc::RtpHeader *>(message->data());
			uint16_t seq = rtp->seqNumber();
			mSsrc = rtp->ssrc();
			mPacketCount++;

			if (!mInitialized) {
				mInitialized = true;
				mLastSeq = seq;
				continue;
			}

			int16_t diff = static_cast<int16_t>(seq - mLastSeq);

			if (diff > 1 && diff < 100) {
				for (uint16_t s = mLastSeq + 1; s != seq; ++s) {
					if (mRequested.find(s) == mRequested.end()) {
						missing.push_back(s);
						mRequested.insert(s);
					}
				}
			}

			if (diff > 0)
				mLastSeq = seq;

			if (mRequested.size() > 1000)
				mRequested.clear();
		}

		if (!missing.empty() && mSsrc != 0) {
			sendNack(missing, send);
		}

		uint64_t now = os_gettime_ns();
		if (now - mLastLogTime > 5000000000ULL) {
			blog(LOG_INFO, "[Daydream WHEP NackSender] packets=%llu lastSeq=%u ssrc=0x%x",
			     (unsigned long long)mPacketCount, mLastSeq, mSsrc);
			mLastLogTime = now;
		}
	}

private:
	void sendNack(const std::vector<uint16_t> &seqs, const rtc::message_callback &send)
	{
		size_t maxFci = (seqs.size() + 16) / 17 + 1;
		size_t bufSize = rtc::RtcpNack::Size((unsigned int)maxFci);
		auto msg = rtc::make_message(bufSize, rtc::Message::Control);
		auto nack = reinterpret_cast<rtc::RtcpNack *>(msg->data());
		memset(msg->data(), 0, bufSize);

		unsigned int fciCount = 0;
		uint16_t fciPid = 0;

		for (uint16_t seq : seqs) {
			nack->addMissingPacket(&fciCount, &fciPid, seq);
		}

		nack->preparePacket(mSsrc, fciCount);

		size_t actualSize = rtc::RtcpNack::Size(fciCount);
		msg->resize(actualSize);

		send(msg);

		blog(LOG_INFO, "[Daydream WHEP] Sent NACK for %zu missing packets (fci=%u)", seqs.size(), fciCount);
	}

	uint32_t mSsrc;
	bool mInitialized;
	uint16_t mLastSeq;
	std::set<uint16_t> mRequested;
	uint64_t mPacketCount;
	uint64_t mLastLogTime;
};

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

	auto rtcpSession = std::make_shared<rtc::RtcpReceivingSession>();
	auto nackSender = std::make_shared<NackSender>();
	auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();

	depacketizer->addToChain(nackSender);
	nackSender->addToChain(rtcpSession);
	whep->track->setMediaHandler(depacketizer);

	blog(LOG_INFO, "[Daydream WHEP] Video track with H264RtpDepacketizer -> NackSender -> RtcpReceivingSession");

	whep->track->onFrame([whep](rtc::binary data, rtc::FrameInfo info) {
		int size = static_cast<int>(data.size());
		if (size <= 4)
			return;

		const uint8_t *frame_data = reinterpret_cast<const uint8_t *>(data.data());

		static uint64_t frame_count = 0;
		static uint64_t last_log = 0;
		frame_count++;

		int nal_count = 0;
		int sps_count = 0, pps_count = 0, idr_count = 0, non_idr_count = 0;
		bool has_idr = false;

		for (int i = 0; i + 4 < size; i++) {
			if (frame_data[i] == 0 && frame_data[i + 1] == 0 && frame_data[i + 2] == 0 &&
			    frame_data[i + 3] == 1) {
				nal_count++;
				uint8_t nal_type = frame_data[i + 4] & 0x1F;
				if (nal_type == 7)
					sps_count++;
				else if (nal_type == 8)
					pps_count++;
				else if (nal_type == 5) {
					idr_count++;
					has_idr = true;
				} else if (nal_type == 1)
					non_idr_count++;
			}
		}

		if (whep->waiting_keyframe && !has_idr)
			return;
		if (has_idr)
			whep->waiting_keyframe = false;

		uint64_t now = os_gettime_ns();
		if (now - last_log > 1000000000ULL) {
			blog(LOG_INFO, "[Daydream WHEP] frames=%llu size=%d nals=%d (sps=%d pps=%d idr=%d p=%d) ts=%u",
			     (unsigned long long)frame_count, size, nal_count, sps_count, pps_count, idr_count,
			     non_idr_count, info.timestamp);
			last_log = now;
		}

		if (whep->on_frame)
			whep->on_frame(frame_data, size, info.timestamp, has_idr, whep->userdata);
	});

	blog(LOG_INFO, "[Daydream WHEP] Frame callback set on track");

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

bool daydream_whep_request_keyframe(struct daydream_whep *whep)
{
	if (!whep || !whep->track || !whep->connected)
		return false;

	bool result = whep->track->requestKeyframe();
	if (result)
		blog(LOG_INFO, "[Daydream WHEP] Requested keyframe (PLI sent)");
	return result;
}
