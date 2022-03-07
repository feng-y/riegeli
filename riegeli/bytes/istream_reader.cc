// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "riegeli/bytes/istream_reader.h"

#include <stddef.h>

#include <cerrno>
#include <ios>
#include <istream>
#include <limits>
#include <string>

#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "riegeli/base/base.h"
#include "riegeli/base/errno_mapping.h"
#include "riegeli/bytes/buffered_reader.h"

namespace riegeli {

void IStreamReaderBase::Initialize(std::istream* src,
                                   absl::optional<Position> assumed_pos) {
  RIEGELI_ASSERT(src != nullptr)
      << "Failed precondition of IStreamReader: null stream pointer";
  RIEGELI_ASSERT(supports_random_access_ == LazyBoolState::kFalse)
      << "Failed precondition of IStreamReaderBase::Initialize(): "
         "supports_random_access_ not reset";
  if (ABSL_PREDICT_FALSE(src->fail())) {
    // Either constructing the stream failed or the stream was already in a
    // failed state. In any case `IStreamReaderBase` should fail.
    FailOperation("istream::istream()");
    return;
  }
  // A sticky `std::ios_base::eofbit` breaks future operations like
  // `std::istream::peek()` and `std::istream::tellg()`.
  src->clear(src->rdstate() & ~std::ios_base::eofbit);
  if (assumed_pos != absl::nullopt) {
    if (ABSL_PREDICT_FALSE(
            *assumed_pos >
            Position{std::numeric_limits<std::streamoff>::max()})) {
      FailOverflow();
      return;
    }
    set_limit_pos(*assumed_pos);
  } else {
    const std::streamoff stream_pos = src->tellg();
    if (stream_pos < 0) {
      // Random access is not supported. Assume 0 as the initial position.
      return;
    }
    set_limit_pos(IntCast<Position>(stream_pos));
    // `std::istream::tellg()` succeeded, and `std::istream::seekg()` will be
    // checked later.
    supports_random_access_ = LazyBoolState::kUnknown;
  }
}

void IStreamReaderBase::Done() {
  BufferedReader::Done();
  // If `supports_random_access_` is still `LazyBoolState::kUnknown`, change it
  // to `LazyBoolState::kFalse`, because trying to resolve it later might access
  // a closed stream. The resolution is no longer interesting anyway.
  if (supports_random_access_ == LazyBoolState::kUnknown) {
    supports_random_access_ = LazyBoolState::kFalse;
  }
}

bool IStreamReaderBase::FailOperation(absl::string_view operation) {
  // There is no way to get details why a stream operation failed without
  // letting the stream throw exceptions. Hopefully low level failures have set
  // `errno` as a side effect.
  //
  // This requires resetting `errno` to 0 before the stream operation because
  // the operation may fail without setting `errno`.
  const int error_number = errno;
  const std::string message = absl::StrCat(operation, " failed");
  return Fail(error_number == 0
                  ? absl::UnknownError(message)
                  : ErrnoToCanonicalStatus(error_number, message));
}

bool IStreamReaderBase::supports_random_access() {
  switch (supports_random_access_) {
    case LazyBoolState::kFalse:
      return false;
    case LazyBoolState::kTrue:
      return true;
    case LazyBoolState::kUnknown:
      break;
  }
  RIEGELI_ASSERT(is_open())
      << "Failed invariant of IStreamReaderBase: "
         "unresolved supports_random_access_ but object closed";
  std::istream& src = *src_stream();
  bool supported = false;
  src.seekg(0, std::ios_base::end);
  if (src.fail()) {
    src.clear(src.rdstate() & ~std::ios_base::failbit);
  } else {
    errno = 0;
    const std::streamoff stream_size = src.tellg();
    if (ABSL_PREDICT_FALSE(stream_size < 0)) {
      FailOperation("istream::tellg()");
    } else {
      src.seekg(IntCast<std::streamoff>(limit_pos()), std::ios_base::beg);
      if (ABSL_PREDICT_FALSE(src.fail())) {
        FailOperation("istream::seekg()");
      } else {
        FoundSize(IntCast<Position>(stream_size));
        supported = true;
      }
    }
  }
  supports_random_access_ =
      supported ? LazyBoolState::kTrue : LazyBoolState::kFalse;
  return supported;
}

inline void IStreamReaderBase::FoundSize(Position size) {
  if (!growing_source_) size_ = size;
  set_size_hint(size);
}

bool IStreamReaderBase::ReadInternal(size_t min_length, size_t max_length,
                                     char* dest) {
  RIEGELI_ASSERT_GT(min_length, 0u)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "nothing to read";
  RIEGELI_ASSERT_GE(max_length, min_length)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "max_length < min_length";
  RIEGELI_ASSERT(ok())
      << "Failed precondition of BufferedReader::ReadInternal(): " << status();
  if (size_ != absl::nullopt && ABSL_PREDICT_FALSE(limit_pos() >= size_)) {
    return false;
  }
  std::istream& src = *src_stream();
  if (ABSL_PREDICT_FALSE(max_length >
                         Position{std::numeric_limits<std::streamoff>::max()} -
                             limit_pos())) {
    max_length =
        Position{std::numeric_limits<std::streamoff>::max()} - limit_pos();
    if (ABSL_PREDICT_FALSE(max_length < min_length)) return FailOverflow();
  }
  errno = 0;
  for (;;) {
    std::streamsize length_to_read = IntCast<std::streamsize>(UnsignedMin(
        min_length, size_t{std::numeric_limits<std::streamsize>::max()}));
    const std::streamsize max_length_to_read =
        IntCast<std::streamsize>(UnsignedMin(
            max_length, size_t{std::numeric_limits<std::streamsize>::max()}));
    std::streamsize length_read;
    if (length_to_read < max_length_to_read) {
      if (supports_random_access() && size_ != absl::nullopt) {
        // Increase `length_to_read` to cover the rest of the stream.
        length_to_read = SignedMin(
            SignedMax(length_to_read, SaturatingIntCast<std::streamsize>(
                                          SaturatingSub(*size_, limit_pos()))),
            max_length_to_read);
      } else {
        // Use `std::istream::readsome()` to read as much data as is available,
        // up to `max_length_to_read`.
        //
        // `std::istream::peek()` asks to read some characters into the buffer,
        // otherwise `std::istream::readsome()` may return 0.
        if (ABSL_PREDICT_FALSE(src.peek() == std::char_traits<char>::eof())) {
          if (ABSL_PREDICT_FALSE(src.fail())) {
            return FailOperation("istream::peek()");
          }
          // A sticky `std::ios_base::eofbit` breaks future operations like
          // `std::istream::peek()` and `std::istream::tellg()`.
          src.clear(src.rdstate() & ~std::ios_base::eofbit);
          FoundSize(limit_pos());
          return false;
        }
        length_read = src.readsome(dest, max_length_to_read);
        RIEGELI_ASSERT_GE(length_read, 0) << "negative istream::readsome()";
        RIEGELI_ASSERT_LE(IntCast<size_t>(length_read), max_length)
            << "istream::readsome() read more than requested";
        if (ABSL_PREDICT_TRUE(length_read > 0)) goto fragment_read;
        // `std::istream::peek()` returned non-`eof()` but
        // `std::istream::readsome()` returned 0. This might happen if
        // `src.rdbuf()->sgetc()` does not use the get area but leaves the next
        // character buffered elsewhere, e.g. for `std::cin` synchronized to
        // stdio. Fall back to `std::istream::read()`.
      }
    }
    // Use `std::istream::read()` to read a fixed length of `length_to_read`.
    src.read(dest, length_to_read);
    length_read = src.gcount();
    RIEGELI_ASSERT_GE(length_read, 0) << "negative istream::gcount()";
    RIEGELI_ASSERT_LE(IntCast<size_t>(length_read), length_to_read)
        << "istream::read() read more than requested";
  fragment_read:
    move_limit_pos(IntCast<size_t>(length_read));
    if (ABSL_PREDICT_FALSE(src.fail())) {
      if (ABSL_PREDICT_FALSE(src.bad())) {
        FailOperation("istream::read()");
      } else {
        // End of stream is not a failure.
        //
        // A sticky `std::ios_base::eofbit` breaks future operations like
        // `std::istream::peek()` and `std::istream::tellg()`.
        src.clear(src.rdstate() &
                  ~(std::ios_base::eofbit | std::ios_base::failbit));
        FoundSize(limit_pos());
      }
      return IntCast<size_t>(length_read) >= min_length;
    }
    if (IntCast<size_t>(length_read) >= min_length) return true;
    dest += length_read;
    min_length -= IntCast<size_t>(length_read);
    max_length -= IntCast<size_t>(length_read);
  }
}

bool IStreamReaderBase::SeekBehindBuffer(Position new_pos) {
  RIEGELI_ASSERT(new_pos < start_pos() || new_pos > limit_pos())
      << "Failed precondition of BufferedReader::SeekBehindBuffer(): "
         "position in the buffer, use Seek() instead";
  RIEGELI_ASSERT_EQ(start_to_limit(), 0u)
      << "Failed precondition of BufferedReader::SeekBehindBuffer(): "
         "buffer not empty";
  if (ABSL_PREDICT_FALSE(!supports_random_access())) {
    return BufferedReader::SeekBehindBuffer(new_pos);
  }
  if (ABSL_PREDICT_FALSE(!ok())) return false;
  std::istream& src = *src_stream();
  errno = 0;
  if (new_pos > limit_pos()) {
    // Seeking forwards.
    if (size_ != absl::nullopt) {
      if (ABSL_PREDICT_FALSE(new_pos > *size_)) {
        // Stream ends.
        src.seekg(IntCast<std::streamoff>(*size_), std::ios_base::beg);
        if (ABSL_PREDICT_FALSE(src.fail())) {
          return FailOperation("istream::seekg()");
        }
        set_limit_pos(*size_);
        return false;
      }
    } else {
      src.seekg(0, std::ios_base::end);
      if (ABSL_PREDICT_FALSE(src.fail())) {
        return FailOperation("istream::seekg()");
      }
      const std::streamoff stream_size = src.tellg();
      if (ABSL_PREDICT_FALSE(stream_size < 0)) {
        return FailOperation("istream::tellg()");
      }
      FoundSize(IntCast<Position>(stream_size));
      if (ABSL_PREDICT_FALSE(new_pos > IntCast<Position>(stream_size))) {
        // Stream ends.
        set_limit_pos(IntCast<Position>(stream_size));
        return false;
      }
    }
  }
  src.seekg(IntCast<std::streamoff>(new_pos), std::ios_base::beg);
  if (ABSL_PREDICT_FALSE(src.fail())) return FailOperation("istream::seekg()");
  set_limit_pos(new_pos);
  return true;
}

absl::optional<Position> IStreamReaderBase::SizeImpl() {
  if (ABSL_PREDICT_FALSE(!supports_random_access())) {
    // Delegate to base class version which fails, to avoid duplicating the
    // failure message here.
    return BufferedReader::SizeImpl();
  }
  if (ABSL_PREDICT_FALSE(!ok())) return absl::nullopt;
  if (size_ != absl::nullopt) return *size_;
  std::istream& src = *src_stream();
  errno = 0;
  src.seekg(0, std::ios_base::end);
  if (ABSL_PREDICT_FALSE(src.fail())) {
    FailOperation("istream::seekg()");
    return absl::nullopt;
  }
  const std::streamoff stream_size = src.tellg();
  if (ABSL_PREDICT_FALSE(stream_size < 0)) {
    FailOperation("istream::tellg()");
    return absl::nullopt;
  }
  src.seekg(IntCast<std::streamoff>(limit_pos()), std::ios_base::beg);
  if (ABSL_PREDICT_FALSE(src.fail())) {
    FailOperation("istream::seekg()");
    return absl::nullopt;
  }
  FoundSize(IntCast<Position>(stream_size));
  return IntCast<Position>(stream_size);
}

}  // namespace riegeli
