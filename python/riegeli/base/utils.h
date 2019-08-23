// Copyright 2018 Google LLC
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

#ifndef PYTHON_RIEGELI_BASE_UTILS_H_
#define PYTHON_RIEGELI_BASE_UTILS_H_

// From https://docs.python.org/3/c-api/intro.html:
// Since Python may define some pre-processor definitions which affect the
// standard headers on some systems, you must include Python.h before any
// standard headers are included.
#include <Python.h>
// clang-format: do not reorder the above include.

#include <stddef.h>

#include <memory>
#include <new>
#include <string>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/meta/type_traits.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/status.h"

namespace riegeli {
namespace python {

#if PY_VERSION_HEX < 0x03020000
using Py_hash_t = long;
#endif

// Ensures that Python GIL is locked. Reentrant.
//
// Same as PyGILState_Ensure() / PyGILState_Release().
class PythonLock {
 public:
  static void AssertHeld() {
    RIEGELI_ASSERT(
#if PY_MAJOR_VERSION >= 3
        PyGILState_Check()
#else
        _PyThreadState_Current != nullptr &&
        _PyThreadState_Current == PyGILState_GetThisThreadState()
#endif
            )
        << "Python GIL was assumed to be held";
  }

  PythonLock() { gstate_ = PyGILState_Ensure(); }

  PythonLock(const PythonLock&) = delete;
  PythonLock& operator=(const PythonLock&) = delete;

  ~PythonLock() { PyGILState_Release(gstate_); }

 private:
  PyGILState_STATE gstate_;
};

// Unlocks Python GIL, allowing non-Python threads to run.
//
// Same as Py_BEGIN_ALLOW_THREADS / Py_END_ALLOW_THREADS.
class PythonUnlock {
 public:
  PythonUnlock() {
    PythonLock::AssertHeld();
    tstate_ = PyEval_SaveThread();
  }

  PythonUnlock(const PythonUnlock&) = delete;
  PythonUnlock& operator=(const PythonUnlock&) = delete;

  ~PythonUnlock() { PyEval_RestoreThread(tstate_); }

 private:
  PyThreadState* tstate_;
};

// Apply a function with Python GIL unlocked, allowing non-Python threads to
// run.
//
// Same as Py_BEGIN_ALLOW_THREADS / Py_END_ALLOW_THREADS.
//
// TODO: When C++17 is available:
// template <typename Function, typename... Args>
// std::invoke_result_t<Function> PythonUnlocked(Function&& f, Args&&... args) {
//   PythonUnlock unlock;
//   return std::invoke(std::forward<Function>(f), std::forward<Args>(args)...);
// }
template <typename Function>
absl::result_of_t<Function()> PythonUnlocked(Function&& f) {
  PythonUnlock unlock;
  return std::forward<Function>(f)();
}

// Owned PyObject which assumes that Python GIL is held.

struct Deleter {
  template <typename T>
  void operator()(T* ptr) const {
    PythonLock::AssertHeld();
    Py_DECREF(ptr);
  }
};

using PythonPtr = std::unique_ptr<PyObject, Deleter>;

// Owned PyObject which does not assume that Python GIL is held.

struct LockingDeleter {
  template <typename T>
  void operator()(T* ptr) const {
    PythonLock lock;
    Py_DECREF(ptr);
  }
};

using PythonPtrLocking = std::unique_ptr<PyObject, LockingDeleter>;

// Allows a C++ object to be safely embedded in a Python object allocated with
// PyType_GenericAlloc().
//
// PythonWrapped<T> is similar to optional<T>, but:
//  * PythonWrapped<T> is POD.
//  * PythonWrapped<T> supports only a subset of optional<T> API.
//  * PythonWrapped<T> filled with zero bytes is valid and absent
//    (PyType_GenericAlloc() fills the Python object with zero bytes).
//  * PythonWrapped<T> should be explicitly reset() in the implementation of
//    tp_dealloc (there is no C++ destructor).
template <typename T>
class PythonWrapped {
 public:
  static_assert(alignof(T) <= alignof(max_align_t),
                "PythonWrapped does not support overaligned types");

  template <typename... Args>
  void emplace(Args&&... args) {
    if (has_value_) {
      reinterpret_cast<T*>(storage_)->~T();
    } else {
      has_value_ = true;
    }
    new (storage_) T(std::forward<Args>(args)...);
  }

  void reset() {
    if (has_value_) {
      has_value_ = false;
      reinterpret_cast<T*>(storage_)->~T();
    }
  }

  bool has_value() const { return has_value_; }

  T* get() {
    RIEGELI_ASSERT(has_value_) << "Object uninitialized";
    return reinterpret_cast<T*>(storage_);
  }
  const T* get() const {
    RIEGELI_ASSERT(has_value_) << "Object uninitialized";
    return reinterpret_cast<const T*>(storage_);
  }
  T& operator*() { return *get(); }
  const T& operator*() const { return *get(); }
  T* operator->() { return get(); }
  const T* operator->() const { return get(); }

  bool Verify() const {
    PythonLock::AssertHeld();
    if (ABSL_PREDICT_FALSE(!has_value())) {
      PyErr_SetString(PyExc_ValueError, "Object uninitialized");
      return false;
    }
    return true;
  }

 private:
  bool has_value_;
  alignas(T) char storage_[sizeof(T)];
};

// Represents an optional Python exception being raised.
class Exception {
 public:
  // No exception.
  Exception() noexcept {}

  Exception(const Exception& that) noexcept;
  Exception& operator=(const Exception& that) noexcept;

  Exception(Exception&& that) noexcept;
  Exception& operator=(Exception&& that) noexcept;

  // Fetches the active Python exception.
  static Exception Fetch();

  // Restores the active Python exception.
  PyObject* Restore() const&;
  PyObject* Restore() &&;

  bool ok() const { return type_ == nullptr; }

  std::string message() const;

  // For implementing tp_traverse of objects containing Exception.
  int Traverse(visitproc visit, void* arg);

 private:
  // Steals references.
  explicit Exception(PyObject* type, PyObject* value, PyObject* traceback)
      : type_(type), value_(value), traceback_(traceback) {}

  PythonPtrLocking type_;
  PythonPtrLocking value_;
  PythonPtrLocking traceback_;
};

// Translate a failed status to the active Python exception, a class extending
// RiegeliError.
void SetRiegeliError(const Status& status);

namespace internal {

// Lazily initialized pointer to a Python object, persisting until interpreter
// shutdown.
class StaticObject {
 protected:
  mutable PyObject* value_ = nullptr;
  mutable const StaticObject* next_ = nullptr;

  // Register this object in a global list of static objects. This must be
  // called when value_ is allocated.
  void RegisterThis() const;

 private:
  friend void FreeStaticObjectsImpl();
};

// Template parameter invariant part of ImportedCapsule.
class ImportedCapsuleBase {
 public:
  // Forces importing the value, returning false on failures (with Python
  // exception set).
  //
  // If Verify() returns true, get() does not die.
  bool Verify() const {
    PythonLock::AssertHeld();
    if (ABSL_PREDICT_FALSE(value_ == nullptr)) return ImportValue();
    return true;
  }

 protected:
  explicit constexpr ImportedCapsuleBase(const char* capsule_name)
      : capsule_name_(capsule_name) {}

  bool ImportValue() const;

  mutable void* value_ = nullptr;

 private:
  const char* capsule_name_;
};

}  // namespace internal

// Creates a Python string (type str) which persists until interpreter shutdown.
// This is useful for attribute or method names in PyObject_GetAttr() or
// PyObject_CallMethodObjArgs().
//
// An instance of Identifier should be allocated statically:
//
//   static constexpr Identifier id_write("write");
//
// Then id_write.get() is a borrowed reference to the Python object.
class Identifier : public internal::StaticObject {
 public:
  explicit constexpr Identifier(absl::string_view name) : name_(name) {}

  // Forces allocating the value, returning false on failures (with Python
  // exception set).
  //
  // If Verify() returns true, get() does not die.
  bool Verify() const {
    PythonLock::AssertHeld();
    if (ABSL_PREDICT_FALSE(value_ == nullptr)) return AllocateValue();
    return true;
  }

  // Returns the value, allocating it on the first call. Dies on failure
  // (use Verify() to prevent this).
  PyObject* get() const {
    PythonLock::AssertHeld();
    if (ABSL_PREDICT_FALSE(value_ == nullptr)) {
      RIEGELI_CHECK(AllocateValue()) << Exception::Fetch().message();
    }
    return value_;
  }

 private:
  bool AllocateValue() const;

  absl::string_view name_;
};

// Imports a Python module and gets its attribute, which persists until
// interpreter shutdown.
//
// An instance of ImportedConstant should be allocated statically:
//
//   static constexpr ImportedConstant kRiegeliError(
//       "riegeli.base.riegeli_error", "RiegeliError");
//
// Then kRiegeliError.get() is a borrowed reference to the Python object.
class ImportedConstant : public internal::StaticObject {
 public:
  explicit constexpr ImportedConstant(absl::string_view module_name,
                                      absl::string_view attr_name)
      : module_name_(module_name), attr_name_(attr_name) {}

  // Forces importing the value, returning false on failures (with Python
  // exception set).
  //
  // If Verify() returns true, get() does not die.
  bool Verify() const {
    PythonLock::AssertHeld();
    if (ABSL_PREDICT_FALSE(value_ == nullptr)) return AllocateValue();
    return true;
  }

  // Returns the value, importing it on the first call. Dies on failure
  // (use Verify() to prevent this).
  PyObject* get() const {
    PythonLock::AssertHeld();
    if (ABSL_PREDICT_FALSE(value_ == nullptr)) {
      RIEGELI_CHECK(AllocateValue()) << Exception::Fetch().message();
    }
    return value_;
  }

 private:
  bool AllocateValue() const;

  absl::string_view module_name_;
  absl::string_view attr_name_;
};

// Exports a Python capsule containing a C++ pointer, which should be valid
// forever, by adding it to the given module.
//
// capsule_name must be "module_name.attr_name" with module_name corresponding
// to PyModule_GetName(module).
//
// Returns false on failure (with Python exception set).
bool ExportCapsule(PyObject* module, const char* capsule_name, const void* ptr);

// Imports a Python capsule and gets its stored pointer, which persists forever.
//
// capsule_name must be "module_name.attr_name".
//
// An instance of ImportedCapsule should be allocated statically:
//
//   static constexpr ImportedCapsule<RecordPositionApi> kRecordPositionApi(
//       "riegeli.records.record_position._CPPAPI");
//
// Then kRecordPositionApi.get() is a pointer stored in the capsule.
template <typename T>
class ImportedCapsule : public internal::ImportedCapsuleBase {
 public:
  explicit constexpr ImportedCapsule(const char* capsule_name)
      : ImportedCapsuleBase(capsule_name) {}

  // Returns the value, importing it on the first call. Dies on failure
  // (use Verify() to prevent this).
  const T* get() const {
    PythonLock::AssertHeld();
    if (ABSL_PREDICT_FALSE(value_ == nullptr)) {
      RIEGELI_CHECK(ImportValue()) << Exception::Fetch().message();
    }
    return static_cast<const T*>(value_);
  }

  const T& operator*() const { return *get(); }
  const T* operator->() const { return get(); }
};

// Converts C++ long to a Python int object.
//
// Returns nullptr on failure (with Python exception set).
inline PythonPtr IntToPython(long value) {
#if PY_MAJOR_VERSION >= 3
  return PythonPtr(PyLong_FromLong(value));
#else
  return PythonPtr(PyInt_FromLong(value));
#endif
}

// Converts C++ string_view to a Python bytes object (AKA str in Python2).
//
// Returns nullptr on failure (with Python exception set).
inline PythonPtr BytesToPython(absl::string_view value) {
  return PythonPtr(PyBytes_FromStringAndSize(
      value.data(), IntCast<Py_ssize_t>(value.size())));
}

// Refers to internals of a Python bytes-like object, using the buffer protocol.
class BytesLike {
 public:
  BytesLike() noexcept { buffer_.obj = nullptr; }

  BytesLike(const BytesLike&) = delete;
  BytesLike& operator=(const BytesLike&) = delete;

  ~BytesLike() {
    PythonLock::AssertHeld();
    if (buffer_.obj != nullptr) PyBuffer_Release(&buffer_);
  }

  // Converts from a Python object.
  //
  // Returns false on failure (with Python exception set).
  bool FromPython(PyObject* object) {
    return PyObject_GetBuffer(object, &buffer_, PyBUF_CONTIG_RO) == 0;
  }

  // Returns the binary contents.
  absl::string_view data() const {
    return absl::string_view(static_cast<const char*>(buffer_.buf),
                             IntCast<size_t>(buffer_.len));
  }

 private:
  Py_buffer buffer_;
};

// Converts C++ string_view to a Python str object. In Python3 Unicode is
// converted from UTF-8.
//
// Returns nullptr on failure (with Python exception set).
inline PythonPtr StringToPython(absl::string_view value) {
#if PY_MAJOR_VERSION >= 3
  return PythonPtr(PyUnicode_FromStringAndSize(
      value.data(), IntCast<Py_ssize_t>(value.size())));
#else
  return PythonPtr(PyString_FromStringAndSize(
      value.data(), IntCast<Py_ssize_t>(value.size())));
#endif
}

// Refers to internals of a Python object representing text. Valid Python
// objects are Text (i.e. str in Python3, unicode in Python2) or bytes
// (AKA str in Python2). Unicode is converted to UTF-8.
class TextOrBytes {
 public:
  TextOrBytes() noexcept {}

  TextOrBytes(const TextOrBytes&) = delete;
  TextOrBytes& operator=(const TextOrBytes&) = delete;

  // Converts from a Python object.
  //
  // Returns false on failure (with Python exception set).
  bool FromPython(PyObject* object);

  // Returns the text contents.
  absl::string_view data() const { return data_; }

 private:
  absl::string_view data_;
#if PY_VERSION_HEX < 0x03030000
  PythonPtr utf8_;
#endif
};

// Type for docstrings.
#if PY_MAJOR_VERSION >= 3
#define RIEGELI_TEXT_OR_BYTES "Union[str, bytes]"
#else
#define RIEGELI_TEXT_OR_BYTES "Union[str, unicode]"
#endif

// Converts C++ Chain to a Python bytes object (AKA str in Python2).
//
// Returns nullptr on failure (with Python exception set).
PythonPtr ChainToPython(const Chain& value);

// Converts C++ Chain from a Python bytes-like object, using the buffer
// protocol.
//
// Returns false on failure (with Python exception set).
bool ChainFromPython(PyObject* object, Chain* value);

// Converts C++ size_t to a Python int object (or possibly long in Python2).
//
// Returns nullptr on failure (with Python exception set).
PythonPtr SizeToPython(size_t value);

// Converts a Python object to C++ size_t. Valid Python objects are the same
// as for slicing: int, long (in Python2), or objects supporting __index__().
//
// Returns false on failure (with Python exception set).
bool SizeFromPython(PyObject* object, size_t* value);

// Converts C++ Position to a Python int object (or possibly long in Python2).
//
// Returns nullptr on failure (with Python exception set).
PythonPtr PositionToPython(Position value);

// Converts a Python object to C++ Position. Valid Python objects are the same
// as for slicing: int, long (in Python2), or objects supporting __index__().
//
// Returns false on failure (with Python exception set).
bool PositionFromPython(PyObject* object, Position* value);

// Implementation details follow.

inline Exception::Exception(const Exception& that) noexcept { *this = that; }

inline Exception::Exception(Exception&& that) noexcept
    : type_(std::move(that.type_)),
      value_(std::move(that.value_)),
      traceback_(std::move(that.traceback_)) {}

inline Exception& Exception::operator=(Exception&& that) noexcept {
  type_ = std::move(that.type_);
  value_ = std::move(that.value_);
  traceback_ = std::move(that.traceback_);
  return *this;
}

inline int Exception::Traverse(visitproc visit, void* arg) {
  Py_VISIT(type_.get());
  Py_VISIT(value_.get());
  Py_VISIT(traceback_.get());
  return 0;
}

}  // namespace python
}  // namespace riegeli

#endif  // PYTHON_RIEGELI_BASE_UTILS_H_
