#ifndef TISA_PARSER_H_
#define TISA_PARSER_H_

#include <string_view>

#include "ast.h"

namespace tisa {

// Throws std::invalid_argument for malformed source.
Program Parse(std::string_view source);

}  // namespace tisa

#endif  // TISA_PARSER_H_
