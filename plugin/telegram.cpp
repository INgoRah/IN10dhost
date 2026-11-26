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
 *
 * A picture, e.g. a camera snapshot, goes through telegram/photo: the
 * first line is the URL of the image, an optional second line the
 * caption. The worker downloads it and uploads it with sendPhoto. If
 * the download fails the caption is sent as a text, so the event is
 * not lost.
 *
 *   printf 'http://cam/snap.jpg\nDoorbell' > /mnt/1wire/telegram/photo
 *
 * "photo_insecure": true skips the certificate check of that download
 * only (cameras with a self-signed certificate); api.telegram.org is
 * always verified. From a script: ow.notify_photo(url, caption).
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
#define PHOTO_FILE "telegram/photo"
#define MAX_CAPTION 1024
/* Telegram takes photos up to 10 MB */
#define MAX_IMAGE (10u * 1024 * 1024)
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
	bool photo_insecure = false;

	mutable std::mutex mtx;
	std::condition_variable cv;
	/* a message; with a url it is a picture and text its caption */
	struct Item {
		std::string text;
		std::string url;
	};
	std::deque<Item> queue;
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

	static size_t collect(char* p, size_t size, size_t nmemb, void* out)
	{
		std::string* s = static_cast<std::string*>(out);
		size_t n = size * nmemb;

		/* more than Telegram takes: stop the transfer */
		if (s->size() + n > MAX_IMAGE)
			return 0;
		s->append(p, n);
		return n;
	}

	/* Downloads the picture. Returns the HTTP status, 0 when there was
	   no answer; err gets the reason. Only http and https, redirects are
	   not followed. */
	long download(const std::string& url, bool insecure, std::string& image, std::string& err)
	{
		CURL* c = curl_easy_init();
		char errbuf[CURL_ERROR_SIZE] = "";
		long code = 0;

		if (c == nullptr) {
			err = "curl_easy_init failed";
			return 0;
		}
		CURLcode rc = CURLE_OK;
		auto set = [&](CURLoption opt, auto val) {
			CURLcode r = curl_easy_setopt(c, opt, val);
			if (rc == CURLE_OK)
				rc = r;
		};
		set(CURLOPT_URL, url.c_str());
		set(CURLOPT_CONNECTTIMEOUT, 5L);
		set(CURLOPT_TIMEOUT, 15L);
		set(CURLOPT_NOSIGNAL, 1L);
		set(CURLOPT_FOLLOWLOCATION, 0L);
		set(CURLOPT_PROTOCOLS_STR, "http,https");
		if (insecure) {
			set(CURLOPT_SSL_VERIFYPEER, 0L);
			set(CURLOPT_SSL_VERIFYHOST, 0L);
		}
		set(CURLOPT_WRITEFUNCTION, collect);
		set(CURLOPT_WRITEDATA, &image);
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

	static bool is_image(const std::string& d)
	{
		return (d.size() > 3 && (uint8_t)d[0] == 0xff && (uint8_t)d[1] == 0xd8 && (uint8_t)d[2] == 0xff) ||
			(d.size() > 4 && d.compare(1, 3, "PNG") == 0 && (uint8_t)d[0] == 0x89);
	}

	/* One try. Without image text goes out as a message, with one as the
	   caption of the picture. Returns the HTTP status, 0 when there was
	   no answer; err gets the reason. The token is part of the URL and
	   so is never logged, curl's own error text does not contain it. */
	long post(const std::string& tok, const std::string& chat, const std::string& text,
		const std::string* image, std::string& err)
	{
		CURL* c = curl_easy_init();
		char errbuf[CURL_ERROR_SIZE] = "";
		long code = 0;

		if (c == nullptr) {
			err = "curl_easy_init failed";
			return 0;
		}
		std::string url = "https://api.telegram.org/bot" + tok +
			(image ? "/sendPhoto" : "/sendMessage");
		std::string body;
		curl_mime* mime = nullptr;
		/* the first call curl refuses ends the try */
		CURLcode rc = CURLE_OK;
		auto check = [&](CURLcode r) {
			if (rc == CURLE_OK)
				rc = r;
		};
		auto set = [&](CURLoption opt, auto val) {
			check(curl_easy_setopt(c, opt, val));
		};
		/* one part of the form; the data is copied by curl */
		auto field = [&](const char* name, const char* data, size_t len, const char* file) {
			curl_mimepart* part = curl_mime_addpart(mime);

			if (part == nullptr) {
				check(CURLE_OUT_OF_MEMORY);
				return;
			}
			check(curl_mime_name(part, name));
			if (file)
				check(curl_mime_filename(part, file));
			check(curl_mime_data(part, data, len));
		};
		if (image) {
			mime = curl_mime_init(c);
			if (mime == nullptr) {
				check(CURLE_OUT_OF_MEMORY);
			} else {
				field("chat_id", chat.c_str(), CURL_ZERO_TERMINATED, nullptr);
				if (!text.empty())
					field("caption", text.c_str(), CURL_ZERO_TERMINATED, nullptr);
				field("photo", image->data(), image->size(),
					(uint8_t)(*image)[0] == 0x89 ? "snap.png" : "snap.jpg");
			}
		} else {
			char* e_chat = curl_easy_escape(c, chat.c_str(), (int)chat.size());
			char* e_text = curl_easy_escape(c, text.c_str(), (int)text.size());
			body = std::string("chat_id=") + (e_chat ? e_chat : "") +
				"&text=" + (e_text ? e_text : "");
			curl_free(e_chat);
			curl_free(e_text);
		}

		set(CURLOPT_URL, url.c_str());
		if (image) {
			set(CURLOPT_MIMEPOST, mime);
		} else {
			set(CURLOPT_POSTFIELDS, body.c_str());
			set(CURLOPT_POSTFIELDSIZE, (long)body.size());
		}
		set(CURLOPT_CONNECTTIMEOUT, 5L);
		set(CURLOPT_TIMEOUT, image ? 30L : 10L);
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
		curl_mime_free(mime);
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
			Item item;
			std::string tok, chat;
			bool insecure;
			int gap;
			{
				std::unique_lock<std::mutex> lk(mtx);
				cv.wait(lk, [this] { return stop || !queue.empty(); });
				if (stop)
					return;
				item = std::move(queue.front());
				queue.pop_front();
				tok = token;
				chat = chat_id;
				insecure = photo_insecure;
				gap = min_interval_ms;
			}
			std::string err;
			std::string image;
			const std::string* img = nullptr;
			if (!item.url.empty()) {
				long dc = download(item.url, insecure, image, err);
				if (dc == 200 && is_image(image)) {
					img = &image;
				} else {
					if (dc != 0)
						err = dc == 200 ? "not an image" : std::format("HTTP {}", dc);
					/* the event is the news, not the picture */
					logger->warn(std::format("telegram: no photo: {}", err));
					item.text += (item.text.empty() ? "" : " ") + std::format("(no photo: {})", err);
					err.clear();
				}
			}
			bool ok = false;
			for (int i = 0; i < MAX_TRIES && !stop; i++) {
				err.clear();
				long code = post(tok, chat, item.text, img, err);
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
		if (files) {
			files->file_add(this, SEND_FILE, 256);
			files->file_add(this, PHOTO_FILE, 256);
		}
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
		if (files) {
			files->file_remove(this, SEND_FILE);
			files->file_remove(this, PHOTO_FILE);
		}
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
		j["photo_insecure"] = photo_insecure;
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
		photo_insecure = j.value("photo_insecure", photo_insecure);
	}

	int action(const ActionEvent&) override
	{
		return 0;
	}

	int fs_read(const string& path, char* buf, size_t size) override
	{
		if (path != SEND_FILE && path != PHOTO_FILE)
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

	/* queues the text or the picture; what the worker does with it is
	   not the writer's business, so a bad token still reads as a good
	   write */
	int fs_write(const string& path, const char* buf, size_t size) override
	{
		Item item;

		if (path == SEND_FILE) {
			item.text.assign(buf, size);
		} else if (path == PHOTO_FILE) {
			/* "url" or "url\ncaption" */
			std::string in(buf, size);
			size_t nl = in.find('\n');
			item.url = in.substr(0, nl);
			if (nl != std::string::npos)
				item.text = in.substr(nl + 1);
			while (!item.url.empty() && (item.url.back() == '\r' || item.url.back() == ' '))
				item.url.pop_back();
			if (item.url.rfind("http://", 0) != 0 && item.url.rfind("https://", 0) != 0)
				return -EINVAL;
		} else {
			return Plugin::fs_write(path, buf, size);
		}
		while (!item.text.empty() && (item.text.back() == '\n' || item.text.back() == '\r'))
			item.text.pop_back();
		if (item.text.size() > (item.url.empty() ? MAX_TEXT : MAX_CAPTION))
			item.text.resize(item.url.empty() ? MAX_TEXT : MAX_CAPTION);
		if (item.text.empty() && item.url.empty())
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
			queue.push_back(std::move(item));
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
