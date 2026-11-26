/*
 * Samsung Galaxy A23 4G (SM-A235M) GSI Signal Strength Shim + rild Replacement
 *
 * Replaces /vendor/bin/hw/rild (via rw-system.sh bind mount onto the existing
 * vendor binary) while keeping vendor libril_sem.so and libsec-ril.so intact.
 *
 * Why a native rild replacement instead of LD_PRELOAD:
 *   1. libsec-ril.so invokes OnUnsolicitedResponse / OnRequestComplete through
 *      the struct RIL_Env* passed to RIL_Init(), NOT via PLT/GOT calls to
 *      libril_sem.so. Passing a wrapped RIL_Env cleanly intercepts both.
 *   2. /vendor is read-only; bind-mounting directly over the existing
 *      /vendor/bin/hw/rild with context u:object_r:rild_exec:s0 avoids
 *      non-existent *.real mount targets and shell_exec SELinux denials.
 *
 * Exact binary offsets verified on stock A23 Android 11 libril_sem.so:
 *   - RIL_CellInfo_v16 stride: 1128 bytes (0x468)
 *   - CellInfo SignalStrength offsets:
 *       GSM   (type 1): +0x428 (ss, ber, ta)
 *       WCDMA (type 4): +0x428 (ss, ber, rscp, ecno)
 *       LTE   (type 3): +0x42c (ss, rsrp, rsrq, rssnr, cqi, ta)
 *       NR    (type 6): +0x430 (ssRsrp, ssRsrq, ssSinr, csiRsrp, csiRsrq, csiSinr)
 *   - RIL_SignalStrength size: 132 bytes (33 x int32_t)
 *       [0..2]   gsm, [3..4] cdma, [5..7] evdo, [8..13] lte,
 *       [14..15] tdscdma, [16..19] wcdma, [20..25] nr, [26..32] reserved
 */

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#else
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#endif

#define LOG_TAG_SHIM "SignalShim"
#define LOG_TAG_RILD "RILD"

#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG_SHIM, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG_SHIM, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG_SHIM, fmt, ##__VA_ARGS__)

#define RIL_E_SUCCESS 0

#define RIL_REQUEST_SIGNAL_STRENGTH 19
#define RIL_REQUEST_GET_CELL_INFO_LIST 109

#define RIL_UNSOL_SIGNAL_STRENGTH 1009
#define RIL_UNSOL_CELL_INFO_LIST 1036

#define RIL_CELL_INFO_TYPE_GSM 1
#define RIL_CELL_INFO_TYPE_CDMA 2
#define RIL_CELL_INFO_TYPE_LTE 3
#define RIL_CELL_INFO_TYPE_WCDMA 4
#define RIL_CELL_INFO_TYPE_TDSCDMA 5
#define RIL_CELL_INFO_TYPE_NR 6

#define CELL_INFO_V16_STRIDE 1128u
#define CELL_OFF_TYPE 0x00u
#define CELL_OFF_REGISTERED 0x04u
#define CELL_OFF_GSM_SS 0x428u
#define CELL_OFF_WCDMA_SS 0x428u
#define CELL_OFF_LTE_SS 0x42cu
#define CELL_OFF_NR_SS 0x430u

#define RIL_SAP_SERVICE 1
#define RIL_SIGNAL_STRENGTH_SIZE 132u
#define MAX_SLOTS 2
#define PENDING_REQ_CAP 64
#define MIN_INJECT_INTERVAL_NS 800000000LL

typedef void *RIL_Token;
typedef void (*RIL_TimedCallback)(void *param);

struct RIL_Env {
    void (*OnRequestComplete)(RIL_Token t, int e, void *response, size_t responselen);
    void (*OnUnsolicitedResponse)(int unsolResponse, const void *data, size_t datalen, int socket_id);
    void (*RequestTimedCallback)(RIL_TimedCallback callback, void *param, const struct timeval *relativeTime);
    void (*OnRequestAck)(RIL_Token t);
};

/* Matches Samsung 48-byte RIL_RadioFunctions struct consumed by libril_sem.so. */
struct RIL_RadioFunctions {
    int version;
    int pad;
    void (*onRequest)(int request, void *data, size_t datalen, RIL_Token t, int socket_id);
    int (*onStateRequest)(int socket_id);
    int (*supports)(int request);
    void (*onCancel)(RIL_Token t);
    const char *(*getVersion)(void);
};

typedef int32_t RilSignalStrengthV14[33];

_Static_assert(sizeof(struct RIL_Env) == 32, "RIL_Env size must be 32 bytes");
_Static_assert(sizeof(struct RIL_RadioFunctions) == 48, "RIL_RadioFunctions size must be 48 bytes");
_Static_assert(sizeof(RilSignalStrengthV14) == 132, "RilSignalStrengthV14 size must be 132 bytes");

#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92
#endif

struct PendingRequest {
    bool in_use;
    RIL_Token token;
    int request;
    int socket_id;
};

static void (*g_real_on_req_complete)(RIL_Token t, int e, void *response, size_t responselen) = NULL;
static void (*g_real_on_unsol)(int unsolResponse, const void *data, size_t datalen, int socket_id) = NULL;
static void (*g_real_req_timed_cb)(RIL_TimedCallback callback, void *param, const struct timeval *relativeTime) = NULL;
static void (*g_real_on_req_ack)(RIL_Token t) = NULL;
static void (*g_real_on_request)(int request, void *data, size_t datalen, RIL_Token t, int socket_id) = NULL;

static int (*g_prop_get)(const char *key, char *value, const char *default_value) = NULL;
static int (*g_prop_set)(const char *key, const char *value) = NULL;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static RilSignalStrengthV14 g_cached_ss[MAX_SLOTS];
static bool g_has_cached_ss[MAX_SLOTS] = {false, false};
static int64_t g_last_inject_ns[MAX_SLOTS] = {0, 0};
static int32_t g_last_rat[MAX_SLOTS] = {0, 0};
static bool g_disabled = false;
static bool g_logged_native_unsol = false;
static struct PendingRequest g_pending_reqs[PENDING_REQ_CAP];

static int64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((int64_t)ts.tv_sec * 1000000000LL) + (int64_t)ts.tv_nsec;
}

static int slot_index(int socket_id) {
    return (socket_id >= 0 && socket_id < MAX_SLOTS) ? socket_id : 0;
}

static void read_prop(const char *key, char *value, const char *def_val) {
    if (g_prop_get) {
        g_prop_get(key, value, def_val);
        return;
    }
    int len = __system_property_get(key, value);
    if (len <= 0 && def_val) {
        snprintf(value, PROP_VALUE_MAX, "%s", def_val);
    }
}

static int32_t read_i32(const uint8_t *base, size_t offset) {
    int32_t val;
    memcpy(&val, base + offset, sizeof(val));
    return val;
}

static void init_empty_signal_strength(RilSignalStrengthV14 ss) {
    memset(ss, 0, sizeof(RilSignalStrengthV14));

    /* GSM */
    ss[0] = 99;
    ss[1] = 99;
    ss[2] = INT32_MAX;

    /* CDMA / EVDO */
    ss[3] = -1;
    ss[4] = -1;
    ss[5] = -1;
    ss[6] = -1;
    ss[7] = -1;

    /* LTE (convertRilSignalStrengthToHal_1_4 turns -1 -> INT32_MAX for rsrp/rsrq/cqi) */
    ss[8] = 99;
    ss[9] = -1;
    ss[10] = -1;
    ss[11] = INT32_MAX;
    ss[12] = -1;
    ss[13] = INT32_MAX;

    /* TD-SCDMA: libril_sem writes INT32_MAX to [56..63] and copies ss[15] to tdscdma.rscp */
    ss[14] = INT32_MAX;
    ss[15] = INT32_MAX;

    /* WCDMA: libril_sem reads [16..19] and maps 255 -> UNAVAILABLE via ConvertRscpToAsu/ConvertEcnoToAsu */
    ss[16] = 99;
    ss[17] = 99;
    ss[18] = 255;
    ss[19] = 255;

    /* NR: libril_sem negates ss[20], ss[21], ss[23], ss[24] with signed overflow -> abort()!
     * INT32_MIN + 1 negates safely to INT32_MAX without triggering abort(). */
    ss[20] = INT32_MIN + 1;
    ss[21] = INT32_MIN + 1;
    ss[22] = INT32_MAX;
    ss[23] = INT32_MIN + 1;
    ss[24] = INT32_MIN + 1;
    ss[25] = INT32_MAX;
}

static bool has_valid_signal_strength(const int32_t *ss, size_t len) {
    if (!ss || len < RIL_SIGNAL_STRENGTH_SIZE) {
        return false;
    }
    if (ss[0] >= 0 && ss[0] <= 31) {
        return true;
    }
    if (ss[8] >= 0 && ss[8] <= 31) {
        return true;
    }
    if ((ss[9] >= 44 && ss[9] <= 140) || (ss[9] <= -44 && ss[9] >= -140)) {
        return true;
    }
    if (ss[16] >= 0 && ss[16] <= 31) {
        return true;
    }
    if (ss[18] != 255 && ss[18] != INT32_MAX && ss[18] != -1) {
        return true;
    }
    if ((ss[20] >= 44 && ss[20] <= 140) || (ss[20] <= -44 && ss[20] >= -140)) {
        return true;
    }
    return false;
}

static bool build_signal_strength_from_cell_info(
    const void *data, size_t datalen, RilSignalStrengthV14 ss, int32_t *out_primary_rat) {
    if (!data || datalen == 0 || (datalen % CELL_INFO_V16_STRIDE) != 0) {
        return false;
    }

    init_empty_signal_strength(ss);

    size_t count = datalen / CELL_INFO_V16_STRIDE;
    const uint8_t *base = (const uint8_t *)data;
    bool has_lte = false;
    bool has_wcdma = false;
    bool has_gsm = false;
    bool has_nr = false;

    for (size_t i = 0; i < count; i++) {
        const uint8_t *cell = base + (i * CELL_INFO_V16_STRIDE);
        int32_t type = read_i32(cell, CELL_OFF_TYPE);
        int32_t registered = read_i32(cell, CELL_OFF_REGISTERED);
        if (registered == 0) {
            continue;
        }

        if (type == RIL_CELL_INFO_TYPE_LTE && !has_lte) {
            int32_t asu = read_i32(cell, CELL_OFF_LTE_SS + 0);
            int32_t rsrp = read_i32(cell, CELL_OFF_LTE_SS + 4);
            int32_t rsrq = read_i32(cell, CELL_OFF_LTE_SS + 8);
            int32_t rssnr = read_i32(cell, CELL_OFF_LTE_SS + 12);
            int32_t cqi = read_i32(cell, CELL_OFF_LTE_SS + 16);
            int32_t ta = read_i32(cell, CELL_OFF_LTE_SS + 20);

            int32_t rsrp_pos = -1;
            if (rsrp >= 44 && rsrp <= 140) {
                rsrp_pos = rsrp;
            } else if (rsrp <= -44 && rsrp >= -140) {
                rsrp_pos = -rsrp;
            }

            if (rsrp_pos > 0 || (asu >= 0 && asu <= 31)) {
                ss[8] = (asu >= 0 && asu <= 31) ? asu : 99;
                ss[9] = rsrp_pos;
                if (rsrq >= 1 && rsrq <= 35) {
                    ss[10] = rsrq;
                } else if (rsrq <= -1 && rsrq >= -35) {
                    ss[10] = -rsrq;
                }
                ss[11] = (rssnr >= -200 && rssnr <= 300) ? rssnr : INT32_MAX;
                ss[12] = (cqi >= 0 && cqi <= 15) ? cqi : -1;
                ss[13] = (ta >= 0 && ta <= 1282) ? ta : INT32_MAX;
                has_lte = true;
            }
        } else if (type == RIL_CELL_INFO_TYPE_WCDMA && !has_wcdma) {
            int32_t asu = read_i32(cell, CELL_OFF_WCDMA_SS + 0);
            int32_t ber = read_i32(cell, CELL_OFF_WCDMA_SS + 4);
            int32_t rscp = read_i32(cell, CELL_OFF_WCDMA_SS + 8);
            int32_t ecno = read_i32(cell, CELL_OFF_WCDMA_SS + 12);

            int32_t valid_rscp = 255;
            if (rscp >= -120 && rscp <= -25) {
                valid_rscp = rscp;
            } else if (rscp >= 25 && rscp <= 120) {
                valid_rscp = -rscp;
            } else if (asu >= 0 && asu <= 31) {
                /* Stock FillWcdmaCellInfo zeroes rscp/ecno; derive real RSCP from ASU
                 * instead of letting libril_sem ConvertRscpToAsu(0) emit -24 dBm. */
                valid_rscp = -113 + (2 * asu);
            }

            if ((asu >= 0 && asu <= 31) || valid_rscp != 255) {
                ss[16] = (asu >= 0 && asu <= 31) ? asu : 99;
                ss[17] = (ber >= 0 && ber <= 7) ? ber : 99;
                ss[18] = valid_rscp;
                /* ecno == 0 paired with raw rscp == 0 is memset-zero, not a real 0 dB value. */
                ss[19] = (rscp != 0 && ecno >= -24 && ecno < 0) ? ecno : 255;
                has_wcdma = true;
            }
        } else if (type == RIL_CELL_INFO_TYPE_GSM && !has_gsm) {
            int32_t asu = read_i32(cell, CELL_OFF_GSM_SS + 0);
            int32_t ber = read_i32(cell, CELL_OFF_GSM_SS + 4);
            int32_t ta = read_i32(cell, CELL_OFF_GSM_SS + 8);

            if (asu >= 0 && asu <= 31) {
                ss[0] = asu;
                ss[1] = (ber >= 0 && ber <= 7) ? ber : 99;
                ss[2] = (ta >= 0 && ta <= 219) ? ta : INT32_MAX;
                has_gsm = true;
            }
        } else if (type == RIL_CELL_INFO_TYPE_NR && !has_nr) {
            int32_t ss_rsrp = read_i32(cell, CELL_OFF_NR_SS + 0);
            int32_t ss_rsrq = read_i32(cell, CELL_OFF_NR_SS + 4);
            int32_t ss_sinr = read_i32(cell, CELL_OFF_NR_SS + 8);
            int32_t csi_rsrp = read_i32(cell, CELL_OFF_NR_SS + 12);
            int32_t csi_rsrq = read_i32(cell, CELL_OFF_NR_SS + 16);
            int32_t csi_sinr = read_i32(cell, CELL_OFF_NR_SS + 20);

            int32_t nr_neg_rsrp = INT32_MIN + 1;
            if (ss_rsrp >= 44 && ss_rsrp <= 140) {
                nr_neg_rsrp = -ss_rsrp;
            } else if (ss_rsrp <= -44 && ss_rsrp >= -140) {
                nr_neg_rsrp = ss_rsrp;
            }

            if (nr_neg_rsrp != (INT32_MIN + 1)) {
                ss[20] = nr_neg_rsrp;
                ss[21] = (ss_rsrq > INT32_MIN && ss_rsrq <= 40 && ss_rsrq >= -40)
                             ? (ss_rsrq > 0 ? -ss_rsrq : ss_rsrq)
                             : (INT32_MIN + 1);
                ss[22] = (ss_sinr >= -230 && ss_sinr <= 400) ? ss_sinr : INT32_MAX;
                ss[23] = (csi_rsrp >= 44 && csi_rsrp <= 140) ? -csi_rsrp
                         : (csi_rsrp <= -44 && csi_rsrp >= -140) ? csi_rsrp
                                                                 : (INT32_MIN + 1);
                ss[24] = (csi_rsrq > INT32_MIN && csi_rsrq <= 40 && csi_rsrq >= -40)
                             ? (csi_rsrq > 0 ? -csi_rsrq : csi_rsrq)
                             : (INT32_MIN + 1);
                ss[25] = (csi_sinr >= -230 && csi_sinr <= 400) ? csi_sinr : INT32_MAX;
                has_nr = true;
            }
        }
    }

    if (has_nr) {
        *out_primary_rat = RIL_CELL_INFO_TYPE_NR;
    } else if (has_lte) {
        *out_primary_rat = RIL_CELL_INFO_TYPE_LTE;
    } else if (has_wcdma) {
        *out_primary_rat = RIL_CELL_INFO_TYPE_WCDMA;
    } else if (has_gsm) {
        *out_primary_rat = RIL_CELL_INFO_TYPE_GSM;
    } else {
        *out_primary_rat = 0;
        return false;
    }

    return true;
}

static void record_pending_request(RIL_Token t, int request, int socket_id) {
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < PENDING_REQ_CAP; i++) {
        if (!g_pending_reqs[i].in_use) {
            g_pending_reqs[i].in_use = true;
            g_pending_reqs[i].token = t;
            g_pending_reqs[i].request = request;
            g_pending_reqs[i].socket_id = socket_id;
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    /* Overwrite slot 0 only if saturated. */
    g_pending_reqs[0].in_use = true;
    g_pending_reqs[0].token = t;
    g_pending_reqs[0].request = request;
    g_pending_reqs[0].socket_id = socket_id;
    pthread_mutex_unlock(&g_lock);
}

static bool pop_pending_request(RIL_Token t, int *out_request, int *out_socket_id) {
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < PENDING_REQ_CAP; i++) {
        if (g_pending_reqs[i].in_use && g_pending_reqs[i].token == t) {
            *out_request = g_pending_reqs[i].request;
            *out_socket_id = g_pending_reqs[i].socket_id;
            g_pending_reqs[i].in_use = false;
            pthread_mutex_unlock(&g_lock);
            return true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return false;
}

static void maybe_inject_signal_from_cell_info(
    int socket_id, const void *data, size_t datalen, const char *source) {
    RilSignalStrengthV14 ss;
    int32_t rat = 0;
    if (!build_signal_strength_from_cell_info(data, datalen, ss, &rat)) {
        return;
    }

    int slot = slot_index(socket_id);
    int64_t now = monotonic_ns();
    bool should_inject = false;
    bool first_or_rat_change = false;

    pthread_mutex_lock(&g_lock);
    memcpy(g_cached_ss[slot], ss, sizeof(ss));
    if (!g_has_cached_ss[slot] || g_last_rat[slot] != rat) {
        should_inject = true;
        first_or_rat_change = true;
    } else if (now == 0 || (now - g_last_inject_ns[slot]) >= MIN_INJECT_INTERVAL_NS) {
        should_inject = true;
    }
    g_has_cached_ss[slot] = true;
    if (should_inject) {
        g_last_inject_ns[slot] = now;
        g_last_rat[slot] = rat;
    }
    pthread_mutex_unlock(&g_lock);

    if (should_inject && g_real_on_unsol) {
        if (first_or_rat_change) {
            LOGI("injecting signal slot=%d src=%s rat=%d lte_ss=%d lte_rsrp=%d wcdma_ss=%d wcdma_rscp=%d gsm_ss=%d",
                 socket_id, source, rat, ss[8], ss[9], ss[16], ss[18], ss[0]);
        }
        g_real_on_unsol(RIL_UNSOL_SIGNAL_STRENGTH, ss, sizeof(ss), socket_id);
    }
}

static void wrapped_on_request(int request, void *data, size_t datalen, RIL_Token t, int socket_id) {
    if (!g_disabled && (request == RIL_REQUEST_SIGNAL_STRENGTH || request == RIL_REQUEST_GET_CELL_INFO_LIST)) {
        record_pending_request(t, request, socket_id);
    }
    if (g_real_on_request) {
        g_real_on_request(request, data, datalen, t, socket_id);
    }
}

static void wrapped_on_request_complete(RIL_Token t, int e, void *response, size_t responselen) {
    int req = 0;
    int socket_id = 0;
    if (!g_disabled && pop_pending_request(t, &req, &socket_id)) {
        int slot = slot_index(socket_id);
        if (req == RIL_REQUEST_GET_CELL_INFO_LIST && e == RIL_E_SUCCESS && response && responselen > 0) {
            maybe_inject_signal_from_cell_info(socket_id, response, responselen, "GET_CELL_INFO");
        } else if (req == RIL_REQUEST_SIGNAL_STRENGTH) {
            if (e == RIL_E_SUCCESS && has_valid_signal_strength((const int32_t *)response, responselen)) {
                pthread_mutex_lock(&g_lock);
                memcpy(g_cached_ss[slot], response, sizeof(RilSignalStrengthV14));
                g_has_cached_ss[slot] = true;
                pthread_mutex_unlock(&g_lock);
            } else {
                RilSignalStrengthV14 cached;
                bool have_cached = false;
                pthread_mutex_lock(&g_lock);
                if (g_has_cached_ss[slot]) {
                    memcpy(cached, g_cached_ss[slot], sizeof(cached));
                    have_cached = true;
                }
                pthread_mutex_unlock(&g_lock);
                if (have_cached && g_real_on_req_complete) {
                    g_real_on_req_complete(t, RIL_E_SUCCESS, cached, sizeof(cached));
                    return;
                }
            }
        }
    }

    if (g_real_on_req_complete) {
        g_real_on_req_complete(t, e, response, responselen);
    }
}

static void wrapped_on_unsolicited_response(int unsol_response, const void *data, size_t datalen, int socket_id) {
    if (g_real_on_unsol) {
        g_real_on_unsol(unsol_response, data, datalen, socket_id);
    }

    if (g_disabled) {
        return;
    }

    if (unsol_response == RIL_UNSOL_SIGNAL_STRENGTH) {
        if (has_valid_signal_strength((const int32_t *)data, datalen)) {
            int slot = slot_index(socket_id);
            pthread_mutex_lock(&g_lock);
            memcpy(g_cached_ss[slot], data, sizeof(RilSignalStrengthV14));
            g_has_cached_ss[slot] = true;
            g_last_inject_ns[slot] = monotonic_ns();
            pthread_mutex_unlock(&g_lock);
            if (!g_logged_native_unsol) {
                g_logged_native_unsol = true;
                LOGI("native RIL_UNSOL_SIGNAL_STRENGTH received on slot=%d (len=%zu)", socket_id, datalen);
            }
        }
        return;
    }

    if (unsol_response == RIL_UNSOL_CELL_INFO_LIST) {
        maybe_inject_signal_from_cell_info(socket_id, data, datalen, "UNSOL_CELL_INFO");
    }
}

static void wrapped_request_timed_callback(
    RIL_TimedCallback callback, void *param, const struct timeval *relative_time) {
    if (g_real_req_timed_cb) {
        g_real_req_timed_cb(callback, param, relative_time);
    }
}

static void wrapped_on_request_ack(RIL_Token t) {
    if (g_real_on_req_ack) {
        g_real_on_req_ack(t);
    }
}

static void *must_dlsym(void *handle, const char *sym, const char *lib_name) {
    dlerror();
    void *ptr = dlsym(handle, sym);
    if (!ptr) {
        LOGE("dlsym(%s, %s) failed: %s", lib_name, sym, dlerror());
        exit(EXIT_FAILURE);
    }
    return ptr;
}

int main(int argc, char **argv) {
    const char *ril_lib_path = NULL;
    const char *client_id = "0";
    const char *sim_slot_str = NULL;
    char slot_buf[PROP_VALUE_MAX] = "1";
    char disable_buf[PROP_VALUE_MAX] = "0";

    umask(0077);

    void *lib_cutils = dlopen("libcutils.so", RTLD_NOW);
    if (lib_cutils) {
        g_prop_get = (int (*)(const char *, char *, const char *))dlsym(lib_cutils, "property_get");
        g_prop_set = (int (*)(const char *, const char *))dlsym(lib_cutils, "property_set");
    }

    read_prop("persist.sys.phh.signal_shim.disable", disable_buf, "0");
    g_disabled = (strcmp(disable_buf, "1") == 0 || strcmp(disable_buf, "true") == 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 && (i + 1) < argc) {
            ril_lib_path = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && (i + 1) < argc) {
            sim_slot_str = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && (i + 1) < argc) {
            client_id = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            break;
        }
    }

    if (!sim_slot_str) {
        read_prop("ro.vendor.multisim.simslotcount", slot_buf, "1");
        sim_slot_str = slot_buf;
    }

    int sim_slot_count = atoi(sim_slot_str);
    if (strncmp(client_id, "0", 2) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG_RILD,
                            "Max Number of rild's supported is: 0 (got -c %s)", client_id);
        return 0;
    }

    if (g_prop_set && sim_slot_count >= 1) {
        for (int s = 1; s <= sim_slot_count; s++) {
            char key[64];
            snprintf(key, sizeof(key), "ril.phone.connected.%d", s);
            g_prop_set(key, "false");
            snprintf(key, sizeof(key), "ril.halservice.registered.%d", s);
            g_prop_set(key, "false");
        }
    }

    void *lib_sem = dlopen("libril_sem.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib_sem) {
        lib_sem = dlopen("/vendor/lib64/libril_sem.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!lib_sem) {
        LOGE("dlopen(libril_sem.so) failed: %s", dlerror());
        return EXIT_FAILURE;
    }

    void (*ril_start_event_loop)(void) =
        (void (*)(void))must_dlsym(lib_sem, "RIL_startEventLoop", "libril_sem.so");
    void (*rilc_configure_thread_pool)(void) =
        (void (*)(void))must_dlsym(lib_sem, "rilc_configure_thread_pool", "libril_sem.so");
    void (*ril_register)(const struct RIL_RadioFunctions *) =
        (void (*)(const struct RIL_RadioFunctions *))must_dlsym(lib_sem, "RIL_register", "libril_sem.so");
    void (*ril_register_socket)(
        const struct RIL_RadioFunctions *(*)(const struct RIL_Env *, int, char **),
        int, int, char **) =
        (void (*)(const struct RIL_RadioFunctions *(*)(const struct RIL_Env *, int, char **), int, int, char **))
            dlsym(lib_sem, "RIL_register_socket");
    void (*rilc_thread_pool)(void) =
        (void (*)(void))must_dlsym(lib_sem, "rilc_thread_pool", "libril_sem.so");

    g_real_on_req_complete =
        (void (*)(RIL_Token, int, void *, size_t))must_dlsym(lib_sem, "RIL_onRequestComplete", "libril_sem.so");
    g_real_on_unsol =
        (void (*)(int, const void *, size_t, int))must_dlsym(lib_sem, "RIL_onUnsolicitedResponse", "libril_sem.so");
    g_real_req_timed_cb = (void (*)(RIL_TimedCallback, void *, const struct timeval *))
        must_dlsym(lib_sem, "RIL_requestTimedCallback", "libril_sem.so");
    g_real_on_req_ack =
        (void (*)(RIL_Token))must_dlsym(lib_sem, "RIL_onRequestAck", "libril_sem.so");

    if (!ril_lib_path || !ril_lib_path[0]) {
        ril_lib_path = "libsec-ril.so";
    }
    void *dl_handle = dlopen(ril_lib_path, RTLD_NOW);
    if (!dl_handle && strcmp(ril_lib_path, "libsec-ril.so") != 0) {
        dl_handle = dlopen("libsec-ril.so", RTLD_NOW);
    }
    if (!dl_handle) {
        LOGE("dlopen(%s) failed: %s", ril_lib_path, dlerror());
        return EXIT_FAILURE;
    }

    ril_start_event_loop();

    const struct RIL_RadioFunctions *(*ril_init)(const struct RIL_Env *, int, char **) =
        (const struct RIL_RadioFunctions *(*)(const struct RIL_Env *, int, char **))
            must_dlsym(dl_handle, "RIL_Init", ril_lib_path);

    const struct RIL_RadioFunctions *(*ril_sap_init)(const struct RIL_Env *, int, char **) =
        (const struct RIL_RadioFunctions *(*)(const struct RIL_Env *, int, char **))
            dlsym(dl_handle, "RIL_SAP_Init");

    char *ril_argv[6] = {
        argv[0],
        (char *)"-s",
        (char *)sim_slot_str,
        (char *)"-c",
        (char *)client_id,
        NULL,
    };
    int ril_argc = 5;

    rilc_configure_thread_pool();

    static struct RIL_Env wrapped_env;
    wrapped_env.OnRequestComplete = wrapped_on_request_complete;
    wrapped_env.OnUnsolicitedResponse = wrapped_on_unsolicited_response;
    wrapped_env.RequestTimedCallback = wrapped_request_timed_callback;
    wrapped_env.OnRequestAck = wrapped_on_request_ack;

    const struct RIL_RadioFunctions *real_funcs = ril_init(&wrapped_env, ril_argc, ril_argv);
    if (!real_funcs) {
        LOGE("RIL_Init returned NULL");
        return EXIT_FAILURE;
    }

    static struct RIL_RadioFunctions wrapped_funcs;
    wrapped_funcs = *real_funcs;
    g_real_on_request = real_funcs->onRequest;
    wrapped_funcs.onRequest = wrapped_on_request;

    LOGI("rild signal shim active (disabled=%d sim_slots=%d version=%d)",
         g_disabled ? 1 : 0, sim_slot_count, real_funcs->version);

    ril_register(&wrapped_funcs);

    if (ril_register_socket && ril_sap_init) {
        ril_register_socket(ril_sap_init, RIL_SAP_SERVICE, ril_argc, ril_argv);
    }

    rilc_thread_pool();

    for (;;) {
        sleep(0xffffffffu);
    }
    return 0;
}
