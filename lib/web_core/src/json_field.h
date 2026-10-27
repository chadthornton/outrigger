// Just enough JSON for the web server: read one field of a flat request body,
// and escape a string for a response. Bodies are small objects the page sends.
#pragma once

#include <stddef.h>

namespace web_core {

// The string value of "key" with \" and \\ unescaped. False when the key is
// missing, isn't a string, or doesn't fit out (len includes the terminator).
bool json_string(const char *body, const char *key, char *out, size_t len);
// The integer value of "key". False when missing or not a number.
bool json_int(const char *body, const char *key, long *out);
// Escapes quotes, backslashes and control characters; truncates to fit len
// without splitting an escape. Returns the length written.
size_t json_escape(const char *in, char *out, size_t len);

}  // namespace web_core
