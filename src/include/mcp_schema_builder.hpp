#pragma once

#include <crow/json.h>
#include <vector>

namespace flapi {

struct RequestFieldConfig;

// Builds an MCP tool `inputSchema` (JSON Schema, draft used by MCP) from a
// flAPI endpoint's request-field configuration. The field validators
// (int / double / boolean / date / time / uuid / email / enum, plus
// min/max/regex) already drive prepared-statement binding on the execution
// path; projecting them into the schema lets the model see the real parameter
// types and constraints instead of every parameter being an untyped string.
//
// Mapping (per validator; multiple validators on one field are merged):
//   int                -> {"type":"integer"}, min/max -> minimum/maximum
//   double/float/number-> {"type":"number"},  min/max -> minimum/maximum
//   boolean/bool       -> {"type":"boolean"}
//   date               -> {"type":"string","format":"date"}    (+min/maxDate in description)
//   time               -> {"type":"string","format":"time"}    (+min/maxTime in description)
//   uuid               -> {"type":"string","format":"uuid"}
//   email              -> {"type":"string","format":"email"}
//   enum               -> {"enum":[...]} from allowedValues
//   string (default)   -> {"type":"string"}, min/max -> minLength/maxLength, regex -> pattern
//
// A field with no validators is typed as a plain string. The result is always a
// {"type":"object","properties":{...},"required":[...]} schema; a zero-field
// endpoint yields {"type":"object","properties":{}}.
class MCPSchemaBuilder {
public:
    static crow::json::wvalue buildInputSchema(const std::vector<RequestFieldConfig>& fields);
};

} // namespace flapi
