#pragma once

#include <cstdlib>
#include <string>

/**
 * Extracts a control query from a supported HTTP GET request line.
 *
 * Accepts `/api/v1/control?` and the legacy `/?` path, requiring a non-empty
 * query followed by a space (the start of the HTTP version).
 *
 * @param request HTTP request line.
 * @param query Receives the query substring on success; unchanged on failure.
 * @return True when a supported request target and query are found.
 */
inline bool ParseControlQuery(const std::string &request, std::string &query)
{
    constexpr char kRequestPrefix[] = "GET ";
    constexpr char kVersionedPath[] = "/api/v1/control?";
    constexpr char kLegacyPath[] = "/?";

    if (request.compare(0, sizeof(kRequestPrefix) - 1, kRequestPrefix) != 0) {
        return false;
    }

    const size_t targetStart = sizeof(kRequestPrefix) - 1;
    size_t queryStart = std::string::npos;
    if (request.compare(targetStart, sizeof(kVersionedPath) - 1, kVersionedPath) == 0) {
        queryStart = targetStart + sizeof(kVersionedPath) - 1;
    } else if (request.compare(targetStart, sizeof(kLegacyPath) - 1, kLegacyPath) == 0) {
        queryStart = targetStart + sizeof(kLegacyPath) - 1;
    } else {
        return false;
    }

    const size_t queryEnd = request.find(' ', queryStart);
    if (queryEnd == std::string::npos || queryEnd == queryStart) {
        return false;
    }
    query = request.substr(queryStart, queryEnd - queryStart);
    return true;
}

/**
 * Parses a numeric value for a named key from a query string.
 *
 * Finds the first occurrence of `key=` and uses strtod to parse the value.
 * Additional trailing characters are not rejected.
 *
 * @param query Query string to search.
 * @param key Parameter name without an equals sign.
 * @param output Receives the parsed value on success; unchanged on failure.
 * @return True if the key is found and at least one numeric character parses.
 */
inline bool ParseParam(const std::string &query, const std::string &key, double &output)
{
    const std::string keyPrefix = key + "=";
    auto pos = query.find(keyPrefix);
    if (pos == std::string::npos) {
        return false;
    }
    pos += keyPrefix.size();
    char *end = nullptr;
    const char *start = query.c_str() + pos;
    const double value = std::strtod(start, &end);
    if (start == end) {
        return false;
    }
    output = value;
    return true;
}
