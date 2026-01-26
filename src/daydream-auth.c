#include "daydream-auth.h"
#include "daydream-api.h"
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define close_socket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int socket_t;
#define INVALID_SOCK -1
#define close_socket close
#endif

#define CREDENTIALS_PATH "/.daydream/credentials"
#define AUTH_STATE_LEN 32
#define AUTH_TIMEOUT_SEC 300

struct daydream_auth {
	char *api_key;
	bool logged_in;

	socket_t server_socket;
	int server_port;
	char auth_state[AUTH_STATE_LEN + 1];

	pthread_t auth_thread;
	bool auth_thread_running;
	volatile bool auth_cancelled;

	daydream_auth_callback callback;
	void *callback_userdata;

	pthread_mutex_t mutex;
};

static void generate_random_state(char *buf, size_t len)
{
	static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	for (size_t i = 0; i < len; i++) {
		buf[i] = charset[rand() % (sizeof(charset) - 1)];
	}
	buf[len] = '\0';
}

static char *get_credentials_path(void)
{
	char *home = getenv("HOME");
	if (!home) {
#ifdef _WIN32
		home = getenv("USERPROFILE");
#endif
	}
	if (!home)
		return NULL;

	size_t len = strlen(home) + strlen(CREDENTIALS_PATH) + 1;
	char *path = malloc(len);
	if (path)
		snprintf(path, len, "%s%s", home, CREDENTIALS_PATH);
	return path;
}

static bool ensure_credentials_dir(void)
{
	char *home = getenv("HOME");
	if (!home) {
#ifdef _WIN32
		home = getenv("USERPROFILE");
#endif
	}
	if (!home)
		return false;

	char dir[512];
	snprintf(dir, sizeof(dir), "%s/.daydream", home);

	os_mkdirs(dir);
	return true;
}

static char *extract_param(const char *query, const char *param)
{
	char search[128];
	snprintf(search, sizeof(search), "%s=", param);

	const char *start = strstr(query, search);
	if (!start)
		return NULL;

	start += strlen(search);
	const char *end = start;
	while (*end && *end != '&' && *end != ' ' && *end != '\r' && *end != '\n')
		end++;

	size_t len = end - start;
	char *value = malloc(len + 1);
	if (value) {
		memcpy(value, start, len);
		value[len] = '\0';
	}
	return value;
}

static void open_browser(const char *url)
{
#ifdef __APPLE__
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "open \"%s\"", url);
	system(cmd);
#elif defined(_WIN32)
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", url);
	system(cmd);
#else
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "xdg-open \"%s\"", url);
	system(cmd);
#endif
}

struct auth_response_buffer {
	char *data;
	size_t size;
};

static size_t auth_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct auth_response_buffer *buf = userp;
	char *ptr = realloc(buf->data, buf->size + realsize + 1);
	if (!ptr)
		return 0;
	buf->data = ptr;
	memcpy(&buf->data[buf->size], contents, realsize);
	buf->size += realsize;
	buf->data[buf->size] = 0;
	return realsize;
}

static char *exchange_jwt_for_api_key(const char *jwt_token)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return NULL;

	struct auth_response_buffer response = {0};

	char auth_header[2048];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", jwt_token);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, auth_header);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "x-client-source: obs");

	const char *body = "{\"name\":\"OBS Studio\",\"user_type\":\"obs\"}";

	curl_easy_setopt(curl, CURLOPT_URL, "https://api.daydream.live/v1/api-key");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, auth_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	CURLcode res = curl_easy_perform(curl);

	char *api_key = NULL;
	if (res == CURLE_OK && response.data) {
		const char *key_start = strstr(response.data, "\"apiKey\"");
		if (key_start) {
			key_start = strchr(key_start + 8, '"');
			if (key_start) {
				key_start++;
				const char *key_end = strchr(key_start, '"');
				if (key_end) {
					size_t len = key_end - key_start;
					api_key = malloc(len + 1);
					if (api_key) {
						memcpy(api_key, key_start, len);
						api_key[len] = '\0';
					}
				}
			}
		}
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(response.data);

	return api_key;
}

static void send_http_response(socket_t client, int status, const char *body)
{
	char response[4096];
	const char *status_text = (status == 200)   ? "OK"
				  : (status == 302) ? "Found"
				  : (status == 400) ? "Bad Request"
						    : "Error";

	if (status == 302) {
		snprintf(response, sizeof(response),
			 "HTTP/1.1 302 Found\r\n"
			 "Location: https://app.daydream.live/sign-in/local/success\r\n"
			 "Content-Length: 0\r\n"
			 "Connection: close\r\n"
			 "\r\n");
	} else {
		int body_len = body ? (int)strlen(body) : 0;
		snprintf(response, sizeof(response),
			 "HTTP/1.1 %d %s\r\n"
			 "Content-Type: text/html\r\n"
			 "Content-Length: %d\r\n"
			 "Connection: close\r\n"
			 "\r\n"
			 "%s",
			 status, status_text, body_len, body ? body : "");
	}

	send(client, response, (int)strlen(response), 0);
}

static void *auth_thread_func(void *data)
{
	struct daydream_auth *auth = data;

	struct timeval tv;
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	setsockopt(auth->server_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

	time_t start_time = time(NULL);

	while (!auth->auth_cancelled) {
		if (time(NULL) - start_time > AUTH_TIMEOUT_SEC) {
			blog(LOG_WARNING, "[Daydream] Auth timeout");
			if (auth->callback)
				auth->callback(false, NULL, "Login timeout", auth->callback_userdata);
			break;
		}

		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		socket_t client = accept(auth->server_socket, (struct sockaddr *)&client_addr, &client_len);

		if (client == INVALID_SOCK)
			continue;

		char buffer[4096] = {0};
		recv(client, buffer, sizeof(buffer) - 1, 0);

		if (strstr(buffer, "GET /callback") || strstr(buffer, "GET /?")) {
			char *token = extract_param(buffer, "token");
			char *state = extract_param(buffer, "state");

			bool valid = token && state && strcmp(state, auth->auth_state) == 0;

			if (valid) {
				blog(LOG_INFO, "[Daydream] Received valid auth callback");

				char *api_key = exchange_jwt_for_api_key(token);

				if (api_key) {
					pthread_mutex_lock(&auth->mutex);
					free(auth->api_key);
					auth->api_key = bstrdup(api_key);
					auth->logged_in = true;
					pthread_mutex_unlock(&auth->mutex);

					daydream_auth_save_credentials(auth, api_key);

					send_http_response(client, 302, NULL);

					blog(LOG_INFO, "[Daydream] Login successful");
					if (auth->callback)
						auth->callback(true, api_key, NULL, auth->callback_userdata);

					free(api_key);
				} else {
					send_http_response(client, 400, "<h1>Failed to create API key</h1>");
					if (auth->callback)
						auth->callback(false, NULL, "Failed to create API key",
							       auth->callback_userdata);
				}

				free(token);
				free(state);
				close_socket(client);
				break;
			} else {
				send_http_response(client, 400, "<h1>Invalid state</h1>");
				free(token);
				free(state);
			}
		}

		close_socket(client);
	}

	close_socket(auth->server_socket);
	auth->server_socket = INVALID_SOCK;
	auth->auth_thread_running = false;

	return NULL;
}

struct daydream_auth *daydream_auth_create(void)
{
	struct daydream_auth *auth = bzalloc(sizeof(struct daydream_auth));
	auth->server_socket = INVALID_SOCK;
	pthread_mutex_init(&auth->mutex, NULL);

	srand((unsigned int)time(NULL));

	daydream_auth_load_credentials(auth);

	return auth;
}

void daydream_auth_destroy(struct daydream_auth *auth)
{
	if (!auth)
		return;

	auth->auth_cancelled = true;

	if (auth->auth_thread_running) {
		pthread_join(auth->auth_thread, NULL);
	}

	if (auth->server_socket != INVALID_SOCK) {
		close_socket(auth->server_socket);
	}

	pthread_mutex_destroy(&auth->mutex);
	bfree(auth->api_key);
	bfree(auth);
}

bool daydream_auth_is_logged_in(struct daydream_auth *auth)
{
	if (!auth)
		return false;

	pthread_mutex_lock(&auth->mutex);
	bool result = auth->logged_in;
	pthread_mutex_unlock(&auth->mutex);

	return result;
}

const char *daydream_auth_get_api_key(struct daydream_auth *auth)
{
	if (!auth)
		return NULL;
	return auth->api_key;
}

static void cancel_pending_login(struct daydream_auth *auth)
{
	if (!auth->auth_thread_running)
		return;

	blog(LOG_INFO, "[Daydream] Cancelling previous login attempt");
	auth->auth_cancelled = true;

	if (auth->server_socket != INVALID_SOCK) {
		close_socket(auth->server_socket);
		auth->server_socket = INVALID_SOCK;
	}

	pthread_join(auth->auth_thread, NULL);
	auth->auth_thread_running = false;
}

void daydream_auth_login(struct daydream_auth *auth, daydream_auth_callback callback, void *userdata)
{
	if (!auth)
		return;

	cancel_pending_login(auth);

	auth->callback = callback;
	auth->callback_userdata = userdata;
	auth->auth_cancelled = false;

	auth->server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (auth->server_socket == INVALID_SOCK) {
		blog(LOG_ERROR, "[Daydream] Failed to create socket");
		if (callback)
			callback(false, NULL, "Failed to create socket", userdata);
		return;
	}

	int opt = 1;
	setsockopt(auth->server_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(auth->server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		blog(LOG_ERROR, "[Daydream] Failed to bind socket");
		close_socket(auth->server_socket);
		auth->server_socket = INVALID_SOCK;
		if (callback)
			callback(false, NULL, "Failed to bind socket", userdata);
		return;
	}

	socklen_t addr_len = sizeof(addr);
	getsockname(auth->server_socket, (struct sockaddr *)&addr, &addr_len);
	auth->server_port = ntohs(addr.sin_port);

	if (listen(auth->server_socket, 1) < 0) {
		blog(LOG_ERROR, "[Daydream] Failed to listen on socket");
		close_socket(auth->server_socket);
		auth->server_socket = INVALID_SOCK;
		if (callback)
			callback(false, NULL, "Failed to listen", userdata);
		return;
	}

	generate_random_state(auth->auth_state, AUTH_STATE_LEN);

	auth->auth_thread_running = true;
	pthread_create(&auth->auth_thread, NULL, auth_thread_func, auth);

	char url[512];
	snprintf(url, sizeof(url), "https://app.daydream.live/sign-in/local?port=%d&state=%s", auth->server_port,
		 auth->auth_state);

	blog(LOG_INFO, "[Daydream] Opening browser for login: %s", url);
	open_browser(url);
}

void daydream_auth_logout(struct daydream_auth *auth)
{
	if (!auth)
		return;

	pthread_mutex_lock(&auth->mutex);
	bfree(auth->api_key);
	auth->api_key = NULL;
	auth->logged_in = false;
	pthread_mutex_unlock(&auth->mutex);

	char *path = get_credentials_path();
	if (path) {
		remove(path);
		free(path);
	}

	blog(LOG_INFO, "[Daydream] Logged out");
}

bool daydream_auth_load_credentials(struct daydream_auth *auth)
{
	if (!auth)
		return false;

	char *path = get_credentials_path();
	if (!path)
		return false;

	FILE *f = fopen(path, "r");
	free(path);

	if (!f)
		return false;

	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "DAYDREAM_API_KEY:", 17) == 0) {
			char *key = line + 17;
			while (*key == ' ' || *key == '\t')
				key++;

			size_t len = strlen(key);
			while (len > 0 && (key[len - 1] == '\n' || key[len - 1] == '\r' || key[len - 1] == ' '))
				len--;
			key[len] = '\0';

			if (len > 0) {
				pthread_mutex_lock(&auth->mutex);
				bfree(auth->api_key);
				auth->api_key = bstrdup(key);
				auth->logged_in = true;
				pthread_mutex_unlock(&auth->mutex);

				blog(LOG_INFO, "[Daydream] Loaded credentials from file");
				fclose(f);
				return true;
			}
		}
	}

	fclose(f);
	return false;
}

bool daydream_auth_save_credentials(struct daydream_auth *auth, const char *api_key)
{
	UNUSED_PARAMETER(auth);

	if (!ensure_credentials_dir())
		return false;

	char *path = get_credentials_path();
	if (!path)
		return false;

	FILE *f = fopen(path, "w");
	free(path);

	if (!f)
		return false;

	fprintf(f, "DAYDREAM_API_KEY: %s\n", api_key);
	fclose(f);

	blog(LOG_INFO, "[Daydream] Saved credentials to file");
	return true;
}
