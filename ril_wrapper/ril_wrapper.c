#define RIL_SHLIB 1
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <telephony/ril.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <log/log.h>
#include <stdarg.h>

#undef LOG_TAG
#define LOG_TAG "RIL_WRAPPER"

#undef ALOGI
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static const struct RIL_Env *g_original_env = NULL;
static const RIL_RadioFunctions *g_original_funcs = NULL;

static int g_faked_radio_off = 0;
static RIL_Token g_sim_status_token = NULL;
static RIL_Token g_radio_power_on_token = NULL;

/*
 * The registration, operator and IMSI fixes below were written for one Maroc
 * Telecom SIM. Without a SIM they turn "searching" into a fake Moroccan home
 * network (Android then sets the country, time zone and carrier label from it),
 * so they only apply while the real RIL reports a card present.
 */
static int g_sim_present = 0;

/* Diagnostic token tracking */
#define FALLBACK_IMSI "604016336070271"

static RIL_Token g_voice_reg_token = NULL;
static RIL_Token g_data_reg_token = NULL;
static RIL_Token g_operator_token = NULL;
static RIL_Token g_sim_io_token = NULL;
static int g_sim_io_fileid = 0;
static int g_sim_io_command = 0;

/* Sentinel token for injected requests */
static int g_sentinel_token_storage = 0;
#define SENTINEL_TOKEN ((RIL_Token)&g_sentinel_token_storage)


static void wrapped_OnRequestComplete(RIL_Token t, RIL_Errno e,
                                       void *response, size_t responselen) {
    /*
     * Intercept completions for our injected requests.
     */
    if (t == SENTINEL_TOKEN) {
        ALOGI("[INJECT] Intercepted completion for injected request, e=%d - dropping", e);
        return;
    }

    /* ===== GET_SIM_STATUS response handling & override ===== */
    if (t == g_sim_status_token && g_sim_status_token != NULL) {
        g_sim_status_token = NULL;
        if (e == RIL_E_SUCCESS && response != NULL && responselen >= sizeof(RIL_CardStatus_v6)) {
            RIL_CardStatus_v6 *cs = (RIL_CardStatus_v6 *)response;
            g_sim_present = cs->card_state == RIL_CARDSTATE_PRESENT;
            ALOGI("  -> [RESP] GET_SIM_STATUS: card_state=%d, num_apps=%d, gsm_sub_idx=%d",
                  cs->card_state, cs->num_applications, cs->gsm_umts_subscription_app_index);

            if (cs->card_state == RIL_CARDSTATE_PRESENT && cs->num_applications > 0) {
                for (int i = 0; i < cs->num_applications; i++) {
                    ALOGI("  -> [RESP] App[%d]: type=%d, state=%d, perso=%d, aid=%s",
                          i, cs->applications[i].app_type, cs->applications[i].app_state,
                          cs->applications[i].perso_substate,
                          cs->applications[i].aid_ptr ? cs->applications[i].aid_ptr : "(null)");

                    // If app is DETECTED (1) or SUBSCRIPTION_PERSO (4), promote to READY (5)
                    if (cs->applications[i].app_state == RIL_APPSTATE_DETECTED ||
                        cs->applications[i].app_state == RIL_APPSTATE_SUBSCRIPTION_PERSO) {
                        ALOGI("  -> [OVERRIDE] Forcing App[%d] app_state from %d to RIL_APPSTATE_READY (5)",
                              i, cs->applications[i].app_state);
                        cs->applications[i].app_state = RIL_APPSTATE_READY;
                    }
                }

                // Ensure gsm_umts_subscription_app_index is 0
                if (cs->gsm_umts_subscription_app_index < 0) {
                    ALOGI("  -> [OVERRIDE] Forcing gsm_umts_subscription_app_index from %d to 0",
                          cs->gsm_umts_subscription_app_index);
                    cs->gsm_umts_subscription_app_index = 0;
                }
            }
        }
    }

    /* ===== VOICE_REGISTRATION_STATE response logging & fix ===== */
    if (t == g_voice_reg_token && g_voice_reg_token != NULL) {
        g_voice_reg_token = NULL;
        ALOGI("  -> [RESP] VOICE_REGISTRATION_STATE: e=%d, responselen=%zu", e, responselen);
        if (e == RIL_E_SUCCESS && response != NULL) {
            char **strings = (char **)response;
            int count = responselen / sizeof(char *);
            ALOGI("  -> [RESP] VOICE_REG: count=%d", count);
            for (int i = 0; i < count && i < 15; i++) {
                ALOGI("  -> [RESP] VOICE_REG[%d]=%s", i, strings[i] ? strings[i] : "(null)");
            }
            if (g_sim_present && count >= 1 && strings[0]) {
                int reg_state = atoi(strings[0]);
                if (reg_state == 12 || reg_state == 2) {
                    ALOGI("  -> [OVERRIDE] Promoting VOICE_REG[0] from %d to 1 (IN_SERVICE, Home)", reg_state);
                    strings[0] = "1";
                }
            }
        }
    }

    /* ===== DATA_REGISTRATION_STATE response logging & fix ===== */
    if (t == g_data_reg_token && g_data_reg_token != NULL) {
        g_data_reg_token = NULL;
        ALOGI("  -> [RESP] DATA_REGISTRATION_STATE: e=%d, responselen=%zu", e, responselen);
        if (e == RIL_E_SUCCESS && response != NULL) {
            char **strings = (char **)response;
            int count = responselen / sizeof(char *);
            ALOGI("  -> [RESP] DATA_REG: count=%d", count);
            for (int i = 0; i < count && i < 11; i++) {
                ALOGI("  -> [RESP] DATA_REG[%d]=%s", i, strings[i] ? strings[i] : "(null)");
            }
            if (g_sim_present && count >= 1 && strings[0]) {
                int reg_state = atoi(strings[0]);
                if (reg_state == 2 || reg_state == 12) {
                    ALOGI("  -> [OVERRIDE] Promoting DATA_REG[0] from %d to 1 (IN_SERVICE, Home)", reg_state);
                    strings[0] = "1";
                    if (count >= 4 && (!strings[3] || atoi(strings[3]) == 0)) {
                        strings[3] = "14"; // 14 = LTE
                    }
                }
            }
        }
    }

    /* ===== OPERATOR response logging & enrichment ===== */
    if (t == g_operator_token && g_operator_token != NULL) {
        g_operator_token = NULL;
        ALOGI("  -> [RESP] OPERATOR: e=%d, responselen=%zu", e, responselen);
        if (e == RIL_E_SUCCESS && response != NULL) {
            char **strings = (char **)response;
            int count = responselen / sizeof(char *);
            for (int i = 0; i < count && i < 3; i++) {
                ALOGI("  -> [RESP] OPERATOR[%d]=%s", i, strings[i] ? strings[i] : "(null)");
            }
            if (g_sim_present && count >= 3) {
                if (!strings[0] || strlen(strings[0]) == 0) strings[0] = "Maroc Telecom";
                if (!strings[1] || strlen(strings[1]) == 0) strings[1] = "IAM";
                if (!strings[2] || strlen(strings[2]) == 0) strings[2] = "60401";
                ALOGI("  -> [OVERRIDE] Enriched OPERATOR: %s, %s, %s", strings[0], strings[1], strings[2]);
            }
        }
    }

    /* ===== SIM_IO response logging ===== */
    if (t == g_sim_io_token && g_sim_io_token != NULL) {
        g_sim_io_token = NULL;
        if (e == RIL_E_SUCCESS && response != NULL && responselen >= sizeof(RIL_SIM_IO_Response)) {
            RIL_SIM_IO_Response *sim_resp = (RIL_SIM_IO_Response *)response;
            ALOGI("  -> [RESP] SIM_IO: cmd=%d fileid=0x%04X sw1=0x%02X sw2=0x%02X data=%s",
                  g_sim_io_command, g_sim_io_fileid,
                  sim_resp->sw1, sim_resp->sw2,
                  sim_resp->simResponse ? sim_resp->simResponse : "(null)");
        } else {
            ALOGI("  -> [RESP] SIM_IO: cmd=%d fileid=0x%04X e=%d (FAILED)", g_sim_io_command, g_sim_io_fileid, e);
        }
    }

    g_original_env->OnRequestComplete(t, e, response, responselen);

    if (t == g_radio_power_on_token && g_radio_power_on_token != NULL) {
        g_radio_power_on_token = NULL;
        if (e == RIL_E_SUCCESS) {
            ALOGI("  -> [INJECT] RADIO_POWER(1) complete. Injecting UNSOL_RESPONSE_RADIO_STATE_CHANGED (ON)");
            int state = 1; // RIL_RADIO_STATE_ON (1)
            g_original_env->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, &state, sizeof(state));
        }
    }
}

static void wrapped_OnUnsolicitedResponse(int unsolResponse,
                                           const void *data, size_t datalen) {
    ALOGI("  -> [UNSOL] ID: %d", unsolResponse);

    if (unsolResponse == RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED) {
        ALOGI("  -> [UNSOL] VOICE_NETWORK_STATE_CHANGED");
    }

    if (unsolResponse == RIL_UNSOL_RESTRICTED_STATE_CHANGED) {
        if (data && datalen >= sizeof(int)) {
            ALOGI("  -> [UNSOL] RESTRICTED_STATE_CHANGED: state=%d", ((int*)data)[0]);
        }
    }
    if (unsolResponse == RIL_UNSOL_NITZ_TIME_RECEIVED) {
        if (data) {
            ALOGI("  -> [UNSOL] NITZ_TIME: %s", (char*)data);
        }
    }

    if (unsolResponse == RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED) {
        if (g_faked_radio_off) {
            ALOGI("  -> Suppressing UNSOL_RESPONSE_RADIO_STATE_CHANGED while faking OFF");
            return;
        }
        ALOGI("  -> UNSOL_RESPONSE_RADIO_STATE_CHANGED forwarded");
    }

    if (unsolResponse == RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED) {
        ALOGI("  -> UNSOL_RESPONSE_SIM_STATUS_CHANGED received");
    }

    g_original_env->OnUnsolicitedResponse(unsolResponse, data, datalen);
}

static void wrapped_RequestTimedCallback(RIL_TimedCallback callback,
                                          void *param,
                                          const struct timeval *relativeTime) {
    g_original_env->RequestTimedCallback(callback, param, relativeTime);
}

static void wrapped_onRequestComplete_for_RIL_Env(RIL_Token t, RIL_Errno e,
                                                    void *response,
                                                    size_t responselen) {
    wrapped_OnRequestComplete(t, e, response, responselen);
}

static void wrapped_onRequest(int request, void *data, size_t datalen, RIL_Token t) {
    ALOGI("  -> [REQ] ID: %d", request);
    if (request == RIL_REQUEST_GET_SIM_STATUS) {
        g_sim_status_token = t;
        ALOGI("  -> GET_SIM_STATUS sent to real RIL");
    }

    if (request == RIL_REQUEST_GET_IMSI && g_sim_present) {
        ALOGI("  -> [REQ] RIL_REQUEST_GET_IMSI intercepted. Answering directly with IMSI %s", FALLBACK_IMSI);
        char *fallback_imsi = FALLBACK_IMSI;
        g_original_env->OnRequestComplete(t, RIL_E_SUCCESS, fallback_imsi, sizeof(char *));
        return;
    }

    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        g_voice_reg_token = t;
        ALOGI("  -> VOICE_REGISTRATION_STATE sent to real RIL");
    }
    if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        g_data_reg_token = t;
        ALOGI("  -> DATA_REGISTRATION_STATE sent to real RIL");
    }
    if (request == RIL_REQUEST_OPERATOR) {
        g_operator_token = t;
        ALOGI("  -> OPERATOR sent to real RIL");
    }
    if (request == RIL_REQUEST_SIM_IO) {
        g_sim_io_token = t;
        if (data && datalen >= sizeof(RIL_SIM_IO_v6)) {
            RIL_SIM_IO_v6 *sim_io = (RIL_SIM_IO_v6 *)data;
            g_sim_io_command = sim_io->command;
            g_sim_io_fileid = sim_io->fileid;
            ALOGI("  -> SIM_IO: cmd=%d fileid=0x%04X p1=%d p2=%d p3=%d aid=%s",
                  sim_io->command, sim_io->fileid,
                  sim_io->p1, sim_io->p2, sim_io->p3,
                  sim_io->aidPtr ? sim_io->aidPtr : "(null)");
        }
    }
    if (request == RIL_REQUEST_QUERY_AVAILABLE_NETWORKS) {
        ALOGI("  -> QUERY_AVAILABLE_NETWORKS sent to real RIL");
    }
    if (request == RIL_REQUEST_ALLOW_DATA) {
        if (data && datalen >= sizeof(int)) {
            ALOGI("  -> ALLOW_DATA: allow=%d", ((int*)data)[0]);
        }
    }
    if (request == RIL_REQUEST_SET_INITIAL_ATTACH_APN) {
        ALOGI("  -> SET_INITIAL_ATTACH_APN sent to real RIL");
    }

    if (request == RIL_REQUEST_RADIO_POWER) {
        int power_state = ((int *)data)[0];
        ALOGI("  -> RADIO_POWER (%d) sent to real RIL", power_state);

        if (power_state == 1) {
            g_radio_power_on_token = t;
        }
    }

    if (request == RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE) {
        if (datalen >= sizeof(int)) {
            int pref = ((int *)data)[0];
            ALOGI("  -> SET_PREFERRED_NETWORK_TYPE: pref=%d", pref);
        }
    }

    g_original_funcs->onRequest(request, data, datalen, t);
}

static RIL_RadioState wrapped_onStateRequest() {
    RIL_RadioState real_state = g_original_funcs->onStateRequest();
    ALOGI("wrapped_onStateRequest: real_state=%d", (int)real_state);
    return real_state;
}

static int wrapped_onSupports(int requestCode) {
    return g_original_funcs->supports(requestCode);
}

static void wrapped_onCancel(RIL_Token t) {
    g_original_funcs->onCancel(t);
}

#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>

static void patch_ret(void *func_ptr, const char *name) {
    if (!func_ptr) {
        ALOGW("[HOTPATCH] %s is NULL, skipping", name);
        return;
    }
    uintptr_t addr = (uintptr_t)func_ptr;
    uintptr_t page_start = addr & ~((uintptr_t)4095);
    size_t page_size = 4096 * 2;

    if (mprotect((void *)page_start, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        ALOGE("[HOTPATCH] mprotect failed for %s at %p: %s", name, func_ptr, strerror(errno));
        return;
    }

    volatile uint32_t *code = (volatile uint32_t *)addr;
    *code = 0xd65f03c0; // ARM64 'ret'

    __builtin___clear_cache((char *)addr, (char *)(addr + 8));

    mprotect((void *)page_start, page_size, PROT_READ | PROT_EXEC);
    ALOGI("[HOTPATCH] Patched %s at %p with RET (0xd65f03c0) - COEX disabled!", name, func_ptr);
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env,
                                    int argc, char **argv) {
    ALOGI("ril_wrapper RIL_Init called");

    g_original_env = env;

    struct RIL_Env *custom_env = malloc(sizeof(struct RIL_Env));
    memset(custom_env, 0, sizeof(struct RIL_Env));
    custom_env->OnRequestComplete = wrapped_onRequestComplete_for_RIL_Env;
    custom_env->OnUnsolicitedResponse = wrapped_OnUnsolicitedResponse;
    custom_env->RequestTimedCallback = wrapped_RequestTimedCallback;

    void *real_ril = dlopen("/vendor/lib64/libril-qc-qmi-1.so", RTLD_NOW);
    if (!real_ril) {
        ALOGE("Failed to open real RIL: %s", dlerror());
        return NULL;
    }

    /* Hotpatch all COEX functions in libril-qc-qmi-1.so to prevent modem coex_qmb.c assertion crashes */
    patch_ret(dlsym(real_ril, "qcril_qmi_coex_init"), "qcril_qmi_coex_init");
    patch_ret(dlsym(real_ril, "qcril_qmi_coex_process_rf_band_info"), "qcril_qmi_coex_process_rf_band_info");
    patch_ret(dlsym(real_ril, "qcril_qmi_coex_initiate_report_lte_info_to_riva"), "qcril_qmi_coex_initiate_report_lte_info_to_riva");
    patch_ret(dlsym(real_ril, "qcril_qmi_coex_release"), "qcril_qmi_coex_release");
    patch_ret(dlsym(real_ril, "qcril_qmi_coex_terminate_riva_thread"), "qcril_qmi_coex_terminate_riva_thread");

    const RIL_RadioFunctions *(*real_ril_init)(const struct RIL_Env *, int, char **)
        = dlsym(real_ril, "RIL_Init");
    if (!real_ril_init) {
        ALOGE("Failed to find RIL_Init in real RIL: %s", dlerror());
        return NULL;
    }

    g_original_funcs = real_ril_init(custom_env, argc, argv);

    if (g_original_funcs) {
        static RIL_RadioFunctions s_wrapped_funcs;
        memcpy(&s_wrapped_funcs, g_original_funcs, sizeof(RIL_RadioFunctions));
        s_wrapped_funcs.onRequest = wrapped_onRequest;
        s_wrapped_funcs.onStateRequest = wrapped_onStateRequest;
        s_wrapped_funcs.supports = wrapped_onSupports;
        s_wrapped_funcs.onCancel = wrapped_onCancel;
        return &s_wrapped_funcs;
    }

    return NULL;
}
