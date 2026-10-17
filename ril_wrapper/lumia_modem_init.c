/*
 * Copyright (C) 2026 The LineageOS Project
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <errno.h>
#include <dlfcn.h>
#include <time.h>
#include <log/log.h>

#undef LOG_TAG
#define LOG_TAG "LumiaModemInit"

#undef ALOGI
#define ALOGI(...) do { printf(__VA_ARGS__); printf("\n"); __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); } while(0)
#undef ALOGE
#define ALOGE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); } while(0)
#undef ALOGW
#define ALOGW(...) do { printf(__VA_ARGS__); printf("\n"); __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__); } while(0)

/* Qualcomm QCCI Types */
typedef void *qmi_client_type;
typedef void *qmi_idl_service_object_type;
typedef int qmi_service_instance;
typedef void (*qmi_client_ind_cb)(qmi_client_type user_handle,
                                   unsigned int msg_id,
                                   void *ind_buf,
                                   unsigned int ind_buf_len,
                                   void *ind_cb_data);
typedef int qmi_client_error_type;

typedef struct {
    char data[32];
} qmi_service_info;

#define QMI_NO_ERR 0
#define QMI_CLIENT_INSTANCE_ANY 0xFFFF

/* Function pointers loaded dynamically from libqmi_cci.so and libqmiservices.so */
static qmi_idl_service_object_type (*p_dpm_get_service_object)(int, int, int) = NULL;
static qmi_idl_service_object_type (*p_wda_get_service_object)(int, int, int) = NULL;
static qmi_idl_service_object_type (*p_uim_get_service_object)(int, int, int) = NULL;
static qmi_idl_service_object_type (*p_nas_get_service_object)(int, int, int) = NULL;
static qmi_idl_service_object_type (*p_dms_get_service_object)(int, int, int) = NULL;

static qmi_client_error_type (*p_qmi_client_init_instance)(
    qmi_idl_service_object_type service_obj,
    qmi_service_instance instance_id,
    qmi_client_ind_cb ind_cb,
    void *ind_cb_data,
    void *os_params,
    uint32_t timeout_msecs,
    qmi_client_type *user_handle
) = NULL;

static qmi_client_error_type (*p_qmi_client_get_service_list)(
    qmi_idl_service_object_type service_obj,
    qmi_service_info *service_info_array,
    unsigned int *num_entries,
    unsigned int *num_services
) = NULL;

static qmi_client_error_type (*p_qmi_client_init)(
    qmi_service_info *service_info,
    qmi_idl_service_object_type service_obj,
    qmi_client_ind_cb ind_cb,
    void *ind_cb_data,
    void *os_params,
    qmi_client_type *user_handle
) = NULL;

static qmi_client_error_type (*p_qmi_client_send_raw_msg_sync)(
    qmi_client_type user_handle,
    uint32_t msg_id,
    void *req_buf,
    uint32_t req_buf_len,
    void *resp_buf,
    uint32_t resp_buf_len,
    uint32_t *resp_len,
    uint32_t timeout_msecs
) = NULL;

static qmi_client_error_type (*p_qmi_client_release)(
    qmi_client_type user_handle
) = NULL;

static int load_qmi_symbols(void) {
    void *lib_services = dlopen("libqmiservices.so", RTLD_NOW);
    if (!lib_services) {
        lib_services = dlopen("/vendor/lib64/libqmiservices.so", RTLD_NOW);
    }
    if (!lib_services) {
        ALOGE("Failed to open libqmiservices.so: %s", dlerror());
        return -1;
    }

    void *lib_cci = dlopen("libqmi_cci.so", RTLD_NOW);
    if (!lib_cci) {
        lib_cci = dlopen("/vendor/lib64/libqmi_cci.so", RTLD_NOW);
    }
    if (!lib_cci) {
        ALOGE("Failed to open libqmi_cci.so: %s", dlerror());
        dlclose(lib_services);
        return -1;
    }

    p_dpm_get_service_object = dlsym(lib_services, "dpm_get_service_object_internal_v01");
    p_wda_get_service_object = dlsym(lib_services, "wda_get_service_object_internal_v01");
    p_uim_get_service_object = dlsym(lib_services, "uim_get_service_object_internal_v01");
    p_nas_get_service_object = dlsym(lib_services, "nas_get_service_object_internal_v01");
    p_dms_get_service_object = dlsym(lib_services, "dms_get_service_object_internal_v01");

    p_qmi_client_init_instance = dlsym(lib_cci, "qmi_client_init_instance");
    p_qmi_client_get_service_list = dlsym(lib_cci, "qmi_client_get_service_list");
    p_qmi_client_init = dlsym(lib_cci, "qmi_client_init");
    p_qmi_client_send_raw_msg_sync = dlsym(lib_cci, "qmi_client_send_raw_msg_sync");
    p_qmi_client_release = dlsym(lib_cci, "qmi_client_release");

    if (!p_uim_get_service_object || !p_nas_get_service_object || !p_dms_get_service_object ||
        !p_wda_get_service_object || !p_qmi_client_init_instance ||
        !p_qmi_client_send_raw_msg_sync || !p_qmi_client_release) {
        ALOGE("Failed to resolve one or more essential QMI symbols");
        return -1;
    }

    ALOGI("QMI symbols loaded successfully from libqmiservices and libqmi_cci");
    return 0;
}

/* Helper to discover and get service object */
static qmi_idl_service_object_type get_service_obj(
    qmi_idl_service_object_type (*func)(int, int, int),
    int def_maj, int def_min, int def_tool,
    const char *name
) {
    if (!func) return NULL;
    qmi_idl_service_object_type obj = func(def_maj, def_min, def_tool);
    if (obj) return obj;

    for (int maj = 0; maj <= 5; maj++) {
        for (int tool = 0; tool <= 10; tool++) {
            for (int min = 200; min >= 0; min--) {
                obj = func(maj, min, tool);
                if (obj) {
                    ALOGI("Found %s service object at maj=%d, min=%d, tool=%d", name, maj, min, tool);
                    return obj;
                }
            }
        }
    }
    ALOGE("Could not find %s service object", name);
    return NULL;
}

/* Helper to connect to a QMI service using multiple fallback strategies */
static qmi_client_error_type connect_qmi_client(
    qmi_idl_service_object_type service_obj,
    const char *service_name,
    qmi_client_type *out_client
) {
    if (!service_obj || !out_client) return -1;
    *out_client = NULL;

    char os_params[256];
    memset(os_params, 0, sizeof(os_params));

    /* Strategy 1: Try QMI_CLIENT_INSTANCE_ANY (0xFFFF) */
    qmi_client_error_type rc = p_qmi_client_init_instance(
        service_obj, QMI_CLIENT_INSTANCE_ANY, NULL, NULL, os_params, 2000, out_client
    );
    if (rc == QMI_NO_ERR && *out_client) {
        ALOGI("[%s] Connected to QMI via INSTANCE_ANY", service_name);
        return QMI_NO_ERR;
    }

    /* Strategy 2: Try instance 0 */
    memset(os_params, 0, sizeof(os_params));
    ALOGI("[%s] INSTANCE_ANY returned %d, trying instance 0...", service_name, rc);
    rc = p_qmi_client_init_instance(
        service_obj, 0, NULL, NULL, os_params, 2000, out_client
    );
    if (rc == QMI_NO_ERR && *out_client) {
        ALOGI("[%s] Connected to QMI instance 0", service_name);
        return QMI_NO_ERR;
    }

    /* Strategy 3: Enumerate with qmi_client_get_service_list */
    if (p_qmi_client_get_service_list && p_qmi_client_init) {
        qmi_service_info info_list[8];
        unsigned int num_entries = 8;
        unsigned int num_services = 0;
        memset(info_list, 0, sizeof(info_list));

        rc = p_qmi_client_get_service_list(service_obj, info_list, &num_entries, &num_services);
        ALOGI("[%s] get_service_list: rc=%d, num_services=%u, num_entries=%u",
              service_name, rc, num_services, num_entries);

        if (rc == QMI_NO_ERR && num_entries > 0) {
            memset(os_params, 0, sizeof(os_params));
            rc = p_qmi_client_init(&info_list[0], service_obj, NULL, NULL, os_params, out_client);
            if (rc == QMI_NO_ERR && *out_client) {
                ALOGI("[%s] Connected to QMI via get_service_list entry 0", service_name);
                return QMI_NO_ERR;
            }
        }
    }

    ALOGE("[%s] All connection methods failed: rc=%d", service_name, rc);
    return rc;
}

/* Helper to check QMI result TLV (type 0x02) in raw response */
static int parse_qmi_result(const uint8_t *resp, uint32_t resp_len, uint16_t *qmi_err) {
    if (!resp || resp_len < 7) {
        ALOGW("Response buffer too short: len=%u", resp_len);
        return -1;
    }

    /* Iterate through TLVs looking for Type 0x02 (Result Code) */
    uint32_t offset = 0;
    while (offset + 3 <= resp_len) {
        uint8_t type = resp[offset];
        uint16_t len = (uint16_t)resp[offset + 1] | ((uint16_t)resp[offset + 2] << 8);
        offset += 3;

        if (offset + len > resp_len) break;

        if (type == 0x02 && len >= 4) {
            uint16_t result = (uint16_t)resp[offset] | ((uint16_t)resp[offset + 1] << 8);
            uint16_t error = (uint16_t)resp[offset + 2] | ((uint16_t)resp[offset + 3] << 8);
            if (qmi_err) *qmi_err = error;
            if (result == 0) return 0; // Success
            ALOGW("QMI returned error code: 0x%04x (%u)", error, error);
            return (int)error;
        }
        offset += len;
    }
    return 0;
}

/* Keep DPM client held resident */
static qmi_client_type s_dpm_client = NULL;

/*
 * Step: DPM Open Port (Message ID 0x0020)
 * Binds DATA5_CNTL and hardware data port for modem services
 */
static int send_dpm_open_port(void) {
    if (!p_dpm_get_service_object) {
        ALOGW("p_dpm_get_service_object not available");
        return -1;
    }

    if (s_dpm_client && p_qmi_client_release) {
        ALOGI("[DPM] Releasing previous DPM client before reconnecting...");
        p_qmi_client_release(s_dpm_client);
        s_dpm_client = NULL;
    }

    qmi_idl_service_object_type dpm_obj = get_service_obj(p_dpm_get_service_object, 1, 2, 6, "DPM");
    if (!dpm_obj) {
        ALOGW("Failed to get DPM service object (DPM may be handled elsewhere)");
        return -1;
    }

    qmi_client_error_type rc = connect_qmi_client(dpm_obj, "DPM", &s_dpm_client);
    if (rc != QMI_NO_ERR || !s_dpm_client) {
        ALOGW("Failed to connect to DPM via QMI (rc=%d), skipping DPM", rc);
        return rc;
    }

    ALOGI("[DPM] Connected to QMI successfully!");

    /* TLV 0x10: Control Ports */
    uint8_t tlv10_data[] = {
        0x01, // Array length = 1
        0x0A, 'D', 'A', 'T', 'A', '5', '_', 'C', 'N', 'T', 'L', // Port Name (size 10)
        0x04, 0x00, 0x00, 0x00, // Endpoint Type (4: EMBEDDED)
        0x01, 0x00, 0x00, 0x00  // Interface Number (1)
    };
    /* TLV 0x11: Hardware Data Ports */
    uint8_t tlv11_data[] = {
        0x01, // Array length = 1
        0x04, 0x00, 0x00, 0x00, // Endpoint Type (4: EMBEDDED)
        0x01, 0x00, 0x00, 0x00, // Interface Number (1)
        0x04, 0x00, 0x00, 0x00, // RX Endpoint (4)
        0x05, 0x00, 0x00, 0x00  // TX Endpoint (5)
    };

    uint8_t req[256];
    uint32_t req_len = 0;

    // TLV 0x10
    req[req_len++] = 0x10;
    req[req_len++] = sizeof(tlv10_data) & 0xFF;
    req[req_len++] = (sizeof(tlv10_data) >> 8) & 0xFF;
    memcpy(req + req_len, tlv10_data, sizeof(tlv10_data));
    req_len += sizeof(tlv10_data);

    // TLV 0x11
    req[req_len++] = 0x11;
    req[req_len++] = sizeof(tlv11_data) & 0xFF;
    req[req_len++] = (sizeof(tlv11_data) >> 8) & 0xFF;
    memcpy(req + req_len, tlv11_data, sizeof(tlv11_data));
    req_len += sizeof(tlv11_data);

    uint8_t resp[256];
    uint32_t resp_len = 0;
    uint16_t qmi_err = 0;

    ALOGI("[DPM] Sending Open Port request...");
    rc = p_qmi_client_send_raw_msg_sync(
        s_dpm_client, 0x0020, req, req_len, resp, sizeof(resp), &resp_len, 5000
    );

    if (rc == QMI_NO_ERR) {
        parse_qmi_result(resp, resp_len, &qmi_err);
        ALOGI("[DPM] Open Port completed (qmi_err=%u)", qmi_err);
    } else {
        ALOGW("[DPM] Open Port failed to send: %d", rc);
    }

    return 0;
}

/*
 * Step: WDA SET_DATA_FORMAT (0x0020)
 * Configures RAW_IP link layer and QMAP packet aggregation
 */
static int send_wda_data_format(void) {
    qmi_idl_service_object_type wda_obj = get_service_obj(p_wda_get_service_object, 1, 16, 6, "WDA");
    if (!wda_obj) {
        ALOGE("Failed to get WDA service object");
        return -1;
    }

    qmi_client_type wda_client = NULL;
    int rc = connect_qmi_client(wda_obj, "WDA", &wda_client);
    if (rc != QMI_NO_ERR || !wda_client) {
        ALOGE("Failed to init WDA client: %d", rc);
        return rc;
    }

    uint8_t req[256];
    uint8_t resp[256];
    memset(req, 0, sizeof(req));
    memset(resp, 0, sizeof(resp));
    uint32_t req_len = 0;

    // TLV 0x10: Link Prot: RAW_IP (2)
    req[req_len++] = 0x10;
    req[req_len++] = 4;
    req[req_len++] = 0;
    req[req_len++] = 2; // RAW_IP (2)
    req[req_len++] = 0;
    req[req_len++] = 0;
    req[req_len++] = 0;

    // TLV 0x13: DL Data Aggregation: QMAP (5)
    req[req_len++] = 0x13;
    req[req_len++] = 4;
    req[req_len++] = 0;
    req[req_len++] = 5; // QMAP (5)
    req[req_len++] = 0;
    req[req_len++] = 0;
    req[req_len++] = 0;

    // TLV 0x14: UL Data Aggregation: QMAP (5)
    req[req_len++] = 0x14;
    req[req_len++] = 4;
    req[req_len++] = 0;
    req[req_len++] = 5; // QMAP (5)
    req[req_len++] = 0;
    req[req_len++] = 0;
    req[req_len++] = 0;

    // TLV 0x15: Data EP ID: DATA_EP_TYPE_HSUSB (2), IFACE 4
    req[req_len++] = 0x15;
    req[req_len++] = 8;
    req[req_len++] = 0;
    req[req_len++] = 2; // DATA_EP_TYPE_HSUSB (2)
    req[req_len++] = 0;
    req[req_len++] = 0;
    req[req_len++] = 0;
    req[req_len++] = 4; // EP_IFACE_4 (4)
    req[req_len++] = 0;
    req[req_len++] = 0;
    req[req_len++] = 0;

    uint32_t resp_recv_len = 0;
    uint16_t qmi_err = 0;
    ALOGI("Sending WDA SET_DATA_FORMAT (0x0020)...");
    rc = p_qmi_client_send_raw_msg_sync(wda_client, 0x0020, req, req_len, resp, sizeof(resp), &resp_recv_len, 5000);
    if (rc == QMI_NO_ERR) {
        parse_qmi_result(resp, resp_recv_len, &qmi_err);
        ALOGI("WDA SET_DATA_FORMAT completed (qmi_err=%u, resp_len=%u)", qmi_err, resp_recv_len);
    } else {
        ALOGW("WDA SET_DATA_FORMAT send failed: rc=%d", rc);
    }

    p_qmi_client_release(wda_client);
    return rc;
}

/*
 * Diagnostic: Query Physical Slot Status (0x002D)
 */
static void query_uim_slot_status(qmi_client_type uim_client) {
    uint8_t resp[256];
    uint32_t resp_len = 0;
    qmi_client_error_type rc = p_qmi_client_send_raw_msg_sync(
        uim_client, 0x002D, NULL, 0, resp, sizeof(resp), &resp_len, 5000
    );
    if (rc == QMI_NO_ERR) {
        ALOGI("QMI_UIM_GET_SLOT_STATUS (0x002D) len=%u", resp_len);
        char hex[512] = {0};
        int hlen = 0;
        for (uint32_t i = 0; i < resp_len && hlen < (int)sizeof(hex) - 4; i++) {
            hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X ", resp[i]);
        }
        ALOGI("  Raw slot status: %s", hex);
    } else {
        ALOGW("QMI_UIM_GET_SLOT_STATUS returned rc=%d", rc);
    }
}

/*
 * Power up SIM slot with ignore_hot_swap=1
 */
static int uim_power_up_slot(qmi_client_type uim_client, uint8_t slot) {
    uint8_t pwr_req[] = {
        0x01, 0x01, 0x00, slot,  // TLV 0x01: slot = slot
        0x10, 0x01, 0x00, 0x01   // TLV 0x10: ignore_hot_swap = 1
    };
    uint8_t pwr_resp[256];
    uint32_t pwr_resp_len = 0;
    uint16_t pwr_err = 0;
    qmi_client_error_type rc = p_qmi_client_send_raw_msg_sync(
        uim_client, 0x0031, pwr_req, sizeof(pwr_req), pwr_resp, sizeof(pwr_resp), &pwr_resp_len, 5000
    );
    if (rc == QMI_NO_ERR) {
        parse_qmi_result(pwr_resp, pwr_resp_len, &pwr_err);
        ALOGI("UIM POWER_UP Slot %u (ignore_hot_swap=1) returned qmi_err=%u", slot, pwr_err);
        return pwr_err;
    } else {
        ALOGW("UIM POWER_UP Slot %u send failed: rc=%d", slot, rc);
        return rc;
    }
}

/*
 * Diagnostic & Provisioning: Check SIM card presence and select USIM AID
 */
static int check_and_provision_uim(int *already_ready) {
    if (already_ready) *already_ready = 0;
    qmi_idl_service_object_type uim_obj = get_service_obj(p_uim_get_service_object, 1, 49, 6, "UIM");
    if (!uim_obj) return -1;

    qmi_client_type uim_client = NULL;
    qmi_client_error_type rc = connect_qmi_client(uim_obj, "UIM", &uim_client);
    if (rc != QMI_NO_ERR || !uim_client) return rc;

    query_uim_slot_status(uim_client);

    uint8_t aid[32];
    uint8_t aid_len = 0;
    int card_found = 0;
    uint8_t active_slot = 1;
    int session_ready = 0;

    /* Poll card status for up to 15 seconds */
    for (int poll_iter = 0; poll_iter < 15; poll_iter++) {
        uint8_t resp[512];
        uint32_t resp_len = 0;
        rc = p_qmi_client_send_raw_msg_sync(
            uim_client, 0x002F, NULL, 0, resp, sizeof(resp), &resp_len, 5000
        );
        if (rc != QMI_NO_ERR) {
            ALOGW("UIM GET_CARD_STATUS failed: rc=%d", rc);
            sleep(1);
            continue;
        }

        uint32_t offset = 0;
        while (offset + 3 <= resp_len) {
            uint8_t type = resp[offset];
            uint16_t len = (uint16_t)resp[offset + 1] | ((uint16_t)resp[offset + 2] << 8);
            offset += 3;
            if (offset + len > resp_len) break;

            if (type == 0x10 && len >= 9) {
                uint8_t num_cards = resp[offset + 8];
                uint32_t c_off = offset + 9;
                for (uint8_t c = 0; c < num_cards && c_off < offset + len; c++) {
                    uint8_t card_state = resp[c_off];
                    uint8_t error_code = resp[c_off + 4];
                    uint8_t num_apps = resp[c_off + 5];
                    ALOGI("  Card[%u] (poll %d): state=%u (%s), err=0x%02X (%u), num_apps=%u",
                          c, poll_iter, card_state,
                          (card_state == 1) ? "PRESENT" : (card_state == 0) ? "ABSENT" : "ERROR",
                          error_code, error_code, num_apps);

                    if (card_state == 1) {
                        card_found = 1;
                        active_slot = c + 1;
                    }

                    uint32_t a_off = c_off + 6;
                    for (uint8_t a = 0; a < num_apps && a_off < offset + len; a++) {
                        uint8_t a_type = resp[a_off];
                        uint8_t app_state = resp[a_off + 1];
                        uint8_t a_len = resp[a_off + 6];
                        ALOGI("    App[%u]: type=%u (%s), state=%u, aid_len=%u",
                              a, a_type, (a_type == 2) ? "USIM" : (a_type == 1) ? "SIM" : "OTHER",
                              app_state, a_len);
                        if (a_type == 2) {
                            if (app_state == 7 || app_state == 5) { // 7 = QMI_UIM_APP_STATE_READY
                                ALOGI("    -> USIM application is ALREADY READY (state %u)!", app_state);
                                session_ready = 1;
                            }
                            if (a_len <= sizeof(aid) && aid_len == 0) {
                                aid_len = a_len;
                                memcpy(aid, &resp[a_off + 7], a_len);
                            }
                        }
                        a_off += 7 + a_len;
                    }
                    c_off = a_off;
                }
            }
            offset += len;
        }

        if (card_found) {
            ALOGI(">>> SIM CARD IS PRESENT ON SLOT %u! <<<", active_slot);
            break;
        }

        /* If error or absent on first iter, force POWER_UP on both slots */
        if (poll_iter == 0) {
            ALOGI("Forcing UIM POWER_UP on Slot 1 and Slot 2 (ignore_hot_swap=1)...");
            uim_power_up_slot(uim_client, 1);
            uim_power_up_slot(uim_client, 2);
        }

        sleep(1);
    }

    if (already_ready) *already_ready = session_ready;

    if (!card_found) {
        ALOGW(">>> SIM CARD NOT DETECTED AFTER 15 SECONDS! <<<");
    } else if (session_ready) {
        ALOGI("USIM session already active and provisioned. Skipping CHANGE_PROVISIONING_SESSION.");
    } else if (aid_len > 0) {
        ALOGI("Selecting SIM provisioning session for USIM application (aid_len=%u, slot=%u)...", aid_len, active_slot);
        uint8_t s_req[64];
        uint32_t s_len = 0;
        s_req[s_len++] = 0x01; // TLV 0x01: Session Change (mandatory)
        s_req[s_len++] = 0x02; s_req[s_len++] = 0x00; // length = 2
        s_req[s_len++] = 0x00; // Primary GW Provisioning
        s_req[s_len++] = 0x01; // Activate = TRUE

        s_req[s_len++] = 0x10; // TLV 0x10: Application Information (optional) [CRITICAL FIX: was 0x02]
        s_req[s_len++] = (uint8_t)(2 + aid_len); s_req[s_len++] = 0x00;
        s_req[s_len++] = active_slot; // Slot number
        s_req[s_len++] = aid_len;
        memcpy(&s_req[s_len], aid, aid_len);
        s_len += aid_len;

        uint8_t s_resp[256];
        uint32_t s_resp_len = 0;
        uint16_t q_err = 0;
        rc = p_qmi_client_send_raw_msg_sync(
            uim_client, 0x0038, s_req, s_len, s_resp, sizeof(s_resp), &s_resp_len, 5000
        );
        if (rc == QMI_NO_ERR) {
            parse_qmi_result(s_resp, s_resp_len, &q_err);
            ALOGI("UIM CHANGE_PROVISIONING_SESSION completed (q_err=%u)", q_err);
            usleep(1500000); // 1.5s settle delay
        } else {
            ALOGW("UIM CHANGE_PROVISIONING_SESSION returned rc=%d", rc);
        }
    }

    p_qmi_client_release(uim_client);
    return card_found ? 0 : -1;
}


/*
 * Step: UIM SUBSCRIPTION_OK (0x0040)
 * Resolves halt_subscription=1 personalization state
 */
static int send_uim_subscription_ok(void) {
    qmi_idl_service_object_type uim_obj = get_service_obj(p_uim_get_service_object, 1, 49, 6, "UIM");
    if (!uim_obj) {
        ALOGE("Failed to get UIM service object");
        return -1;
    }

    qmi_client_type uim_client = NULL;
    qmi_client_error_type rc = connect_qmi_client(uim_obj, "UIM", &uim_client);
    if (rc != QMI_NO_ERR || !uim_client) {
        return rc;
    }

    /*
     * TLV 0x01: Session Information: 0x00, 0x00 (Primary GW Provisioning)
     * TLV 0x02: OK for Subscription: 0x01 (TRUE)
     */
    uint8_t req[] = {
        0x01, 0x02, 0x00, 0x00, 0x00,
        0x02, 0x01, 0x00, 0x01
    };
    uint8_t resp[256];
    uint32_t resp_len = 0;
    uint16_t qmi_err = 0;

    ALOGI("Sending UIM SUBSCRIPTION_OK (0x0040)...");
    rc = p_qmi_client_send_raw_msg_sync(
        uim_client, 0x0040, req, sizeof(req), resp, sizeof(resp), &resp_len, 5000
    );

    if (rc == QMI_NO_ERR) {
        parse_qmi_result(resp, resp_len, &qmi_err);
        if (qmi_err == 0 || qmi_err == 3) {
            ALOGI("UIM SUBSCRIPTION_OK succeeded! (qmi_err=%u)", qmi_err);
            p_qmi_client_release(uim_client);
            return 0;
        } else {
            ALOGW("UIM SUBSCRIPTION_OK failed: qmi_err=%u", qmi_err);
        }
    } else {
        ALOGW("UIM SUBSCRIPTION_OK send returned rc=%d", rc);
    }

    p_qmi_client_release(uim_client);
    return (rc == QMI_NO_ERR) ? 0 : rc;
}

/*
 * Step: NAS DUAL_STANDBY_PREF & SET_SYSTEM_SELECTION_PREFERENCE
 */
static int send_nas_preferences(void) {
    qmi_idl_service_object_type nas_obj = get_service_obj(p_nas_get_service_object, 1, 139, 6, "NAS");
    if (!nas_obj) {
        ALOGE("Failed to get NAS service object");
        return -1;
    }

    qmi_client_type nas_client = NULL;
    qmi_client_error_type rc = connect_qmi_client(nas_obj, "NAS", &nas_client);
    if (rc != QMI_NO_ERR || !nas_client) {
        return rc;
    }

    uint8_t resp[256];
    uint32_t resp_len = 0;
    uint16_t qmi_err = 0;

    /* NAS DUAL_STANDBY_PREF (0x004B) */
    uint8_t req_dual[] = {
        0x14, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ALOGI("Sending NAS DUAL_STANDBY_PREF (0x004B)...");
    rc = p_qmi_client_send_raw_msg_sync(
        nas_client, 0x004B, req_dual, sizeof(req_dual), resp, sizeof(resp), &resp_len, 5000
    );
    if (rc == QMI_NO_ERR) {
        parse_qmi_result(resp, resp_len, &qmi_err);
        ALOGI("NAS DUAL_STANDBY_PREF completed (qmi_err=%u)", qmi_err);
    } else {
        ALOGW("NAS DUAL_STANDBY_PREF send returned %d", rc);
    }

    /* NAS SET_SYSTEM_SELECTION_PREFERENCE (0x0033: GSM/UMTS/LTE) */
    uint8_t req_pref[] = {
        0x11, 0x02, 0x00, 0x1C, 0x00
    };
    ALOGI("Sending NAS SET_SYSTEM_SELECTION_PREFERENCE (0x0033: GSM/UMTS/LTE)...");
    rc = p_qmi_client_send_raw_msg_sync(
        nas_client, 0x0033, req_pref, sizeof(req_pref), resp, sizeof(resp), &resp_len, 5000
    );
    if (rc == QMI_NO_ERR) {
        parse_qmi_result(resp, resp_len, &qmi_err);
        ALOGI("NAS SET_SYSTEM_SELECTION_PREFERENCE completed (qmi_err=%u)", qmi_err);
    } else {
        ALOGW("NAS SET_SYSTEM_SELECTION_PREFERENCE send returned %d", rc);
    }

    p_qmi_client_release(nas_client);
    return 0;
}

/*
 * Step: DMS SET_OPERATING_MODE (0x0020) -> ONLINE
 */
static int send_dms_online(void) {
    qmi_idl_service_object_type dms_obj = get_service_obj(p_dms_get_service_object, 1, 47, 6, "DMS");
    if (!dms_obj) {
        ALOGE("Failed to get DMS service object");
        return -1;
    }

    qmi_client_type dms_client = NULL;
    qmi_client_error_type rc = connect_qmi_client(dms_obj, "DMS", &dms_client);
    if (rc != QMI_NO_ERR || !dms_client) {
        return rc;
    }

    uint8_t req_online[] = {
        0x01, 0x01, 0x00, 0x00
    };
    uint8_t resp[256];
    uint32_t resp_len = 0;
    uint16_t qmi_err = 0;

    ALOGI("Sending DMS SET_OPERATING_MODE (0x0020: ONLINE)...");
    rc = p_qmi_client_send_raw_msg_sync(
        dms_client, 0x0020, req_online, sizeof(req_online), resp, sizeof(resp), &resp_len, 5000
    );
    if (rc == QMI_NO_ERR) {
        parse_qmi_result(resp, resp_len, &qmi_err);
        ALOGI("DMS SET_OPERATING_MODE completed (qmi_err=%u)", qmi_err);
    } else {
        ALOGW("DMS SET_OPERATING_MODE send returned %d", rc);
    }

    p_qmi_client_release(dms_client);
    return (rc == QMI_NO_ERR) ? 0 : rc;
}

/*
 * AT Command Helpers
 */
static int open_at_port(void) {
    const char *ports[] = { "/dev/smd0", "/dev/smd11", NULL };
    int fd = -1;

    for (int i = 0; ports[i] != NULL; i++) {
        fd = open(ports[i], O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            ALOGI("Connected to AT command port: %s", ports[i]);
            break;
        }
    }

    if (fd < 0) {
        ALOGW("Could not open AT SMD serial port (/dev/smd0, /dev/smd11): %s", strerror(errno));
        return -1;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tcsetattr(fd, TCSANOW, &tio);
    return fd;
}

static void at_drain(int fd) {
    char buf[512];
    while (read(fd, buf, sizeof(buf)) > 0) {}
}

static int at_cmd(int fd, const char *cmd, int timeout_ms, char *resp_out, size_t max_resp) {
    if (fd < 0) return -1;
    at_drain(fd);

    char send_buf[256];
    snprintf(send_buf, sizeof(send_buf), "%s\r\n", cmd);
    write(fd, send_buf, strlen(send_buf));

    if (resp_out && max_resp > 0) resp_out[0] = '\0';
    size_t total = 0;

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int ret = poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            char buf[256];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                if (resp_out && total + n < max_resp) {
                    memcpy(resp_out + total, buf, n);
                    total += n;
                    resp_out[total] = '\0';
                }
                if (strstr(resp_out ? resp_out : buf, "\r\nOK\r\n") ||
                    strstr(resp_out ? resp_out : buf, "\r\nERROR\r\n") ||
                    strstr(resp_out ? resp_out : buf, "+CME ERROR:") ||
                    strstr(resp_out ? resp_out : buf, "+CMS ERROR:")) {
                    break;
                }
            }
        }
        elapsed += 100;
    }

    /* Clean string for logging */
    char clean[256] = {0};
    if (resp_out && total > 0) {
        size_t c = 0;
        for (size_t i = 0; i < total && c < sizeof(clean) - 1; i++) {
            if (resp_out[i] == '\r') continue;
            if (resp_out[i] == '\n') {
                if (c > 0 && clean[c-1] != ' ') clean[c++] = ' ';
            } else {
                clean[c++] = resp_out[i];
            }
        }
        clean[c] = '\0';
    }
    ALOGI("AT [%s] -> %s", cmd, clean);
    return 0;
}

static void at_radio_cycle(int at_fd) {
    if (at_fd < 0) return;
    char resp[256];
    at_cmd(at_fd, "AT+CFUN?", 1500, resp, sizeof(resp));
    if (strstr(resp, "+CFUN: 1")) {
        ALOGI("Modem already ONLINE (+CFUN: 1), skipping AT+CFUN=1");
        return;
    }
    ALOGI("Ensuring modem is in ONLINE mode (AT+CFUN=1)...");
    at_cmd(at_fd, "AT+CFUN=1", 3000, resp, sizeof(resp));
}

static void at_network_registration(int at_fd) {
    if (at_fd < 0) return;
    char resp[512];

    at_cmd(at_fd, "AT+CREG?", 1000, resp, sizeof(resp));
    if (strstr(resp, "+CREG: 0,1") || strstr(resp, "+CREG: 0,5") ||
        strstr(resp, "+CREG: 1,1") || strstr(resp, "+CREG: 1,5") ||
        strstr(resp, "+CREG: 2,1") || strstr(resp, "+CREG: 2,5")) {
        ALOGI("Modem already registered on CS network (+CREG), skipping COPS=0");
        at_cmd(at_fd, "AT+CGSMS=1", 2000, resp, sizeof(resp));
        return;
    }

    at_cmd(at_fd, "AT+CEREG?", 1000, resp, sizeof(resp));
    if (strstr(resp, "+CEREG: 0,1") || strstr(resp, "+CEREG: 0,5") ||
        strstr(resp, "+CEREG: 1,1") || strstr(resp, "+CEREG: 1,5") ||
        strstr(resp, "+CEREG: 2,1") || strstr(resp, "+CEREG: 2,5")) {
        ALOGI("Modem already registered on LTE/EPS network (+CEREG), skipping COPS=0");
        at_cmd(at_fd, "AT+CGSMS=1", 2000, resp, sizeof(resp));
        return;
    }

    ALOGI("Ensuring Automatic Network Selection (AT+COPS=0)...");
    at_cmd(at_fd, "AT+COPS=0", 3000, resp, sizeof(resp));

    ALOGI("Polling network registration (AT+CREG? / AT+CEREG?)...");
    int registered = 0;
    for (int i = 0; i < 5; i++) {
        at_cmd(at_fd, "AT+CREG?", 1000, resp, sizeof(resp));
        if (strstr(resp, "+CREG: 0,1") || strstr(resp, "+CREG: 0,5") ||
            strstr(resp, "+CREG: 1,1") || strstr(resp, "+CREG: 1,5") ||
            strstr(resp, "+CREG: 2,1") || strstr(resp, "+CREG: 2,5")) {
            ALOGI(">>> NETWORK REGISTERED SUCCESSFULLY (CS)! <<<");
            registered = 1;
            break;
        }
        at_cmd(at_fd, "AT+CEREG?", 1000, resp, sizeof(resp));
        if (strstr(resp, "+CEREG: 0,1") || strstr(resp, "+CEREG: 0,5") ||
            strstr(resp, "+CEREG: 1,1") || strstr(resp, "+CEREG: 1,5") ||
            strstr(resp, "+CEREG: 2,1") || strstr(resp, "+CEREG: 2,5")) {
            ALOGI(">>> NETWORK REGISTERED SUCCESSFULLY (LTE/EPS)! <<<");
            registered = 1;
            break;
        }
        sleep(1);
    }

    if (!registered) {
        ALOGI("Registration in progress or searching, RIL will continue in background...");
    }

    at_cmd(at_fd, "AT+CGSMS=1", 2000, resp, sizeof(resp));
    at_cmd(at_fd, "AT+CSQ", 1500, resp, sizeof(resp));
}

static char modem_sysfs_path[64] = "/sys/bus/msm_subsys/devices/subsys3";

static void detect_modem_sysfs(void) {
    char path[64], name[32];
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "/sys/bus/msm_subsys/devices/subsys%d/name", i);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, name, sizeof(name) - 1);
            close(fd);
            if (n > 0) {
                name[n] = '\0';
                if (strstr(name, "modem")) {
                    snprintf(modem_sysfs_path, sizeof(modem_sysfs_path), "/sys/bus/msm_subsys/devices/subsys%d", i);
                    ALOGI("Found modem subsystem at %s", modem_sysfs_path);
                    return;
                }
            }
        }
    }
}

static int read_crash_count(void) {
    char path[128], buf[32];
    snprintf(path, sizeof(path), "%s/crash_count", modem_sysfs_path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return atoi(buf);
}

static int is_modem_online(void) {
    char path[128], buf[32];
    snprintf(path, sizeof(path), "%s/state", modem_sysfs_path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (strstr(buf, "ONLINE") != NULL);
}

static int wait_for_modem_online(int timeout_sec) {
    for (int i = 0; i < timeout_sec * 5; i++) {
        if (is_modem_online()) return 1;
        usleep(200000); // 200ms
    }
    return is_modem_online();
}

static int run_full_initialization_sequence(int *out_card_provisioned) {
    ALOGI("--------------------------------------------------");
    ALOGI(" Executing full Lumia modem provisioning sequence ");
    ALOGI("--------------------------------------------------");

    /* Step 1: DPM Open Port */
    send_dpm_open_port();

    /* Step 2: WDA SET_DATA_FORMAT (RAW_IP, QMAP) */
    send_wda_data_format();

    /* Step 3: Check SIM card status and provision USIM session if not already ready */
    int already_ready = 0;
    int card_ret = check_and_provision_uim(&already_ready);
    int card_provisioned = (card_ret == 0);
    if (out_card_provisioned) *out_card_provisioned = card_provisioned;

    /* Step 4: Publish SIM Subscription if not already ready */
    if (!already_ready && card_provisioned) {
        int retries = 10;
        int success = 0;
        while (retries-- > 0) {
            ALOGI("Attempting UIM SUBSCRIPTION_OK (retries left: %d)...", retries);
            if (send_uim_subscription_ok() == 0) {
                success = 1;
                break;
            }
            sleep(1);
        }
        if (!success) {
            ALOGW("UIM SUBSCRIPTION_OK did not complete cleanly, proceeding...");
        }
        usleep(1500000); // 1.5s settle delay
    } else if (already_ready) {
        ALOGI("USIM subscription is already published and READY!");
    }

    /* Step 5: Unhide Dual Standby & Set RAT Preferences (GSM/UMTS/LTE) */
    send_nas_preferences();

    /* Step 6: Put DMS in ONLINE mode */
    send_dms_online();

    ALOGI("--------------------------------------------------");
    ALOGI(" Lumia modem provisioning sequence finished!      ");
    ALOGI(" (Pure QMI mode - RIL handles cellular registration)");
    ALOGI("--------------------------------------------------");
    return 0;
}

int main(int argc __attribute__((unused)), char **argv __attribute__((unused))) {
    ALOGI("==================================================");
    ALOGI(" Lumia 950 XL (cityman) Modem Initialization Tool ");
    ALOGI(" Applying MPSS.BO.2.5 Quirks from EpicLPer/ipa-netmgr");
    ALOGI("==================================================");

    detect_modem_sysfs();

    ALOGI("Waiting for modem subsystem to be ONLINE...");
    wait_for_modem_online(30);

    if (load_qmi_symbols() != 0) {
        ALOGE("Initialization aborted: could not load QMI libraries.");
        return 1;
    }

    int last_crash_count = read_crash_count();
    ALOGI("Initial modem crash_count: %d", last_crash_count);

    int card_provisioned = 0;
    run_full_initialization_sequence(&card_provisioned);

    ALOGI("==================================================");
    ALOGI(" Lumia Modem Initialization sequence finished!    ");
    ALOGI(" Staying resident to keep DPM/modem session alive ");
    ALOGI(" Monitoring modem health and SIM provisioning...   ");
    ALOGI("==================================================");

    /* Stay resident to hold sessions open & monitor modem health */
    int last_modem_online = 1;
    int late_sim_counter = 0;
    int consecutive_crashes = 0;
    time_t last_crash_time = 0;

    while (1) {
        sleep(1); // 1-second watchdog tick for rapid SSR detection

        int cur_crash_count = read_crash_count();
        int modem_online = is_modem_online();

        /* Check 1: Modem Subsystem Restart (SSR) detected via crash_count or state transition */
        int crash_detected = (cur_crash_count > last_crash_count && cur_crash_count >= 0);
        int state_recovered = (!last_modem_online && modem_online);

        if (!modem_online) {
            if (last_modem_online) {
                ALOGW("[WATCHDOG] Modem subsystem went OFFLINE. Waiting for recovery...");
            }
            last_modem_online = 0;
            card_provisioned = 0;
            continue;
        }

        if (crash_detected || state_recovered) {
            time_t now = time(NULL);
            if (now - last_crash_time < 60) {
                consecutive_crashes++;
            } else {
                consecutive_crashes = 1;
            }
            last_crash_time = now;

            int settle_delay = 3;
            if (consecutive_crashes == 2) {
                settle_delay = 8;
            } else if (consecutive_crashes == 3) {
                settle_delay = 15;
            } else if (consecutive_crashes >= 4) {
                settle_delay = 30;
            }

            ALOGI("==================================================");
            ALOGI("[WATCHDOG] Modem restart / SSR detected! (crash_count: %d -> %d, consecutive: %d)",
                  last_crash_count, cur_crash_count, consecutive_crashes);
            ALOGI("[WATCHDOG] Backing off %d seconds to allow modem and RIL to stabilize...", settle_delay);
            ALOGI("==================================================");

            // Wait for modem to stabilize
            wait_for_modem_online(30);
            sleep(settle_delay);

            last_crash_count = (cur_crash_count >= 0) ? cur_crash_count : read_crash_count();
            last_modem_online = 1;
            card_provisioned = 0;

            ALOGI("[WATCHDOG] Modem recovered from SSR; re-running full QMI provisioning");
            run_full_initialization_sequence(&card_provisioned);
            continue;
        }

        if (consecutive_crashes > 0 && time(NULL) - last_crash_time > 120) {
            ALOGI("[WATCHDOG] Modem has been stable for > 2 minutes, resetting crash counter");
            consecutive_crashes = 0;
        }

        /* Check 2: Late SIM card insertion if SIM was not detected on boot */
        if (!card_provisioned) {
            if (++late_sim_counter >= 10) {
                late_sim_counter = 0;
                int ready = 0;
                if (check_and_provision_uim(&ready) == 0) {
                    ALOGI("[WATCHDOG] Late SIM insertion detected! Provisioning...");
                    card_provisioned = 1;
                    if (!ready) {
                        send_uim_subscription_ok();
                    }
                    send_nas_preferences();
                    send_dms_online();
                    ALOGI("[WATCHDOG] Late SIM provisioning complete!");
                }
            }
        }
    }

    return 0;
}
