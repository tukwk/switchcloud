// Minimal C++ runtime pieces.
//
// The static libstdc++ shipped with this toolchain was built without -fPIC, so
// its RTTI tables and the libiberty demangler tables sit in .rodata and carry
// R_AARCH64_RELATIVE relocations. hbloader maps the NRO's second segment
// read-only, so applying those relocations would fault.
//
// None of that machinery is needed here: the code is compiled with
// -fno-exceptions -fno-rtti. By providing our own operator new/delete and the
// std::__throw_* helpers we stop the linker from pulling libstdc++'s exception
// runtime (functexcept.o, vterminate.o, cp-demangle.o, stdexcept.o, ...),
// leaving only position-independent code and writable-segment relocations.

#include <switch.h>
#include <cstdlib>
#include <cstddef>
#include <new>

#if defined(__cpp_aligned_new)
#include <malloc.h>
#endif

namespace {

[[noreturn]] void oom() {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
    abort();
}

} // namespace

void* operator new(std::size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) oom();
    return p;
}

void* operator new[](std::size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) oom();
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return malloc(size ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return malloc(size ? size : 1);
}

void* operator new(std::size_t size, std::align_val_t align) {
    void* p = memalign((std::size_t)align, size ? size : 1);
    if (!p) oom();
    return p;
}

void* operator new[](std::size_t size, std::align_val_t align) {
    void* p = memalign((std::size_t)align, size ? size : 1);
    if (!p) oom();
    return p;
}

void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, std::size_t) noexcept { free(p); }
void operator delete[](void* p, std::size_t) noexcept { free(p); }
void operator delete(void* p, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { free(p); }

// The following are normally provided by libstdc++'s functexcept.cc. They are
// only reached on a logic/allocation error, so aborting is fine for a homebrew
// app (and keeps the exception/RTTI runtime out of the binary).
namespace std {

[[noreturn]] static void fail(const char*) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
    abort();
}

[[noreturn]] void __throw_bad_exception() { fail("bad_exception"); }
[[noreturn]] void __throw_bad_alloc() { fail("bad_alloc"); }
[[noreturn]] void __throw_bad_array_new_length() { fail("bad_array_new_length"); }
[[noreturn]] void __throw_bad_cast() { fail("bad_cast"); }
[[noreturn]] void __throw_bad_typeid() { fail("bad_typeid"); }
[[noreturn]] void __throw_logic_error(const char*) { fail("logic_error"); }
[[noreturn]] void __throw_domain_error(const char*) { fail("domain_error"); }
[[noreturn]] void __throw_invalid_argument(const char*) { fail("invalid_argument"); }
[[noreturn]] void __throw_length_error(const char*) { fail("length_error"); }
[[noreturn]] void __throw_out_of_range(const char*) { fail("out_of_range"); }
[[noreturn]] void __throw_out_of_range_fmt(const char*, ...) { fail("out_of_range"); }
[[noreturn]] void __throw_runtime_error(const char*) { fail("runtime_error"); }
[[noreturn]] void __throw_range_error(const char*) { fail("range_error"); }
[[noreturn]] void __throw_overflow_error(const char*) { fail("overflow_error"); }
[[noreturn]] void __throw_underflow_error(const char*) { fail("underflow_error"); }
[[noreturn]] void __throw_ios_failure(const char*) { fail("ios_failure"); }
[[noreturn]] void __throw_system_error(int) { fail("system_error"); }
[[noreturn]] void __throw_future_error(int) { fail("future_error"); }
[[noreturn]] void __throw_bad_function_call() { fail("bad_function_call"); }

} // namespace std
