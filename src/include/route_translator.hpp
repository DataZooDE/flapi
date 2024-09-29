#pragma once

#include <string>
#include <vector>
#include <regex>
#include <map>

namespace flapi {

class RouteTranslator {
public:
    static std::string translateRoutePath(const std::string& flapiPath, std::vector<std::string>& paramNames);
    static bool matchAndExtractParams(const std::string& routePattern, const std::string& actualPath, 
                                      std::vector<std::string>& paramNames, std::map<std::string, std::string>& pathParams);

private:
    static const std::regex param_regex;
};

} // namespace flapi