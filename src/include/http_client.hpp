#pragma once

#include <string>
#include <optional>
#include <map>
#include <memory>

namespace flapi {

/**
 * HTTP Client for making outbound requests to OIDC providers
 * Supports GET and POST requests with JSON responses
 */
class HTTPClient {
public:
    /**
     * HTTP request methods
     */
    enum class Method {
        GET,
        POST
    };

    /**
     * HTTP response data
     */
    struct Response {
        int status_code;                           // HTTP status (200, 404, 500, etc.)
        std::string body;                          // Response body
        std::map<std::string, std::string> headers; // Response headers
    };

    /**
     * Make HTTP request to URL
     * @param method GET or POST
     * @param url Full URL to request
     * @param data Optional request body (for POST)
     * @param headers Optional request headers
     * @return Response object with status, body, headers
     */
    static std::optional<Response> request(
        Method method,
        const std::string& url,
        const std::string& data = "",
        const std::map<std::string, std::string>& headers = {}
    );

    /**
     * Make GET request
     * @param url URL to fetch
     * @param headers Optional request headers
     * @return Response or nullopt if failed
     */
    static std::optional<Response> get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}
    );

    /**
     * Make POST request with form data
     * @param url URL to POST to
     * @param form_data Form data as query string (key1=value1&key2=value2)
     * @param headers Optional request headers
     * @return Response or nullopt if failed
     */
    static std::optional<Response> post_form(
        const std::string& url,
        const std::string& form_data,
        const std::map<std::string, std::string>& headers = {}
    );

    /**
     * Make POST request with JSON body
     * @param url URL to POST to
     * @param json_body JSON request body
     * @param headers Optional request headers
     * @return Response or nullopt if failed
     */
    static std::optional<Response> post_json(
        const std::string& url,
        const std::string& json_body,
        const std::map<std::string, std::string>& headers = {}
    );

    /**
     * Set connection timeout in seconds (default: 10)
     */
    static void setConnectTimeout(int seconds);

    /**
     * Set request timeout in seconds (default: 30)
     */
    static void setRequestTimeout(int seconds);

    /**
     * Enable/disable SSL certificate verification (default: enabled)
     * ⚠️ Only disable for testing/development!
     */
    static void setVerifySSL(bool verify);

private:
    static int connect_timeout_seconds_;
    static int request_timeout_seconds_;
    static bool verify_ssl_;

    /**
     * Perform actual HTTP request using libcurl
     */
    static std::optional<Response> performRequest(
        Method method,
        const std::string& url,
        const std::string& data,
        const std::map<std::string, std::string>& headers
    );
};

} // namespace flapi
