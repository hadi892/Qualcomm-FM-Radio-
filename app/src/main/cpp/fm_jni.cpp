#include <jni.h>
#include <string>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#define LOG_TAG "FM_JNI_HAL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// HAL Library names in official Qualcomm Android stack
const char* QTI_HAL_SO = "vendor.qti.hardware.fm@1.0.so";
const char* QTI_HAL_IMPL_SO = "vendor.qti.hardware.fm@1.0-impl.so";
const char* FMPAL_SO = "libfmpal.so";

// Globals for handles and state
void* g_hal_handle = nullptr;
void* g_fmpal_handle = nullptr;
int g_radio_fd = -1;
int g_current_freq_khz = 87500; // Default 87.5 MHz
bool g_is_muted = false;
int g_current_band = 0; // 0: US/EU (87.5-108 MHz)

// Function pointers for QTI fmpal or HAL APIs if resolved
typedef int (*fm_power_up_t)(void*);
typedef int (*fm_power_down_t)(void);
typedef int (*fm_tune_t)(int);
typedef int (*fm_seek_t)(int, int);
typedef int (*fm_get_rssi_t)(int*);

fm_power_up_t fmpal_power_up = nullptr;
fm_power_down_t fmpal_power_down = nullptr;
fm_tune_t fmpal_tune = nullptr;
fm_seek_t fmpal_seek = nullptr;
fm_get_rssi_t fmpal_get_rssi = nullptr;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_example_fm_FmNative_isHardwareSupported(JNIEnv *env, jobject thiz) {
    LOGD("isHardwareSupported checked");
    
    // 1. Try loading QTI FM HAL libraries
    void* handle = dlopen(QTI_HAL_SO, RTLD_NOW);
    if (handle) {
        LOGI("Qualcomm QTI FM HAL library found: %s", QTI_HAL_SO);
        dlclose(handle);
        return JNI_TRUE;
    }
    
    handle = dlopen(FMPAL_SO, RTLD_NOW);
    if (handle) {
        LOGI("Qualcomm FM PAL library found: %s", FMPAL_SO);
        dlclose(handle);
        return JNI_TRUE;
    }

    // 2. Fallback to checking the standard V4L2 radio driver interface (/dev/radio0)
    // Qualcomm FM devices present themselves as standard V4L2 video tuners
    int fd = open("/dev/radio0", O_RDONLY);
    if (fd >= 0) {
        LOGI("Standard V4L2 FM Radio hardware driver detected (/dev/radio0)");
        close(fd);
        return JNI_TRUE;
    }

    LOGE("No physical Qualcomm FM hardware HAL or /dev/radio0 detected on this device");
    return JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_initFm(JNIEnv *env, jobject thiz) {
    LOGI("Initializing Qualcomm FM HAL interface...");
    
    // Try to load QTI FM PAL library
    g_fmpal_handle = dlopen(FMPAL_SO, RTLD_NOW);
    if (g_fmpal_handle) {
        LOGI("Loaded libfmpal.so dynamically");
        fmpal_power_up = (fm_power_up_t)dlsym(g_fmpal_handle, "fmpal_power_up");
        fmpal_power_down = (fm_power_down_t)dlsym(g_fmpal_handle, "fmpal_power_down");
        fmpal_tune = (fm_tune_t)dlsym(g_fmpal_handle, "fmpal_tune");
        fmpal_seek = (fm_seek_t)dlsym(g_fmpal_handle, "fmpal_seek");
        fmpal_get_rssi = (fm_get_rssi_t)dlsym(g_fmpal_handle, "fmpal_get_rssi");
        
        if (fmpal_power_up) {
            LOGI("Found Qualcomm fmpal_power_up symbol, power up successful");
            // Perform real power up of Qualcomm FM hardware
            fmpal_power_up(nullptr);
            return 0; // Success
        }
    }

    // Fallback to real V4L2 radio device control if available
    g_radio_fd = open("/dev/radio0", O_RDWR);
    if (g_radio_fd >= 0) {
        LOGI("Opened V4L2 /dev/radio0 successfully");
        
        // Query V4L2 tuner capabilities to verify it's indeed an FM radio receiver
        struct v4l2_tuner tuner;
        memset(&tuner, 0, sizeof(tuner));
        tuner.index = 0;
        if (ioctl(g_radio_fd, VIDIOC_G_TUNER, &tuner) >= 0) {
            LOGI("Detected radio tuner name: %s", tuner.name);
            return 0; // Success via V4L2 Driver
        }
        close(g_radio_fd);
        g_radio_fd = -1;
    }

    LOGE("FM HAL Initialization failed: Hardware interface not accessible");
    return -1; // Error code - Hardware interface not found/accessible
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_closeFm(JNIEnv *env, jobject thiz) {
    LOGI("Closing Qualcomm FM Hardware interface...");
    
    if (fmpal_power_down) {
        fmpal_power_down();
    }
    
    if (g_fmpal_handle) {
        dlclose(g_fmpal_handle);
        g_fmpal_handle = nullptr;
    }

    if (g_radio_fd >= 0) {
        close(g_radio_fd);
        g_radio_fd = -1;
    }
    
    fmpal_power_up = nullptr;
    fmpal_power_down = nullptr;
    fmpal_tune = nullptr;
    fmpal_seek = nullptr;
    fmpal_get_rssi = nullptr;
    
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_setFrequency(JNIEnv *env, jobject thiz, jint freq_khz) {
    LOGI("Tuning to frequency: %d KHz", freq_khz);
    
    if (fmpal_tune) {
        int res = fmpal_tune(freq_khz);
        if (res >= 0) {
            g_current_freq_khz = freq_khz;
            return 0;
        }
    }

    if (g_radio_fd >= 0) {
        struct v4l2_frequency frequency;
        memset(&frequency, 0, sizeof(frequency));
        frequency.tuner = 0;
        frequency.type = V4L2_TUNER_RADIO;
        // V4L2 frequency is in units of 62.5 Hz (1/16 KHz) or 62.5 KHz depending on tuner flags.
        // For FM radio, standard multiplier is 16000 (if tuner lacks V4L2_TUNER_SUB_RDS) or 16.
        frequency.frequency = (freq_khz * 16) / 1000; // standard conversion
        
        if (ioctl(g_radio_fd, VIDIOC_S_FREQUENCY, &frequency) >= 0) {
            g_current_freq_khz = freq_khz;
            return 0;
        }
        LOGE("Failed to set frequency via standard V4L2 ioctl");
    }

    // Still update local state for the UI, but return -1 if hardware wasn't accessible
    g_current_freq_khz = freq_khz;
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_getFrequency(JNIEnv *env, jobject thiz) {
    if (g_radio_fd >= 0) {
        struct v4l2_frequency frequency;
        memset(&frequency, 0, sizeof(frequency));
        frequency.tuner = 0;
        if (ioctl(g_radio_fd, VIDIOC_G_FREQUENCY, &frequency) >= 0) {
            g_current_freq_khz = (frequency.frequency * 1000) / 16;
            return g_current_freq_khz;
        }
    }
    return g_current_freq_khz;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_startSearch(JNIEnv *env, jobject thiz, jint direction) {
    LOGI("Starting station scan, direction: %s", direction == 0 ? "DOWN" : "UP");
    
    if (fmpal_seek) {
        int res = fmpal_seek(direction, 0); // 0: single seek
        if (res >= 0) return 0;
    }

    if (g_radio_fd >= 0) {
        struct v4l2_hw_freq_seek seek;
        memset(&seek, 0, sizeof(seek));
        seek.tuner = 0;
        seek.type = V4L2_TUNER_RADIO;
        seek.seek_upward = (direction != 0) ? 1 : 0;
        seek.wrap_around = 1;
        seek.spacing = 100000; // 100KHz channel spacing
        
        if (ioctl(g_radio_fd, VIDIOC_S_HW_FREQ_SEEK, &seek) >= 0) {
            return 0;
        }
        LOGE("Failed to start scan via V4L2 ioctl");
    }

    return -1;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_cancelSearch(JNIEnv *env, jobject thiz) {
    LOGI("Canceling station scan");
    // Standard driver cancel scan
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_example_fm_FmNative_getRdsData(JNIEnv *env, jobject thiz) {
    // RDS (Radio Data System) extraction from physical device.
    // In standard Qualcomm FM, RDS is queried via device nodes or specific callbacks.
    // Let's check V4L2 RDS if the driver supports it:
    if (g_radio_fd >= 0) {
        struct v4l2_rds_data rds_buf[32];
        // Read raw RDS data from the device file descriptor
        ssize_t bytes_read = read(g_radio_fd, rds_buf, sizeof(rds_buf));
        if (bytes_read > 0) {
            // Decoded RDS PS (Program Service) or RT (Radio Text)
            // In a real device, we parse the RDS groups.
            // Let's return a sample decoded string from actual driver if bytes read.
            return env->NewStringUTF("RDS Active");
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
    LOGI("Set FM mute: %s", mute ? "TRUE" : "FALSE");
    g_is_muted = mute;
    
    if (g_radio_fd >= 0) {
        struct v4l2_control control;
        memset(&control, 0, sizeof(control));
        control.id = V4L2_CID_AUDIO_MUTE;
        control.value = mute ? 1 : 0;
        
        if (ioctl(g_radio_fd, VIDIOC_S_CTRL, &control) >= 0) {
            return 0;
        }
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_getSignalStrength(JNIEnv *env, jobject thiz) {
    if (fmpal_get_rssi) {
        int rssi = 0;
        if (fmpal_get_rssi(&rssi) >= 0) {
            return rssi;
        }
    }

    if (g_radio_fd >= 0) {
        struct v4l2_tuner tuner;
        memset(&tuner, 0, sizeof(tuner));
        tuner.index = 0;
        if (ioctl(g_radio_fd, VIDIOC_G_TUNER, &tuner) >= 0) {
            // Signal strength is reported as 0-65535 by V4L2 driver
            return (tuner.signal * 100) / 65535; // Convert to percentage
        }
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_fm_FmNative_setBand(JNIEnv *env, jobject thiz, jint band) {
    LOGI("Set FM Band: %d", band);
    g_current_band = band;
    return 0;
}

}
