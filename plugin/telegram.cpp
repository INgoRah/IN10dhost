/*
 * telegram - sends messages to a Telegram bot chat.
 *
 * Config (data.json):
 *   "telegram": { "token": "123456:ABC...", "chat_id": "-100123456",
 *                 "min_interval_ms": 1100, "max_queue": 20 }
 *
 * Whatever is written to the file telegram/send is queued and sent by
 * a worker thread of its own; a blocking HTTPS call must never run in
 * the callbacks of the daemon, it would stall the bus polling:
 *
 *   echo "front door opened" > /mnt/1wire/telegram/send
 *
 * JavaScript plugins reach it through ow.notify(text), which is just a
 * write to that file. Reading telegram/send gives the counters.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <iostream>
#include <mutex>
#include <thread>
#include <curl/curl.h>
#include "plugin.h"

#define SEND_FILE "telegram/send"
/* Telegram refuses longer messages */
#define MAX_TEXT 4096
/* tries for one message before it is dropped */
#define MAX_TRIES 3

class Telegram : public Plugin {
private:
	ILogger* logger = nullptr;
	/* the settings, guarded by mtx; the worker takes a copy per message */
	std::string token;
	std::string chat_id;
	int min_interval_ms = 1100;
	size_t max_queue = 20;

	mutable std::mutex mtx;
	std::condition_variable cv;
	std::deque<std::string> queue;
	std::thread worker;
	std::atomic<bool> stop{false};
	bool curl_ready = false;
	unsigned sent = 0, failed = 0, dropped = 0;
	std::string last_error;

	static size_t discard(char*, size_t size, size_t nmemb, void*)
	{
		return size * nmemb;
	}

	/* lets a transfer in flight end at once when the plugin is unloaded */
	static int progress(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
	{
		return static_cast<Telegram*>(p)->stop ? 1 : 0;
	}

	/* One try. Returns the HTTP status, 0 when there was no answer; err
	   gets the reason. The token is part of the URL and so is never
	   logged, curl's own error text does not contain it. */
	long post(const std::string& tok, const std::string& chat, const std::string& text, std::string& err)
	{
		CURL* c = curl_easy_init();
		char errbuf[CURL_ERROR_SIZE] = "";
		long code = 0;

		if (c == nullptr) {
			err = "curl_easy_init failed";
			return 0;
		}
		char* e_chat = curl_easy_escape(c, chat.c_str(), (int)chat.size());
		char* e_text = curl_easy_escape(c, text.c_str(), (int)text.size());
		std::string url = "https://api.telegram.org/bot" + tok + "/sendMessage";
		std::string body = std::string("chat_id=") + (e_chat ? e_chat : "") +
			"&text=" + (e_text ? e_text : "");
		curl_free(e_chat);
		curl_free(e_text);

		/* the first option curl refuses ends the try */
		CURLcode rc = CURLE_OK;
		auto set = [&](CURLoption opt, auto val) {
			CURLcode r = curl_easy_setopt(c, opt, val);
			if (rc == CURLE_OK)
				rc = r;
		};
		set(CURLOPT_URL, url.c_str());
		set(CURLOPT_POSTFIELDS, body.c_str());
		set(CURLOPT_POSTFIELDSIZE, (long)body.size());
		set(CURLOPT_CONNECTTIMEOUT, 5L);
		set(CURLOPT_TIMEOUT, 10L);
		set(CURLOPT_NOSIGNAL, 1L);
		set(CURLOPT_WRITEFUNCTION, discard);
		set(CURLOPT_ERRORBUFFER, errbuf);
		set(CURLOPT_NOPROGRESS, 0L);
		set(CURLOPT_XFERINFOFUNCTION, progress);
		set(CURLOPT_XFERINFODATA, this);
		if (rc == CURLE_OK)
			rc = curl_easy_perform(c);
		if (rc == CURLE_OK)
			rc = curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		if (rc != CURLE_OK)
			err = errbuf[0] ? errbuf : curl_easy_strerror(rc);
		curl_easy_cleanup(c);

		return code;
	}

	/* sleeps unless the plugin is stopped meanwhile, true if it was */
	bool nap(std::chrono::milliseconds d)
	{
		std::unique_lock<std::mutex> lk(mtx);

		return cv.wait_for(lk, d, [this] { return stop.load(); });
	}

	void run()
	{
		for (;;) {
			std::string text, tok, chat;
			int gap;
			{
				std::unique_lock<std::mutex> lk(mtx);
				cv.wait(lk, [this] { return stop || !queue.empty(); });
				if (stop)
					return;
				text = std::move(queue.front());
				queue.pop_front();
				tok = token;
				chat = chat_id;
				gap = min_interval_ms;
			}
			std::string err;
			bool ok = false;
			for (int i = 0; i < MAX_TRIES && !stop; i++) {
				err.clear();
				long code = post(tok, chat, text, err);
				if (code == 200) {
					ok = true;
					break;
				}
				if (code != 0)
					err = std::format("HTTP {}", code);
				/* a refusal other than rate limiting will not get
				   better by asking again */
				if (code >= 400 && code < 500 && code != 429)
					break;
				if (nap(std::chrono::seconds(code == 429 ? 5 : 2 << i)))
					return;
			}
			{
				std::lock_guard<std::mutex> lk(mtx);
				if (ok) {
					sent++;
				} else {
					failed++;
					last_error = err;
				}
			}
			if (!ok && !stop)
				logger->warn(std::format("telegram: message dropped: {}", err));
			/* Telegram allows about one message a second per chat */
			if (nap(std::chrono::milliseconds(gap)))
				return;
		}
	}

public:
	void info() override {
		std::cout << "Telegram Version 1.0" << std::endl;
	}

	void init(ILogger* logger, IDevices*) override
	{
		this->logger = logger;
		if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK)
			curl_ready = true;
		else
			logger->error("telegram: curl_global_init failed");
		if (files)
			files->file_add(this, SEND_FILE, 256);
		stop = false;
		worker = std::thread([this] { run(); });
		logger->debug("telegram: initialized");
	}

	void exit() override
	{
		{
			std::lock_guard<std::mutex> lk(mtx);
			stop = true;
		}
		cv.notify_all();
		if (worker.joinable())
			worker.join();
		if (files)
			files->file_remove(this, SEND_FILE);
		if (curl_ready)
			curl_global_cleanup();
		curl_ready = false;
		logger = nullptr;
	}

	json config_get() const override
	{
		std::lock_guard<std::mutex> lk(mtx);
		json j;

		j["token"] = token;
		j["chat_id"] = chat_id;
		j["min_interval_ms"] = min_interval_ms;
		j["max_queue"] = max_queue;
		return j;
	}

	void config_set(const json& j) override
	{
		std::lock_guard<std::mutex> lk(mtx);

		token = j.value("token", token);
		/* a chat id is a number, but may be written as a string */
		if (j.contains("chat_id"))
			chat_id = j["chat_id"].is_string() ? j["chat_id"].get<std::string>()
				: j["chat_id"].dump();
		min_interval_ms = std::clamp(j.value("min_interval_ms", min_interval_ms), 0, 60000);
		max_queue = (size_t)std::clamp(j.value("max_queue", (int)max_queue), 1, 1000);
	}

	int action(const ActionEvent&) override
	{
		return 0;
	}

	int fs_read(const string& path, char* buf, size_t size) override
	{
		if (path != SEND_FILE)
			return -ENOENT;
		std::string s;
		{
			std::lock_guard<std::mutex> lk(mtx);
			s = std::format("sent {} failed {} dropped {} queued {}\n", sent, failed, dropped, queue.size());
			if (!last_error.empty())
				s += "last error: " + last_error + "\n";
		}
		if (s.size() > size)
			s.resize(size);
		std::memcpy(buf, s.data(), s.size());
		return (int)s.size();
	}

	/* queues the text; what the worker does with it is not the
	   writer's business, so a bad token still reads as a good write */
	int fs_write(const string& path, const char* buf, size_t size) override
	{
		if (path != SEND_FILE)
			return Plugin::fs_write(path, buf, size);
		std::string text(buf, size);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
			text.pop_back();
		if (text.size() > MAX_TEXT)
			text.resize(MAX_TEXT);
		if (text.empty())
			return (int)size;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (token.empty() || chat_id.empty()) {
				failed++;
				last_error = "token or chat_id not configured";
				return -ENOTCONN;
			}
			/* a flood is not worth keeping: the oldest goes */
			if (queue.size() >= max_queue) {
				queue.pop_front();
				dropped++;
			}
			queue.push_back(std::move(text));
		}
		cv.notify_one();
		return (int)size;
	}
};

extern "C" Plugin* create_plugin() {
	return new Telegram();
}

extern "C" void destroy_plugin(Plugin* p) {
	delete p;
}
