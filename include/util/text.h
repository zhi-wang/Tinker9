#ifndef TINKER_UTIL_TEXT_H_
#define TINKER_UTIL_TEXT_H_

#include "cxx.h"

TINKER_NAMESPACE_BEGIN
class Text : public std::vector<std::string> {
public:
  static constexpr const char* whitespaces = " \t\n\v\f\r";

  template <size_t __Len>
  static std::string string(const char (&__src)[__Len]) {
    return std::string(&__src[0], &__src[0] + __Len);
  }

  // Replace
  static void replace(std::string& s, std::string old, char r);
  static void replace_by_kv(std::string& src, std::string key,
                            std::string value);

  // Split
  static std::vector<std::string> split(std::string str,
                                        std::string delimiters = whitespaces);

  // Case
  static void upcase(std::string&);
  static void lowcase(std::string&);
};
TINKER_NAMESPACE_END

#endif
