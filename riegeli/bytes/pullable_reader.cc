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

#include "riegeli/bytes/pullable_reader.h"

#include <stddef.h>

#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "riegeli/base/base.h"
#include "riegeli/base/buffer.h"
#include "riegeli/base/chain.h"
#include "riegeli/bytes/backward_writer.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

void PullableReader::DoneBehindScratch() {
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of PullableReader::DoneBehindScratch(): "
         "scratch used";
  SyncBehindScratch(SyncType::kFromObject);
}

void PullableReader::Done() {
  if (ABSL_PREDICT_FALSE(scratch_used()) && !ScratchEnds()) {
    if (!SupportsRandomAccess()) {
      // Seeking back is not feasible.
      Reader::Done();
      scratch_.reset();
      return;
    }
    const Position new_pos = pos();
    SyncScratch();
    Seek(new_pos);
  }
  DoneBehindScratch();
  Reader::Done();
  scratch_.reset();
}

void PullableReader::SyncScratch() {
  RIEGELI_ASSERT(scratch_used())
      << "Failed precondition of PullableReader::SyncScratch(): "
         "scratch not used";
  RIEGELI_ASSERT(start() == scratch_->buffer.data())
      << "Failed invariant of PullableReader: "
         "scratch used but buffer pointers do not point to scratch";
  RIEGELI_ASSERT_EQ(start_to_limit(), scratch_->buffer.size())
      << "Failed invariant of PullableReader: "
         "scratch used but buffer pointers do not point to scratch";
  scratch_->buffer.Clear();
  set_buffer(scratch_->original_start, scratch_->original_start_to_limit,
             scratch_->original_start_to_cursor);
  move_limit_pos(available());
}

inline bool PullableReader::ScratchEnds() {
  RIEGELI_ASSERT(scratch_used())
      << "Failed precondition of PullableReader::ScratchEnds(): "
         "scratch not used";
  const size_t available_length = available();
  if (scratch_->original_start_to_cursor >= available_length) {
    SyncScratch();
    set_cursor(cursor() - available_length);
    return true;
  }
  return false;
}

bool PullableReader::PullSlow(size_t min_length, size_t recommended_length) {
  RIEGELI_ASSERT_LT(available(), min_length)
      << "Failed precondition of Reader::PullSlow(): "
         "enough data available, use Pull() instead";
  if (ABSL_PREDICT_FALSE(min_length > 1)) {
    if (scratch_used() && ScratchEnds() && available() >= min_length) {
      return true;
    }
    if (available() == 0) {
      RIEGELI_ASSERT(!scratch_used())
          << "Scratch should have ended but is still used";
      if (ABSL_PREDICT_FALSE(!PullBehindScratch())) return false;
      if (available() >= min_length) return true;
    }
    size_t remaining_min_length = min_length;
    recommended_length = UnsignedMax(min_length, recommended_length);
    size_t max_length = SaturatingAdd(recommended_length, recommended_length);
    std::unique_ptr<Scratch> new_scratch;
    if (ABSL_PREDICT_FALSE(scratch_ == nullptr)) {
      new_scratch = std::make_unique<Scratch>();
    } else {
      new_scratch = std::move(scratch_);
      if (!new_scratch->buffer.empty()) {
        // Scratch is used but it does have enough data after the cursor.
        new_scratch->buffer.RemovePrefix(start_to_cursor());
        remaining_min_length -= new_scratch->buffer.size();
        recommended_length -= new_scratch->buffer.size();
        max_length -= new_scratch->buffer.size();
        set_buffer(new_scratch->original_start,
                   new_scratch->original_start_to_limit,
                   new_scratch->original_start_to_cursor);
        move_limit_pos(available());
      }
    }
    const absl::Span<char> flat_buffer = new_scratch->buffer.AppendBuffer(
        remaining_min_length, recommended_length, max_length);
    char* dest = flat_buffer.data();
    char* const min_limit = flat_buffer.data() + remaining_min_length;
    char* const max_limit = flat_buffer.data() + flat_buffer.size();
    do {
      const size_t length =
          UnsignedMin(available(), PtrDistance(dest, max_limit));
      if (
          // `std::memcpy(_, nullptr, 0)` is undefined.
          length > 0) {
        std::memcpy(dest, cursor(), length);
        move_cursor(length);
        dest += length;
        if (dest >= min_limit) break;
      }
      if (ABSL_PREDICT_FALSE(scratch_used())) {
        SyncScratch();
        if (available() > 0) continue;
      }
    } while (PullBehindScratch());
    new_scratch->buffer.RemoveSuffix(PtrDistance(dest, max_limit));
    set_limit_pos(pos());
    new_scratch->original_start = start();
    new_scratch->original_start_to_limit = start_to_limit();
    new_scratch->original_start_to_cursor = start_to_cursor();
    scratch_ = std::move(new_scratch);
    set_buffer(scratch_->buffer.data(), scratch_->buffer.size());
    return available() >= min_length;
  }
  if (ABSL_PREDICT_FALSE(scratch_used())) {
    SyncScratch();
    if (available() > 0) return true;
  }
  return PullBehindScratch();
}

bool PullableReader::ReadBehindScratch(size_t length, char* dest) {
  RIEGELI_ASSERT_LT(available(), length)
      << "Failed precondition of PullableReader::ReadBehindScratch(char*): "
         "enough data available, use Read(char*) instead";
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of PullableReader::ReadBehindScratch(char*): "
         "scratch used";
  do {
    const size_t available_length = available();
    if (
        // `std::memcpy(_, nullptr, 0)` is undefined.
        available_length > 0) {
      std::memcpy(dest, cursor(), available_length);
      move_cursor(available_length);
      dest += available_length;
      length -= available_length;
    }
    if (ABSL_PREDICT_FALSE(!PullBehindScratch())) return false;
  } while (length > available());
  std::memcpy(dest, cursor(), length);
  move_cursor(length);
  return true;
}

bool PullableReader::ReadBehindScratch(size_t length, Chain& dest) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of PullableReader::ReadBehindScratch(Chain&): "
         "enough data available, use Read(Chain&) instead";
  RIEGELI_ASSERT_LE(length, std::numeric_limits<size_t>::max() - dest.size())
      << "Failed precondition of PullableReader::ReadBehindScratch(Chain&): "
         "Chain size overflow";
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of PullableReader::ReadBehindScratch(Chain&): "
         "scratch used";
  do {
    const absl::Span<char> buffer = dest.AppendBuffer(1, length, length);
    const Position pos_before = pos();
    if (ABSL_PREDICT_FALSE(!Read(buffer.size(), buffer.data()))) {
      RIEGELI_ASSERT_GE(pos(), pos_before)
          << "Reader::Read(char*) decreased pos()";
      const Position length_read = pos() - pos_before;
      RIEGELI_ASSERT_LE(length_read, buffer.size())
          << "Reader::Read(char*) read more than requested";
      dest.RemoveSuffix(buffer.size() - IntCast<size_t>(length_read));
      return false;
    }
    length -= buffer.size();
  } while (length > 0);
  return true;
}

bool PullableReader::ReadBehindScratch(size_t length, absl::Cord& dest) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of PullableReader::ReadBehindScratch(Cord&): "
         "enough data available, use Read(Cord&) instead";
  RIEGELI_ASSERT_LE(length, std::numeric_limits<size_t>::max() - dest.size())
      << "Failed precondition of PullableReader::ReadBehindScratch(Cord&): "
         "Cord size overflow";
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of PullableReader::ReadBehindScratch(Cord&): "
         "scratch used";
  Buffer buffer;
  do {
    buffer.Reset(UnsignedMin(length, kMaxBufferSize));
    const size_t length_to_read = UnsignedMin(length, buffer.capacity());
    const Position pos_before = pos();
    if (ABSL_PREDICT_FALSE(!Read(length_to_read, buffer.data()))) {
      RIEGELI_ASSERT_GE(pos(), pos_before)
          << "Reader::Read(char*) decreased pos()";
      const Position length_read = pos() - pos_before;
      RIEGELI_ASSERT_LE(length_read, length_to_read)
          << "Reader::Read(char*) read more than requested";
      if (length_read <= MaxBytesToCopyToCord(dest)) {
        dest.Append(
            absl::string_view(buffer.data(), IntCast<size_t>(length_read)));
      } else {
        dest.Append(buffer.ToCord(
            absl::string_view(buffer.data(), IntCast<size_t>(length_read))));
      }
      return false;
    }
    if (length_to_read <= MaxBytesToCopyToCord(dest)) {
      dest.Append(absl::string_view(buffer.data(), length_to_read));
    } else {
      dest.Append(
          buffer.ToCord(absl::string_view(buffer.data(), length_to_read)));
    }
    length -= length_to_read;
  } while (length > 0);
  return true;
}

bool PullableReader::CopyBehindScratch(Position length, Writer& dest) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of PullableReader::CopyBehindScratch(Writer&): "
         "enough data available, use Copy(Writer&) instead";
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of PullableReader::CopyBehindScratch(Writer&): "
         "scratch used";
  while (length > available()) {
    const absl::string_view data(cursor(), available());
    move_cursor(data.size());
    if (ABSL_PREDICT_FALSE(!dest.Write(data))) return false;
    length -= data.size();
    if (ABSL_PREDICT_FALSE(!PullBehindScratch())) return false;
  }
  const absl::string_view data(cursor(), IntCast<size_t>(length));
  move_cursor(IntCast<size_t>(length));
  return dest.Write(data);
}

bool PullableReader::CopyBehindScratch(size_t length, BackwardWriter& dest) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of "
         "PullableReader::CopyBehindScratch(BackwardWriter&): "
         "enough data available, use Copy(BackwardWriter&) instead";
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of "
         "PullableReader::CopyBehindScratch(BackwardWriter&): "
         "scratch used";
  if (length <= available()) {
    const absl::string_view data(cursor(), length);
    move_cursor(length);
    return dest.Write(data);
  }
  if (length <= kMaxBytesToCopy) {
    if (ABSL_PREDICT_FALSE(!dest.Push(length))) return false;
    dest.move_cursor(length);
    if (ABSL_PREDICT_FALSE(!ReadBehindScratch(length, dest.cursor()))) {
      dest.set_cursor(dest.cursor() + length);
      return false;
    }
    return true;
  }
  Chain data;
  if (ABSL_PREDICT_FALSE(!ReadBehindScratch(length, data))) return false;
  return dest.Write(std::move(data));
}

void PullableReader::ReadHintBehindScratch(size_t length) {
  RIEGELI_ASSERT_LT(available(), length)
      << "Failed precondition of PullableReader::ReadHintBehindScratch(): "
         "enough data available, use ReadHint() instead";
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of PullableReader::ReadHintBehindScratch(): "
         "scratch used";
}

bool PullableReader::SyncBehindScratch(SyncType sync_type) {
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of PullableReader::SyncBehindScratch(): "
         "scratch used";
  return healthy();
}

bool PullableReader::SeekBehindScratch(Position new_pos) {
  RIEGELI_ASSERT(new_pos < start_pos() || new_pos > limit_pos())
      << "Failed precondition of PullableReader::SeekBehindScratch(): "
         "position in the buffer, use Seek() instead";
  RIEGELI_ASSERT(!scratch_used())
      << "Failed precondition of PullableReader::SeekBehindScratch(): "
         "scratch used";
  if (ABSL_PREDICT_FALSE(new_pos <= limit_pos())) {
    return Fail(
        absl::UnimplementedError("Reader::Seek() backwards not supported"));
  }
  // Seeking forwards.
  do {
    move_cursor(available());
    if (ABSL_PREDICT_FALSE(!PullBehindScratch())) return false;
  } while (new_pos > limit_pos());
  const Position available_length = limit_pos() - new_pos;
  RIEGELI_ASSERT_LE(available_length, start_to_limit())
      << "PullableReader::PullBehindScratch() skipped some data";
  set_cursor(limit() - available_length);
  return true;
}

bool PullableReader::ReadSlow(size_t length, char* dest) {
  RIEGELI_ASSERT_LT(available(), length)
      << "Failed precondition of Reader::ReadSlow(char*): "
         "enough data available, use Read(char*) instead";
  if (ABSL_PREDICT_FALSE(scratch_used())) {
    if (!ScratchEnds()) {
      const size_t length_to_read = available();
      std::memcpy(dest, cursor(), length_to_read);
      dest += length_to_read;
      length -= length_to_read;
      move_cursor(length_to_read);
      SyncScratch();
    }
    if (available() >= length) {
      // `std::memcpy(nullptr, _, 0)` and `std::memcpy(_, nullptr, 0)` are
      // undefined.
      if (ABSL_PREDICT_TRUE(length > 0)) {
        std::memcpy(dest, cursor(), length);
        move_cursor(length);
      }
      return true;
    }
  }
  return ReadBehindScratch(length, dest);
}

bool PullableReader::ReadSlow(size_t length, Chain& dest) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of Reader::ReadSlow(Chain&): "
         "enough data available, use Read(Chain&) instead";
  RIEGELI_ASSERT_LE(length, std::numeric_limits<size_t>::max() - dest.size())
      << "Failed precondition of Reader::ReadSlow(Chain&): "
         "Chain size overflow";
  if (ABSL_PREDICT_FALSE(scratch_used())) {
    if (!ScratchEnds()) {
      const size_t length_to_read = UnsignedMin(length, available());
      scratch_->buffer.AppendSubstrTo(
          absl::string_view(cursor(), length_to_read), dest);
      move_cursor(length_to_read);
      length -= length_to_read;
      if (length == 0) return true;
      SyncScratch();
    }
    if (available() >= length && length <= kMaxBytesToCopy) {
      dest.Append(absl::string_view(cursor(), length));
      move_cursor(length);
      return true;
    }
  }
  return ReadBehindScratch(length, dest);
}

bool PullableReader::ReadSlow(size_t length, absl::Cord& dest) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of Reader::ReadSlow(Cord&): "
         "enough data available, use Read(Cord&) instead";
  RIEGELI_ASSERT_LE(length, std::numeric_limits<size_t>::max() - dest.size())
      << "Failed precondition of Reader::ReadSlow(Cord&): "
         "Cord size overflow";
  if (ABSL_PREDICT_FALSE(scratch_used())) {
    if (!ScratchEnds()) {
      const size_t length_to_read = UnsignedMin(length, available());
      scratch_->buffer.AppendSubstrTo(
          absl::string_view(cursor(), length_to_read), dest);
      move_cursor(length_to_read);
      length -= length_to_read;
      if (length == 0) return true;
      SyncScratch();
    }
    if (available() >= length && length <= kMaxBytesToCopy) {
      dest.Append(absl::string_view(cursor(), length));
      move_cursor(length);
      return true;
    }
  }
  return ReadBehindScratch(length, dest);
}

bool PullableReader::CopySlow(Position length, Writer& dest) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of Reader::CopySlow(Writer&): "
         "enough data available, use Copy(Writer&) instead";
  if (ABSL_PREDICT_FALSE(scratch_used())) {
    if (!ScratchEnds()) {
      const size_t length_to_copy = UnsignedMin(length, available());
      bool ok;
      if (length_to_copy <= kMaxBytesToCopy || dest.PrefersCopying()) {
        ok = dest.Write(cursor(), length_to_copy);
      } else {
        Chain data;
        scratch_->buffer.AppendSubstrTo(
            absl::string_view(cursor(), length_to_copy), data);
        ok = dest.Write(std::move(data));
      }
      move_cursor(length_to_copy);
      if (ABSL_PREDICT_FALSE(!ok)) return false;
      length -= length_to_copy;
      if (length == 0) return true;
      SyncScratch();
    }
    if (available() >= length && length <= kMaxBytesToCopy) {
      const absl::string_view data(cursor(), IntCast<size_t>(length));
      move_cursor(IntCast<size_t>(length));
      return dest.Write(data);
    }
  }
  return CopyBehindScratch(length, dest);
}

bool PullableReader::CopySlow(size_t length, BackwardWriter& dest) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of Reader::CopySlow(BackwardWriter&): "
         "enough data available, use Copy(BackwardWriter&) instead";
  if (ABSL_PREDICT_FALSE(scratch_used())) {
    Chain from_scratch;
    if (!ScratchEnds()) {
      if (available() >= length) {
        bool ok;
        if (length <= kMaxBytesToCopy || dest.PrefersCopying()) {
          ok = dest.Write(cursor(), length);
        } else {
          Chain data;
          scratch_->buffer.AppendSubstrTo(absl::string_view(cursor(), length),
                                          data);
          ok = dest.Write(std::move(data));
        }
        move_cursor(length);
        return ok;
      }
      scratch_->buffer.AppendSubstrTo(absl::string_view(cursor(), available()),
                                      from_scratch);
      length -= available();
      SyncScratch();
    }
    if (available() >= length && length <= kMaxBytesToCopy) {
      const absl::string_view data(cursor(), length);
      move_cursor(length);
      if (ABSL_PREDICT_FALSE(!dest.Write(data))) return false;
    } else {
      if (ABSL_PREDICT_FALSE(!CopyBehindScratch(length, dest))) return false;
    }
    return dest.Write(std::move(from_scratch));
  }
  return CopyBehindScratch(length, dest);
}

void PullableReader::ReadHintSlow(size_t length) {
  RIEGELI_ASSERT_LT(available(), length)
      << "Failed precondition of Reader::ReadHintSlow(): "
         "enough data available, use ReadHint() instead";
  if (ABSL_PREDICT_FALSE(scratch_used())) {
    if (!ScratchEnds()) {
      length -= available();
      BehindScratch behind_scratch(this);
      if (available() < length) ReadHintBehindScratch(length);
      return;
    }
    if (available() >= length) return;
  }
  ReadHintBehindScratch(length);
}

bool PullableReader::SyncImpl(SyncType sync_type) {
  if (ABSL_PREDICT_FALSE(scratch_used()) && !ScratchEnds()) {
    if (!SupportsRandomAccess()) {
      // Seeking back is not feasible.
      return healthy();
    }
    const Position new_pos = pos();
    SyncScratch();
    Seek(new_pos);
  }
  return SyncBehindScratch(sync_type);
}

bool PullableReader::SeekSlow(Position new_pos) {
  RIEGELI_ASSERT(new_pos < start_pos() || new_pos > limit_pos())
      << "Failed precondition of Reader::SeekSlow(): "
         "position in the buffer, use Seek() instead";
  if (ABSL_PREDICT_FALSE(scratch_used())) {
    SyncScratch();
    if (new_pos >= start_pos() && new_pos <= limit_pos()) {
      set_cursor(limit() - (limit_pos() - new_pos));
      return true;
    }
  }
  return SeekBehindScratch(new_pos);
}

void PullableReader::BehindScratch::Enter() {
  RIEGELI_ASSERT(context_->scratch_used())
      << "Failed precondition of PullableReader::BehindScratch::Enter(): "
         "scratch not used";
  RIEGELI_ASSERT(context_->start() == context_->scratch_->buffer.data())
      << "Failed invariant of PullableReader: "
         "scratch used but buffer pointers do not point to scratch";
  RIEGELI_ASSERT_EQ(context_->start_to_limit(),
                    context_->scratch_->buffer.size())
      << "Failed invariant of PullableReader: "
         "scratch used but buffer pointers do not point to scratch";
  scratch_ = std::move(context_->scratch_);
  read_from_scratch_ = context_->start_to_cursor();
  context_->set_buffer(scratch_->original_start,
                       scratch_->original_start_to_limit,
                       scratch_->original_start_to_cursor);
  context_->move_limit_pos(context_->available());
}

void PullableReader::BehindScratch::Leave() {
  RIEGELI_ASSERT(scratch_ != nullptr)
      << "Failed precondition of PullableReader::BehindScratch::Leave(): "
         "scratch not used";
  context_->set_limit_pos(context_->pos());
  scratch_->original_start = context_->start();
  scratch_->original_start_to_limit = context_->start_to_limit();
  scratch_->original_start_to_cursor = context_->start_to_cursor();
  context_->set_buffer(scratch_->buffer.data(), scratch_->buffer.size(),
                       read_from_scratch_);
  context_->scratch_ = std::move(scratch_);
}

}  // namespace riegeli
