#pragma once
#include "Arduino.h"
#include "WiFi.h"
#include <functional>
#include <map>
#include <memory>
#include <vector>
#define RESPONSE_TRY_AGAIN 0xFFFFFFFF

constexpr int HTTP_GET = 0, HTTP_POST = 1;
class AsyncWebServerResponse {
public:
	virtual ~AsyncWebServerResponse() = default;
	void addHeader(const String &name, const String &value) { headers[name] = value; }
	std::map<std::string, String> headers;
};
class AsyncWebServerRequest {
public:
	bool hasArg(const char *name) const { return arguments.count(name); }
	String arg(const char *name) const { return arguments.at(name); }
	void send(int code, const char *, const String &body) { status = code; content = body; }
	void send_P(int code, const char *type, const char *body) { send(code, type, body); }
	void send(AsyncWebServerResponse *value) { response.reset(value); status = 200; }
	std::map<std::string, String> arguments;
	int status = 0;
	String content;
	std::unique_ptr<AsyncWebServerResponse> response;
};
enum AwsEventType { WS_EVT_CONNECT, WS_EVT_DISCONNECT, WS_EVT_DATA };
class AsyncWebSocketClient {
public:
	unsigned id() const { return 1; }
	IPAddress remoteIP() const { return {}; }
};
class AsyncWebSocket {
public:
	explicit AsyncWebSocket(const char *) { instances.push_back(this); }
	~AsyncWebSocket() { instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end()); }
	template <typename Callback> void onEvent(Callback callback) { event = callback; }
	unsigned count() const { return clients; }
	void textAll(const char *text) { frames.emplace_back(text); }
	unsigned clients = 0;
	std::vector<std::string> frames;
	std::function<void(AsyncWebSocket *, AsyncWebSocketClient *, AwsEventType, void *, uint8_t *, size_t)> event;
	inline static std::vector<AsyncWebSocket *> instances;
};
class AsyncWebServer {
public:
	using Handler = std::function<void(AsyncWebServerRequest *)>;
	explicit AsyncWebServer(int) { instances.push_back(this); }
	~AsyncWebServer() { instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end()); }
	void addHandler(AsyncWebSocket *) {}
	void on(const char *path, int method, Handler handler) { routes[{path, method}] = handler; }
	void onNotFound(Handler handler) { notFound = handler; }
	void begin() { ++starts; }
	void dispatch(const char *path, int method, AsyncWebServerRequest &request) {
		auto route = routes.find({path, method});
		if (route == routes.end()) notFound(&request); else route->second(&request);
	}
	unsigned starts = 0;
	std::map<std::pair<std::string, int>, Handler> routes;
	Handler notFound;
	inline static std::vector<AsyncWebServer *> instances;
};
