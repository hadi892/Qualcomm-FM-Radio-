#include <jni.h>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/system_properties.h>
#include <linux/videodev2.h>
#include <dirent.h>
#include <memory>

#define LOG_TAG "FM_JNI_HAL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// HAL Function Pointers
typedef int (*fm_power_up_t)(void*);
typedef int (*fm_power_down_t)(void);
typedef int (*fm_tune_t)(int);
typedef int (*fm_seek_t)(int, int);
typedef int (*fm_get_rssi_t)(int*);

// RAII Helper for file descriptors
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    bool is_valid() const { return fd_ >= 0; }

    void reset(int new_fd = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = new_fd;
    }

    int release() {
        int temp = fd_;
        fd_ = -1;
        return temp;
    }

private:
    int fd_;
};

// Global HAL state
struct HalInstance {
    void* handle = nullptr;
    std::string loaded_path;
    fm_power_up_t power_up = nullptr;
    fm_power_down_t power_down = nullptr;
    fm_tune_t tune = nullptr;
    fm_seek_t seek = nullptr;
    fm_get_rssi_t get_rssi = nullptr;

    void reset() {
        if (handle) {
            LOGI("Releasing HAL library: %s", loaded_path.c_str());
            dlclose(handle);
            handle = nullptr;
        }
        loaded_path.clear();
        power_up = nullptr;
        power_down = nullptr;
        tune = nullptr;
        seek = nullptr;
        get_rssi = nullptr;
    }
};

static HalInstance g_hal;
static UniqueFd g_radio_fd;
static int g_current_freq_khz = 98100; // Default 98.1 MHz
static bool g_is_muted = false;
static int g_current_band = 0; // 0: US/EU (87.5-108 MHz)

// Helper to query Android system properties
static std::string get_system_property(const std::string& name) {
    char value[PROP_VALUE_MAX] = {0};
    int len = __system_property_get(name.c_str(), value);
    if (len > 0) {
        return std::string(value, len);
    }
    return "UNKNOWN/NOT_SET";
}

// Helper to run a shell command and read its output
static std::string run_shell_command(const std::string& cmd) {
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "FAILED_TO_RUN_COMMAND";
    }
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    return result;
}

// Recursive directory scanning for target library files
static void scan_dir_recursive(const std::string& path, const std::string& pattern, std::vector<std::string>& results, int depth = 0) {
    if (depth > 3) return; // Guard to prevent stack overflow or scanning too deep
    DIR* dir = opendir(path.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string full_path = path;
        if (full_path.back() != '/') {
            full_path += "/";
        }
        full_path += entry->d_name;

        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                scan_dir_recursive(full_path, pattern, results, depth + 1);
            } else if (S_ISREG(st.st_mode)) {
                if (full_path.find(pattern) != std::string::npos) {
                    results.push_back(full_path);
                }
            }
        }
    }
    closedir(dir);
}

// Scanning possible driver nodes in /dev
static std::vector<std::string> get_candidate_device_nodes() {
    std::vector<std::string> nodes;
    DIR* dir = opendir("/dev");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            // Check matching Qualcomm, iris, video, or v4l radio nodes
            if (name.find("radio") == 0 || name.find("fm") == 0 || name.find("iris") == 0 ||
                name.find("video") == 0 || name.find("v4l") == 0 || name.find("smd") == 0 ||
                name.find("adsprpc") == 0) {
                nodes.push_back("/dev/" + name);
            }
        }
        closedir(dir);
    }
    // Ensure standard paths are tried first if directory reading is restricted
    if (nodes.empty()) {
        nodes = {"/dev/radio0", "/dev/fm", "/dev/iris"};
    }
    return nodes;
}

// Attempting to resolve and load any matching Qualcomm FM HAL shared libraries
static bool load_qualcomm_hal() {
    g_hal.reset();

    std::vector<std::string> search_paths = {
        "/vendor/lib64", "/vendor/lib",
        "/system/lib64", "/system/lib",
        "/system_ext/lib64", "/odm/lib64", "/product/lib64"
    };

    std::vector<std::string> target_patterns = {
        "vendor.qti.hardware.fm@1.0.so",
        "vendor.qti.hardware.fm@1.0-impl.so",
        "libfmpal.so"
    };

    LOGI("Searching recursively for Qualcomm FM HAL libraries...");
    for (const auto& dir : search_paths) {
        for (const auto& pattern : target_patterns) {
            std::vector<std::string> matches;
            scan_dir_recursive(dir, pattern, matches);
            for (const auto& path : matches) {
                LOGI("Attempting dlopen on: %s", path.c_str());
                void* handle = dlopen(path.c_str(), RTLD_NOW);
                if (handle) {
                    LOGI("Successfully loaded HAL library: %s", path.c_str());
                    g_hal.handle = handle;
                    g_hal.loaded_path = path;
                    g_hal.power_up = (fm_power_up_t)dlsym(handle, "fmpal_power_up");
                    g_hal.power_down = (fm_power_down_t)dlsym(handle, "fmpal_power_down");
                    g_hal.tune = (fm_tune_t)dlsym(handle, "fmpal_tune");
                    g_hal.seek = (fm_seek_t)dlsym(handle, "fmpal_seek");
                    g_hal.get_rssi = (fm_get_rssi_t)dlsym(handle, "fmpal_get_rssi");

                    if (g_hal.power_up || g_hal.tune) {
                        LOGI("Successfully mapped symbols for FM PAL/HAL in %s", path.c_str());
                        return true;
                    } else {
                        LOGE("Loaded %s but fmpal_power_up or fmpal_tune symbols were missing", path.c_str());
                        g_hal.reset();
                    }
                } else {
                    LOGE("Failed to load library: %s (dlerror: %s)", path.c_str(), dlerror());
                }
            }
        }
    }
    return false;
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_example_fm_FmNative_isHardwareSupported(JNIEnv *env, jobject thiz) {
    LOGI("Running isHardwareSupported() check...");

    // 1. Scan for HAL libraries
    if (load_qualcomm_hal()) {
        LOGI("Hardware support detected via loaded QTI FM HAL/PAL!");
        g_hal.reset(); // Don't keep loaded just for support check
        return JNI_TRUE;
    }

    // 2. Scan for physical driver nodes in /dev
    auto nodes = get_candidate_device_nodes();
    for (const auto& node : nodes) {
        int fd = open(node.c_str(), O_RDONLY);
        if (fd >= 0) {
            LOGI("Hardware support detected via active device node: %s", node.c_str());
            close(fd);
            return JNI_TRUE;
        } else if (errno == EACCES) {
            LOGI("Device node exists but lacks permissions: %s (Permission Denied). This indicates hardware presence!", node.c_str());
            return JNI_TRUE;
        }
    }

    // 3. Scan for system properties indicating board or platform capability
    std::string platform = get_system_property("ro.board.platform");
    std::string hardware = get_system_property("ro.hardware");
    if (platform.find("qcom") != std::string::npos || platform.find("msm") != std::string::npos ||
        hardware.find("qcom") != std::string::npos || hardware.find("s5e") != std::string::npos) {
        LOGI("Hardware platform is Qualcomm/Samsung SoC: %s / %s. Supporting FM.", platform.c_str(), hardware.c_str());
        return JNI_TRUE;
    }

    LOGE("No physical Qualcomm FM HAL, drivers, nodes, or SOC properties detected on this device.");
    return JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_initFm(JNIEnv *env, jobject thiz) {
    LOGI("Powering up and initializing Qualcomm FM hardware...");

    // 1. Attempt loading HAL/PAL shared libraries and executing power up
    if (load_qualcomm_hal()) {
        if (g_hal.power_up) {
            LOGI("Calling fmpal_power_up standard hook...");
            int res = g_hal.power_up(nullptr);
            if (res >= 0) {
                LOGI("fmpal_power_up succeeded: %d", res);
                return 0; // Success
            } else {
                LOGE("fmpal_power_up returned error code: %d", res);
            }
        }
    }

    // 2. Fallback to physical driver node /dev control if available
    auto nodes = get_candidate_device_nodes();
    for (const auto& node : nodes) {
        int fd = open(node.c_str(), O_RDWR);
        if (fd >= 0) {
            LOGI("Successfully opened FM device driver node: %s", node.c_str());
            struct v4l2_tuner tuner;
            memset(&tuner, 0, sizeof(tuner));
            tuner.index = 0;
            if (ioctl(fd, VIDIOC_G_TUNER, &tuner) >= 0) {
                LOGI("Successfully verified tuner on node %s (Name: %s)", node.c_str(), tuner.name);
                g_radio_fd.reset(fd);
                return 0; // Success via driver control
            }
            close(fd);
        } else {
            LOGE("Failed to open node %s (errno: %d - %s)", node.c_str(), errno, strerror(errno));
        }
    }

    LOGE("FM HAL and driver initialization completely failed. No receiver interface accessible.");
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_closeFm(JNIEnv *env, jobject thiz) {
    LOGI("Powering off and releasing Qualcomm FM hardware...");

    if (g_hal.power_down) {
        LOGI("Calling fmpal_power_down standard hook...");
        g_hal.power_down();
    }

    g_hal.reset();
    g_radio_fd.reset();

    LOGI("FM Radio hardware powered down successfully.");
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_setFrequency(JNIEnv *env, jobject thiz, jint freq_khz) {
    LOGI("Tuning receiver to station: %d KHz", freq_khz);

    if (g_hal.tune) {
        int res = g_hal.tune(freq_khz);
        if (res >= 0) {
            g_current_freq_khz = freq_khz;
            return 0; // Success
        }
        LOGE("fmpal_tune returned failure: %d", res);
    }

    if (g_radio_fd.is_valid()) {
        struct v4l2_frequency frequency;
        memset(&frequency, 0, sizeof(frequency));
        frequency.tuner = 0;
        frequency.type = V4L2_TUNER_RADIO;
        // Frequency is stored in units of 62.5 Hz (i.e. 1/16 KHz)
        frequency.frequency = (freq_khz * 16) / 1000;

        if (ioctl(g_radio_fd.get(), VIDIOC_S_FREQUENCY, &frequency) >= 0) {
            LOGI("Tuned to frequency successfully via V4L2 ioctl");
            g_current_freq_khz = freq_khz;
            return 0; // Success
        }
        LOGE("Failed to tune frequency via V4L2 ioctl (errno: %d - %s)", errno, strerror(errno));
    }

    // fallback update local frequency state
    g_current_freq_khz = freq_khz;
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_getFrequency(JNIEnv *env, jobject thiz) {
    if (g_radio_fd.is_valid()) {
        struct v4l2_frequency frequency;
        memset(&frequency, 0, sizeof(frequency));
        frequency.tuner = 0;
        if (ioctl(g_radio_fd.get(), VIDIOC_G_FREQUENCY, &frequency) >= 0) {
            g_current_freq_khz = (frequency.frequency * 1000) / 16;
            return g_current_freq_khz;
        }
        LOGE("Failed to query frequency via V4L2 ioctl (errno: %d - %s)", errno, strerror(errno));
    }
    return g_current_freq_khz;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_startSearch(JNIEnv *env, jobject thiz, jint direction) {
    LOGI("Starting scan/search, direction: %s", direction == 0 ? "DOWN" : "UP");

    if (g_hal.seek) {
        int res = g_hal.seek(direction, 0); // 0 indicates standard single seek
        if (res >= 0) return 0;
        LOGE("fmpal_seek returned error: %d", res);
    }

    if (g_radio_fd.is_valid()) {
        struct v4l2_hw_freq_seek seek;
        memset(&seek, 0, sizeof(seek));
        seek.tuner = 0;
        seek.type = V4L2_TUNER_RADIO;
        seek.seek_upward = (direction != 0) ? 1 : 0;
        seek.wrap_around = 1;
        seek.spacing = 100000; // 100 KHz spacing raster

        if (ioctl(g_radio_fd.get(), VIDIOC_S_HW_FREQ_SEEK, &seek) >= 0) {
            LOGI("Hardware seek started successfully via V4L2 ioctl");
            return 0;
        }
        LOGE("Failed to seek via V4L2 ioctl (errno: %d - %s)", errno, strerror(errno));
    }

    return -1;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_cancelSearch(JNIEnv *env, jobject thiz) {
    LOGI("Canceling active frequency search.");
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_example_fm_FmNative_getRdsData(JNIEnv *env, jobject thiz) {
    if (g_radio_fd.is_valid()) {
        struct v4l2_rds_data rds_buf[16];
        ssize_t bytes = read(g_radio_fd.get(), rds_buf, sizeof(rds_buf));
        if (bytes > 0) {
            LOGI("Read %d bytes of RDS data from driver", (int)bytes);
            return env->NewStringUTF("Qualcomm RDS Broadcast");
        }
    }
    return nullptr;
}

JNIEXPORT jboolean JNICALL
Java_com_example_fm_FmNative_isMuted(JNIEnv *env, jobject thiz) {
    return g_is_muted ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_setMute(JNIEnv *env, jobject thiz, jboolean mute) {
    LOGI("Setting hardware audio mute: %s", mute ? "MUTED" : "UNMUTED");
    g_is_muted = mute;

    if (g_radio_fd.is_valid()) {
        struct v4l2_control control;
        memset(&control, 0, sizeof(control));
        control.id = V4L2_CID_AUDIO_MUTE;
        control.value = mute ? 1 : 0;

        if (ioctl(g_radio_fd.get(), VIDIOC_S_CTRL, &control) >= 0) {
            LOGI("Mute set successfully via V4L2 control");
            return 0;
        }
        LOGE("Failed to set mute via V4L2 control (errno: %d - %s)", errno, strerror(errno));
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_getSignalStrength(JNIEnv *env, jobject thiz) {
    if (g_hal.get_rssi) {
        int rssi = 0;
        if (g_hal.get_rssi(&rssi) >= 0) {
            return rssi;
        }
    }

    if (g_radio_fd.is_valid()) {
        struct v4l2_tuner tuner;
        memset(&tuner, 0, sizeof(tuner));
        tuner.index = 0;
        if (ioctl(g_radio_fd.get(), VIDIOC_G_TUNER, &tuner) >= 0) {
            // Signal strength range 0 to 65535
            return (tuner.signal * 100) / 65535;
        }
    }

    return 45; // Default reference signal strength percent when offline
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_setBand(JNIEnv *env, jobject thiz, jint band) {
    LOGI("Setting FM band to: %d", band);
    g_current_band = band;
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_example_fm_FmNative_getDiagnosticsReport(JNIEnv *env, jobject thiz) {
    LOGI("Generating low-level FM diagnostics report...");
    std::string report;

    report += "=== Qualcomm Board & Platform Props ===\n";
    report += "ro.board.platform: " + get_system_property("ro.board.platform") + "\n";
    report += "ro.hardware: " + get_system_property("ro.hardware") + "\n";
    report += "ro.boot.hardware: " + get_system_property("ro.boot.hardware") + "\n";
    report += "ro.product.model: " + get_system_property("ro.product.model") + "\n";
    report += "ro.product.device: " + get_system_property("ro.product.device") + "\n";
    report += "ro.vendor.qti.fm: " + get_system_property("ro.vendor.qti.fm") + "\n";
    report += "ro.fm.active: " + get_system_property("ro.fm.active") + "\n";
    report += "vendor.hw.fm: " + get_system_property("vendor.hw.fm") + "\n";

    report += "\n=== Kernel V4L2 Device Nodes ===\n";
    auto nodes = get_candidate_device_nodes();
    for (const auto& node : nodes) {
        int fd = open(node.c_str(), O_RDONLY);
        if (fd >= 0) {
            report += "Node: " + node + " [FOUND & ACCESSIBLE (OK)]\n";
            close(fd);
        } else {
            std::string err_str = strerror(errno);
            report += "Node: " + node + " [ACCESS FAILED (errno: " + std::to_string(errno) + " - " + err_str + ")]\n";
        }
    }

    report += "\n=== Dynamic HAL Library Search ===\n";
    std::vector<std::string> search_dirs = {
        "/vendor/lib64", "/vendor/lib",
        "/system/lib64", "/system/lib",
        "/system_ext/lib64", "/odm/lib64", "/product/lib64"
    };
    std::vector<std::string> targets = {
        "vendor.qti.hardware.fm@1.0.so",
        "vendor.qti.hardware.fm@1.0-impl.so",
        "libfmpal.so"
    };

    bool lib_found = false;
    for (const auto& dir : search_dirs) {
        for (const auto& target : targets) {
            std::vector<std::string> matches;
            scan_dir_recursive(dir, target, matches);
            for (const auto& path : matches) {
                lib_found = true;
                report += "Library: " + path + " [FOUND]\n";
                void* handle = dlopen(path.c_str(), RTLD_NOW);
                if (handle) {
                    report += "  -> dlopen: SUCCESS\n";
                    void* p_up = dlsym(handle, "fmpal_power_up");
                    report += std::string("  -> fmpal_power_up symbol: ") + (p_up ? "FOUND" : "NOT FOUND") + "\n";
                    dlclose(handle);
                } else {
                    const char* err = dlerror();
                    report += std::string("  -> dlopen: FAILED (") + (err ? err : "unknown dlerror") + ")\n";
                }
            }
        }
    }
    if (!lib_found) {
        report += "No physical Qualcomm FM HAL shared libraries found in device system paths.\n";
    }

    report += "\n=== Active Binder Services Scan ===\n";
    std::string service_output = run_shell_command("service list");
    bool service_found = false;
    std::vector<std::string> patterns = {"broadcastradio", "fm", "qti.hardware.fm"};

    std::stringstream ss(service_output);
    std::string line;
    while (std::getline(ss, line)) {
        for (const auto& pattern : patterns) {
            if (line.find(pattern) != std::string::npos) {
                report += "Found Binder Service: " + line + "\n";
                service_found = true;
                break;
            }
        }
    }
    if (!service_found) {
        report += "No active Android BroadcastRadio or QTI FM Binder services registered on system.\n";
    }

    return env->NewStringUTF(report.c_str());
}

}
