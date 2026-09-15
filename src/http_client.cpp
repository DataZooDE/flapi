#include "include/http_client.hpp"
#include "include/flapi_tracing.hpp"
#include "include/trace_context.hpp"
#include "include/trace_semconv.hpp"
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


namespace {

// Outbound CLIENT spans, so flAPI stops being a trace terminator for what it
// calls.
//
// These matter out of all proportion to their volume: a slow or flapping identity
// provider presents to the user as "flAPI is slow" or "flAPI rejects my token",
// and with no client span there is nothing in the trace to contradict that. A
// JWKS refresh stalling inside a request is exactly the latency that otherwise
// gets misattributed to the database.
flapi::SpanScope startClientSpan(const std::string& method_name, const std::string& url) {
    flapi::SpanScope span = flapi::Tracing().startSpan(method_name.c_str(),
                                                      flapi::SpanKind::Client);
    if (!span) {
        return span;
    }
    span.setAttr(flapi::semconv::http::kRequestMethod, method_name);

    // url.full with the QUERY STRING STRIPPED. An IdP URL can carry parameters,
    // and a query string is never safe to export. Host and path only.
    std::string_view trimmed(url);
    if (const auto q = trimmed.find('?'); q != std::string_view::npos) {
        trimmed = trimmed.substr(0, q);
    }
    span.setAttr("url.full", trimmed);

    // server.address without scheme or path, so the attribute stays bounded.
    std::string_view host = trimmed;
    if (const auto scheme = host.find("://"); scheme != std::string_view::npos) {
        host.remove_prefix(scheme + 3);
    }
    if (const auto slash = host.find('/'); slash != std::string_view::npos) {
        host = host.substr(0, slash);
    }
    span.setAttr(flapi::semconv::net::kServerAddress, host);
    return span;
}

}  // namespace

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

    const std::string method_name = (method == Method::GET) ? "GET" : "POST";
    SpanScope span = startClientSpan(method_name, url);

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
        // Propagate context OUTWARD. flAPI should not be a trace terminator for
        // the services it calls: an IdP that is itself instrumented can then join
        // the same trace instead of starting a disconnected one.
        if (span) {
            const auto ids = span.ids();
            if (ids.valid()) {
                const std::string traceparent = "traceparent: " + formatTraceparent(ids);
                header_list = curl_slist_append(header_list, traceparent.c_str());
            }
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
            // Enumerated, not curl's message: that string can contain the URL
            // including its query string.
            span.setError("connection_error");
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
        if (span) {
            span.setAttr(semconv::http::kResponseStatus,
                         static_cast<std::int64_t>(response.status_code));
            if (response.status_code >= 400) {
                span.setError(response.status_code >= 500 ? "server_error" : "client_error");
            }
        }
        return response;

    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "HTTP request exception: " << e.what();
        curl_easy_cleanup(curl);
        return std::nullopt;
    }
}

} // namespace flapi
