/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2014 The CyanogenMod Project
 * Copyright (C) 2014-2015 Andreas Schneider <asn@cryptomilk.org>
 * Copyright (C) 2014-2017 Christopher N. Hesse <raymanfx@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>

#define LOG_TAG "SamsungPowerHAL"
/* #define LOG_NDEBUG 0 */
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>
#include <liblights/samsung_lights_helper.h>

#include "samsung_power.h"
#include "util.h"
#include "tsp.h"

struct samsung_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    int boost_fd;
    int boostpulse_fd;
    char hispeed_freqs[CLUSTER_COUNT][PARAM_MAXLEN];
    char max_freqs[CLUSTER_COUNT][PARAM_MAXLEN];
    char* touchscreen_power_path;
    char* touchkey_power_path;
    bool dt2w_supported;
    bool dt2w_active;
};

enum power_profile_e {
    PROFILE_POWER_SAVE = 0,
    PROFILE_BALANCED,
    PROFILE_HIGH_PERFORMANCE,
    PROFILE_MAX
};

static enum power_profile_e current_power_profile = PROFILE_BALANCED;

/**********************************************************
 *** POWER FUNCTIONS
 **********************************************************/

static void set_power_profile(struct samsung_power_module *samsung_pwr,
                              int profile)
{
    int rc;

    if (profile < 0 || profile >= PROFILE_MAX) {
        return;
    }

    if (current_power_profile == profile) {
        return;
    }

    ALOGV("%s: profile=%d", __func__, profile);

    switch (profile) {
        case PROFILE_POWER_SAVE:
            // Grab value set by init.*.rc
            cpu_interactive_read(HISPEED_FREQ_PATH, samsung_pwr->hispeed_freqs);
            // Limit to hispeed freq
            cpu_sysfs_write(MAX_FREQ_PATH, samsung_pwr->hispeed_freqs);
            ALOGV("%s: set powersave mode", __func__);
            break;
        case PROFILE_BALANCED:
            // Restore normal max freq
            cpu_sysfs_write(MAX_FREQ_PATH, samsung_pwr->max_freqs);
            ALOGV("%s: set balanced mode", __func__);
            break;
        case PROFILE_HIGH_PERFORMANCE:
            // Restore normal max freq
            cpu_sysfs_write(MAX_FREQ_PATH, samsung_pwr->max_freqs);
            ALOGV("%s: set performance mode", __func__);
            break;
        default:
            ALOGW("%s: Unknown power profile: %d", __func__, profile);
            return;
    }

    current_power_profile = profile;
}

static void find_input_nodes(struct samsung_power_module *samsung_pwr, char *dir)
{
    const char filename[] = "name";
    char errno_str[64];
    struct dirent *de;
    char file_content[20];
    char *path = NULL;
    char *node_path = NULL;
    size_t pathsize;
    size_t node_pathsize;
    DIR *d;

    d = opendir(dir);
    if (d == NULL) {
        return;
    }

    while ((de = readdir(d)) != NULL) {
        if (strncmp(filename, de->d_name, sizeof(filename)) == 0) {
            pathsize = strlen(dir) + strlen(de->d_name) + 2;
            node_pathsize = strlen(dir) + strlen("enabled") + 2;

            path = malloc(pathsize);
            node_path = malloc(node_pathsize);
            if (path == NULL || node_path == NULL) {
                strerror_r(errno, errno_str, sizeof(errno_str));
                ALOGE("Out of memory: %s", errno_str);
                return;
            }

            snprintf(path, pathsize, "%s/%s", dir, filename);
            sysfs_read(path, file_content, sizeof(file_content));

            snprintf(node_path, node_pathsize, "%s/%s", dir, "enabled");

            if (strncmp(file_content, "sec_touchkey", 12) == 0) {
                ALOGV("%s: found touchkey path: %s", __func__, node_path);
                samsung_pwr->touchkey_power_path = malloc(node_pathsize);
                if (samsung_pwr->touchkey_power_path == NULL) {
                    strerror_r(errno, errno_str, sizeof(errno_str));
                    ALOGE("Out of memory: %s", errno_str);
                    return;
                }
                snprintf(samsung_pwr->touchkey_power_path, node_pathsize,
                         "%s", node_path);
            }

            if (strncmp(file_content, "sec_touchscreen", 15) == 0) {
                ALOGV("%s: found touchscreen path: %s", __func__, node_path);
                samsung_pwr->touchscreen_power_path = malloc(node_pathsize);
                if (samsung_pwr->touchscreen_power_path == NULL) {
                    strerror_r(errno, errno_str, sizeof(errno_str));
                    ALOGE("Out of memory: %s", errno_str);
                    return;
                }
                snprintf(samsung_pwr->touchscreen_power_path, node_pathsize,
                         "%s", node_path);
            }
        }
    }

    if (path)
        free(path);
    if (node_path)
        free(node_path);
    closedir(d);
}

/**********************************************************
 *** INIT FUNCTIONS
 **********************************************************/

static void init_cpufreqs(struct samsung_power_module *samsung_pwr)
{
    cpu_interactive_read(HISPEED_FREQ_PATH, samsung_pwr->hispeed_freqs);
    cpu_sysfs_read(MAX_FREQ_PATH, samsung_pwr->max_freqs);
}

static void init_touch_input_power_path(struct samsung_power_module *samsung_pwr)
{
    char dir[1024];
    uint32_t i;

    for (i = 0; i < 20; i++) {
        snprintf(dir, sizeof(dir), "/sys/class/input/input%d", i);
        find_input_nodes(samsung_pwr, dir);
    }
}

static void boost_open(struct samsung_power_module *samsung_pwr)
{
    char path[PATH_MAX];

    // the boost node is only valid for the LITTLE cluster
    sprintf(path, "%s%s", CPU_INTERACTIVE_PATHS[0], BOOST_PATH);

    samsung_pwr->boost_fd = open(path, O_WRONLY);
    if (samsung_pwr->boost_fd < 0) {
        ALOGE("Error opening %s: %s\n", path, strerror(errno));
    }
}

static void boostpulse_open(struct samsung_power_module *samsung_pwr)
{
    char path[PATH_MAX];

    // the boostpulse node is only valid for the LITTLE cluster
    sprintf(path, "%s%s", CPU_INTERACTIVE_PATHS[0], BOOSTPULSE_PATH);

    samsung_pwr->boostpulse_fd = open(path, O_WRONLY);
    if (samsung_pwr->boostpulse_fd < 0) {
        ALOGE("Error opening %s: %s\n", path, strerror(errno));
    }
}

static void samsung_power_init(struct power_module *module)
{
    struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;

    init_cpufreqs(samsung_pwr);

    // keep interactive boost fds opened
    boost_open(samsung_pwr);
    boostpulse_open(samsung_pwr);

    samsung_pwr->touchscreen_power_path = NULL;
    samsung_pwr->touchkey_power_path = NULL;
    init_touch_input_power_path(samsung_pwr);

    ALOGI("Initialized settings:");
    char max_freqs[PATH_MAX];
    sprintf(max_freqs, "max_freqs: cluster[0]: %s", samsung_pwr->max_freqs[0]);
    for (unsigned int i = 1; i < CLUSTER_COUNT; i++) {
        sprintf(max_freqs, "%s, %s[%d]: %s", max_freqs, "cluster", i, samsung_pwr->max_freqs[i]);
    }
    ALOGI("%s", max_freqs);
    char hispeed_freqs[PATH_MAX];
    sprintf(hispeed_freqs, "hispeed_freqs: cluster[0]: %s", samsung_pwr->hispeed_freqs[0]);
    for (unsigned int i = 1; i < CLUSTER_COUNT; i++) {
        sprintf(hispeed_freqs, "%s, %s[%d]: %s", hispeed_freqs, "cluster", i,
                samsung_pwr->hispeed_freqs[i]);
    }
    samsung_pwr->dt2w_supported = tsp_has_doubletap();
    samsung_pwr->dt2w_active = false;
    ALOGI("%s", hispeed_freqs);
    ALOGI("boostpulse_fd: %d", samsung_pwr->boostpulse_fd);
    ALOGI("touchscreen_power_path: %s",
            samsung_pwr->touchscreen_power_path ? samsung_pwr->touchscreen_power_path : "NULL");
    ALOGI("touchkey_power_path: %s",
            samsung_pwr->touchkey_power_path ? samsung_pwr->touchkey_power_path : "NULL");
}

/**********************************************************
 *** API FUNCTIONS
 ***
 *** Refer to power.h for documentation.
 **********************************************************/

static void samsung_power_set_interactive(struct power_module *module, int on)
{
    struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;
    int panel_brightness;
    char button_state[2];
    int rc;
    static bool touchkeys_blocked = false;
    char ON[CLUSTER_COUNT][PARAM_MAXLEN]  = {"1", "1"};
    char OFF[CLUSTER_COUNT][PARAM_MAXLEN] = {"0", "0"};

    ALOGV("power_set_interactive: %d", on);

    /*
     * Do not disable any input devices if the screen is on but we are in a non-interactive
     * state.
     */
    if (!on) {
        panel_brightness = get_cur_panel_brightness();
        if (panel_brightness < 0) {
            ALOGE("%s: Failed to read panel brightness", __func__);
        } else if (panel_brightness > 0) {
            ALOGV("%s: Moving to non-interactive state, but screen is still on,"
                  " not disabling input devices", __func__);
            goto out;
        }
    }

    /* Sanity check the touchscreen path */
    if (samsung_pwr->touchscreen_power_path) {
        sysfs_write(samsung_pwr->touchscreen_power_path, on ? "1" : "0");
    }

    /* Bail out if the device does not have touchkeys */
    if (samsung_pwr->touchkey_power_path == NULL) {
        goto out;
    }

    if (!on) {
        rc = sysfs_read(samsung_pwr->touchkey_power_path, button_state, ARRAY_SIZE(button_state));
        if (rc < 0) {
            ALOGE("%s: Failed to read touchkey state", __func__);
            goto out;
        }
        /*
         * If button_state is 0, the keys have been disabled by another component
         * (for example cmhw), which means we don't want them to be enabled when resuming
         * from suspend.
         */
        if (button_state[0] == '0') {
            touchkeys_blocked = true;
        } else {
            touchkeys_blocked = false;
        }
    }

    if (!touchkeys_blocked) {
        sysfs_write(samsung_pwr->touchkey_power_path, on ? "1" : "0");
    }

out:
    cpu_interactive_write(IO_IS_BUSY_PATH, on ? ON : OFF);

    ALOGV("power_set_interactive: %d done", on);
}

static void samsung_power_hint(struct power_module *module,
                                  power_hint_t hint,
                                  void *data)
{
    struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;
    int len;

    /* Bail out if low-power mode is active */
    if (current_power_profile == PROFILE_POWER_SAVE && hint != POWER_HINT_LOW_POWER
            && hint != POWER_HINT_SET_PROFILE) {
        ALOGW("%s: PROFILE_POWER_SAVE active, ignoring hint %d", __func__, hint);
        return;
    }

    switch (hint) {
        case POWER_HINT_VSYNC:
            ALOGV("%s: POWER_HINT_VSYNC", __func__);
            break;
        case POWER_HINT_INTERACTION:
            ALOGV("%s: POWER_HINT_INTERACTION", __func__);
            send_boostpulse(samsung_pwr->boostpulse_fd);
            break;
        case POWER_HINT_LOW_POWER:
            ALOGV("%s: POWER_HINT_LOW_POWER", __func__);
            set_power_profile(samsung_pwr, data ? PROFILE_POWER_SAVE : PROFILE_BALANCED);
            break;
        case POWER_HINT_LAUNCH:
            ALOGV("%s: POWER_HINT_LAUNCH", __func__);
            send_boostpulse(samsung_pwr->boostpulse_fd);
            break;
        case POWER_HINT_CPU_BOOST:
            ALOGV("%s: POWER_HINT_CPU_BOOST", __func__);
            int32_t duration_us = *((int32_t *)data);
            send_boost(samsung_pwr->boost_fd, duration_us);
            break;
        case POWER_HINT_SET_PROFILE:
            ALOGV("%s: POWER_HINT_SET_PROFILE", __func__);
            int profile = *((intptr_t *)data);
            set_power_profile(samsung_pwr, profile);
            break;
        case POWER_HINT_DISABLE_TOUCH:
            ALOGV("%s: POWER_HINT_DISABLE_TOUCH", __func__);
            sysfs_write(samsung_pwr->touchscreen_power_path, data ? "0" : "1");
            break;
        default:
            ALOGW("%s: Unknown power hint: %d", __func__, hint);
            break;
    }
}

static int samsung_get_feature(struct power_module *module __unused,
                               feature_t feature)
{
    struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;
    if (feature == POWER_FEATURE_SUPPORTED_PROFILES) {
        return PROFILE_MAX;
    } else if (feature == POWER_FEATURE_DOUBLE_TAP_TO_WAKE &&
            samsung_pwr->dt2w_supported) {
        return samsung_pwr->dt2w_active;
    }

    return -1;
}

static void samsung_set_feature(struct power_module *module, feature_t feature, int state __unused)
{
    struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;

    switch (feature) {
        case POWER_FEATURE_DOUBLE_TAP_TO_WAKE:
#ifdef TARGET_TAP_TO_WAKE_NODE
            ALOGV("%s: %s double tap to wake", __func__, state ? "enabling" : "disabling");
            sysfs_write(TARGET_TAP_TO_WAKE_NODE, state > 0 ? "1" : "0");
            break;
#else
            if (samsung_pwr->dt2w_supported) {
                state = !!state;
                ALOGV("%s: %s double tap to wake", __func__, state ? "enabling" : "disabling");
                if (tsp_set_doubletap(state, 1920, 1080, 0, 0)) {
                    samsung_pwr->dt2w_active = state;
                }
            }
#endif
        default:
            break;
    }
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct samsung_power_module HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = POWER_MODULE_API_VERSION_0_2,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = POWER_HARDWARE_MODULE_ID,
            .name = "Samsung Power HAL",
            .author = "The LineageOS Project",
            .methods = &power_module_methods,
        },

        .init = samsung_power_init,
        .setInteractive = samsung_power_set_interactive,
        .powerHint = samsung_power_hint,
        .getFeature = samsung_get_feature,
        .setFeature = samsung_set_feature
    },

    .lock = PTHREAD_MUTEX_INITIALIZER,
    .boostpulse_fd = -1,
};
