/*
 * Copyright (C) 2016 Unwired Devices <info@unwds.com>
 *               2017 Inria Chile
 *               2017 Inria
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     tests
 * @{
 * @file
 * @brief       Test application for SX127X modem driver
 *
 * @author      Eugene P. <ep@unwds.com>
 * @author      José Ignacio Alamos <jose.alamos@inria.cl>
 * @author      Alexandre Abadie <alexandre.abadie@inria.fr>
 * @}
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "thread.h"
#include "shell.h"
//#include "shell_commands.h"

#include "net/netdev.h"
#include "net/netdev/lora.h"
#include "net/lora.h"

#include "board.h"

#include "sx127x_internal.h"
#include "sx127x_params.h"
#include "sx127x_netdev.h"

#include "fmt.h"

// for autotelem
#include <math.h>
#include "saul_reg.h"
#include "xtimer.h"


#define SX127X_LORA_MSG_QUEUE   (16U)
#ifndef SX127X_STACKSIZE
#define SX127X_STACKSIZE        (THREAD_STACKSIZE_DEFAULT)
#endif

#define MSG_TYPE_ISR            (0x3456)

#define MY_NODE_ID "6767"

static char stack[SX127X_STACKSIZE];
static kernel_pid_t _recv_pid;

static char message[255];
static int current_msg_id = 1;
static sx127x_t sx127x;

// for autotelem
static char telemetry_stack[THREAD_STACKSIZE_DEFAULT];
static char relay_stack[THREAD_STACKSIZE_DEFAULT];
static uint32_t telemetry_interval = 0; /* 0 means disabled */

#define CHAT_TTL_DEFAULT 7
#define CHAT_BUF_SIZE    255
#define CHAT_AD_FIELD    "ad="
#define CHAT_MSG_FIELD   "msg="


#define NODE_ID_MAXLEN   32
#define NODE_TABLE_SIZE  16   /* max distinct nodes we track */

typedef struct {
    char     node_id[NODE_ID_MAXLEN];  /* emitter string, e.g. "NODE1"  */
    uint16_t last_msg_id;              /* last sequence number seen      */
    uint8_t  active;                   /* 0 = slot free                  */
} known_node_t;


known_node_t node_table[NODE_TABLE_SIZE] = {0};

void node_table_update(const char *emitter_id, uint16_t msg_id)
{
    int free_slot = -1;

    for (int i = 0; i < NODE_TABLE_SIZE; i++) {
        if (!node_table[i].active) {
            if (free_slot < 0) free_slot = i;   /* remember first free slot */
            continue;
        }
        if (strncmp(node_table[i].node_id, emitter_id, NODE_ID_MAXLEN) == 0) {
            /* Known node — update in place */
            node_table[i].last_msg_id  = msg_id;
            return;
        }
    }

    /* New node */
    if (free_slot < 0) {
        puts("[nodes] table full, dropping entry");
        return;
    }
    strncpy(node_table[free_slot].node_id, emitter_id, NODE_ID_MAXLEN - 1);
    node_table[free_slot].node_id[NODE_ID_MAXLEN - 1] = '\0';
    node_table[free_slot].last_msg_id = msg_id;
    node_table[free_slot].active      = 1;
}

#define MAX_SALONS         5
#define SALON_NAME_MAXLEN  32

typedef struct {
    char name[SALON_NAME_MAXLEN];
    uint8_t active;
} salon_t;

static salon_t my_salons[MAX_SALONS] = {0};

static int is_in_salon(const char *salon_name) {
    for (int i = 0; i < MAX_SALONS; i++) {
        if (my_salons[i].active && 
            strncmp(my_salons[i].name, salon_name, SALON_NAME_MAXLEN) == 0) {
            return 1;
        }
    }
    return 0;
}

static size_t convert_hex(uint8_t *dest, const char *src) {
    size_t i;
    int value;
    size_t count = strlen(src);
    for (i = 0; i < count && sscanf(src + i * 2, "%2x", &value) == 1; i++) {
        dest[i] = value;
    }
    return i;
}

/*
 * HISTORY & RELAY MANAGEMENT
*/

#define HISTORY_MAX 10  
#define RELAY_QUEUE_MAX 8

/* Message relay status */
typedef enum {
    MSG_STATUS_RECEIVED = 0,    /* Just received, pending relay decision */
    MSG_STATUS_RELAYED = 1,     /* Successfully relayed */
    MSG_STATUS_NOT_RELAYED = 2, /* Decided not to relay (SNR threshold exceeded) */
    MSG_STATUS_DROPPED = 3      /* Dropped due to TTL=0 or other reason */
} msg_status_t;

typedef struct {
    char sender[NODE_ID_MAXLEN];
    char target[NODE_ID_MAXLEN];
    char type;
    char payload[128];
    uint16_t msg_id;
    uint8_t ttl;
    int8_t snr;                 /* Signal-to-Noise Ratio */
    msg_status_t status;        /* Relay status */
    uint32_t timestamp;         /* When message was received (ms) */
} chat_msg_t;


static chat_msg_t msg_history[HISTORY_MAX];
static uint8_t history_head = 0;  /* Index where the NEXT message will be written */
static uint8_t history_count = 0; /* How many messages are currently in the queue */

/* Relay management */
static int8_t snr_threshold = 0;  /* Default: relay all messages (SNR threshold) */

typedef struct {
    char raw_msg[255];
    uint32_t relay_delay_ms;    /* Milliseconds to wait before relaying */
    uint8_t ttl;
    int8_t snr;
    uint32_t queued_at_ms;      /* Timestamp when message was queued */
} relay_msg_t;

static relay_msg_t relay_queue[RELAY_QUEUE_MAX];
static uint8_t relay_queue_count = 0;

static void add_to_history(const char* sender, char type, const char* target, const char* payload, uint16_t msg_id, uint8_t ttl, 
                          int8_t snr, msg_status_t status) {
    /* Copy data into the current head position */
    strncpy(msg_history[history_head].sender, sender, sizeof(msg_history[history_head].sender) - 1);
    msg_history[history_head].type = type;
    strncpy(msg_history[history_head].target, target, sizeof(msg_history[history_head].target) - 1);
    strncpy(msg_history[history_head].payload, payload, sizeof(msg_history[history_head].payload) - 1);
    msg_history[history_head].msg_id = msg_id;
    msg_history[history_head].ttl = ttl;
    msg_history[history_head].snr = snr;
    msg_history[history_head].status = status;
    msg_history[history_head].timestamp = xtimer_now_usec() / 1000;  /* Convert to ms */
    /* Ensure null termination */
    msg_history[history_head].sender[NODE_ID_MAXLEN - 1] = '\0';
    msg_history[history_head].target[NODE_ID_MAXLEN - 1] = '\0';
    msg_history[history_head].payload[127] = '\0';

    /* Advance head (wrap around if it reaches max) */
    history_head = (history_head + 1) % HISTORY_MAX;
    
    if (history_count < HISTORY_MAX) {
        history_count++;
    }
}

/* ================================================== *
 * RELAY DELAY CALCULATION
 * ================================================== */

/* Calculate relay delay in milliseconds based on SNR and other parameters
 * Higher SNR = lower priority = longer delay
 * This implements a back-off mechanism for mesh networking
 */
static uint32_t calculate_relay_delay(int8_t snr, uint8_t sf, uint8_t bw) {
    /* Base delay: time-on-air calculation 
     * Approximate values for different SF/BW combinations (in ms) */
    uint32_t base_delay = 100;  /* default 100ms */
    
    /* Increase delay based on SF (higher SF = longer transmissions) */
    if (sf >= 12) base_delay = 200;
    else if (sf >= 10) base_delay = 150;
    else if (sf >= 8) base_delay = 120;
    
    /* SNR-based priority backoff
     * If SNR is very good, node waits longer (lower priority to relay)
     * If SNR is poor, node waits less (higher priority to relay) */
    uint32_t snr_delay;
    if (snr >= 10) {
        snr_delay = 300;  /* Excellent signal: wait 300ms */
    } else if (snr >= 5) {
        snr_delay = 200;  /* Good signal: wait 200ms */
    } else if (snr >= 0) {
        snr_delay = 100;  /* Decent signal: wait 100ms */
    } else if (snr >= -10) {
        snr_delay = 50;   /* Poor signal: wait 50ms */
    } else {
        snr_delay = 20;   /* Very poor signal: wait 20ms */
    }
    
    /* Add some randomization to prevent collision storms */
    snr_delay += (rand() % 50);  /* Add 0-50ms random jitter */
    
    return base_delay + snr_delay;
}

/* Add message to relay queue if it should be relayed */
static int queue_for_relay(const char *raw_msg, uint8_t ttl, int8_t snr, uint8_t sf, uint8_t bw) {
    /* Don't relay if TTL is 0 or not present */
    if (ttl == 0) {
        return 0;  /* Message end-of-life, don't relay */
    }
    
    /* Check SNR threshold - if signal too strong, don't bother relaying */
    if (snr > snr_threshold) {
        printf("[RELAY] SNR %d > threshold %d, skipping relay\n", snr, snr_threshold);
        return 0;
    }
    
    /* Check if queue is full */
    if (relay_queue_count >= RELAY_QUEUE_MAX) {
        printf("[RELAY] Queue full, dropping message\n");
        return -1;
    }
    
    /* Add to relay queue */
    relay_msg_t *relay_msg = &relay_queue[relay_queue_count];
    strncpy(relay_msg->raw_msg, raw_msg, sizeof(relay_msg->raw_msg) - 1);
    relay_msg->raw_msg[sizeof(relay_msg->raw_msg) - 1] = '\0';
    relay_msg->relay_delay_ms = calculate_relay_delay(snr, sf, bw);
    relay_msg->ttl = ttl - 1;  /* Decrement TTL */
    relay_msg->snr = snr;
    relay_msg->queued_at_ms = xtimer_now_usec() / 1000;  /* Timestamp in ms */
    
    relay_queue_count++;
    
    printf("[RELAY] Queued: delay=%lu ms, TTL will be %u\n", 
           relay_msg->relay_delay_ms, relay_msg->ttl);
    
    return 1;  /* Successfully queued */
}


int lora_setup_cmd(int argc, char **argv)
{

    if (argc < 4) {
        puts("usage: setup "
             "<bandwidth (125, 250, 500)> "
             "<spreading factor (7..12)> "
             "<code rate (5..8)>");
        return -1;
    }

    /* Check bandwidth value */
    int bw = atoi(argv[1]);
    uint8_t lora_bw;

    switch (bw) {
    case 125:
        puts("setup: setting 125KHz bandwidth");
        lora_bw = LORA_BW_125_KHZ;
        break;

    case 250:
        puts("setup: setting 250KHz bandwidth");
        lora_bw = LORA_BW_250_KHZ;
        break;

    case 500:
        puts("setup: setting 500KHz bandwidth");
        lora_bw = LORA_BW_500_KHZ;
        break;

    default:
        puts("[Error] setup: invalid bandwidth value given, "
             "only 125, 250 or 500 allowed.");
        return -1;
    }

    /* Check spreading factor value */
    uint8_t lora_sf = atoi(argv[2]);

    if (lora_sf < 7 || lora_sf > 12) {
        puts("[Error] setup: invalid spreading factor value given");
        return -1;
    }

    /* Check coding rate value */
    int cr = atoi(argv[3]);

    if (cr < 5 || cr > 8) {
        puts("[Error ]setup: invalid coding rate value given");
        return -1;
    }
    uint8_t lora_cr = (uint8_t)(cr - 4);

    /* Configure radio device */
    netdev_t *netdev = &sx127x.netdev;

    netdev->driver->set(netdev, NETOPT_BANDWIDTH,
                        &lora_bw, sizeof(lora_bw));
    netdev->driver->set(netdev, NETOPT_SPREADING_FACTOR,
                        &lora_sf, sizeof(lora_sf));
    netdev->driver->set(netdev, NETOPT_CODING_RATE,
                        &lora_cr, sizeof(lora_cr));

    puts("[Info] setup: configuration set with success");

    return 0;
}

int random_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    netdev_t *netdev = &sx127x.netdev;
    uint32_t rand;

    netdev->driver->get(netdev, NETOPT_RANDOM, &rand, sizeof(rand));
    printf("random: number from sx127x: %u\n",
           (unsigned int)rand);

    /* reinit the transceiver to default values */
    sx127x_init_radio_settings(&sx127x);

    return 0;
}

int register_cmd(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: register <get | set>");
        return -1;
    }

    if (strstr(argv[1], "get") != NULL) {
        if (argc < 3) {
            puts("usage: register get <all | allinline | regnum>");
            return -1;
        }

        if (strcmp(argv[2], "all") == 0) {
            puts("- listing all registers -");
            uint8_t reg = 0, data = 0;
            /* Listing registers map */
            puts("Reg   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F");
            for (unsigned i = 0; i <= 7; i++) {
                printf("0x%02X ", i << 4);

                for (unsigned j = 0; j <= 15; j++, reg++) {
                    data = sx127x_reg_read(&sx127x, reg);
                    printf("%02X ", data);
                }
                puts("");
            }
            puts("-done-");
            return 0;
        }
        else if (strcmp(argv[2], "allinline") == 0) {
            puts("- listing all registers in one line -");
            /* Listing registers map */
            for (uint16_t reg = 0; reg < 256; reg++) {
                printf("%02X ", sx127x_reg_read(&sx127x, (uint8_t)reg));
            }
            puts("- done -");
            return 0;
        }
        else {
            long int num = 0;
            /* Register number in hex */
            if (strstr(argv[2], "0x") != NULL) {
                num = strtol(argv[2], NULL, 16);
            }
            else {
                num = atoi(argv[2]);
            }

            if (num >= 0 && num <= 255) {
                printf("[regs] 0x%02X = 0x%02X\n",
                       (uint8_t)num,
                       sx127x_reg_read(&sx127x, (uint8_t)num));
            }
            else {
                puts("regs: invalid register number specified");
                return -1;
            }
        }
    }
    else if (strstr(argv[1], "set") != NULL) {
        if (argc < 4) {
            puts("usage: register set <regnum> <value>");
            return -1;
        }

        long num, val;

        /* Register number in hex */
        if (strstr(argv[2], "0x") != NULL) {
            num = strtol(argv[2], NULL, 16);
        }
        else {
            num = atoi(argv[2]);
        }

        /* Register value in hex */
        if (strstr(argv[3], "0x") != NULL) {
            val = strtol(argv[3], NULL, 16);
        }
        else {
            val = atoi(argv[3]);
        }

        sx127x_reg_write(&sx127x, (uint8_t)num, (uint8_t)val);
    }
    else {
        puts("usage: register get <all | allinline | regnum>");
        return -1;
    }

    return 0;
}

int send_cmd(int argc, char **argv)
{
    if (argc <= 1) {
        puts("usage: send <payload>");
        return -1;
    }

    printf("sending \"%s\" payload (%u bytes)\n",
           argv[1], (unsigned)strlen(argv[1]) + 1);

    iolist_t iolist = {
        .iol_base = argv[1],
        .iol_len = (strlen(argv[1]) + 1)
    };

    netdev_t *netdev = &sx127x.netdev;

    if (netdev->driver->send(netdev, &iolist) == -ENOTSUP) {
        puts("Cannot send: radio is still transmitting");
    }

    return 0;
}

int sendhex_cmd(int argc, char **argv)
{
    if (argc <= 1) {
        puts("usage: sendhex <hexpayload>");
        return -1;
    }

    uint8_t payload[255]; /* Maximum payload size for LoRa */
    size_t len = convert_hex(payload, argv[1]);

    if (len == 0 && strlen(argv[1]) > 0) {
        puts("Erreur: charge utile hexadécimale invalide.");
        return -1;
    }

    printf("sending hex payload (%u bytes)\n", (unsigned)len);

    iolist_t iolist = {
        .iol_base = payload,
        .iol_len = len
    };

    netdev_t *netdev = &sx127x.netdev;

    if (netdev->driver->send(netdev, &iolist) == -ENOTSUP) {
        puts("Cannot send: radio is still transmitting");
    }

    return 0;
}

int listen_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    netdev_t *netdev = &sx127x.netdev;
    /* Switch to continuous listen mode */
    const netopt_enable_t single = false;

    netdev->driver->set(netdev, NETOPT_SINGLE_RECEIVE, &single, sizeof(single));
    const uint32_t timeout = 0;

    netdev->driver->set(netdev, NETOPT_RX_TIMEOUT, &timeout, sizeof(timeout));

    /* Switch to RX state */
    netopt_state_t state = NETOPT_STATE_RX;

    netdev->driver->set(netdev, NETOPT_STATE, &state, sizeof(state));

    printf("Listen mode set\n");

    return 0;
}

int syncword_cmd(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: syncword <get|set>");
        return -1;
    }

    netdev_t *netdev = &sx127x.netdev;
    uint8_t syncword;

    if (strstr(argv[1], "get") != NULL) {
        netdev->driver->get(netdev, NETOPT_SYNCWORD, &syncword,
                            sizeof(syncword));
        printf("Syncword: 0x%02x\n", syncword);
        return 0;
    }

    if (strstr(argv[1], "set") != NULL) {
        if (argc < 3) {
            puts("usage: syncword set <syncword>");
            return -1;
        }
        syncword = fmt_hex_byte(argv[2]);
        netdev->driver->set(netdev, NETOPT_SYNCWORD, &syncword,
                            sizeof(syncword));
        printf("Syncword set to %02x\n", syncword);
    }
    else {
        puts("usage: syncword <get|set>");
        return -1;
    }

    return 0;
}
int channel_cmd(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: channel <get|set>");
        return -1;
    }

    netdev_t *netdev = &sx127x.netdev;
    uint32_t chan;

    if (strstr(argv[1], "get") != NULL) {
        netdev->driver->get(netdev, NETOPT_CHANNEL_FREQUENCY, &chan,
                            sizeof(chan));
        printf("Channel: %i\n", (int)chan);
        return 0;
    }

    if (strstr(argv[1], "set") != NULL) {
        if (argc < 3) {
            puts("usage: channel set <channel>");
            return -1;
        }
        chan = atoi(argv[2]);
        netdev->driver->set(netdev, NETOPT_CHANNEL_FREQUENCY, &chan,
                            sizeof(chan));
        printf("New channel set\n");
    }
    else {
        puts("usage: channel <get|set>");
        return -1;
    }

    return 0;
}

int rx_timeout_cmd(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: channel <get|set>");
        return -1;
    }

    netdev_t *netdev = &sx127x.netdev;
    uint16_t rx_timeout;

    if (strstr(argv[1], "set") != NULL) {
        if (argc < 3) {
            puts("usage: rx_timeout set <rx_timeout>");
            return -1;
        }
        rx_timeout = atoi(argv[2]);
        netdev->driver->set(netdev, NETOPT_RX_SYMBOL_TIMEOUT, &rx_timeout,
                            sizeof(rx_timeout));
        printf("rx_timeout set to %i\n", rx_timeout);
    }
    else {
        puts("usage: rx_timeout set");
        return -1;
    }

    return 0;
}

int reset_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    netdev_t *netdev = &sx127x.netdev;

    puts("resetting sx127x...");
    netopt_state_t state = NETOPT_STATE_RESET;

    netdev->driver->set(netdev, NETOPT_STATE, &state, sizeof(netopt_state_t));
    return 0;
}

static void _set_opt(netdev_t *netdev, netopt_t opt, bool val, char *str_help)
{
    netopt_enable_t en = val ? NETOPT_ENABLE : NETOPT_DISABLE;

    netdev->driver->set(netdev, opt, &en, sizeof(en));
    printf("Successfully ");
    if (val) {
        printf("enabled ");
    }
    else {
        printf("disabled ");
    }
    printf("%s\n", str_help);
}

int crc_cmd(int argc, char **argv)
{
    netdev_t *netdev = &sx127x.netdev;

    if (argc < 3 || strcmp(argv[1], "set") != 0) {
        printf("usage: %s set <1|0>\n", argv[0]);
        return 1;
    }

    int tmp = atoi(argv[2]);

    _set_opt(netdev, NETOPT_INTEGRITY_CHECK, tmp, "CRC check");
    return 0;
}

int implicit_cmd(int argc, char **argv)
{
    netdev_t *netdev = &sx127x.netdev;

    if (argc < 3 || strcmp(argv[1], "set") != 0) {
        printf("usage: %s set <1|0>\n", argv[0]);
        return 1;
    }

    int tmp = atoi(argv[2]);

    _set_opt(netdev, NETOPT_FIXED_HEADER, tmp, "implicit header");
    return 0;
}

int payload_cmd(int argc, char **argv)
{
    netdev_t *netdev = &sx127x.netdev;

    if (argc < 3 || strcmp(argv[1], "set") != 0) {
        printf("usage: %s set <payload length>\n", argv[0]);
        return 1;
    }

    uint16_t tmp = atoi(argv[2]);

    netdev->driver->set(netdev, NETOPT_PDU_SIZE, &tmp, sizeof(tmp));
    printf("Successfully set payload to %i\n", tmp);
    return 0;
}

/* ================================================== *
 * RECEIVED MESSAGE HANDLERS (for handle_chat_message)
 * ================================================== */

static void handle_rdv_received(const char *sender, uint16_t msg_id, const char *payload) {
    /* Parse: RDV <frequency> <SFXBWY>
     * Example: RDV 868100000 SF7BW125
     */
    char payload_copy[128];
    strncpy(payload_copy, payload, sizeof(payload_copy) - 1);
    payload_copy[sizeof(payload_copy) - 1] = '\0';

    char *freq_str = payload_copy + 3;  /* skip "RDV" */
    while (*freq_str == ' ') freq_str++;

    char *sf_bw_str = strchr(freq_str, ' ');
    if (!sf_bw_str) {
        printf("[RDV] ERROR from %s: missing SF/BW specification\n", sender);
        return;
    }
    *sf_bw_str = '\0';
    sf_bw_str++;
    while (*sf_bw_str == ' ') sf_bw_str++;

    uint32_t frequency = (uint32_t)atol(freq_str);

    printf("\n✈️  [RDV] RENDEZVOUS from node %s (msg %u)\n", sender, msg_id);
    printf("    Frequency: %lu Hz (%.1f MHz)\n", frequency, frequency / 1000000.0);
    printf("    Configuration: %s\n", sf_bw_str);
    printf("    → TODO: Switch to freq %s with %s\n", freq_str, sf_bw_str);
}

static void handle_sos_received(const char *sender, uint16_t msg_id, const char *payload) {
    /* Parse: sos<latitude>,<longitude>
     * Example: sos3.86292,11.50003
     */
    char payload_copy[128];
    strncpy(payload_copy, payload, sizeof(payload_copy) - 1);
    payload_copy[sizeof(payload_copy) - 1] = '\0';

    char *coords = payload_copy + 3;  /* skip "sos" */
    
    char *comma = strchr(coords, ',');
    if (!comma) {
        printf("[SOS] ERROR from %s: invalid coordinate format\n", sender);
        return;
    }
    *comma = '\0';

    float latitude = (float)atof(coords);
    float longitude = (float)atof(comma + 1);

    /* Validate ranges */
    if (latitude < -90.0 || latitude > 90.0) {
        printf("[SOS] ERROR: invalid latitude %f\n", latitude);
        return;
    }
    if (longitude < -180.0 || longitude > 180.0) {
        printf("[SOS] ERROR: invalid longitude %f\n", longitude);
        return;
    }

    printf("\n🆘 [SOS] **DISTRESS SIGNAL** from node %s (msg %u)\n", sender, msg_id);
    printf("    Position: LAT=%.5f, LON=%.5f\n", latitude, longitude);
    printf("    *** URGENT: Node %s requires ASSISTANCE ***\n", sender);
    printf("    Location: https://maps.google.com/?q=%.5f,%.5f\n", latitude, longitude);
}

static void handle_lpp_received(const char *sender, uint16_t msg_id, const char *payload) {
    /* Parse: lpp[hex_string]
     * Two formats:
     * 1. lpp<hex> - pre-formatted LPP Cayenne payload
     * 2. lpp     - SAUL sensor request confirmation
     */
    char payload_copy[256];
    strncpy(payload_copy, payload, sizeof(payload_copy) - 1);
    payload_copy[sizeof(payload_copy) - 1] = '\0';

    char *lpp_data = payload_copy + 3;  /* skip "lpp" */

    printf("\n📊 [TELEMETRY] LPP Cayenne from node %s (msg %u)\n", sender, msg_id);

    if (*lpp_data == '\0') {
        /* Request for SAUL sensor reading */
        printf("    Type: SAUL sensor request\n");
        printf("    → Node %s is requesting sensor telemetry\n", sender);
        return;
    }

    /* Pre-formatted hex payload */
    size_t hex_len = strlen(lpp_data);
    printf("    Type: Pre-formatted LPP payload\n");
    printf("    Data size: %zu bytes (hex: %s)\n", hex_len / 2, lpp_data);
    printf("    Raw hex: %s\n", lpp_data);
    
    /* Provide hint about what might be in LPP format */
    printf("    LPP Cayenne format typically includes:\n");
    printf("      - Temperature sensors\n");
    printf("      - Humidity readings\n");
    printf("      - Accelerometer data (X, Y, Z)\n");
    printf("      - GPS position (latitude, longitude, altitude)\n");
    printf("      - And other sensor readings\n");
}

static void handle_chat_message(char *raw_msg, int8_t snr, uint8_t sf, uint8_t bw) {
    /*
     Copie de travail pour ne pas altérer le buffer original si besoin 
    char chat_buf[255];
    strncpy(chat_buf, raw_msg, sizeof(chat_buf));
    chat_buf[254] = '\0';

    char *sender = chat_buf;
    char *separator = strpbrk(chat_buf, "@#");
    if (!separator) return;  Ce n'est pas un message LoRaChat valide 

    char type = *separator;  '@' (direct/all) ou '#' (salon) 
    *separator = '\0';
    char *target = separator + 1;

    char *colon1 = strchr(target, ':');
    if (!colon1) return;
    *colon1 = '\0';
    char *msg_id_str = colon1 + 1;

    char *colon2 = strchr(msg_id_str, ':');
    if (!colon2) return;
    *colon2 = '\0';
    char *payload = colon2 + 1;

     Affichage formaté ₍ᐢ֎ﻌ֍ᐢ₎ʃ 
    printf("\n₍ᐢ֎ﻌ֍ᐢ₎ʃ [LoRaChat] De: %s | Pour: %c%s | ID: %s \n> %s\n", 
           sender, type, target, msg_id_str, payload);

     --- GESTION DES RÉACTIONS --- 
    
    Règle 1: "Qui est la?" 
    if (strcmp(payload, "Qui est la?") == 0) {
        if (strcmp(sender, MY_NODE_ID) != 0) { // Ne pas se répondre à soi-même
            printf("[LoRaChat] -> Ping reçu, préparation de la réponse...\n");
            
            char reply[255];
            snprintf(reply, sizeof(reply), "%s@%.16s:%d:Je suis la !", 
                     MY_NODE_ID, sender, current_msg_id++);
            
            iolist_t iolist = { .iol_base = reply, .iol_len = strlen(reply) + 1 };
            netdev_t *netdev = &sx127x.netdev;
            netdev->driver->send(netdev, &iolist);
            printf("[LoRaChat] -> Réponse envoyée: %s\n", reply);
        }
    }
     Règle 2: "RDV" (À implémenter plus tard) 
    else if (strncmp(payload, "RDV", 3) == 0) {
        printf("[LoRaChat] -> Ordre de saut de fréquence détecté (non exécuté).\n");
    }
        */

    char chat_buf[255];
    strncpy(chat_buf, raw_msg, sizeof(chat_buf) - 1);
    chat_buf[254] = '\0';

    /* -------------------------------------------------- *
     * Parse header: <emitter><type><target>:<msg_id>,<ttl>:<payload>
     * Format:   NODE1#salon:42,7:Hello world
      - emitter: NODE1
      - type: '#' (salon), '@' (direct/all), '*' (broadcast)
      - target: salon name or peer node ID (ignored for '*')
      - msg_id: 16-bit integer message ID
      - ttl: 8-bit integer time-to-live (hops remaining)
      - payload: message content
     * -------------------------------------------------- */

    char *sender = chat_buf;

    /* 1. Find type character ('#', '@', '*') — ends the emitter field */
    char *separator = strpbrk(chat_buf, "@#*");
    if (!separator) return;

    char type = *separator;
    *separator = '\0';
    char *target = separator + 1;

    /* 2. First colon — ends the target, begins msg_id,ttl:payload */
    char *colon1 = strchr(target, ':');
    if (!colon1) return;
    *colon1 = '\0';
    char *after_target = colon1 + 1;

    /* 3. Parse msg_id,ttl:payload format
     *    Format: <msg_id>,<ttl>:<payload>
     *    Example: 42,7:Hello world */
    
    char *comma = strchr(after_target, ',');
    if (!comma) return;
    *comma = '\0';
    
    uint16_t msg_id = (uint16_t)atoi(after_target);
    
    char *after_msg_id = comma + 1;
    char *colon2 = strchr(after_msg_id, ':');
    if (!colon2) return;
    *colon2 = '\0';
    
    uint8_t ttl = (uint8_t)atoi(after_msg_id);
    char *payload = colon2 + 1;

    /* -------------------------------------------------- *
     * Node table — update as soon as we have a valid frame
     * (do this regardless of whether we display or react)
     * -------------------------------------------------- */
    if (sender[0] != '\0') {
        node_table_update(sender, msg_id);
    }

    /* -------------------------------------------------- *
     * Display with SNR and TTL info
     * -------------------------------------------------- */
    printf("\n₍ᐢ֎ﻌ֍ᐢ₎ʃ [LoRaChat] De: %s | Pour: %c%s | ID: %u | TTL: %u | SNR: %d dB\n> %s\n",
           sender, type, target, msg_id, ttl, snr, payload ? payload : "[payload unavailable]");

     /* -------------------------------------------------- *
     * Relay Management: Queue message if TTL > 0 and not from us
     * -------------------------------------------------- */
    if (strcmp(sender, MY_NODE_ID) != 0) {
        /* Check if we should relay this message */
        int relay_result = queue_for_relay(raw_msg, ttl, snr, sf, bw);
        msg_status_t status = (relay_result > 0) ? MSG_STATUS_RELAYED : 
                             (relay_result == 0) ? MSG_STATUS_NOT_RELAYED : MSG_STATUS_DROPPED;
        
        /* Add to history with relay status */
        if (payload != NULL) {
            add_to_history(sender, type, target, payload, msg_id, ttl, snr, status);
        }
    }
    /* -------------------------------------------------- *
     * Reaction rules (guard: never react to own messages)
     * -------------------------------------------------- */
    if (strcmp(sender, MY_NODE_ID) == 0) return;

    if (payload == NULL || payload[0] == '\0') return;

    /* Rule 1: ping */
    if (strcmp(payload, "Qui est la?") == 0) {
        printf("[LoRaChat] -> Ping received, preparing response...\n");

        char reply[512];
        snprintf(reply, sizeof(reply), "%s@%s:%u,%d:Je suis la !",
                 MY_NODE_ID, sender, current_msg_id++, CHAT_TTL_DEFAULT);

        iolist_t iolist = { .iol_base = reply, .iol_len = strlen(reply) + 1 };
        netdev_t *netdev = &sx127x.netdev;
        netdev->driver->send(netdev, &iolist);
        printf("[LoRaChat] -> Response sent: %s\n", reply);
    }
    /* Rule 2: RDV (Rendezvous/Frequency hop) */
    else if (strncmp(payload, "RDV ", 4) == 0) {
        handle_rdv_received(sender, msg_id, payload);
    }
    /* Rule 3: SOS (Distress signal with coordinates) */
    else if (strncmp(payload, "sos", 3) == 0) {
        handle_sos_received(sender, msg_id, payload);
    }
    /* Rule 4: LPP Telemetry (Cayenne Low Power Payload) */
    else if (strncmp(payload, "lpp", 3) == 0) {
        handle_lpp_received(sender, msg_id, payload);
    }
}

int chat_cmd(int argc, char **argv) {
    if (argc < 3) {
        puts("usage: chat <#salon|@peer|*> <message...> [ttl=N]");
        puts("  Regular message:");
        puts("    chat #frblabla Salut tout le monde");
        puts("    chat @node42 Hello ttl=3");
        puts("  RDV (rendezvous):");
        puts("    chat * RDV 868100000 SF7BW125");
        puts("  SOS (distress):");
        puts("    chat * sos 3.86292 11.50003");
        puts("  Telemetry (LPP Cayenne):");
        puts("    chat * lpp 03670110056700ff...");
        puts("    chat * lpp");
        return -1;
    }

    /* --- Parse optional keyword args and collect payload --- */
    int ttl = CHAT_TTL_DEFAULT;
    char payload[256] = {0};
    int msg_type = 0;  /* 0=normal, 1=RDV, 2=SOS, 3=LPP */

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "ttl=", 4) == 0) {
            ttl = atoi(argv[i] + 4);
            if (ttl <= 0 || ttl > 255) ttl = CHAT_TTL_DEFAULT;
        } else {
            if (payload[0]) strncat(payload, " ", sizeof(payload) - strlen(payload) - 1);
            strncat(payload, argv[i], sizeof(payload) - strlen(payload) - 1);
        }
    }

    if (payload[0] == '\0') {
        puts("error: no message content");
        return -1;
    }

    /* --- Detect and parse special message types --- */
    char formatted_payload[256] = {0};

    if (strncmp(payload, "RDV ", 4) == 0) {
        /* RDV <frequency> <SFXBWY> */
        msg_type = 1;
        char freq_str[32] = {0};
        char sf_bw_str[32] = {0};
        
        if (sscanf(payload + 4, "%31s %31s", freq_str, sf_bw_str) != 2) {
            puts("error: RDV requires frequency and SF/BW");
            puts("  usage: RDV <frequency_hz> <SFXBWY>");
            puts("  ex: RDV 868100000 SF7BW125");
            return -1;
        }

        uint32_t freq = (uint32_t)atol(freq_str);
        if (freq < 860000000 || freq > 870000000) {
            puts("error: frequency out of range (860-870 MHz)");
            return -1;
        }

        if (strncmp(sf_bw_str, "SF", 2) != 0) {
            printf("error: invalid SF/BW format: %s\n", sf_bw_str);
            puts("  expected format: SFXBWY (e.g., SF7BW125)");
            return -1;
        }

        snprintf(formatted_payload, sizeof(formatted_payload), "RDV %s %s", 
                 freq_str, sf_bw_str);
        printf("[RDV] Parsing: %s\n", formatted_payload);

    } else if (strncmp(payload, "sos ", 4) == 0) {
        /* sos <latitude> <longitude> */
        msg_type = 2;
        char lat_str[32] = {0};
        char lon_str[32] = {0};
        
        if (sscanf(payload + 4, "%31s %31s", lat_str, lon_str) != 2) {
            puts("error: SOS requires latitude and longitude");
            puts("  usage: sos <latitude> <longitude>");
            puts("  ex: sos 3.86292 11.50003");
            return -1;
        }

        float lat = (float)atof(lat_str);
        float lon = (float)atof(lon_str);

        if (lat < -90.0 || lat > 90.0) {
            printf("error: invalid latitude: %f (range: -90 to 90)\n", lat);
            return -1;
        }

        if (lon < -180.0 || lon > 180.0) {
            printf("error: invalid longitude: %f (range: -180 to 180)\n", lon);
            return -1;
        }

        snprintf(formatted_payload, sizeof(formatted_payload), "sos%s,%s", 
                 lat_str, lon_str);

    } else if (strncmp(payload, "lpp", 3) == 0) {
        /* lpp [hex_payload] */
        msg_type = 3;
        
        if (strlen(payload) == 3) {
            /* Just "lpp" - request SAUL sensors */
            snprintf(formatted_payload, sizeof(formatted_payload), "lpp");
            printf("[TELEMETRY] Parsing: LPP SAUL sensor request\n");
        } else if (payload[3] == ' ') {
            /* "lpp <hex_string>" - pre-formatted LPP data */
            char *hex_data = payload + 4;
            
            /* Validate hex string (even number of chars, only 0-9a-fA-F) */
            size_t hex_len = strlen(hex_data);
            if (hex_len % 2 != 0) {
                puts("error: hex payload must have even number of characters");
                return -1;
            }
            
            for (size_t i = 0; i < hex_len; i++) {
                char c = hex_data[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    printf("error: invalid hex character: %c\n", c);
                    return -1;
                }
            }

            snprintf(formatted_payload, sizeof(formatted_payload), "lpp%s", hex_data);
            printf("[TELEMETRY] Parsing: LPP hex payload (%zu bytes)\n", hex_len / 2);
        } else {
            puts("error: invalid lpp format");
            puts("  usage: lpp [hex_payload]");
            puts("  ex: lpp 03670110056700");
            puts("  ex: lpp  (request SAUL sensors)");
            return -1;
        }
    } else {
        /* Regular message */
        msg_type = 0;
        strncpy(formatted_payload, payload, sizeof(formatted_payload) - 1);
    }

    /* --- Determine target type and name --- */
    char type[3] = "@";
    char *target = "";

    if (argv[1][0] == '#') {
        strcpy(type, "#");
        target = argv[1] + 1;           /* strip '#' */
    } else if (argv[1][0] == '@') {
        strcpy(type, "@");
        target = argv[1] + 1;           /* strip '@' */
    } else if (argv[1][0] == '*') {
        strcpy(type, "@*");
        target = "";                     /* broadcast */
    } else {
        puts("error: target must start with #, @, or *");
        return -1;
    }

    if (strcmp(type, "#") == 0 && !is_in_salon(target)){
        printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ [Erreur] Cannot send: You are not a member of salon #%s. Use 'join %s' first.\n", target, target);
        return -1; // drop message
    }

    /* --- Build frame ---
     * Format: <emitter><type><target>:<msg_id>,<ttl>:<payload>
     * Example: 6767@node1:42,7:RDV 868100000 SF7BW125
     */
    char chat_buf[CHAT_BUF_SIZE];
    int len = snprintf(chat_buf, sizeof(chat_buf),
                       "%s%s%s:%u,%d:%s",
                       MY_NODE_ID,
                       type,
                       target,
                       current_msg_id,
                       ttl,
                       formatted_payload);

    if (len < 0 || len >= (int)sizeof(chat_buf)) {
        puts("error: message too long");
        return -1;
    }

    current_msg_id++;

    /* --- Send --- */
    iolist_t iolist = { .iol_base = chat_buf, .iol_len = (size_t)len + 1 };
    netdev_t *netdev = &sx127x.netdev;

    if (netdev->driver->send(netdev, &iolist) == -ENOTSUP) {
        puts("error: radio still transmitting");
        return -1;
    }

    switch (msg_type) {
        case 1:
            printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ [RDV] sent to %s [ttl=%d id=%u]\n", 
                   argv[1], ttl, current_msg_id - 1);
            break;
        case 2:
            printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ [SOS] sent to %s [ttl=%d id=%u]\n", 
                   argv[1], ttl, current_msg_id - 1);
            break;
        case 3:
            printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ [TELEMETRY] sent to %s [ttl=%d id=%u]\n", 
                   argv[1], ttl, current_msg_id - 1);
            break;
        default:
            printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ sent to %s [ttl=%d id=%u]: %s\n",
                   argv[1], ttl, current_msg_id - 1, formatted_payload);
            break;
    }
    printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ Frame is: %s\n",chat_buf);

    return 0;
}

static void _event_cb(netdev_t *dev, netdev_event_t event)
{
    if (event == NETDEV_EVENT_ISR) {
        msg_t msg;

        msg.type = MSG_TYPE_ISR;
        msg.content.ptr = dev;

        if (msg_send(&msg, _recv_pid) <= 0) {
            puts("gnrc_netdev: possibly lost interrupt.");
        }
    }
    else {
        size_t len;
        netdev_lora_rx_info_t packet_info;
        switch (event) {
        case NETDEV_EVENT_RX_STARTED:
            puts("Data reception started");
            break;

        case NETDEV_EVENT_RX_COMPLETE:
            len = dev->driver->recv(dev, NULL, 0, 0);
            dev->driver->recv(dev, message, len, &packet_info);
            
            printf("\n--- Nouveau Paquet (%d bytes) ---\n", (int)len);
            printf("RSSI: %i, SNR: %i, TOA: %" PRIu32 " ms\n", 
                   packet_info.rssi, (int)packet_info.snr, 
                   sx127x_get_time_on_air((const sx127x_t *)dev, len));
            
            /* 1. Affichage Hexadécimal (toujours fiable) */
            printf("HEX: ");
            for(size_t i = 0; i < len; i++) {
                printf("%02X ", (unsigned char)message[i]);
            }
            printf("\n");

            /* 2. Affichage Texte sécurisé (remplace les caractères non-imprimables par un point) */
            printf("TXT: ");
            for(size_t i = 0; i < len; i++) {
                unsigned char c = message[i];
                if (c >= 32 && c <= 126) {
                    printf("%c", c);
                } else {
                    printf(".");
                }
            }
            printf("\n--------------------------------\n");

            /* Get current LoRa parameters for relay delay calculation */
            uint8_t sf = 7, bw = LORA_BW_125_KHZ;
            netdev_t *netdev = dev;
            netdev->driver->get(netdev, NETOPT_SPREADING_FACTOR, &sf, sizeof(sf));
            netdev->driver->get(netdev, NETOPT_BANDWIDTH, &bw, sizeof(bw));

            /* Appel de notre analyseur LoRaChat avec SNR, SF, BW pour relay management */
            handle_chat_message(message, (int8_t)packet_info.snr, sf, bw);
            break;

        case NETDEV_EVENT_TX_COMPLETE:
            sx127x_set_sleep(&sx127x);
            puts("Transmission completed");
            break;

        case NETDEV_EVENT_CAD_DONE:
            break;

        case NETDEV_EVENT_TX_TIMEOUT:
            sx127x_set_sleep(&sx127x);
            break;

        default:
            printf("Unexpected netdev event received: %d\n", event);
            break;
        }
    }
}

void *_recv_thread(void *arg)
{
    (void)arg;

    static msg_t _msg_q[SX127X_LORA_MSG_QUEUE];

    msg_init_queue(_msg_q, SX127X_LORA_MSG_QUEUE);

    while (1) {
        msg_t msg;
        msg_receive(&msg);
        if (msg.type == MSG_TYPE_ISR) {
            netdev_t *dev = msg.content.ptr;
            dev->driver->isr(dev);
        }
        else {
            puts("Unexpected msg type");
        }
    }
}

void *_telemetry_thread(void *arg) {
    (void)arg;
    xtimer_ticks32_t last_wakeup = xtimer_now();

    while (1) {
        if (telemetry_interval > 0) {
            uint8_t lpp_payload[51]; 
            uint8_t lpp_len = 0;
            saul_reg_t *dev = saul_reg;
            uint8_t channel = 0;

            if (dev == NULL) {
                puts("No SAUL devices present");
                //return NULL;
            }

            while (dev) {
                phydat_t res;
                int dim = saul_reg_read(dev, &res);

                if (dim > 0) {
                    float actual_value = (float)res.val[0] * pow(10, res.scale);
                    
                    if (dev->driver->type == SAUL_SENSE_TEMP && (lpp_len + 4U <= sizeof(lpp_payload))) {
                        int16_t lpp_temp = (int16_t)(actual_value * 10.0f); 
                        lpp_payload[lpp_len++] = channel++;                       
                        lpp_payload[lpp_len++] = 103; /* LPP Temp Type */                           
                        lpp_payload[lpp_len++] = (uint8_t)((lpp_temp >> 8) & 0xFF); 
                        lpp_payload[lpp_len++] = (uint8_t)(lpp_temp & 0xFF);        
                    } 
                    else if (dev->driver->type == SAUL_SENSE_HUM && (lpp_len + 3U <= sizeof(lpp_payload))) {
                        uint8_t lpp_hum = (uint8_t)(actual_value * 2.0f);
                        lpp_payload[lpp_len++] = channel++;                       
                        lpp_payload[lpp_len++] = 104; /* LPP Hum Type */                         
                        lpp_payload[lpp_len++] = lpp_hum;                         
                    }
                }
                dev = dev->next;
            }

            if (lpp_len > 0) {
                iolist_t iolist = { .iol_base = lpp_payload, .iol_len = lpp_len };
                netdev_t *netdev = &sx127x.netdev;
                
                if (netdev->driver->send(netdev, &iolist) != -ENOTSUP) {
                    printf("\n₍ᐢ֎ﻌ֍ᐢ₎ʃ [Auto-Telemetry] Sent LPP packet (%d bytes)\n", lpp_len);
                }
            }
        }

        /* Sleep. If interval is 0, sleep 1 sec to avoid freezing the CPU */
        uint32_t sleep_time = (telemetry_interval > 0) ? telemetry_interval : (1LU * US_PER_SEC);
        xtimer_periodic_wakeup(&last_wakeup, sleep_time);
    }
    return NULL;
}

void *_relay_thread(void *arg) {
    /* Thread that processes relay queue - waits for delay then resends messages */
    (void)arg;
    
    while (1) {
        /* Check relay queue every 50ms */
        xtimer_usleep(50 * 1000);  /* 50ms = 50000 microseconds */
        
        if (relay_queue_count == 0) {
            continue;  /* Nothing to do */
        }
        
        uint32_t now_ms = xtimer_now_usec() / 1000;
        int i = 0;
        
        /* Process all messages in queue */
        while (i < relay_queue_count) {
            relay_msg_t *msg = &relay_queue[i];
            uint32_t time_elapsed = now_ms - msg->queued_at_ms;
            
            /* Check if delay has expired */
            if (time_elapsed >= msg->relay_delay_ms) {
                printf("[RELAY] Delay expired (waited %lu ms), resending with TTL=%u\n", 
                       time_elapsed, msg->ttl);
                
                /* Reconstruct message with new TTL */
                char modified_msg[255];
                char msg_copy[255];
                strncpy(msg_copy, msg->raw_msg, sizeof(msg_copy) - 1);
                msg_copy[sizeof(msg_copy) - 1] = '\0';
                
                /* Parse and rebuild with new TTL
                 * Format: <emitter><type><target>:<msg_id>,<ttl>:<payload>
                 * We need to find and replace the TTL field */
                char *sender = msg_copy;
                char *separator = strpbrk(msg_copy, "@#*");
                if (separator) {
                    char type = *separator;
                    *separator = '\0';
                    char *target = separator + 1;
                    
                    char *colon1 = strchr(target, ':');
                    if (colon1) {
                        *colon1 = '\0';
                        char *after_target = colon1 + 1;
                        
                        char *comma = strchr(after_target, ',');
                        if (comma) {
                            *comma = '\0';
                            uint16_t msg_id = (uint16_t)atoi(after_target);
                            
                            char *colon2 = strchr(comma + 1, ':');
                            if (colon2) {
                                char *payload = colon2 + 1;
                                
                                /* Build new message with decremented TTL */
                                int len = snprintf(modified_msg, sizeof(modified_msg),
                                                 "%s%c%s:%u,%u:%s",
                                                 sender, type, target, msg_id, msg->ttl, payload);
                                
                                if (len > 0 && len < (int)sizeof(modified_msg)) {
                                    /* Send the relayed message */
                                    iolist_t iolist = { 
                                        .iol_base = modified_msg, 
                                        .iol_len = (size_t)len + 1 
                                    };
                                    netdev_t *netdev = &sx127x.netdev;
                                    
                                    if (netdev->driver->send(netdev, &iolist) != -ENOTSUP) {
                                        printf("[RELAY] ✓ Relayed message from %s (TTL: %u)\n", sender, msg->ttl);
                                    } else {
                                        printf("[RELAY] ✗ Send failed (radio busy)\n");
                                        i++;  /* Skip to next, retry later would need timer */
                                        continue;
                                    }
                                }
                            }
                        }
                    }
                }
                
                /* Remove from queue by shifting everything down */
                for (int j = i; j < relay_queue_count - 1; j++) {
                    relay_queue[j] = relay_queue[j + 1];
                }
                relay_queue_count--;
                
                /* Don't increment i, check same position again (now has next message) */
            } else {
                i++;  /* Move to next message */
            }
        }
    }
    
    return NULL;
}


int autotelm_cmd(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: autotelm <seconds>");
        puts("  ex: autotelm 10   (sends data every 10 seconds)");
        puts("  ex: autotelm 0    (stops sending)");
        return -1;
    }

    int seconds = atoi(argv[1]);

    if (seconds <= 0) {
        telemetry_interval = 0;
        puts("₍ᐢ֎ﻌ֍ᐢ₎ʃ Auto-telemetry disabled.");
    } else {
        telemetry_interval = (uint32_t)seconds * US_PER_SEC;
        printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ Auto-telemetry enabled: sending every %d seconds.\n", seconds);
    }
    return 0;
}



int init_sx1272_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
	    sx127x.params = sx127x_params[0];
	    netdev_t *netdev = &sx127x.netdev;

	    netdev->driver = &sx127x_driver;

        netdev->event_callback = _event_cb;

//        printf("%8x\n", (unsigned int)netdev->driver);
//        printf("%8x\n", (unsigned int)netdev->driver->init);

	    if (netdev->driver->init(netdev) < 0) {
	        puts("Failed to initialize SX127x device, exiting");
	        return 1;
	    }

	    _recv_pid = thread_create(stack, sizeof(stack), THREAD_PRIORITY_MAIN - 1,
	                              THREAD_CREATE_STACKTEST, _recv_thread, NULL,
	                              "recv_thread");

	    if (_recv_pid <= KERNEL_PID_UNDEF) {
	        puts("Creation of receiver thread failed");
	        return 1;
	    }

        // for autotelem
        kernel_pid_t _telm_pid = thread_create(telemetry_stack, sizeof(telemetry_stack), 
                                  THREAD_PRIORITY_MAIN - 2, 
                                  THREAD_CREATE_STACKTEST, _telemetry_thread, NULL,
                                  "telemetry_thread");

        if (_telm_pid <= KERNEL_PID_UNDEF) {
            puts("Creation of telemetry thread failed");
            return 1;
        }

         // for relay queue processing
        kernel_pid_t _relay_pid = thread_create(relay_stack, sizeof(relay_stack),
                                  THREAD_PRIORITY_MAIN - 2,
                                  THREAD_CREATE_STACKTEST, _relay_thread, NULL,
                                  "relay_thread");

        if (_relay_pid <= KERNEL_PID_UNDEF) {
            puts("Creation of relay thread failed");
            return 1;
        }

        puts("5");

        return 0;
}

int nodes_cmd(int argc, char **argv)
{
    (void)argv;

    if (argc > 1) {
        puts("usage: nodes");
        return -1;
    }

    puts("Known nodes/users (last msg id):");
    int any = 0;

    for (int i = 0; i < NODE_TABLE_SIZE; i++) {
        if (!node_table[i].active) {
            continue;
        }

        any = 1;
        printf(" - %s : %u\n",
               node_table[i].node_id,
               (unsigned)node_table[i].last_msg_id);
    }

    if (!any) {
        puts(" (none)");
    }

    return 0;
}

int join_cmd(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: join <salon_name>");
        return -1;
    }

    /* Strip '#' if the user typed it */
    char *s_name = argv[1];
    if (s_name[0] == '#') {
        s_name++;
    }

    if (is_in_salon(s_name)) {
        printf("You are already in salon #%s\n", s_name);
        return 0;
    }

    for (int i = 0; i < MAX_SALONS; i++) {
        if (!my_salons[i].active) {
            strncpy(my_salons[i].name, s_name, SALON_NAME_MAXLEN - 1);
            my_salons[i].name[SALON_NAME_MAXLEN - 1] = '\0';
            my_salons[i].active = 1;
            printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ Joined salon: #%s\n", my_salons[i].name);
            return 0;
        }
    }

    puts("[Error] Cannot join: Max salons reached.");
    return -1;
}

int leave_cmd(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: leave <salon_name>");
        return -1;
    }

    char *s_name = argv[1];
    if (s_name[0] == '#') {
        s_name++;
    }

    for (int i = 0; i < MAX_SALONS; i++) {
        if (my_salons[i].active && 
            strncmp(my_salons[i].name, s_name, SALON_NAME_MAXLEN) == 0) {
            my_salons[i].active = 0;
            printf("₍ᐢ֎ﻌ֍ᐢ₎ʃ Left salon: #%s\n", s_name);
            return 0;
        }
    }

    printf("You are not in salon #%s\n", s_name);
    return 0;
}

int salons_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;

    puts("Active Salons:");
    int any = 0;

    for (int i = 0; i < MAX_SALONS; i++) {
        if (my_salons[i].active) {
            printf(" - #%s\n", my_salons[i].name);
            any = 1;
        }
    }

    if (!any) {
        puts(" (none)");
    }
    return 0;
}

int history_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (history_count == 0) {
        puts("Message history is empty.");
        return 0;
    }

    printf("--- Last %d Messages ---\n", history_count);
    
    /* To read oldest to newest, start at (head - count) */
    int index = (history_head - history_count + HISTORY_MAX) % HISTORY_MAX;

    for (int i = 0; i < history_count; i++) {
         const char *status_str;
        switch (msg_history[index].status) {
            case MSG_STATUS_RELAYED:
                status_str = "✓RELAYED";
                break;
            case MSG_STATUS_NOT_RELAYED:
                status_str = "✗NO_RELAY";
                break;
            case MSG_STATUS_DROPPED:
                status_str = "⊘DROP";
                break;
            default:
                status_str = "?UNKNOWN";
        }
        
        printf("[%d] From: %s->%c%s | ID:%u | TTL:%u | SNR:%d dB | %s\n", 
            i + 1,
            msg_history[index].sender, 
            msg_history[index].type, 
            msg_history[index].target,
            msg_history[index].msg_id,
            msg_history[index].ttl,
            msg_history[index].snr,
            status_str);
        printf("     Payload: %s\n", msg_history[index].payload);
        
        index = (index + 1) % HISTORY_MAX;
    }
    
    puts("-------------------------");
    return 0;
}

int snr_threshold_cmd(int argc, char **argv) {
    /* Get/Set SNR threshold for relay filtering
     * usage: snr_threshold [get|set <value>]
     * If SNR of received message is ABOVE this threshold, node won't relay
     */
    if (argc < 2) {
        puts("usage: snr_threshold <get|set <value>>");
        puts("  snr_threshold get        - Show current SNR threshold");
        puts("  snr_threshold set <dB>   - Set threshold (higher SNR = relay skip)");
        puts("  Example: snr_threshold set 5  (skip relay if SNR > 5 dB)");
        return -1;
    }

    if (strcmp(argv[1], "get") == 0) {
        printf("Current SNR threshold: %d dB\n", snr_threshold);
        printf("Messages with SNR > %d dB will NOT be relayed\n", snr_threshold);
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            puts("usage: snr_threshold set <value_in_dB>");
            return -1;
        }

        int new_threshold = atoi(argv[2]);
        
        /* Reasonable SNR range for LoRa (roughly -20 to +20 dB) */
        if (new_threshold < -30 || new_threshold > 30) {
            printf("Warning: threshold %d dB is outside typical LoRa SNR range\n", new_threshold);
        }

        snr_threshold = (int8_t)new_threshold;
        printf("SNR threshold set to %d dB\n", snr_threshold);
        printf("Messages with SNR > %d dB will be skipped (sender had good signal)\n", snr_threshold);
        return 0;
    }

    puts("Unknown command. usage: snr_threshold <get|set <value>>");
    return -1;
}

int relayq_cmd(int argc, char **argv) {
    /* Display relay queue status - messages pending relay */
    (void)argc;
    (void)argv;

    printf("\n=== RELAY QUEUE STATUS ===\n");
    printf("Queue size: %u/%u messages pending\n", relay_queue_count, RELAY_QUEUE_MAX);

    if (relay_queue_count == 0) {
        puts("No messages in relay queue");
        return 0;
    }

    puts("\nPending relay messages:");
    for (uint8_t i = 0; i < relay_queue_count; i++) {
        relay_msg_t *msg = &relay_queue[i];
        printf("[%u] Delay: %lu ms | TTL→%u | SNR: %d dB\n", 
               i + 1, msg->relay_delay_ms, msg->ttl, msg->snr);
        printf("     Raw: %s\n", msg->raw_msg);
    }

    printf("\n=== RELAY SETTINGS ===\n");
    printf("SNR Threshold: %d dB (skip relay if SNR > threshold)\n", snr_threshold);
    printf("Max queue size: %u messages\n", RELAY_QUEUE_MAX);

    return 0;
}


static const shell_command_t shell_commands[] = {
	{ "init",    "Initialize SX1272",     					init_sx1272_cmd },
	{ "setup",    "Initialize LoRa modulation settings",     lora_setup_cmd },
    { "implicit", "Enable implicit header",                  implicit_cmd },
    { "crc",      "Enable CRC",                              crc_cmd },
    { "payload",  "Set payload length (implicit header)",    payload_cmd },
    { "random",   "Get random number from sx127x",           random_cmd },
    { "syncword", "Get/Set the syncword",                    syncword_cmd },
    { "rx_timeout", "Set the RX timeout",                    rx_timeout_cmd },
    { "channel",  "Get/Set channel frequency (in Hz)",       channel_cmd },
    { "register", "Get/Set value(s) of registers of sx127x", register_cmd },
    { "send",     "Send raw payload string",                 send_cmd },
    { "sendhex",    "Send hex payload string",                 sendhex_cmd },
    { "chat",       "Envoyer un message LoRaChat",              chat_cmd },
    { "listen",   "Start raw payload listener",              listen_cmd },
    { "nodes",    "List known nodes/users and last msg id",  nodes_cmd },
    { "users",    "Alias of nodes",                          nodes_cmd },
    { "reset",    "Reset the sx127x device",                 reset_cmd },
    { "join",     "Join a salon (ex: join #frblabla)",      join_cmd },
    { "leave",    "Leave a salon (ex: leave #frblabla)",    leave_cmd },
    { "salons",   "List joined salons",                     salons_cmd },
    { "autotelm",   "Set auto telemetry interval (0 to stop)", autotelm_cmd },
    { "history",    "Show last received messages (FIFO)",      history_cmd },
    { "snr_threshold", "Get/Set SNR threshold for relay",     snr_threshold_cmd },
    { "relayq",   "Show relay queue status and settings",   relayq_cmd },
    { NULL, NULL, NULL }
};

int main(void) {

    //init_sx1272_cmd(0,NULL);

    /* start the shell */
    puts("Initialization successful - starting the shell now");
    char line_buf[SHELL_DEFAULT_BUFSIZE];

    shell_run(shell_commands, line_buf, SHELL_DEFAULT_BUFSIZE);

    return 0;
}

