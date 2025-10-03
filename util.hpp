// Copyright 2022 Tenstorrent Corporation or its affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <cassert>
#include <utility>
#include <span>
#include <memory>

namespace gsl { template <typename T> using owner = T; }

namespace util
{
  namespace _helpers
  {
    // Splits an unsigned value into its digits.  The contained type
    // is an integer_sequence containing the digits in printed order.
    template <size_t V, size_t... Vs> struct _digit_sequence_helper
    {
      using type = typename _digit_sequence_helper<V / 10, V % 10, Vs...>::type;
    };
    template <> struct _digit_sequence_helper<0>
    {
      using type = std::index_sequence<0>;
    };
    template <std::size_t V, std::size_t... Vs> struct _digit_sequence_helper<0, V, Vs...>
    {
      using type = std::index_sequence<V, Vs...>;
    };
    template <std::size_t V> using digit_sequence = typename _digit_sequence_helper<V>::type;

    // Creates a compile-time string using an integer_sequence of the digits.
    // the template is only defined for integer_sequence<size_t>.
    template <typename, char...> struct _value_to_string_helper;
    template <std::size_t... Digits, char... PREFIX_CHARS>
    struct _value_to_string_helper<std::index_sequence<Digits...>, PREFIX_CHARS...>
    {
      static constexpr auto value_arr = std::to_array<char>({PREFIX_CHARS..., ('0' + Digits)..., 0});
      static constexpr auto value     = std::string_view(value_arr.begin(), std::prev(value_arr.end()));
    };
    template <std::size_t V, char... PREFIX_CHARS>
    using value_to_string = _helpers::_value_to_string_helper<_helpers::digit_sequence<V>, PREFIX_CHARS...>;

    // Creates an array of compile-time register strings.  Specify the number
    // of register names to create and provide the prefix characters at the end
    template <typename, char...> struct _reg_name_array_helper;
    template <std::size_t... Vs, char... PREFIX_CHARS>
    struct _reg_name_array_helper<std::index_sequence<Vs...>, PREFIX_CHARS...>
    {
      static constexpr auto value = std::array{value_to_string<Vs, PREFIX_CHARS...>::value...};
    };
    template <std::size_t V, char... PREFIX_CHARS>
    using make_reg_name_array = _helpers::_reg_name_array_helper<std::make_index_sequence<V>, PREFIX_CHARS...>;
  }

  using _helpers::value_to_string;

  using _helpers::make_reg_name_array;

  /// Combines all of the arguments into a single string using one allocation
  template <typename Arg, typename... Args>
  static std::string join(std::string_view separator, Arg arg, Args&&... args)
  {
    constexpr auto getLen = [](auto&& stringOrStringWiew) -> std::size_t {
      if constexpr (std::is_convertible_v<decltype(stringOrStringWiew), const char*>)
        {
          return std::strlen(stringOrStringWiew);
        }
      else
        {
          return stringOrStringWiew.size();
        }
    };

    std::string result;
    result.reserve((getLen(arg) + ... + getLen(std::forward<Args>(args))) + (separator.size() * sizeof...(Args)));

    result += arg;
    (result.append(separator).append(std::forward<Args>(args)), ...);

    return result;
  }

  /// Until we have C++23 and std::byteswap
  template <typename T,
              std::enable_if_t<std::is_integral_v<T>, int> = 0>
  constexpr T byteswap(T x)
  {
    if constexpr (sizeof(x) == 1)
      return x;
    if constexpr (sizeof(x) == 2)
      return __builtin_bswap16(x);
    if constexpr (sizeof(x) == 4)
      return __builtin_bswap32(x);
    if constexpr (sizeof(x) == 8)
      return __builtin_bswap64(x);
    assert(0 && "Error: Assertion failed");
    return 0;
  }

  struct string_hash  // C++20's transparent hashing
  {
    using hash_type      = std::hash<std::string_view>;
    using is_transparent = void;

    template <typename T>
    auto operator()(T&& str) const -> decltype(hash_type{}(std::forward<T>(str)))
    {
      return hash_type{}(std::forward<T>(str));
    }
  };

  namespace file {

    // For closing owned files.
    enum FileCloseF {
      FCLOSE,
      PCLOSE,
      NONE
    };

    struct FileCloser
    {
      FileCloser(FileCloseF f) : f_(f) {};

      void operator()(FILE* file) const {
        if (not file or file == stdout or file == stderr)
          return;
        if (f_ == FileCloseF::FCLOSE)
          fclose(file);
        if (f_ == FileCloseF::PCLOSE)
          pclose(file);
      }

      FileCloseF f_ = FileCloseF::NONE;
    };

    using SharedFile = std::shared_ptr<FILE>;

    inline SharedFile make_shared_file(gsl::owner<FILE*> file, FileCloseF f = FileCloseF::FCLOSE) {
        return SharedFile(file, FileCloser{f});
    }
  }

  template <typename T>
    requires std::is_arithmetic_v<T>
  auto view_bytes_as_span_of(std::span<uint8_t> bytes) -> std::span<const T> {
    assert(bytes.size() % sizeof(sizeof(T)) == 0);
    return std::span<const T>(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const T*>(bytes.data()),
        bytes.size() / sizeof(T)
    );
  }
}
