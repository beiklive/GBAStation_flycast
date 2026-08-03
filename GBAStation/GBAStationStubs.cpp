/// @file GBAStationStubs.cpp
/// @brief Stub implementations for functions not needed in the GBAStation overlay build
/// These functions are declared but not used in libretro mode with USE_SDL

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// SDL Haptic stubs - declared in core/input/haptic.h when USE_SDL is defined
// but implemented in core/sdl/sdl.cpp which we don't compile for libretro

void sdl_setTorque(int port, float v) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)v;
}

void sdl_setDamper(int port, float param, float speed) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)param;
    (void)speed;
}

void sdl_setSpring(int port, float saturation, float speed) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)saturation;
    (void)speed;
}

void sdl_setSine(int port, float power, float frequency, unsigned int duration_ms) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)power;
    (void)frequency;
    (void)duration_ms;
}

void sdl_stopHaptic(int port) {
    // Stub - haptic not supported in this build
    (void)port;
}

extern "C" {

using EGLBoolean = unsigned int;
using EGLenum = unsigned int;
using EGLint = int;
using EGLAttrib = intptr_t;
using EGLConfig = void *;
using EGLContext = void *;
using EGLDisplay = void *;
using EGLSurface = void *;
using EGLNativeDisplayType = void *;
using EGLNativeWindowType = void *;
using __eglMustCastToProperFunctionPointerType = void (*)();

static constexpr EGLBoolean kEglFalse = 0;
static constexpr EGLDisplay kEglNoDisplay = nullptr;
static constexpr EGLContext kEglNoContext = nullptr;
static constexpr EGLSurface kEglNoSurface = nullptr;
static constexpr EGLint kEglSuccess = 0x3000;

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *)
{
    return nullptr;
}

EGLDisplay eglGetPlatformDisplay(EGLenum, void *, const EGLAttrib *)
{
    return kEglNoDisplay;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType)
{
    return kEglNoDisplay;
}

EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *)
{
    return kEglFalse;
}

EGLBoolean eglTerminate(EGLDisplay)
{
    return kEglFalse;
}

EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)
{
    return kEglFalse;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint *)
{
    return kEglFalse;
}

EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *)
{
    return kEglNoContext;
}

EGLBoolean eglDestroyContext(EGLDisplay, EGLContext)
{
    return kEglFalse;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *)
{
    return kEglNoSurface;
}

EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *)
{
    return kEglNoSurface;
}

EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface)
{
    return kEglFalse;
}

EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext)
{
    return kEglFalse;
}

EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface)
{
    return kEglFalse;
}

EGLBoolean eglSwapInterval(EGLDisplay, EGLint)
{
    return kEglFalse;
}

EGLBoolean eglWaitNative(EGLint)
{
    return kEglFalse;
}

EGLBoolean eglWaitGL()
{
    return kEglFalse;
}

EGLBoolean eglBindAPI(EGLenum)
{
    return kEglFalse;
}

EGLenum eglQueryAPI()
{
    return 0;
}

const char *eglQueryString(EGLDisplay, EGLint)
{
    return nullptr;
}

EGLint eglGetError()
{
    return kEglSuccess;
}

int regcomp(void *preg, const char *regex, int cflags)
{
    (void)preg;
    (void)regex;
    (void)cflags;
    return 0;
}

int regexec(const void *preg, const char *str, size_t nmatch, void *pmatch, int eflags)
{
    (void)preg;
    (void)str;
    (void)nmatch;
    (void)pmatch;
    (void)eflags;
    return 1;
}

void regfree(void *preg)
{
    (void)preg;
}

uid_t getuid()
{
    return 0;
}

uid_t geteuid()
{
    return 0;
}

gid_t getgid()
{
    return 0;
}

gid_t getegid()
{
    return 0;
}

int flock(int fd, int operation)
{
    (void)fd;
    (void)operation;
    return 0;
}

int dirfd(void *dirp)
{
    (void)dirp;
    errno = ENOTSUP;
    return -1;
}

int fstatat(int dirfd_, const char *path, struct stat *st, int flags)
{
    (void)dirfd_;
#ifdef AT_SYMLINK_NOFOLLOW
    if (flags & AT_SYMLINK_NOFOLLOW) {
        return lstat(path, st);
    }
#else
    (void)flags;
#endif
    return stat(path, st);
}

int getpwuid_r(uid_t uid, void *pwd, char *buf, size_t buflen, void **result)
{
    (void)uid;
    (void)pwd;
    (void)buf;
    (void)buflen;
    if (result) {
        *result = nullptr;
    }
    return 0;
}

long sysconf(int name)
{
    switch (name) {
#ifdef _SC_PAGESIZE
    case _SC_PAGESIZE:
        return 0x1000;
#endif
#ifdef _SC_PAGE_SIZE
#if !defined(_SC_PAGESIZE) || _SC_PAGE_SIZE != _SC_PAGESIZE
    case _SC_PAGE_SIZE:
        return 0x1000;
#endif
#endif
#ifdef _SC_NPROCESSORS_ONLN
    case _SC_NPROCESSORS_ONLN:
        return 3;
#endif
#ifdef _SC_NPROCESSORS_CONF
    case _SC_NPROCESSORS_CONF:
        return 4;
#endif
#ifdef _SC_PHYS_PAGES
    case _SC_PHYS_PAGES:
        return (static_cast<uint64_t>(3072) << 20) / 0x1000;
#endif
    default:
        errno = EINVAL;
        return -1;
    }
}

int munmap(void *addr, size_t length)
{
    (void)addr;
    (void)length;
    return 0;
}

}
