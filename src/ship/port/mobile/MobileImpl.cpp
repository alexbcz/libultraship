#if defined(__ANDROID__) || defined(__IOS__)
#include "ship/port/mobile/MobileImpl.h"
#include <SDL2/SDL.h>
#include "libultraship/bridge/consolevariablebridge.h"

#include <imgui_internal.h>

static float cameraYaw;
static float cameraPitch;

static bool isShowingVirtualKeyboard = true;
static bool isUsingTouchscreenControls = true;

void Ship::Mobile::ImGuiProcessEvent(bool wantsTextInput) {
    ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetActiveID());

    if (wantsTextInput) {
        if (!isShowingVirtualKeyboard) {
            state->ClearText();

            isShowingVirtualKeyboard = true;
            SDL_StartTextInput();
        }
    } else {
        if (isShowingVirtualKeyboard) {
            isShowingVirtualKeyboard = false;
            SDL_StopTextInput();
        }
    }
}

#endif
#ifdef __ANDROID__
#include <SDL_gamecontroller.h>
#include <jni.h>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <malloc.h>
#include <chrono>
#include <cstdio>
#include <spdlog/spdlog.h>
#include "ship/port/mobile/CrashFrameWalker.h"

namespace {

constexpr size_t CRASH_REPORT_PATH_SIZE = 4096;
constexpr size_t CRASH_VERSION_SIZE = 128;
constexpr size_t CRASH_CONTEXT_SIZE = 2048;

char sCrashReportPath[CRASH_REPORT_PATH_SIZE] = {};
char sCrashVersion[CRASH_VERSION_SIZE] = {};
char sCrashContext[CRASH_CONTEXT_SIZE] = {};
volatile sig_atomic_t sHandlingCrash = 0;
struct sigaction sPreviousHandlers[NSIG] = {};
bool sHandlersInstalled = false;

// Installed separately on each participating thread, especially SDLThread.
struct ThreadDiagnostics {
    uintptr_t stackLow = 0;
    uintptr_t stackHigh = 0;
    void* alternateStack = MAP_FAILED;
    int memoryFd = -1;
    bool initialized = false;
    size_t alternateBytes = 0;
    static constexpr size_t alternateSize = 128 * 1024;
};
// Keep signal-handler TLS trivially initialized, even on an unregistered thread.
thread_local ThreadDiagnostics sThreadDiagnostics;
struct ThreadDiagnosticsLifetime {
    ~ThreadDiagnosticsLifetime() {
        auto& state = sThreadDiagnostics;
        if (state.alternateStack != MAP_FAILED) {
            stack_t disabled = {};
            disabled.ss_flags = SS_DISABLE;
            if (sigaltstack(&disabled, nullptr) == 0) {
                munmap(state.alternateStack, ThreadDiagnostics::alternateSize);
            }
        }
        if (state.memoryFd >= 0) {
            close(state.memoryFd);
        }
    }
};

void WriteAll(int fd, const char* text, size_t length) {
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written <= 0) {
            return;
        }
        text += written;
        length -= static_cast<size_t>(written);
    }
}

void WriteText(int fd, const char* text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    WriteAll(fd, text, length);
}

void WriteUnsigned(int fd, uint64_t value, unsigned base) {
    char digits[32];
    size_t count = 0;

    do {
        unsigned digit = value % base;
        digits[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= base;
    } while (value != 0 && count < sizeof(digits));

    while (count > 0) {
        WriteAll(fd, &digits[--count], 1);
    }
}

void WriteField(int fd, const char* label, uint64_t value, bool hex = false) {
    WriteText(fd, label);
    if (hex) {
        WriteText(fd, "0x");
    }
    WriteUnsigned(fd, value, hex ? 16 : 10);
    WriteText(fd, "\n");
}

void WriteSignedField(int fd, const char* label, int64_t value) {
    WriteText(fd, label);
    if (value < 0) {
        WriteText(fd, "-");
        WriteUnsigned(fd, static_cast<uint64_t>(-(value + 1)) + 1, 10);
    } else {
        WriteUnsigned(fd, static_cast<uint64_t>(value), 10);
    }
    WriteText(fd, "\n");
}

const char* SignalName(int signal) {
    switch (signal) {
        case SIGABRT:
            return "SIGABRT";
        case SIGBUS:
            return "SIGBUS";
        case SIGFPE:
            return "SIGFPE";
        case SIGILL:
            return "SIGILL";
        case SIGSEGV:
            return "SIGSEGV";
        default:
            return "unknown";
    }
}

void CopyProcessMaps(int reportFd) {
    int mapsFd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (mapsFd < 0) {
        return;
    }

    WriteText(reportFd, "\nProcess memory map:\n");
    char buffer[2048];
    ssize_t bytesRead;
    while ((bytesRead = read(mapsFd, buffer, sizeof(buffer))) > 0) {
        WriteAll(reportFd, buffer, static_cast<size_t>(bytesRead));
    }
    close(mapsFd);
}

void HandleNativeCrash(int signal, siginfo_t* info, void* context) {
    if (sHandlingCrash != 0) {
        _exit(128 + signal);
    }
    sHandlingCrash = 1;

    int fd = open(sCrashReportPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        WriteText(fd, "2 Ship 2 Harkinian Android crash report\n");
        WriteText(fd, "Report type: Native signal\n");
        WriteText(fd, "App version: ");
        WriteText(fd, sCrashVersion);
        WriteText(fd, "\nThread ID: ");
        WriteUnsigned(fd, static_cast<unsigned>(syscall(SYS_gettid)), 10);
        char threadName[17] = {};
        if (prctl(PR_GET_NAME, threadName, 0, 0, 0) == 0) {
            WriteText(fd, "\nThread name: ");
            WriteText(fd, threadName);
        }
        if (sCrashContext[0] != '\0') {
            WriteText(fd, "\nLast diagnostic: ");
            WriteText(fd, sCrashContext);
        }
        WriteText(fd, "\nSignal: ");
        WriteText(fd, SignalName(signal));
        WriteText(fd, " (");
        WriteUnsigned(fd, static_cast<unsigned>(signal), 10);
        WriteText(fd, ")\n");

        if (info != nullptr) {
            WriteSignedField(fd, "Signal code: ", info->si_code);
            WriteField(fd, "Fault address: ", reinterpret_cast<uintptr_t>(info->si_addr), true);
        }

#if defined(__aarch64__)
        if (context != nullptr) {
            auto* machineContext = &static_cast<ucontext_t*>(context)->uc_mcontext;
            WriteField(fd, "Program counter: ", machineContext->pc, true);
            WriteField(fd, "Stack pointer: ", machineContext->sp, true);
            WriteField(fd, "Link register: ", machineContext->regs[30], true);
            WriteField(fd, "Stack low: ", sThreadDiagnostics.stackLow, true);
            WriteField(fd, "Stack high: ", sThreadDiagnostics.stackHigh, true);
            WriteText(fd, "Frame chain (raw return addresses; may contain authentication bits):\n");
            size_t frameCount = Ship::CrashDiagnostics::WalkFrames(
                machineContext->regs[29], sThreadDiagnostics.stackLow, sThreadDiagnostics.stackHigh,
                [](uintptr_t address, void* buffer, size_t size) {
                    return sThreadDiagnostics.memoryFd >= 0 &&
                           pread(sThreadDiagnostics.memoryFd, buffer, size, static_cast<off_t>(address)) ==
                               static_cast<ssize_t>(size);
                },
                [fd](uintptr_t address) { WriteField(fd, "  return pc: ", address, true); });
            WriteField(fd, "Captured frames: ", frameCount);
            WriteText(fd, "Registers:\n");
            for (size_t i = 0; i < 31; i++) {
                WriteText(fd, "  X");
                WriteUnsigned(fd, i, 10);
                WriteText(fd, ": 0x");
                WriteUnsigned(fd, machineContext->regs[i], 16);
                WriteText(fd, "\n");
            }
        }
#endif

        CopyProcessMaps(fd);
        fsync(fd);
        close(fd);
    }

    // Preserve Android's native tombstone reporter and its original ucontext.
    // The Java reporter can recover that trace through ApplicationExitInfo.
    const struct sigaction previous = sPreviousHandlers[signal];
    sigaction(signal, &previous, nullptr);
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        if ((previous.sa_flags & SA_SIGINFO) != 0) {
            previous.sa_sigaction(signal, info, context);
        } else {
            previous.sa_handler(signal);
        }
    }
    struct sigaction fatal = {};
    fatal.sa_handler = SIG_DFL;
    sigemptyset(&fatal.sa_mask);
    sigaction(signal, &fatal, nullptr);
    raise(signal);
}

void InstallSignalHandler(int signal) {
    struct sigaction action = {};
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = HandleNativeCrash;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigaction(signal, &action, &sPreviousHandlers[signal]);
}

void CopyJavaString(JNIEnv* env, jstring source, char* destination, size_t destinationSize) {
    if (source == nullptr || destinationSize == 0) {
        return;
    }

    const char* value = env->GetStringUTFChars(source, nullptr);
    if (value == nullptr) {
        return;
    }
    std::strncpy(destination, value, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
    env->ReleaseStringUTFChars(source, value);
}

} // namespace

static std::atomic<bool> sGamepadBackPressed{false};

extern "C" void JNICALL Java_com_twoshipfork_mm_MainActivity_nativeInstallCrashReporter(
    JNIEnv* env, jobject, jstring reportPath, jstring versionName) {
    CopyJavaString(env, reportPath, sCrashReportPath, sizeof(sCrashReportPath));
    CopyJavaString(env, versionName, sCrashVersion, sizeof(sCrashVersion));

    Ship::Mobile::InitThreadDiagnostics();
    if (!sHandlersInstalled) {
        InstallSignalHandler(SIGABRT);
        InstallSignalHandler(SIGBUS);
        InstallSignalHandler(SIGFPE);
        InstallSignalHandler(SIGILL);
        InstallSignalHandler(SIGSEGV);
        sHandlersInstalled = true;
    }
}

extern "C" void JNICALL Java_com_twoshipfork_mm_MainActivity_nativeTestCrashReporter(JNIEnv*, jobject) {
    raise(SIGSEGV);
}

extern "C" void JNICALL Java_com_twoshipfork_mm_MainActivity_nativeGamepadBackPressed(JNIEnv* env, jobject obj) {
    sGamepadBackPressed = true;
}

bool Ship::Mobile::ConsumeGamepadBackPress() {
    return sGamepadBackPressed.exchange(false);
}

void Ship::Mobile::InitThreadDiagnostics() {
    thread_local ThreadDiagnosticsLifetime lifetime;
    (void)lifetime;
    auto& state = sThreadDiagnostics;
    if (state.initialized) {
        return;
    }
    state.initialized = true;
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
        void* base = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attributes, &base, &size) == 0) {
            state.stackLow = reinterpret_cast<uintptr_t>(base);
            state.stackHigh = state.stackLow + size;
        }
        pthread_attr_destroy(&attributes);
    }
    state.memoryFd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    stack_t existing = {};
    if (sigaltstack(nullptr, &existing) == 0 && (existing.ss_flags & SS_DISABLE) != 0) {
        state.alternateStack = mmap(nullptr, ThreadDiagnostics::alternateSize, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (state.alternateStack != MAP_FAILED) {
            stack_t alternate = {};
            alternate.ss_sp = state.alternateStack;
            alternate.ss_size = ThreadDiagnostics::alternateSize;
            if (sigaltstack(&alternate, nullptr) != 0) {
                munmap(state.alternateStack, ThreadDiagnostics::alternateSize);
                state.alternateStack = MAP_FAILED;
            }
        }
    }
    stack_t installed = {};
    if (sigaltstack(nullptr, &installed) == 0 && (installed.ss_flags & SS_DISABLE) == 0) {
        state.alternateBytes = installed.ss_size;
    }
}

void Ship::Mobile::LogMemorySample(size_t combinerCount) {
    using Clock = std::chrono::steady_clock;
    static auto start = Clock::now();
    static auto next = start;
    const auto now = Clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::seconds(30);
    unsigned long totalPages = 0, residentPages = 0;
    FILE* statm = fopen("/proc/self/statm", "r");
    const bool rssAvailable = statm && fscanf(statm, "%lu %lu", &totalPages, &residentPages) == 2;
    if (statm) {
        fclose(statm);
    }
    const auto heap = mallinfo();
    const uintptr_t currentStack = reinterpret_cast<uintptr_t>(&residentPages);
    const auto& state = sThreadDiagnostics;
    const size_t stackUsed = currentStack >= state.stackLow && currentStack < state.stackHigh
                                 ? state.stackHigh - currentStack : 0;
    SPDLOG_INFO("Android memory: elapsed_s={} rss_kib={} heap_allocated_kib={} combiners={} stack_sample_kib={} alt_stack_kib={} frame_reader={}",
                std::chrono::duration_cast<std::chrono::seconds>(now - start).count(),
                rssAvailable ? static_cast<int64_t>(residentPages * sysconf(_SC_PAGESIZE) / 1024) : -1,
                heap.uordblks / 1024, combinerCount, stackUsed / 1024, state.alternateBytes / 1024,
                state.memoryFd >= 0);
}

void Ship::Mobile::SetCrashContext(const char* context) {
    if (context == nullptr) {
        sCrashContext[0] = '\0';
        return;
    }
    std::strncpy(sCrashContext, context, sizeof(sCrashContext) - 1);
    sCrashContext[sizeof(sCrashContext) - 1] = '\0';
}

void Ship::Mobile::Init() {
    // None (add here Android initialization steps)
}

void Ship::Mobile::Exit() {
    SDL_Event quit_event;
    quit_event.type = SDL_QUIT;
    SDL_PushEvent(&quit_event);
    SDL_Quit();
    exit(0);
}

bool Ship::Mobile::IsUsingTouchscreenControls(){
    return isUsingTouchscreenControls;
}

void Ship::Mobile::EnableTouchArea(){
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject javaObject = (jobject)SDL_AndroidGetActivity();
    jclass javaClass = env->GetObjectClass(javaObject);
    jmethodID enabletoucharea = env->GetMethodID(javaClass, "EnableTouchArea", "()V");
    env->CallVoidMethod(javaObject, enabletoucharea);
}

void Ship::Mobile::DisableTouchArea(){
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject javaObject = (jobject)SDL_AndroidGetActivity();
    jclass javaClass = env->GetObjectClass(javaObject);
    jmethodID disabletoucharea = env->GetMethodID(javaClass, "DisableTouchArea", "()V");
    env->CallVoidMethod(javaObject, disabletoucharea);
}

float Ship::Mobile::GetCameraYaw(){
    return cameraYaw;
}

float Ship::Mobile::GetCameraPitch(){
    return cameraPitch;
}

static int virtual_joystick_id = -1;
static SDL_Joystick *virtual_joystick = nullptr;

extern "C" void JNICALL Java_com_twoshipfork_mm_MainActivity_attachController(JNIEnv* env, jobject obj) {
    virtual_joystick_id = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 18, 0);
    if (virtual_joystick_id == -1) {
            SDL_Log("Could not create overlay virtual controller");
        return;
    }

    virtual_joystick = SDL_JoystickOpen(virtual_joystick_id);
    if (virtual_joystick == nullptr)
        SDL_Log("Could not create virtual joystick");

    isUsingTouchscreenControls = true;
}

extern "C" void JNICALL Java_com_twoshipfork_mm_MainActivity_setCameraState(JNIEnv *env, jobject jobj, jint axis, jfloat value) {
    switch(axis){
        case 0:
            cameraYaw=value;
            break;
        case 1:
            cameraPitch=value;
            break;
    }
}

extern "C" void JNICALL Java_com_twoshipfork_mm_MainActivity_setButton(JNIEnv *env, jobject jobj, jint button, jboolean value) {
    if(button < 0){
        SDL_JoystickSetVirtualAxis(virtual_joystick,-button, value ? SDL_MAX_SINT16 : -SDL_MAX_SINT16); // This should be 0 when false, but I think there's a bug in SDL
    }else{
        SDL_JoystickSetVirtualButton(virtual_joystick, button, value);
    }
}

#include "ship/Context.h"

extern "C" void JNICALL Java_com_twoshipfork_mm_MainActivity_setAxis(JNIEnv *env, jobject jobj, jint axis, jshort value) {
    SDL_JoystickSetVirtualAxis(virtual_joystick, axis, value);
}

extern "C" void JNICALL Java_com_twoshipfork_mm_MainActivity_detachController(JNIEnv *env, jobject jobj) {
    SDL_JoystickClose(virtual_joystick);
    SDL_JoystickDetachVirtual(virtual_joystick_id);
    virtual_joystick = nullptr;
    virtual_joystick_id = -1;
    isUsingTouchscreenControls = false;
}

#endif
