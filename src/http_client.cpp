#include "include/http_client.hpp"
#include <crow/logging.h>
#include <curl/curl.h>
#include <sstream>

namespace flapi {

// Static member initialization
int HTTPClient::connect_timeout_seconds_ = 10;
int HTTPClient::request_timeout_seconds_ = 30;
bool HTTPClient::verify_ssl_ = true;

/**
 * Callback for libcurl to write response data
 */
static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

/**
 * Callback for libcurl to write response headers
 */
static size_t header_callback(char* buffer, size_t size, size_t nmemb, std::map<std::string, std::string>* userp) {
    std::string header_line(buffer, size * nmemb);

    // Parse header line
    size_t colon_pos = header_line.find(':');
    if (colon_pos != std::string::npos) {
        std::string header_name = header_line.substr(0, colon_pos);
        std::string header_value = header_line.substr(colon_pos + 1);

        // Trim whitespace
        header_value.erase(0, header_value.find_first_not_of(" \t"));
        header_value.erase(header_value.find_last_not_of(" \r\n") + 1);

        userp->insert({header_name, header_value});
    }

    return size * nmemb;
}

std::optional<HTTPClient::Response> HTTPClient::request(
    Method method,
    const std::string& url,
    const std::string& data,
    const std::map<std::string, std::string>& headers) {

    return performRequest(method, url, data, headers);
}

std::optional<HTTPClient::Response> HTTPClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers) {

    return performRequest(Method::GET, url, "", headers);
}

std::optional<HTTPClient::Response> HTTPClient::post_form(
    const std::string& url,
    const std::string& form_data,
    const std::map<std::string, std::string>& headers) {

    auto request_headers = headers;
    if (request_headers.find("Content-Type") == request_headers.end()) {
        request_headers["Content-Type"] = "application/x-www-form-urlencoded";
    }

    return performRequest(Method::POST, url, form_data, request_headers);
}

std::optional<HTTPClient::Response> HTTPClient::post_json(
    const std::string& url,
    const std::string& json_body,
    const std::map<std::string, std::string>& headers) {

    auto request_headers = headers;
    if (request_headers.find("Content-Type") == request_headers.end()) {
        request_headers["Content-Type"] = "application/json";
    }

    return performRequest(Method::POST, url, json_body, request_headers);
}

void HTTPClient::setConnectTimeout(int seconds) {
    connect_timeout_seconds_ = seconds;
}

void HTTPClient::setRequestTimeout(int seconds) {
    request_timeout_seconds_ = seconds;
}

void HTTPClient::setVerifySSL(bool verify) {
    verify_ssl_ = verify;
}

std::optional<HTTPClient::Response> HTTPClient::performRequest(
    Method method,
    const std::string& url,
    const std::string& data,
    const std::map<std::string, std::string>& headers) {

    CURL* curl = curl_easy_init();
    if (!curl) {
        CROW_LOG_ERROR << "Failed to initialize CURL";
        return std::nullopt;
    }

    Response response;
    response.status_code = 0;

    try {
        // Set URL
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        // Set method
        if (method == Method::POST) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        } else {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }

        // Set timeouts
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds_);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, request_timeout_seconds_);

        // Set SSL verification
        if (!verify_ssl_) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            CROW_LOG_WARNING << "SSL verification disabled - use only for development";
        } else {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        }

        // Set headers
        struct curl_slist* header_list = nullptr;
        for (const auto& header : headers) {
            std::string header_str = header.first + ": " + header.second;
            header_list = curl_slist_append(header_list, header_str.c_str());
        }
        if (header_list) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        }

        // Set callbacks for response body and headers
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

        // User agent
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "flAPI-OIDC-Client/1.0");

        // Perform request
        CURLcode res = curl_easy_perform(curl);

        // Cleanup headers
        if (header_list) {
            curl_slist_free_all(header_list);
        }

        // Check for errors
        if (res != CURLE_OK) {
            CROW_LOG_ERROR << "HTTP request failed: " << curl_easy_strerror(res) << " (URL: " << url << ")";
            curl_easy_cleanup(curl);
            return std::nullopt;
        }

        // Get HTTP status code
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        response.status_code = (int)http_code;

        CROW_LOG_DEBUG << "HTTP " << (method == Method::GET ? "GET" : "POST")
                       << " " << url << " → " << response.status_code;

        curl_easy_cleanup(curl);
        return response;

    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "HTTP request exception: " << e.what();
        curl_easy_cleanup(curl);
        return std::nullopt;
    }
}

} // namespace flapi
