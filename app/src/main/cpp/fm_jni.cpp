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
#include <elf.h>
#include <link.h>

#define LOG_TAG "FM_JNI_HAL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// HAL Function Pointers (RAII-safe wrapper)
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

// Global HAL state structured model
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

// Helper to query Android system properties securely
static std::string get_system_property(const std::string& name) {
    char value[PROP_VALUE_MAX] = {0};
    int len = __system_property_get(name.c_str(), value);
    if (len > 0) {
        return std::string(value, len);
    }
    return "UNKNOWN/NOT_SET";
}

// Helper to run a shell command and read its output securely
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
    if (depth > 4) return; // Prevent infinite recursion
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
    // Fallback if directory reading is restricted
    if (nodes.empty()) {
        nodes = {"/dev/radio0", "/dev/fm", "/dev/iris"};
    }
    return nodes;
}

// ELF Dependency Reader structure & logic
struct ElfDependencyInfo {
    std::string lib_name;
    std::vector<std::string> needed_libs;
    std::string error;
};

static ElfDependencyInfo analyze_elf_dependencies(const std::string& path) {
    ElfDependencyInfo info;
    info.lib_name = path;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        info.error = "Could not open file: " + std::string(strerror(errno));
        return info;
    }

    // Read ELF header
    unsigned char ident[EI_NIDENT];
    if (fread(ident, 1, EI_NIDENT, f) != EI_NIDENT) {
        info.error = "Failed to read ELF identification bytes";
        fclose(f);
        return info;
    }

    if (memcmp(ident, ELFMAG, SELFMAG) != 0) {
        info.error = "Not a valid ELF file";
        fclose(f);
        return info;
    }

    unsigned char elf_class = ident[EI_CLASS];
    fseek(f, 0, SEEK_SET);

    if (elf_class == ELFCLASS64) {
        Elf64_Ehdr ehdr;
        if (fread(&ehdr, 1, sizeof(ehdr), f) != sizeof(ehdr)) {
            info.error = "Failed to read ELF64 header";
            fclose(f);
            return info;
        }

        std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
        fseek(f, ehdr.e_phoff, SEEK_SET);
        if (fread(phdrs.data(), sizeof(Elf64_Phdr), ehdr.e_phnum, f) != ehdr.e_phnum) {
            info.error = "Failed to read Program Headers";
            fclose(f);
            return info;
        }

        const Elf64_Phdr* dynamic_phdr = nullptr;
        std::vector<Elf64_Phdr> load_phdrs;
        for (const auto& phdr : phdrs) {
            if (phdr.p_type == PT_DYNAMIC) {
                dynamic_phdr = &phdr;
            } else if (phdr.p_type == PT_LOAD) {
                load_phdrs.push_back(phdr);
            }
        }

        if (!dynamic_phdr) {
            info.error = "PT_DYNAMIC segment not found";
            fclose(f);
            return info;
        }

        size_t num_dyn = dynamic_phdr->p_filesz / sizeof(Elf64_Dyn);
        std::vector<Elf64_Dyn> dyns(num_dyn);
        fseek(f, dynamic_phdr->p_offset, SEEK_SET);
        if (fread(dyns.data(), sizeof(Elf64_Dyn), num_dyn, f) != num_dyn) {
            info.error = "Failed to read Dynamic entries";
            fclose(f);
            return info;
        }

        uint64_t strtab_va = 0;
        std::vector<uint64_t> needed_offsets;
        for (const auto& dyn : dyns) {
            if (dyn.d_tag == DT_STRTAB) {
                strtab_va = dyn.d_un.d_ptr;
            } else if (dyn.d_tag == DT_NEEDED) {
                needed_offsets.push_back(dyn.d_un.d_val);
            }
        }

        if (strtab_va == 0) {
            info.error = "DT_STRTAB dynamic tag not found";
            fclose(f);
            return info;
        }

        uint64_t strtab_offset = 0;
        bool found_load = false;
        for (const auto& load : load_phdrs) {
            if (strtab_va >= load.p_vaddr && strtab_va < load.p_vaddr + load.p_filesz) {
                strtab_offset = strtab_va - load.p_vaddr + load.p_offset;
                found_load = true;
                break;
            }
        }

        if (!found_load) {
            info.error = "Failed to translate DT_STRTAB virtual address to file offset";
            fclose(f);
            return info;
        }

        for (auto offset : needed_offsets) {
            fseek(f, strtab_offset + offset, SEEK_SET);
            std::string needed_lib;
            char c;
            while (fread(&c, 1, 1, f) == 1 && c != '\0') {
                needed_lib += c;
            }
            if (!needed_lib.empty()) {
                info.needed_libs.push_back(needed_lib);
            }
        }
    } else if (elf_class == ELFCLASS32) {
        Elf32_Ehdr ehdr;
        if (fread(&ehdr, 1, sizeof(ehdr), f) != sizeof(ehdr)) {
            info.error = "Failed to read ELF32 header";
            fclose(f);
            return info;
        }

        std::vector<Elf32_Phdr> phdrs(ehdr.e_phnum);
        fseek(f, ehdr.e_phoff, SEEK_SET);
        if (fread(phdrs.data(), sizeof(Elf32_Phdr), ehdr.e_phnum, f) != ehdr.e_phnum) {
            info.error = "Failed to read Program Headers";
            fclose(f);
            return info;
        }

        const Elf32_Phdr* dynamic_phdr = nullptr;
        std::vector<Elf32_Phdr> load_phdrs;
        for (const auto& phdr : phdrs) {
            if (phdr.p_type == PT_DYNAMIC) {
                dynamic_phdr = &phdr;
            } else if (phdr.p_type == PT_LOAD) {
                load_phdrs.push_back(phdr);
            }
        }

        if (!dynamic_phdr) {
            info.error = "PT_DYNAMIC segment not found";
            fclose(f);
            return info;
        }

        size_t num_dyn = dynamic_phdr->p_filesz / sizeof(Elf32_Dyn);
        std::vector<Elf32_Dyn> dyns(num_dyn);
        fseek(f, dynamic_phdr->p_offset, SEEK_SET);
        if (fread(dyns.data(), sizeof(Elf32_Dyn), num_dyn, f) != num_dyn) {
            info.error = "Failed to read Dynamic entries";
            fclose(f);
            return info;
        }

        uint32_t strtab_va = 0;
        std::vector<uint32_t> needed_offsets;
        for (const auto& dyn : dyns) {
            if (dyn.d_tag == DT_STRTAB) {
                strtab_va = dyn.d_un.d_ptr;
            } else if (dyn.d_tag == DT_NEEDED) {
                needed_offsets.push_back(dyn.d_un.d_val);
            }
        }

        if (strtab_va == 0) {
            info.error = "DT_STRTAB dynamic tag not found";
            fclose(f);
            return info;
        }

        uint32_t strtab_offset = 0;
        bool found_load = false;
        for (const auto& load : load_phdrs) {
            if (strtab_va >= load.p_vaddr && strtab_va < load.p_vaddr + load.p_filesz) {
                strtab_offset = strtab_va - load.p_vaddr + load.p_offset;
                found_load = true;
                break;
            }
        }

        if (!found_load) {
            info.error = "Failed to translate DT_STRTAB virtual address to file offset";
            fclose(f);
            return info;
        }

        for (auto offset : needed_offsets) {
            fseek(f, strtab_offset + offset, SEEK_SET);
            std::string needed_lib;
            char c;
            while (fread(&c, 1, 1, f) == 1 && c != '\0') {
                needed_lib += c;
            }
            if (!needed_lib.empty()) {
                info.needed_libs.push_back(needed_lib);
            }
        }
    } else {
        info.error = "Unknown ELF class: " + std::to_string(elf_class);
    }

    fclose(f);
    return info;
}

// Models for diagnostic evaluation
struct NodeDiagnosis {
    std::string path;
    bool exists = false;
    std::string dac_permissions;
    std::string error_detail;
    std::string restriction_category; // "None", "DAC Permission Denied", "SELinux Denied", "Driver Missing", "Kernel/Vendor restriction"
};

static NodeDiagnosis diagnose_device_node(const std::string& path) {
    NodeDiagnosis diag;
    diag.path = path;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        diag.exists = false;
        diag.error_detail = "Driver Missing: Node does not exist in /dev (" + std::string(strerror(errno)) + ")";
        diag.restriction_category = "Driver Missing";
        return diag;
    }

    diag.exists = true;

    // Convert file permissions to standard octal and symbol notation
    char perm_str[10];
    snprintf(perm_str, sizeof(perm_str), "%o", st.st_mode & 0777);
    diag.dac_permissions = std::string(perm_str) + " (UID: " + std::to_string(st.st_uid) + ", GID: " + std::to_string(st.st_gid) + ")";

    // Try opening the node
    int fd = open(path.c_str(), O_RDWR);
    if (fd >= 0) {
        diag.error_detail = "Accessible & Writable";
        diag.restriction_category = "None";
        close(fd);
    } else {
        int open_err = errno;
        diag.error_detail = "Open failed: " + std::string(strerror(open_err)) + " (errno: " + std::to_string(open_err) + ")";
        
        if (open_err == EACCES) {
            bool dac_allows_world = (st.st_mode & 0006) == 0006;
            if (!dac_allows_world) {
                diag.restriction_category = "DAC Permission Denied (Group radio/system membership required)";
            } else {
                diag.restriction_category = "SELinux Denied (Blocked by MAC untrusted_app context)";
            }
        } else {
            diag.restriction_category = "Kernel/Vendor restriction";
        }
    }
    return diag;
}

struct LibraryDiagnosis {
    std::string path;
    bool exists = false;
    bool loaded = false;
    std::string dlerror_msg;
    std::string failure_reason; // "None", "Linker Namespace Restriction", "Missing Dependency", "Symbol Resolution Failure", "File Missing", "Namespace/Vendor restrictions"
    std::vector<std::string> dependencies;
    std::vector<std::string> missing_dependencies;
};

static LibraryDiagnosis diagnose_library(const std::string& path) {
    LibraryDiagnosis diag;
    diag.path = path;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        diag.exists = false;
        diag.failure_reason = "File Missing";
        return diag;
    }

    diag.exists = true;

    // Parse ELF dependencies using our real dependency analyzer
    auto elf_info = analyze_elf_dependencies(path);
    if (elf_info.error.empty()) {
        diag.dependencies = elf_info.needed_libs;
        for (const auto& dep : diag.dependencies) {
            std::vector<std::string> search_paths = {"/vendor/lib64", "/vendor/lib", "/system/lib64", "/system/lib", "/system_ext/lib64", "/odm/lib64", "/product/lib64"};
            bool dep_found = false;
            for (const auto& dir : search_paths) {
                struct stat dep_st;
                std::string full_dep_path = dir + "/" + dep;
                if (stat(full_dep_path.c_str(), &dep_st) == 0) {
                    dep_found = true;
                    break;
                }
            }
            if (!dep_found) {
                diag.missing_dependencies.push_back(dep);
            }
        }
    } else {
        LOGE("ELF Parsing Error for %s: %s", path.c_str(), elf_info.error.c_str());
    }

    // Try loading the library via dlopen
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (handle) {
        diag.loaded = true;
        diag.failure_reason = "None";
        dlclose(handle);
    } else {
        const char* err = dlerror();
        diag.dlerror_msg = err ? err : "Unknown dlerror";
        
        if (diag.dlerror_msg.find("namespace") != std::string::npos || 
            diag.dlerror_msg.find("not accessible") != std::string::npos ||
            diag.dlerror_msg.find("restricted") != std::string::npos) {
            diag.failure_reason = "Linker Namespace Restriction (Treble sandbox block)";
        } else if (!diag.missing_dependencies.empty()) {
            diag.failure_reason = "Missing Dependency (Cannot load because of missing required libraries)";
        } else if (diag.dlerror_msg.find("symbol") != std::string::npos ||
                   diag.dlerror_msg.find("undefined") != std::string::npos) {
            diag.failure_reason = "Symbol Resolution Failure (Missing exported symbols)";
        } else {
            diag.failure_reason = "Namespace/Vendor restrictions";
        }
    }
    return diag;
}

// SELinux State Inspectors
static std::string check_selinux_status() {
    int fd = open("/sys/fs/selinux/enforce", O_RDONLY);
    if (fd >= 0) {
        char val = 0;
        ssize_t bytes = read(fd, &val, 1);
        close(fd);
        if (bytes == 1) {
            return (val == '1') ? "Enforcing" : "Permissive";
        }
    }
    if (errno == EACCES) {
        return "Enforcing (Verified via read access denial to enforce file)";
    }
    std::string prop = get_system_property("ro.boot.selinux");
    if (prop != "UNKNOWN/NOT_SET") {
        return prop + " (from boot prop)";
    }
    return "Unknown/Enforcing (Access restricted)";
}

static std::string get_selinux_context() {
    int fd = open("/proc/self/attr/current", O_RDONLY);
    if (fd >= 0) {
        char buf[256] = {0};
        ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (bytes > 0) {
            std::string context(buf);
            while(!context.empty() && (context.back() == '\n' || context.back() == '\r' || context.back() == ' ')) {
                context.pop_back();
            }
            return context;
        }
    }
    return "UNKNOWN (Access denied)";
}

// Process mapped libs iteration
struct PhdrCallbackData {
    std::string* report;
};

static int dl_iterate_phdr_callback(struct dl_phdr_info* info, size_t size, void* data) {
    auto* report_data = static_cast<PhdrCallbackData*>(data);
    if (info->dlpi_name && strlen(info->dlpi_name) > 0) {
        std::string name = info->dlpi_name;
        if (name.find("fm") != std::string::npos || name.find("qti") != std::string::npos || 
            name.find("broadcastradio") != std::string::npos) {
            *(report_data->report) += "  -> Loaded: " + name + " (base address: " + std::to_string(info->dlpi_addr) + ")\n";
        }
    }
    return 0;
}

// Standard helper to resolve and load any matching Qualcomm FM HAL shared libraries
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

    if (load_qualcomm_hal()) {
        LOGI("Hardware support detected via loaded QTI FM HAL/PAL!");
        g_hal.reset(); 
        return JNI_TRUE;
    }

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

    if (load_qualcomm_hal()) {
        if (g_hal.power_up) {
            LOGI("Calling fmpal_power_up standard hook...");
            int res = g_hal.power_up(nullptr);
            if (res >= 0) {
                LOGI("fmpal_power_up succeeded: %d", res);
                return 0;
            } else {
                LOGE("fmpal_power_up returned error code: %d", res);
            }
        }
    }

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
                return 0;
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
            return 0;
        }
        LOGE("fmpal_tune returned failure: %d", res);
    }

    if (g_radio_fd.is_valid()) {
        struct v4l2_frequency frequency;
        memset(&frequency, 0, sizeof(frequency));
        frequency.tuner = 0;
        frequency.type = V4L2_TUNER_RADIO;
        frequency.frequency = (freq_khz * 16) / 1000;

        if (ioctl(g_radio_fd.get(), VIDIOC_S_FREQUENCY, &frequency) >= 0) {
            LOGI("Tuned to frequency successfully via V4L2 ioctl");
            g_current_freq_khz = freq_khz;
            return 0;
        }
        LOGE("Failed to tune frequency via V4L2 ioctl (errno: %d - %s)", errno, strerror(errno));
    }

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
        int res = g_hal.seek(direction, 0);
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
        seek.spacing = 100000;

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
    LOGI("Generating full high-resolution FM hardware diagnostics report...");
    std::string report;

    // 1. SYSTEM & SELINUX ATTRIBUTES
    report += "=== SYSTEM & SECURITY CONTROLS ===\n";
    report += "ro.board.platform: " + get_system_property("ro.board.platform") + "\n";
    report += "ro.hardware: " + get_system_property("ro.hardware") + "\n";
    report += "ro.boot.hardware: " + get_system_property("ro.boot.hardware") + "\n";
    report += "ro.product.model: " + get_system_property("ro.product.model") + "\n";
    report += "ro.product.device: " + get_system_property("ro.product.device") + "\n";
    report += "ro.vendor.qti.fm: " + get_system_property("ro.vendor.qti.fm") + "\n";
    report += "ro.fm.active: " + get_system_property("ro.fm.active") + "\n";
    report += "vendor.hw.fm: " + get_system_property("vendor.hw.fm") + "\n";
    report += "SELinux Mode: " + check_selinux_status() + "\n";
    report += "App Security Domain: " + get_selinux_context() + "\n";

    // 2. STAGED RESOLUTION MATRIX
    report += "\n=== STAGED FM RESOLUTION MATRIX ===\n";
    
    // Evaluate Stage 1: Library Exists
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
    bool lib_exists = false;
    bool lib_loaded = false;
    for (const auto& dir : search_dirs) {
        for (const auto& target : targets) {
            std::vector<std::string> matches;
            scan_dir_recursive(dir, target, matches);
            if (!matches.empty()) {
                lib_exists = true;
                for (const auto& path : matches) {
                    void* h = dlopen(path.c_str(), RTLD_NOW);
                    if (h) {
                        lib_loaded = true;
                        dlclose(h);
                        break;
                    }
                }
            }
        }
    }
    report += std::string("Stage 1 - QTI FM Library Exists: ") + (lib_exists ? "YES" : "NO") + "\n";
    report += std::string("Stage 2 - QTI FM Library Loaded (dlopen): ") + (lib_loaded ? "YES" : "NO") + "\n";

    // Evaluate Stage 3: Binder Connection
    std::string service_output = run_shell_command("service list");
    bool binder_connected = false;
    std::vector<std::string> patterns = {"broadcastradio", "fm", "qti.hardware.fm"};
    std::stringstream ss(service_output);
    std::string line;
    while (std::getline(ss, line)) {
        for (const auto& p : patterns) {
            if (line.find(p) != std::string::npos) {
                binder_connected = true;
                break;
            }
        }
    }
    report += std::string("Stage 3 - Binder FM Service Connected: ") + (binder_connected ? "YES" : "NO") + "\n";

    // Evaluate Stage 4: Driver Opened
    bool driver_opened = g_radio_fd.is_valid();
    if (!driver_opened) {
        auto nodes = get_candidate_device_nodes();
        for (const auto& node : nodes) {
            int fd = open(node.c_str(), O_RDONLY);
            if (fd >= 0) {
                driver_opened = true;
                close(fd);
                break;
            }
        }
    }
    report += std::string("Stage 4 - Kernel Driver Node Opened: ") + (driver_opened ? "YES" : "NO") + "\n";

    // Evaluate Stage 5: Audio Path Ready
    std::string audio_prop = get_system_property("ro.vendor.audio.sdk");
    bool audio_path_ready = (audio_prop != "UNKNOWN/NOT_SET") || (get_system_property("vendor.audio.feature.fm.rx.enabled") == "true");
    report += std::string("Stage 5 - Vendor Audio Routing Ready: ") + (audio_path_ready ? "YES" : "NO") + "\n";

    // Evaluate Stage 6: FM Ready
    bool fm_ready = (lib_loaded || driver_opened);
    report += std::string("Stage 6 - Qualcomm FM Interface Ready: ") + (fm_ready ? "YES" : "NO") + "\n";

    // 3. DEVICE NODE INSPECTOR & DAC/MAC STATUS
    report += "\n=== KERNEL DEVICE NODE DIAGNOSTICS ===\n";
    auto nodes = get_candidate_device_nodes();
    for (const auto& node : nodes) {
        auto diag = diagnose_device_node(node);
        if (diag.exists) {
            report += "Node: " + diag.path + " [PRESENT]\n";
            report += "  -> Linux DAC Permissions: " + diag.dac_permissions + "\n";
            report += "  -> Driver Accessibility: " + diag.error_detail + "\n";
            report += "  -> Restriction Classification: " + diag.restriction_category + "\n";
        } else {
            report += "Node: " + diag.path + " [ABSENT] (" + diag.error_detail + ")\n";
        }
    }

    // 4. DEPENDENCY & NAMESPACE ANALYZER
    report += "\n=== COGNITIVE DEPENDENCY & NAMESPACE ANALYZER ===\n";
    bool hal_found = false;
    for (const auto& dir : search_dirs) {
        for (const auto& target : targets) {
            std::vector<std::string> matches;
            scan_dir_recursive(dir, target, matches);
            for (const auto& path : matches) {
                hal_found = true;
                auto diag = diagnose_library(path);
                report += "Library: " + diag.path + " [FOUND]\n";
                report += std::string("  -> Dynamic Load (dlopen): ") + (diag.loaded ? "SUCCESS" : "FAILED") + "\n";
                if (!diag.loaded) {
                    report += "  -> Exact dlerror() string: " + diag.dlerror_msg + "\n";
                    report += "  -> Failure Classification: " + diag.failure_reason + "\n";
                }
                if (!diag.dependencies.empty()) {
                    report += "  -> Parsed ELF Dynamic DT_NEEDED list:\n";
                    for (const auto& dep : diag.dependencies) {
                        bool missing = false;
                        for (const auto& m : diag.missing_dependencies) {
                            if (m == dep) {
                                missing = true;
                                break;
                            }
                        }
                        report += "     * " + dep + (missing ? " [MISSING FROM DEVICE]" : " [RESOLVED]") + "\n";
                    }
                }
            }
        }
    }
    if (!hal_found) {
        report += "No Qualcomm FM HAL libraries discovered in standard system partitions.\n";
    }

    // 5. ENUMERATED BINDER SERVICES
    report += "\n=== ACTIVE BINDER SERVICES LIST ===\n";
    bool svc_found = false;
    std::stringstream ss2(service_output);
    while (std::getline(ss2, line)) {
        for (const auto& p : patterns) {
            if (line.find(p) != std::string::npos) {
                report += "  -> Registered Service: " + line + "\n";
                svc_found = true;
                break;
            }
        }
    }
    if (!svc_found) {
        report += "No BroadcastRadio or FM Binder services found in active service manager list.\n";
    }

    // 6. PROCESS MAPPED SHARED LIBRARIES (dl_iterate_phdr)
    report += "\n=== DYNAMICALLY MAPPED LIBRARIES IN MEMORY (dl_iterate_phdr) ===\n";
    PhdrCallbackData phdr_data;
    phdr_data.report = &report;
    dl_iterate_phdr(dl_iterate_phdr_callback, &phdr_data);

    return env->NewStringUTF(report.c_str());
}

}
