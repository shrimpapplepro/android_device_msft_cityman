/*
 * Diagnostic tool for Lumia 950 XL modem / network status
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

typedef void *qmi_client_type;
typedef void *qmi_idl_service_object_type;
typedef int qmi_service_instance;
typedef void (*qmi_client_ind_cb)(qmi_client_type user_handle,
                                   unsigned int msg_id,
                                   void *ind_buf,
                                   unsigned int ind_buf_len,
                                   void *ind_cb_data);
typedef int qmi_client_error_type;

#define QMI_NO_ERR 0
#define QMI_CLIENT_INSTANCE_ANY 0xFFFF

static qmi_idl_service_object_type (*p_nas_get_service_object)(int, int, int) = NULL;
static qmi_idl_service_object_type (*p_dms_get_service_object)(int, int, int) = NULL;
static qmi_idl_service_object_type (*p_uim_get_service_object)(int, int, int) = NULL;

static qmi_client_error_type (*p_qmi_client_init_instance)(
    qmi_idl_service_object_type service_obj,
    qmi_service_instance instance_id,
    qmi_client_ind_cb ind_cb,
    void *ind_cb_data,
    void *os_params,
    uint32_t timeout_msecs,
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

static int load_qmi(void) {
    void *lib_services = dlopen("/vendor/lib64/libqmiservices.so", RTLD_NOW);
    if (!lib_services) lib_services = dlopen("libqmiservices.so", RTLD_NOW);
    if (!lib_services) {
        printf("Failed to load libqmiservices.so: %s\n", dlerror());
        return -1;
    }

    void *lib_cci = dlopen("/vendor/lib64/libqmi_cci.so", RTLD_NOW);
    if (!lib_cci) lib_cci = dlopen("libqmi_cci.so", RTLD_NOW);
    if (!lib_cci) {
        printf("Failed to load libqmi_cci.so: %s\n", dlerror());
        return -1;
    }

    p_nas_get_service_object = dlsym(lib_services, "nas_get_service_object_internal_v01");
    p_dms_get_service_object = dlsym(lib_services, "dms_get_service_object_internal_v01");
    p_uim_get_service_object = dlsym(lib_services, "uim_get_service_object_internal_v01");

    p_qmi_client_init_instance = dlsym(lib_cci, "qmi_client_init_instance");
    p_qmi_client_send_raw_msg_sync = dlsym(lib_cci, "qmi_client_send_raw_msg_sync");
    p_qmi_client_release = dlsym(lib_cci, "qmi_client_release");

    return 0;
}

static qmi_idl_service_object_type get_service_obj(
    qmi_idl_service_object_type (*func)(int, int, int),
    int def_maj, int def_min, int def_tool
) {
    if (!func) return NULL;
    qmi_idl_service_object_type obj = func(def_maj, def_min, def_tool);
    if (obj) return obj;
    for (int maj = 0; maj <= 5; maj++) {
        for (int tool = 0; tool <= 10; tool++) {
            for (int min = 200; min >= 0; min--) {
                obj = func(maj, min, tool);
                if (obj) return obj;
            }
        }
    }
    return NULL;
}

static qmi_client_type connect_service(qmi_idl_service_object_type obj) {
    if (!obj) return NULL;
    char os_params[256] = {0};
    qmi_client_type client = NULL;
    qmi_client_error_type rc = p_qmi_client_init_instance(obj, QMI_CLIENT_INSTANCE_ANY, NULL, NULL, os_params, 2000, &client);
    if (rc == QMI_NO_ERR && client) return client;
    memset(os_params, 0, sizeof(os_params));
    rc = p_qmi_client_init_instance(obj, 0, NULL, NULL, os_params, 2000, &client);
    if (rc == QMI_NO_ERR && client) return client;
    return NULL;
}

static void print_tlvs(const char *name, uint8_t *buf, uint32_t len) {
    printf("=== %s (len=%u) ===\n", name, len);
    uint32_t off = 0;
    while (off + 3 <= len) {
        uint8_t type = buf[off];
        uint16_t tlen = (uint16_t)buf[off + 1] | ((uint16_t)buf[off + 2] << 8);
        off += 3;
        if (off + tlen > len) break;
        printf("  TLV 0x%02X (len=%u): ", type, tlen);
        for (uint16_t i = 0; i < tlen && i < 32; i++) {
            printf("%02X ", buf[off + i]);
        }
        if (tlen > 32) printf("...");
        printf("\n");
        off += tlen;
    }
}

static void query_qmi_nas(void) {
    printf("\n--- QMI NAS Queries ---\n");
    qmi_idl_service_object_type nas_obj = get_service_obj(p_nas_get_service_object, 1, 139, 6);
    qmi_client_type nas_client = connect_service(nas_obj);
    if (!nas_client) {
        printf("Failed to connect to NAS service\n");
        return;
    }

    uint8_t resp[1024];
    uint32_t resp_len = 0;

    // 1. Serving System (0x0024)
    if (p_qmi_client_send_raw_msg_sync(nas_client, 0x0024, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("NAS Serving System (0x0024)", resp, resp_len);
    } else {
        printf("NAS Serving System query failed\n");
    }

    // 2. Signal Strength (0x0020)
    if (p_qmi_client_send_raw_msg_sync(nas_client, 0x0020, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("NAS Signal Strength (0x0020)", resp, resp_len);
    } else {
        printf("NAS Signal Strength query failed\n");
    }

    // 3. RF Band Info (0x0031)
    if (p_qmi_client_send_raw_msg_sync(nas_client, 0x0031, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("NAS RF Band Info (0x0031)", resp, resp_len);
    } else {
        printf("NAS RF Band Info query failed\n");
    }

    // 4. System Selection Preference (0x0034)
    if (p_qmi_client_send_raw_msg_sync(nas_client, 0x0034, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("NAS System Selection Pref (0x0034)", resp, resp_len);
    } else {
        printf("NAS System Selection Pref query failed\n");
    }

    // 5. Operator Name (0x003E)
    if (p_qmi_client_send_raw_msg_sync(nas_client, 0x003E, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("NAS Operator Name (0x003E)", resp, resp_len);
    } else {
        printf("NAS Operator Name query failed\n");
    }

    p_qmi_client_release(nas_client);
}

static void query_qmi_dms(void) {
    printf("\n--- QMI DMS Queries ---\n");
    qmi_idl_service_object_type dms_obj = get_service_obj(p_dms_get_service_object, 1, 47, 6);
    qmi_client_type dms_client = connect_service(dms_obj);
    if (!dms_client) {
        printf("Failed to connect to DMS service\n");
        return;
    }

    uint8_t resp[512];
    uint32_t resp_len = 0;

    // Get Operating Mode (0x002D)
    if (p_qmi_client_send_raw_msg_sync(dms_client, 0x002D, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("DMS Get Operating Mode (0x002D)", resp, resp_len);
    }

    p_qmi_client_release(dms_client);
}

static void query_qmi_uim(void) {
    printf("\n--- QMI UIM Queries ---\n");
    qmi_idl_service_object_type uim_obj = get_service_obj(p_uim_get_service_object, 1, 49, 6);
    qmi_client_type uim_client = connect_service(uim_obj);
    if (!uim_client) {
        printf("Failed to connect to UIM service\n");
        return;
    }

    uint8_t resp[1024];
    uint32_t resp_len = 0;

    // Get Slot Status (0x002D)
    if (p_qmi_client_send_raw_msg_sync(uim_client, 0x002D, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("UIM Get Slot Status (0x002D)", resp, resp_len);
    }

    // Get Card Status (0x002F)
    if (p_qmi_client_send_raw_msg_sync(uim_client, 0x002F, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("UIM Get Card Status (0x002F)", resp, resp_len);
    }

    // Get Configuration (0x002E)
    if (p_qmi_client_send_raw_msg_sync(uim_client, 0x002E, NULL, 0, resp, sizeof(resp), &resp_len, 3000) == 0) {
        print_tlvs("UIM Get Configuration (0x002E)", resp, resp_len);
    }

    p_qmi_client_release(uim_client);
}

static void query_at_port(const char *port) {
    printf("\n--- AT Queries on %s ---\n", port);
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        printf("Cannot open %s: %s\n", port, strerror(errno));
        return;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tcsetattr(fd, TCSANOW, &tio);

    const char *cmds[] = {
        "AT",
        "ATI",
        "AT+CPIN?",
        "AT+CFUN?",
        "AT+CSQ",
        "AT+CREG?",
        "AT+CGREG?",
        "AT+CEREG?",
        "AT+COPS?",
        "AT$QCSYSMODE",
        NULL
    };

    for (int i = 0; cmds[i] != NULL; i++) {
        // flush
        char dummy[256];
        while (read(fd, dummy, sizeof(dummy)) > 0) {}

        char cmd_buf[128];
        snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", cmds[i]);
        write(fd, cmd_buf, strlen(cmd_buf));

        char resp[512] = {0};
        size_t total = 0;
        int elapsed = 0;
        while (elapsed < 1500) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            int ret = poll(&pfd, 1, 100);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                ssize_t n = read(fd, resp + total, sizeof(resp) - total - 1);
                if (n > 0) {
                    total += n;
                    resp[total] = '\0';
                    if (strstr(resp, "OK") || strstr(resp, "ERROR")) break;
                }
            }
            elapsed += 100;
        }

        // Clean output
        for (size_t c = 0; c < total; c++) {
            if (resp[c] == '\r' || resp[c] == '\n') resp[c] = ' ';
        }
        printf("  [%s] -> %s\n", cmds[i], total > 0 ? resp : "(timeout)");
    }

    close(fd);
}

int main(int argc, char **argv) {
    printf("=========================================\n");
    printf(" Lumia 950 XL Modem Diagnostics Tool     \n");
    printf("=========================================\n");

    if (argc > 1) {
        printf("Executing custom AT command: %s\n", argv[1]);
        int fd = open("/dev/smd11", O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            printf("Cannot open /dev/smd11: %s\n", strerror(errno));
            return 1;
        }
        struct termios tio;
        memset(&tio, 0, sizeof(tio));
        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);
        tcsetattr(fd, TCSANOW, &tio);

        char dummy[256];
        while (read(fd, dummy, sizeof(dummy)) > 0) {}

        char cmd_buf[256];
        snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", argv[1]);
        write(fd, cmd_buf, strlen(cmd_buf));

        char resp[1024] = {0};
        size_t total = 0;
        int elapsed = 0;
        while (elapsed < 10000) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            int ret = poll(&pfd, 1, 200);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                ssize_t n = read(fd, resp + total, sizeof(resp) - total - 1);
                if (n > 0) {
                    total += n;
                    resp[total] = '\0';
                    if (strstr(resp, "OK\r") || strstr(resp, "ERROR\r") || strstr(resp, "ERROR\n") || strstr(resp, "OK\n")) break;
                }
            }
            elapsed += 200;
        }
        printf("Response:\n%s\n", resp);
        close(fd);
        return 0;
    }

    if (load_qmi() == 0) {
        query_qmi_dms();
        query_qmi_nas();
        query_qmi_uim();
    }

    query_at_port("/dev/smd11");
    query_at_port("/dev/smd0");

    return 0;
}

