#include "prepared_value_converter.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace flapi {

namespace {

constexpr const char* kIntErr  = "Integer parameter is not a valid signed 64-bit integer";
constexpr const char* kDblErr  = "Double parameter is not a valid IEEE-754 number";
constexpr const char* kBoolErr = "Boolean parameter must be 'true', 'false', '1', or '0'";
constexpr const char* kDateErr = "Date parameter must be YYYY-MM-DD";
constexpr const char* kTimeErr = "Time parameter must be HH:MM:SS or HH:MM:SS.ffffff";

bool allDigits(const char* begin, const char* end) {
    if (begin == end) {
        return false;
    }
    for (const char* p = begin; p != end; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

bool parseUnsigned(const char* begin, const char* end, std::int32_t& out) {
    if (!allDigits(begin, end)) {
        return false;
    }
    std::int64_t acc = 0;
    for (const char* p = begin; p != end; ++p) {
        acc = acc * 10 + (*p - '0');
        if (acc > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
    }
    out = static_cast<std::int32_t>(acc);
    return true;
}

ConvertOutcome err(const char* msg) {
    ConvertOutcome o;
    o.ok = false;
    o.error = msg;
    return o;
}

ConvertOutcome makeNull() {
    ConvertOutcome o;
    o.value.kind = PreparedValue::Kind::Null;
    return o;
}

bool isValidDate(std::int32_t y, std::int32_t m, std::int32_t d) {
    if (m < 1 || m > 12) {
        return false;
    }
    if (d < 1) {
        return false;
    }
    static constexpr std::array<int, 13> days_in_month =
        {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = days_in_month[m];
    if (m == 2) {
        const bool leap = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0));
        if (leap) {
            max_day = 29;
        }
    }
    return d <= max_day;
}

ConvertOutcome convertInteger(const std::string& raw) {
    if (raw.empty()) {
        return err(kIntErr);
    }
    errno = 0;
    char* endp = nullptr;
    const long long parsed = std::strtoll(raw.c_str(), &endp, 10);
    if (errno == ERANGE) {
        return err(kIntErr);
    }
    if (endp == raw.c_str()) {
        return err(kIntErr);
    }
    while (*endp == ' ' || *endp == '\t' || *endp == '\n' || *endp == '\r') {
        ++endp;
    }
    if (*endp != '\0') {
        return err(kIntErr);
    }

    ConvertOutcome o;
    o.value.kind = PreparedValue::Kind::Int64;
    o.value.int64_value = static_cast<std::int64_t>(parsed);
    return o;
}

ConvertOutcome convertDouble(const std::string& raw) {
    if (raw.empty()) {
        return err(kDblErr);
    }
    errno = 0;
    char* endp = nullptr;
    const double parsed = std::strtod(raw.c_str(), &endp);
    if (errno == ERANGE) {
        return err(kDblErr);
    }
    if (endp == raw.c_str()) {
        return err(kDblErr);
    }
    while (*endp == ' ' || *endp == '\t' || *endp == '\n' || *endp == '\r') {
        ++endp;
    }
    if (*endp != '\0') {
        return err(kDblErr);
    }

    ConvertOutcome o;
    o.value.kind = PreparedValue::Kind::Double;
    o.value.double_value = parsed;
    return o;
}

ConvertOutcome convertBoolean(const std::string& raw) {
    if (raw.empty()) {
        return err(kBoolErr);
    }
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) {
        lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
    }
    bool value;
    if (lower == "true" || lower == "1") {
        value = true;
    } else if (lower == "false" || lower == "0") {
        value = false;
    } else {
        return err(kBoolErr);
    }
    ConvertOutcome o;
    o.value.kind = PreparedValue::Kind::Bool;
    o.value.bool_value = value;
    return o;
}

ConvertOutcome convertDate(const std::string& raw) {
    if (raw.empty()) {
        return makeNull();
    }
    if (raw.size() != 10) {
        return err(kDateErr);
    }
    if (raw[4] != '-' || raw[7] != '-') {
        return err(kDateErr);
    }

    std::int32_t y = 0, m = 0, d = 0;
    if (!parseUnsigned(raw.data() + 0, raw.data() + 4, y)) {
        return err(kDateErr);
    }
    if (!parseUnsigned(raw.data() + 5, raw.data() + 7, m)) {
        return err(kDateErr);
    }
    if (!parseUnsigned(raw.data() + 8, raw.data() + 10, d)) {
        return err(kDateErr);
    }
    if (!isValidDate(y, m, d)) {
        return err(kDateErr);
    }

    ConvertOutcome o;
    o.value.kind = PreparedValue::Kind::Date;
    o.value.year  = y;
    o.value.month = static_cast<std::int8_t>(m);
    o.value.day   = static_cast<std::int8_t>(d);
    return o;
}

ConvertOutcome convertTime(const std::string& raw) {
    if (raw.empty()) {
        return makeNull();
    }
    if (raw.size() < 8 || raw[2] != ':' || raw[5] != ':') {
        return err(kTimeErr);
    }

    std::int32_t h = 0, mi = 0, s = 0;
    if (!parseUnsigned(raw.data() + 0, raw.data() + 2, h)) {
        return err(kTimeErr);
    }
    if (!parseUnsigned(raw.data() + 3, raw.data() + 5, mi)) {
        return err(kTimeErr);
    }
    if (!parseUnsigned(raw.data() + 6, raw.data() + 8, s)) {
        return err(kTimeErr);
    }
    if (h > 23 || mi > 59 || s > 59) {
        return err(kTimeErr);
    }

    std::int32_t micros = 0;
    if (raw.size() > 8) {
        if (raw[8] != '.') {
            return err(kTimeErr);
        }
        const std::size_t frac_len = raw.size() - 9;
        if (frac_len < 1 || frac_len > 6) {
            return err(kTimeErr);
        }
        std::int32_t frac = 0;
        if (!parseUnsigned(raw.data() + 9, raw.data() + raw.size(), frac)) {
            return err(kTimeErr);
        }
        for (std::size_t i = frac_len; i < 6; ++i) {
            frac *= 10;
        }
        micros = frac;
    }

    ConvertOutcome o;
    o.value.kind = PreparedValue::Kind::Time;
    o.value.hour   = static_cast<std::int8_t>(h);
    o.value.minute = static_cast<std::int8_t>(mi);
    o.value.second = static_cast<std::int8_t>(s);
    o.value.micros = micros;
    return o;
}

} // namespace

ConvertOutcome PreparedValueConverter::convert(SqlParameterType type,
                                               const std::string& raw,
                                               bool present) const {
    if (!present) {
        return makeNull();
    }
    switch (type) {
        case SqlParameterType::Integer: return convertInteger(raw);
        case SqlParameterType::Double:  return convertDouble(raw);
        case SqlParameterType::Boolean: return convertBoolean(raw);
        case SqlParameterType::Date:    return convertDate(raw);
        case SqlParameterType::Time:    return convertTime(raw);
        case SqlParameterType::Varchar: {
            ConvertOutcome o;
            o.value.kind = PreparedValue::Kind::Varchar;
            o.value.varchar = raw;
            return o;
        }
    }
    return makeNull();  // unreachable
}

} // namespace flapi
