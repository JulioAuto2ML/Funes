// =============================================================================
// src/core/base64.h — base64 encode/decode
// =============================================================================
// Used for image attachments: the OpenAI and Anthropic chat APIs both take
// inline images as base64 data (a `data:` URL for OpenAI, a bare base64
// string for Anthropic), and the upload endpoint hands the UI back base64 so
// it can send the same bytes on to /api/chat without re-reading the file.

#pragma once
#include <string>

namespace funes {

std::string base64_encode(const std::string& data);

// Returns false (leaving `out` unspecified) if `data` isn't valid base64.
bool base64_decode(const std::string& data, std::string& out);

} // namespace funes
