#include "route_translator.hpp"
#include <regex>

namespace flapi {

const std::regex RouteTranslator::param_regex(":([^/]+)");

std::string RouteTranslator::translateRoutePath(const std::string& flapiPath, std::vector<std::string>& paramNames) {
    std::string crowPath = flapiPath;
    
    auto words_begin = std::sregex_iterator(flapiPath.begin(), flapiPath.end(), param_regex);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::smatch match = *i;
        std::string paramName = match[1].str();
        paramNames.push_back(paramName);
        crowPath = std::regex_replace(crowPath, std::regex(":" + paramName), "([^/]+)");
    }

    return "^" + crowPath + "$";
}

bool RouteTranslator::matchAndExtractParams(const std::string& routePattern, const std::string& actualPath, 
                                            std::vector<std::string>& paramNames, std::map<std::string, std::string>& pathParams) {
    std::vector<std::string> tempParamNames;
    std::string regexPattern = translateRoutePath(routePattern, tempParamNames);
    
    std::regex routeRegex(regexPattern);
    std::smatch matches;

    if (std::regex_match(actualPath, matches, routeRegex)) {
        for (size_t i = 1; i < matches.size(); ++i) {
            pathParams[tempParamNames[i-1]] = matches[i].str();
        }
        paramNames = std::move(tempParamNames);
        return true;
    }

    return false;
}

} // namespace flapi