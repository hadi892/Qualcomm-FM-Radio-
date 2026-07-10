#include <jni.h>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cctype>
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
#include <sys/utsname.h>
#include <mutex>

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

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
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

// Global HAL state structured model with RAII resource lifetime management
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

static std::string sanitize_utf8(const std::string& str) {
    std::string clean;
    clean.reserve(str.size());
    for (char c : str) {
        if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t') {
            clean += c;
        } else {
            clean += '?';
        }
    }
    return clean;
}

// Recursive directory scanning for target library files with strict depth-limit of 2 using POSIX
static void scan_dir_recursive(const std::string& path_str, const std::string& pattern, std::vector<std::string>& results, int depth = 0) {
    if (depth > 2) return;
    DIR* dir = opendir(path_str.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string full_path = path_str;
        if (full_path.back() != '/') {
            full_path += "/";
        }
        full_path += name;

        struct stat st;
        if (lstat(full_path.c_str(), &st) == 0) {
            bool is_dir = S_ISDIR(st.st_mode);
            bool is_reg = S_ISREG(st.st_mode);

            // Handle symbolic links safely: only check if they point to a regular file,
            // never follow symbolic links to directories to prevent infinite recursion/crashes.
            if (S_ISLNK(st.st_mode)) {
                struct stat target_st;
                if (stat(full_path.c_str(), &target_st) == 0) {
                    if (S_ISREG(target_st.st_mode)) {
                        is_reg = true;
                    }
                }
            }

            if (is_dir) {
                scan_dir_recursive(full_path, pattern, results, depth + 1);
            } else if (is_reg) {
                if (pattern.empty()) {
                    if (name == "vendor.qti.hardware.fm@1.0.so" ||
                        name == "vendor.qti.hardware.fm@1.0-impl.so" ||
                        name == "libfmpal.so" ||
                        (name.rfind("libqcomfm", 0) == 0 && name.size() > 3 && name.substr(name.size() - 3) == ".so") ||
                        (name.rfind("libfm", 0) == 0 && name.size() > 3 && name.substr(name.size() - 3) == ".so")) {
                        results.push_back(full_path);
                    }
                } else {
                    if (name.find(pattern) != std::string::npos) {
                        results.push_back(full_path);
                    }
                }
            }
        }
    }
    closedir(dir);
}

// Scanning possible driver nodes in /dev using POSIX
static std::vector<std::string> get_candidate_device_nodes() {
    std::vector<std::string> nodes;
    DIR* dir = opendir("/dev");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.find("radio") == 0 || name.find("fm") == 0 || name.find("iris") == 0 ||
                name.find("video") == 0 || name.find("v4l") == 0 || name.find("smd") == 0 ||
                name.find("adsprpc") == 0) {
                nodes.push_back("/dev/" + name);
            }
        }
        closedir(dir);
    }
    if (nodes.empty()) {
        nodes = {"/dev/radio0", "/dev/fm", "/dev/iris"};
    }
    return nodes;
}

// ELF Dependency Reader structures
struct ElfDependencyInfo {
    std::string lib_name;
    bool valid = false;
    bool is_64bit = false;
    std::string architecture = "Unknown";
    std::string soname = "";
    std::vector<std::string> needed_libs;
    std::string error;
};

static std::string get_elf_machine_name(uint16_t machine) {
    switch (machine) {
        case 3: return "x86 (Intel 386)";
        case 40: return "ARM 32-bit";
        case 62: return "x86_64 (64-bit Intel)";
        case 183: return "AArch64 (ARM 64-bit)";
        default: return "Unknown (" + std::to_string(machine) + ")";
    }
}

// Unified Templated ELF dependency reader with strict bounds and file size limits
template<typename EhdrT, typename PhdrT, typename DynT, typename AddrT>
static ElfDependencyInfo analyze_elf_templated(FILE* f, const std::string& path, long file_size) {
    ElfDependencyInfo info;
    info.lib_name = path;
    info.valid = true;
    info.is_64bit = (sizeof(AddrT) == 8);

    if (sizeof(EhdrT) > (size_t)file_size) {
        info.error = "File too small for ELF header";
        return info;
    }

    EhdrT ehdr;
    if (fread(&ehdr, 1, sizeof(ehdr), f) != sizeof(ehdr)) {
        info.error = "Failed to read ELF header";
        return info;
    }

    info.architecture = get_elf_machine_name(ehdr.e_machine);

    if (ehdr.e_phnum > 128) {
        info.error = "Too many program headers (corrupt ELF)";
        return info;
    }

    if (ehdr.e_phoff > (unsigned long)file_size || 
        ehdr.e_phnum * sizeof(PhdrT) > (unsigned long)file_size || 
        ehdr.e_phoff + ehdr.e_phnum * sizeof(PhdrT) > (unsigned long)file_size) {
        info.error = "Program headers exceed file boundary";
        return info;
    }

    std::vector<PhdrT> phdrs(ehdr.e_phnum);
    if (fseek(f, ehdr.e_phoff, SEEK_SET) != 0 ||
        fread(phdrs.data(), sizeof(PhdrT), ehdr.e_phnum, f) != ehdr.e_phnum) {
        info.error = "Failed to read Program Headers";
        return info;
    }

    const PhdrT* dynamic_phdr = nullptr;
    std::vector<PhdrT> load_phdrs;
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == PT_DYNAMIC) {
            dynamic_phdr = &phdr;
        } else if (phdr.p_type == PT_LOAD) {
            load_phdrs.push_back(phdr);
        }
    }

    if (!dynamic_phdr) {
        info.error = "PT_DYNAMIC segment not found";
        return info;
    }

    if (dynamic_phdr->p_offset > (unsigned long)file_size || 
        dynamic_phdr->p_filesz > (unsigned long)file_size || 
        dynamic_phdr->p_offset + dynamic_phdr->p_filesz > (unsigned long)file_size) {
        info.error = "Dynamic segment exceeds file boundary";
        return info;
    }

    size_t num_dyn = dynamic_phdr->p_filesz / sizeof(DynT);
    if (num_dyn > 1024) {
        info.error = "Too many dynamic entries (corrupt ELF)";
        return info;
    }

    std::vector<DynT> dyns(num_dyn);
    if (fseek(f, dynamic_phdr->p_offset, SEEK_SET) != 0 ||
        fread(dyns.data(), sizeof(DynT), num_dyn, f) != num_dyn) {
        info.error = "Failed to read Dynamic entries";
        return info;
    }

    AddrT strtab_va = 0;
    AddrT soname_offset = 0;
    std::vector<AddrT> needed_offsets;
    for (const auto& dyn : dyns) {
        if (dyn.d_tag == DT_STRTAB) {
            strtab_va = dyn.d_un.d_ptr;
        } else if (dyn.d_tag == DT_NEEDED) {
            needed_offsets.push_back(dyn.d_un.d_val);
        } else if (dyn.d_tag == DT_SONAME) {
            soname_offset = dyn.d_un.d_val;
        }
    }

    if (strtab_va == 0) {
        info.error = "DT_STRTAB dynamic tag not found";
        return info;
    }

    AddrT strtab_offset = 0;
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
        return info;
    }

    if (strtab_offset > (unsigned long)file_size) {
        info.error = "DT_STRTAB translated offset exceeds file boundary";
        return info;
    }

    if (soname_offset != 0) {
        unsigned long target_offset = strtab_offset + soname_offset;
        if (target_offset < (unsigned long)file_size) {
            if (fseek(f, target_offset, SEEK_SET) == 0) {
                char c;
                while (info.soname.size() < 256 && 
                       (ftell(f) < file_size) &&
                       fread(&c, 1, 1, f) == 1 && c != '\0') {
                    info.soname += c;
                }
            }
        }
    }

    for (auto offset : needed_offsets) {
        unsigned long target_offset = strtab_offset + offset;
        if (target_offset < (unsigned long)file_size) {
            if (fseek(f, target_offset, SEEK_SET) == 0) {
                std::string needed_lib;
                char c;
                while (needed_lib.size() < 256 && 
                       (ftell(f) < file_size) &&
                       fread(&c, 1, 1, f) == 1 && c != '\0') {
                    needed_lib += c;
                }
                if (!needed_lib.empty()) {
                    info.needed_libs.push_back(needed_lib);
                }
            }
        }
    }

    return info;
}

static ElfDependencyInfo analyze_elf_dependencies(const std::string& path) {
    ElfDependencyInfo info;
    info.lib_name = path;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        info.error = "Could not open file: " + std::string(strerror(errno));
        return info;
    }

    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        info.error = "Failed to get ELF file stat: " + std::string(strerror(errno));
        fclose(f);
        return info;
    }
    long file_size = st.st_size;

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
    if (fseek(f, 0, SEEK_SET) != 0) {
        info.error = "Failed to reset stream to beginning of file";
        fclose(f);
        return info;
    }

    if (elf_class == ELFCLASS64) {
        info = analyze_elf_templated<Elf64_Ehdr, Elf64_Phdr, Elf64_Dyn, Elf64_Addr>(f, path, file_size);
    } else if (elf_class == ELFCLASS32) {
        info = analyze_elf_templated<Elf32_Ehdr, Elf32_Phdr, Elf32_Dyn, Elf32_Addr>(f, path, file_size);
    } else {
        info.error = "Unknown ELF class: " + std::to_string(elf_class);
    }

    fclose(f);
    return info;
}

// Enterprise architecture clean design: singleton Context pattern encapsulates state entirely
class FmNativeContext {
public:
    static FmNativeContext& getInstance() {
        static FmNativeContext instance;
        return instance;
    }

    // Explicitly delete copy and move constructors/assignments
    FmNativeContext(const FmNativeContext&) = delete;
    FmNativeContext& operator=(const FmNativeContext&) = delete;
    FmNativeContext(FmNativeContext&&) = delete;
    FmNativeContext& operator=(FmNativeContext&&) = delete;

    std::mutex& getMutex() { return mutex_; }

    bool loadQualcommHal() {
        hal_.reset();

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
                        hal_.handle = handle;
                        hal_.loaded_path = path;
                        hal_.power_up = (fm_power_up_t)dlsym(handle, "fmpal_power_up");
                        hal_.power_down = (fm_power_down_t)dlsym(handle, "fmpal_power_down");
                        hal_.tune = (fm_tune_t)dlsym(handle, "fmpal_tune");
                        hal_.seek = (fm_seek_t)dlsym(handle, "fmpal_seek");
                        hal_.get_rssi = (fm_get_rssi_t)dlsym(handle, "fmpal_get_rssi");

                        if (hal_.power_up || hal_.tune) {
                            LOGI("Successfully mapped symbols for FM PAL/HAL in %s", path.c_str());
                            return true;
                        } else {
                            LOGE("Loaded %s but fmpal_power_up or fmpal_tune symbols were missing", path.c_str());
                            hal_.reset();
                        }
                    } else {
                        LOGE("Failed to load library: %s (dlerror: %s)", path.c_str(), dlerror());
                    }
                }
            }
        }
        return false;
    }

    void resetHal() {
        hal_.reset();
    }

    HalInstance& getHal() { return hal_; }
    UniqueFd& getRadioFd() { return radio_fd_; }

    int getCurrentFreq() const { return current_freq_khz_; }
    void setCurrentFreq(int freq) { current_freq_khz_ = freq; }

    bool isMuted() const { return is_muted_; }
    void setMuted(bool mute) { is_muted_ = mute; }

    int getBand() const { return current_band_; }
    void setBand(int band) { current_band_ = band; }

private:
    FmNativeContext() = default;

    std::mutex mutex_;
    HalInstance hal_;
    UniqueFd radio_fd_;
    int current_freq_khz_ = 98100; // Default 98.1 MHz
    bool is_muted_ = false;
    int current_band_ = 0;
};

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

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_example_fm_FmNative_isHardwareSupported(JNIEnv *env, jobject thiz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Running isHardwareSupported() check...");

    // 1. Check if HAL can be successfully loaded (this is the absolute best indicator)
    if (ctx.loadQualcommHal()) {
        LOGI("Hardware support detected via loaded QTI FM HAL/PAL!");
        ctx.resetHal(); 
        return JNI_TRUE;
    }

    // 2. Check if the library files physically exist, even if we can't dlopen them from untrusted app namespace
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
    for (const auto& dir : search_paths) {
        for (const auto& pattern : target_patterns) {
            std::vector<std::string> matches;
            scan_dir_recursive(dir, pattern, matches);
            if (!matches.empty()) {
                LOGI("Hardware support detected: Qualcomm FM HAL libraries found on disk in %s!", dir.c_str());
                return JNI_TRUE;
            }
        }
    }

    // 3. Check V4L2 device nodes
    auto nodes = get_candidate_device_nodes();
    for (const auto& node : nodes) {
        int fd = open(node.c_str(), O_RDONLY);
        if (fd >= 0) {
            LOGI("Hardware support detected via active device node: %s", node.c_str());
            close(fd);
            return JNI_TRUE;
        } else if (errno != ENOENT) {
            LOGI("Device node exists: %s (Open error: %d - %s). This indicates physical hardware presence!", node.c_str(), errno, strerror(errno));
            return JNI_TRUE;
        }
    }

    // 4. Check system/SoC properties for Qualcomm Snapdragon or Samsung SoC signatures
    std::string platform = get_system_property("ro.board.platform");
    std::string hardware = get_system_property("ro.hardware");
    std::string soc_manufacturer = get_system_property("ro.soc.manufacturer");
    std::string board = get_system_property("ro.product.board");
    std::string device = get_system_property("ro.product.device");

    // Convert all to lowercase for case-insensitive robust checking
    auto to_lower = [](std::string s) {
        for (char &c : s) c = std::tolower((unsigned char)c);
        return s;
    };
    platform = to_lower(platform);
    hardware = to_lower(hardware);
    soc_manufacturer = to_lower(soc_manufacturer);
    board = to_lower(board);
    device = to_lower(device);

    LOGI("SoC Props: platform=%s, hardware=%s, manufacturer=%s, board=%s, device=%s",
         platform.c_str(), hardware.c_str(), soc_manufacturer.c_str(), board.c_str(), device.c_str());

    // Qualcomm / Snapdragon platform indicators (including SM Snapdragon Mobile platforms, SDM, msm, etc.)
    if (platform.find("qcom") != std::string::npos || platform.find("msm") != std::string::npos ||
        platform.find("sdm") != std::string::npos || platform.rfind("sm", 0) == 0 ||
        platform.find("bengal") != std::string::npos || platform.find("taro") != std::string::npos ||
        platform.find("lahaina") != std::string::npos || platform.find("kona") != std::string::npos ||
        platform.find("khaje") != std::string::npos || platform.find("monaco") != std::string::npos ||
        platform.find("gta9") != std::string::npos ||
        hardware.find("qcom") != std::string::npos || hardware.find("s5e") != std::string::npos ||
        soc_manufacturer.find("qti") != std::string::npos || soc_manufacturer.find("qualcomm") != std::string::npos ||
        board.find("qcom") != std::string::npos || board.find("msm") != std::string::npos) {
        LOGI("Hardware platform is Qualcomm/Samsung/Snapdragon SoC. Supporting FM.");
        return JNI_TRUE;
    }

    // 5. If it's a physical ARM or ARM64 Android device (not a qemu / ranchu x86 emulator), default to true
    // so the user can always attempt to initialize FM.
    std::string abi = get_system_property("ro.product.cpu.abi");
    abi = to_lower(abi);
    bool is_emulator = (hardware.find("goldfish") != std::string::npos || 
                        hardware.find("ranchu") != std::string::npos || 
                        hardware.find("sdk_") != std::string::npos ||
                        device.find("emulator") != std::string::npos);
    if (!is_emulator && (abi.find("arm") != std::string::npos || abi.find("aarch64") != std::string::npos)) {
        LOGI("Physical ARM/ARM64 device detected (non-emulator). Allowing FM access support.");
        return JNI_TRUE;
    }

    LOGE("No physical Qualcomm FM HAL, drivers, nodes, or SOC properties detected on this device.");
    return JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_initFm(JNIEnv *env, jobject thiz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Powering up and initializing Qualcomm FM hardware...");

    if (ctx.loadQualcommHal()) {
        auto& hal = ctx.getHal();
        if (hal.power_up) {
            LOGI("Calling fmpal_power_up standard hook...");
            int res = hal.power_up(nullptr);
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
        int fd = open(node.c_str(), O_RDWR | O_NONBLOCK);
        if (fd >= 0) {
            LOGI("Successfully opened FM device driver node: %s", node.c_str());
            struct v4l2_tuner tuner;
            memset(&tuner, 0, sizeof(tuner));
            tuner.index = 0;
            if (ioctl(fd, VIDIOC_G_TUNER, &tuner) >= 0) {
                LOGI("Successfully verified tuner on node %s (Name: %s)", node.c_str(), tuner.name);
                ctx.getRadioFd().reset(fd);
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
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Powering off and releasing Qualcomm FM hardware...");

    auto& hal = ctx.getHal();
    if (hal.power_down) {
        LOGI("Calling fmpal_power_down standard hook...");
        hal.power_down();
    }

    ctx.resetHal();
    ctx.getRadioFd().reset();

    LOGI("FM Radio hardware powered down successfully.");
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_setFrequency(JNIEnv *env, jobject thiz, jint freqKHz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Tuning receiver to station: %d KHz", freqKHz);

    auto& hal = ctx.getHal();
    if (hal.tune) {
        int res = hal.tune(freqKHz);
        if (res >= 0) {
            ctx.setCurrentFreq(freqKHz);
            return 0;
        }
        LOGE("fmpal_tune returned failure: %d", res);
    }

    auto& radio_fd = ctx.getRadioFd();
    if (radio_fd.is_valid()) {
        struct v4l2_frequency frequency;
        memset(&frequency, 0, sizeof(frequency));
        frequency.tuner = 0;
        frequency.type = V4L2_TUNER_RADIO;
        frequency.frequency = (freqKHz * 16) / 1000;

        if (ioctl(radio_fd.get(), VIDIOC_S_FREQUENCY, &frequency) >= 0) {
            LOGI("Tuned to frequency successfully via V4L2 ioctl");
            ctx.setCurrentFreq(freqKHz);
            return 0;
        }
        LOGE("Failed to tune frequency via V4L2 ioctl (errno: %d - %s)", errno, strerror(errno));
    }

    ctx.setCurrentFreq(freqKHz);
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_getFrequency(JNIEnv *env, jobject thiz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    auto& radio_fd = ctx.getRadioFd();
    if (radio_fd.is_valid()) {
        struct v4l2_frequency frequency;
        memset(&frequency, 0, sizeof(frequency));
        frequency.tuner = 0;
        if (ioctl(radio_fd.get(), VIDIOC_G_FREQUENCY, &frequency) >= 0) {
            int freq = (frequency.frequency * 1000) / 16;
            ctx.setCurrentFreq(freq);
            return freq;
        }
        LOGE("Failed to query frequency via V4L2 ioctl (errno: %d - %s)", errno, strerror(errno));
    }
    return ctx.getCurrentFreq();
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_startSearch(JNIEnv *env, jobject thiz, jint direction) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Starting scan/search, direction: %s", direction == 0 ? "DOWN" : "UP");

    auto& hal = ctx.getHal();
    if (hal.seek) {
        int res = hal.seek(direction, 0);
        if (res >= 0) return 0;
        LOGE("fmpal_seek returned error: %d", res);
    }

    auto& radio_fd = ctx.getRadioFd();
    if (radio_fd.is_valid()) {
        struct v4l2_hw_freq_seek seek;
        memset(&seek, 0, sizeof(seek));
        seek.tuner = 0;
        seek.type = V4L2_TUNER_RADIO;
        seek.seek_upward = (direction != 0) ? 1 : 0;
        seek.wrap_around = 1;
        seek.spacing = 100000;

        if (ioctl(radio_fd.get(), VIDIOC_S_HW_FREQ_SEEK, &seek) >= 0) {
            LOGI("Hardware seek started successfully via V4L2 ioctl");
            return 0;
        }
        LOGE("Failed to seek via V4L2 ioctl (errno: %d - %s)", errno, strerror(errno));
    }

    return -1;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_cancelSearch(JNIEnv *env, jobject thiz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Canceling active frequency search.");
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_example_fm_FmNative_getRdsData(JNIEnv *env, jobject thiz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    auto& radio_fd = ctx.getRadioFd();
    if (radio_fd.is_valid()) {
        struct v4l2_rds_data rds_buf[16];
        ssize_t bytes = read(radio_fd.get(), rds_buf, sizeof(rds_buf));
        if (bytes > 0) {
            LOGI("Read %zd bytes of RDS data from driver", bytes);
            return env->NewStringUTF("Qualcomm RDS Broadcast");
        } else {
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // No data available right now, return null without blocking the thread
                return nullptr;
            }
        }
    }
    return nullptr;
}

JNIEXPORT jboolean JNICALL
Java_com_example_fm_FmNative_isMuted(JNIEnv *env, jobject thiz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    return ctx.isMuted() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_setMute(JNIEnv *env, jobject thiz, jboolean mute) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Setting hardware audio mute: %s", mute ? "MUTED" : "UNMUTED");
    ctx.setMuted(mute);

    auto& radio_fd = ctx.getRadioFd();
    if (radio_fd.is_valid()) {
        struct v4l2_control control;
        memset(&control, 0, sizeof(control));
        control.id = V4L2_CID_AUDIO_MUTE;
        control.value = mute ? 1 : 0;

        if (ioctl(radio_fd.get(), VIDIOC_S_CTRL, &control) >= 0) {
            LOGI("Mute set successfully via V4L2 control");
            return 0;
        }
        LOGE("Failed to set mute via V4L2 control (errno: %d - %s)", errno, strerror(errno));
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_getSignalStrength(JNIEnv *env, jobject thiz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    auto& hal = ctx.getHal();
    if (hal.get_rssi) {
        int rssi = 0;
        if (hal.get_rssi(&rssi) >= 0) {
            return rssi;
        }
    }

    auto& radio_fd = ctx.getRadioFd();
    if (radio_fd.is_valid()) {
        struct v4l2_tuner tuner;
        memset(&tuner, 0, sizeof(tuner));
        tuner.index = 0;
        if (ioctl(radio_fd.get(), VIDIOC_G_TUNER, &tuner) >= 0) {
            return (tuner.signal * 100) / 65535;
        }
    }

    return 45; // Default reference signal strength percent when offline
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_setBand(JNIEnv *env, jobject thiz, jint band) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Setting FM band to: %d", band);
    ctx.setBand(band);
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_example_fm_FmNative_getDiagnosticsReport(JNIEnv *env, jobject thiz) {
    auto& ctx = FmNativeContext::getInstance();
    std::lock_guard<std::mutex> lock(ctx.getMutex());
    LOGI("Generating full high-resolution FM hardware diagnostics report...");
    std::string report;

    // Stage 1: CPU, Board, Model, OS, Kernel Version, ABI
    report += "=== STAGE 1: SYSTEM & CPU PLATFORM ===\n";
    struct utsname sys_info;
    std::string kernel_version = "UNKNOWN";
    if (uname(&sys_info) == 0) {
        kernel_version = std::string(sys_info.sysname) + " " + sys_info.release + " " + sys_info.version;
    }
    report += "Platform (ro.board.platform): " + get_system_property("ro.board.platform") + "\n";
    report += "Board (ro.product.board): " + get_system_property("ro.product.board") + "\n";
    report += "Model (ro.product.model): " + get_system_property("ro.product.model") + "\n";
    report += "Android Version (ro.build.version.release): " + get_system_property("ro.build.version.release") + "\n";
    report += "Kernel Version: " + kernel_version + "\n";
    report += "ABI (ro.product.cpu.abi): " + get_system_property("ro.product.cpu.abi") + "\n";
    report += "SELinux Mode: " + check_selinux_status() + "\n";
    report += "App Security Domain: " + get_selinux_context() + "\n\n";

    // Stage 2: Partition Shared Library Search
    report += "=== STAGE 2: PARTITION SHARED LIBRARY SEARCH ===\n";
    std::vector<std::string> search_dirs = {
        "/vendor/lib64", "/vendor/lib",
        "/system/lib64", "/system/lib",
        "/system_ext/lib64", "/system_ext/lib",
        "/odm/lib64", "/odm/lib",
        "/product/lib64", "/product/lib"
    };
    std::vector<std::string> discovered_libs;
    bool lib_exists = false;
    for (const auto& dir : search_dirs) {
        std::vector<std::string> dir_results;
        scan_dir_recursive(dir, "", dir_results);
        report += "Search in " + dir + ": " + (dir_results.empty() ? "NOT FOUND" : "FOUND") + "\n";
        for (const auto& lib_path : dir_results) {
            report += "  -> " + lib_path + "\n";
            discovered_libs.push_back(lib_path);
            lib_exists = true;
        }
    }
    report += "\n";

    // Stage 3: Shared Library Attribute & Metadata Inspection
    report += "=== STAGE 3: SHARED LIBRARY ATTRIBUTES & METADATA ===\n";
    for (const auto& path : discovered_libs) {
        report += "Library: " + path + "\n";
        struct stat st;
        bool exists = (stat(path.c_str(), &st) == 0);
        report += std::string("  -> File Exists: ") + (exists ? "YES" : "NO") + "\n";
        
        FILE* test_read = fopen(path.c_str(), "rb");
        bool readable = (test_read != nullptr);
        if (test_read) fclose(test_read);
        report += std::string("  -> Readable: ") + (readable ? "YES" : "NO") + "\n";

        auto elf_info = analyze_elf_dependencies(path);
        if (elf_info.valid) {
            report += "  -> ELF Valid: YES\n";
            report += "  -> Class: " + std::string(elf_info.is_64bit ? "64-bit" : "32-bit") + "\n";
            report += "  -> Architecture: " + elf_info.architecture + "\n";
            report += "  -> SONAME: " + (elf_info.soname.empty() ? "None" : elf_info.soname) + "\n";
        } else {
            report += "  -> ELF Valid: NO (" + elf_info.error + ")\n";
        }
    }
    report += "\n";

    // Stage 4: Attempt dlopen()
    report += "=== STAGE 4: DYNAMIC LOADING (dlopen) ===\n";
    bool lib_loaded = false;
    for (const auto& path : discovered_libs) {
        void* handle = dlopen(path.c_str(), RTLD_NOW);
        if (handle) {
            report += "Library: " + path + " -> dlopen: SUCCESS\n";
            lib_loaded = true;
            dlclose(handle);
        } else {
            const char* err = dlerror();
            report += "Library: " + path + " -> dlopen: FAILED\n";
            report += "  -> dlerror: " + std::string(err ? err : "Unknown dlerror") + "\n";
        }
    }
    report += "\n";

    // Stage 5: Resolve symbols using dlsym()
    report += "=== STAGE 5: SYMBOL RESOLUTION (dlsym) ===\n";
    bool symbols_resolved = false;
    for (const auto& path : discovered_libs) {
        void* handle = dlopen(path.c_str(), RTLD_NOW);
        if (handle) {
            report += "Library: " + path + "\n";
            std::vector<std::string> symbols = {"fmpal_power_up", "fmpal_power_down", "fmpal_tune", "fmpal_seek", "fmpal_get_rssi"};
            int resolved_count = 0;
            for (const auto& sym : symbols) {
                void* sym_ptr = dlsym(handle, sym.c_str());
                if (sym_ptr) {
                    report += "  -> " + sym + ": SUCCESS\n";
                    resolved_count++;
                } else {
                    report += "  -> " + sym + ": FAILED\n";
                }
            }
            if (resolved_count == (int)symbols.size()) {
                symbols_resolved = true;
            }
            dlclose(handle);
        } else {
            report += "Library: " + path + " -> Skipped (Could not dlopen)\n";
        }
    }
    report += "\n";

    // Stage 6: Search Binder services
    report += "=== STAGE 6: BINDER SERVICES ENUMERATION ===\n";
    std::string service_output = run_shell_command("service list");
    bool binder_connected = false;
    std::vector<std::string> patterns = {"broadcastradio", "fm", "qti.hardware.fm"};
    std::stringstream ss(service_output);
    std::string line;
    while (std::getline(ss, line)) {
        for (const auto& p : patterns) {
            if (line.find(p) != std::string::npos) {
                report += "  -> Registered Service: " + line + "\n";
                binder_connected = true;
                break;
            }
        }
    }
    if (!binder_connected) {
        report += "  -> No registered broadcastradio, fm, or qti.hardware.fm Binder services discovered.\n";
    }
    report += "\n";

    // Stage 7: Search every device node
    report += "=== STAGE 7: KERNEL DEVICE NODES ===\n";
    auto dev_nodes = get_candidate_device_nodes();
    bool node_exists = false;
    bool driver_opened = ctx.getRadioFd().is_valid();
    for (const auto& node : dev_nodes) {
        struct stat st;
        if (stat(node.c_str(), &st) == 0) {
            node_exists = true;
            report += "Node: " + node + " [Exists]\n";
            char perm_str[10];
            snprintf(perm_str, sizeof(perm_str), "%o", st.st_mode & 0777);
            report += "  -> DAC Permissions: " + std::string(perm_str) + " (UID: " + std::to_string(st.st_uid) + ", GID: " + std::to_string(st.st_gid) + ")\n";
            int fd = open(node.c_str(), O_RDONLY);
            if (fd >= 0) {
                report += "  -> Accessibility: Open SUCCESS (O_RDONLY)\n";
                driver_opened = true;
                close(fd);
            } else {
                int err = errno;
                report += "  -> Accessibility: Open FAILED (errno: " + std::to_string(err) + " - " + strerror(err) + ")\n";
            }
        }
    }
    report += "\n";

    // Stage 8: Determine WHY access failed (Failure Analysis)
    report += "=== STAGE 8: ACCESS FAILURE ANALYSIS ===\n";
    bool any_denial = false;
    for (const auto& node : dev_nodes) {
        struct stat st;
        if (stat(node.c_str(), &st) == 0) {
            int fd = open(node.c_str(), O_RDONLY);
            if (fd < 0) {
                any_denial = true;
                int err = errno;
                if (err == EACCES) {
                    bool dac_allows_world = (st.st_mode & 0004) == 0004;
                    if (!dac_allows_world) {
                        report += "Node: " + node + " -> Access failed: Permission Denied [DAC]\n";
                        report += "  -> Cause: Group/Owner permissions restrict process access.\n";
                    } else {
                        report += "Node: " + node + " -> Access failed: Permission Denied [MAC/SELinux]\n";
                        report += "  -> Cause: Enforcing SELinux policy blocks untrusted_app domain from /dev access.\n";
                    }
                } else {
                    report += "Node: " + node + " -> Access failed: " + strerror(err) + " [Kernel Restriction]\n";
                }
            }
        }
    }
    if (!node_exists) {
        report += "  -> Driver Missing: No tuner/radio device nodes found in /dev.\n";
    } else if (!any_denial) {
        report += "  -> All discovered device nodes are readable.\n";
    }
    report += "\n";

    // Stage 9: Inspect Android Linker Namespaces
    report += "=== STAGE 9: LINKER NAMESPACE INSPECTION ===\n";
    bool namespace_isolated = false;
    for (const auto& path : discovered_libs) {
        void* h = dlopen(path.c_str(), RTLD_NOW);
        if (!h) {
            std::string err_str = dlerror() ? dlerror() : "";
            if (err_str.find("namespace") != std::string::npos ||
                err_str.find("not accessible") != std::string::npos ||
                err_str.find("restricted") != std::string::npos) {
                report += "Library: " + path + "\n";
                report += "  -> Linker Namespace Blocked: YES\n";
                report += "  -> Reason: Treble dynamic linker isolation prevents loading vendor/system libraries directly into untrusted apps.\n";
                namespace_isolated = true;
            }
        } else {
            dlclose(h);
        }
    }
    if (!namespace_isolated) {
        report += "  -> Linker Namespace isolate block not detected directly on candidate libraries.\n";
    }
    report += "\n";

    // Stage 10: Dependency/Import Inspection
    report += "=== STAGE 10: DEPENDENCY & IMPORT INSPECTION ===\n";
    for (const auto& path : discovered_libs) {
        report += "Library: " + path + "\n";
        auto elf_info = analyze_elf_dependencies(path);
        if (elf_info.valid) {
            bool broken = false;
            for (const auto& dep : elf_info.needed_libs) {
                std::vector<std::string> search_dirs_dep;
                if (elf_info.is_64bit) {
                    search_dirs_dep = {"/vendor/lib64", "/system/lib64", "/system_ext/lib64", "/odm/lib64", "/product/lib64"};
                } else {
                    search_dirs_dep = {"/vendor/lib", "/system/lib", "/system_ext/lib", "/odm/lib", "/product/lib"};
                }
                bool found = false;
                for (const auto& d : search_dirs_dep) {
                    struct stat dep_st;
                    if (stat((d + "/" + dep).c_str(), &dep_st) == 0) {
                        found = true;
                        break;
                    }
                }
                report += "  -> Import: " + dep + (found ? " [OK]" : " [BROKEN / MISSING]") + "\n";
                if (!found) broken = true;
            }
            report += std::string("  -> Dependency Chain: ") + (broken ? "BROKEN" : "INTACT") + "\n";
        } else {
            report += "  -> Failed to analyze dependencies: " + elf_info.error + "\n";
        }
    }
    report += "\n";

    // Process mapped libraries (dl_iterate_phdr)
    report += "=== PROCESS MAPPED SHARED LIBRARIES (dl_iterate_phdr) ===\n";
    PhdrCallbackData phdr_data;
    phdr_data.report = &report;
    dl_iterate_phdr(dl_iterate_phdr_callback, &phdr_data);
    report += "\n";

    // Compute status states
    std::string state_lib_found = lib_exists ? "GREEN" : "RED";
    std::string state_lib_loaded = lib_loaded ? "GREEN" : (lib_exists ? "YELLOW" : "RED");
    std::string state_symbols_resolved = symbols_resolved ? "GREEN" : (lib_loaded ? "YELLOW" : "RED");
    std::string state_binder_connected = binder_connected ? "GREEN" : "RED";
    std::string state_kernel_driver = driver_opened ? "GREEN" : (node_exists ? "YELLOW" : "RED");

    std::string audio_prop = get_system_property("ro.vendor.audio.sdk");
    bool audio_path_ready = (audio_prop != "UNKNOWN/NOT_SET") || (get_system_property("vendor.audio.feature.fm.rx.enabled") == "true");
    std::string state_audio_routing = audio_path_ready ? "GREEN" : "RED";

    bool fm_ready = (lib_loaded || driver_opened);
    std::string state_fm_interface = fm_ready ? "GREEN" : "RED";

    bool fm_powered = (ctx.getHal().handle != nullptr || ctx.getRadioFd().is_valid());
    std::string state_fm_powered = fm_powered ? "GREEN" : "RED";

    bool fm_operational = (fm_powered && ctx.getCurrentFreq() > 0);
    std::string state_fm_operational = fm_operational ? "GREEN" : "RED";

    report += "=== STATES ===\n";
    report += "Library Found: " + state_lib_found + "\n";
    report += "Library Loaded: " + state_lib_loaded + "\n";
    report += "Symbols Resolved: " + state_symbols_resolved + "\n";
    report += "Binder Connected: " + state_binder_connected + "\n";
    report += "Kernel Driver Ready: " + state_kernel_driver + "\n";
    report += "Audio Routing Ready: " + state_audio_routing + "\n";
    report += "FM Interface Ready: " + state_fm_interface + "\n";
    report += "FM Powered: " + state_fm_powered + "\n";
    report += "FM Operational: " + state_fm_operational + "\n";

    return env->NewStringUTF(sanitize_utf8(report).c_str());
}

}
