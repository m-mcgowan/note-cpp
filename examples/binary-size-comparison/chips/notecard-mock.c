// Mock Notecard — Wokwi custom chip for AVR integration testing.
//
// Receives JSON or JSONB requests over UART, responds in the same format.
// Handles the NotecardSerial reset handshake (bare \n → echo \r\n).

#include "wokwi-api.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#define BUF_SIZE 512

// ── JSONB opcodes and framing ──────────────────────────────────────────────

#define JSONB_BEGIN_OBJECT  0x10
#define JSONB_END_OBJECT    0x11
#define JSONB_NULL          0x20
#define JSONB_TRUE          0x21
#define JSONB_FALSE         0x22
#define JSONB_ITEM          0x30
#define JSONB_STRING        0x40
#define JSONB_INT32         0x64
#define JSONB_DOUBLE        0x88
#define JSONB_COBS_XOR      '\n'

static uint32_t cobs_decode(uint8_t *ptr, uint32_t length, uint8_t xor_byte, uint8_t *dst) {
    const uint8_t *start = dst, *end = ptr + length;
    uint8_t code = 0xFF, copy = 0;
    for (; ptr < end; copy--) {
        if (copy != 0) {
            *dst++ = (*ptr++) ^ xor_byte;
        } else {
            if (code != 0xFF) *dst++ = 0;
            copy = code = (*ptr++) ^ xor_byte;
            if (code == 0) break;
        }
    }
    return (uint32_t)(dst - start);
}

static uint32_t cobs_encode(uint8_t *ptr, uint32_t length, uint8_t xor_byte, uint8_t *dst) {
    uint8_t *start = dst;
    uint8_t code = 1;
    uint8_t *code_ptr = dst++;
    while (length--) {
        uint8_t ch = *ptr++;
        if (ch != 0) { *dst++ = ch ^ xor_byte; code++; }
        if (ch == 0 || code == 0xFF) { *code_ptr = code ^ xor_byte; code = 1; code_ptr = dst++; }
    }
    *code_ptr = code ^ xor_byte;
    return (uint32_t)(dst - start);
}

// Find the value of the "req" field in decoded JSONB opcodes.
// Returns pointer to the null-terminated string, or NULL if not found.
static const char* jsonb_find_req(const uint8_t *buf, uint32_t len) {
    uint32_t pos = 0;
    while (pos < len) {
        uint8_t op = buf[pos++];
        switch (op) {
        case JSONB_BEGIN_OBJECT:
        case JSONB_END_OBJECT:
        case JSONB_TRUE:
        case JSONB_FALSE:
        case JSONB_NULL:
            break;
        case JSONB_ITEM: {
            const char *name = (const char *)&buf[pos];
            // Skip past null-terminated name
            while (pos < len && buf[pos] != 0) pos++;
            if (pos >= len) return NULL;
            pos++;  // skip null terminator
            if (pos >= len) return NULL;
            // Read value opcode
            uint8_t vop = buf[pos++];
            if (vop == JSONB_STRING) {
                const char *val = (const char *)&buf[pos];
                if (strcmp(name, "req") == 0) return val;
                // Skip past string value
                while (pos < len && buf[pos] != 0) pos++;
                pos++;  // null
            } else if (vop == JSONB_INT32) { pos += 4; }
            else if (vop == JSONB_DOUBLE) { pos += 8; }
            else if (vop == JSONB_BEGIN_OBJECT) {
                // Nested object — skip by counting depth
                int depth = 1;
                while (pos < len && depth > 0) {
                    if (buf[pos] == JSONB_BEGIN_OBJECT) depth++;
                    else if (buf[pos] == JSONB_END_OBJECT) depth--;
                    pos++;
                }
            }
            // bool/null: no payload
            break;
        }
        case JSONB_STRING:
            // Bare string (array element) — skip
            while (pos < len && buf[pos] != 0) pos++;
            pos++;
            break;
        case JSONB_INT32: pos += 4; break;
        case JSONB_DOUBLE: pos += 8; break;
        default:
            // Unknown opcode — can't parse further
            return NULL;
        }
    }
    return NULL;
}

// Build a JSONB response into buf. Returns total framed length including {: :}\n.
static uint32_t jsonb_build_response(const char *req_name, uint8_t *buf, uint32_t buf_size) {
    // Build raw opcodes into a scratch area
    uint8_t opcodes[256];
    uint32_t pos = 0;

    opcodes[pos++] = JSONB_BEGIN_OBJECT;

    if (strstr(req_name, "card.temp")) {
        // {"value":22.5}
        opcodes[pos++] = JSONB_ITEM;
        memcpy(&opcodes[pos], "value\0", 6); pos += 6;
        opcodes[pos++] = JSONB_DOUBLE;
        double val = 22.5;
        memcpy(&opcodes[pos], &val, 8); pos += 8;
    } else if (strstr(req_name, "note.template")) {
        // {"bytes":14,"template":true}
        opcodes[pos++] = JSONB_ITEM;
        memcpy(&opcodes[pos], "bytes\0", 6); pos += 6;
        opcodes[pos++] = JSONB_INT32;
        int32_t bytes = 14;
        memcpy(&opcodes[pos], &bytes, 4); pos += 4;

        opcodes[pos++] = JSONB_ITEM;
        memcpy(&opcodes[pos], "template\0", 9); pos += 9;
        opcodes[pos++] = JSONB_TRUE;
    } else if (strstr(req_name, "note.get")) {
        // {"body":{"temperature":22.5,"humidity":60}}
        opcodes[pos++] = JSONB_ITEM;
        memcpy(&opcodes[pos], "body\0", 5); pos += 5;
        opcodes[pos++] = JSONB_BEGIN_OBJECT;

        opcodes[pos++] = JSONB_ITEM;
        memcpy(&opcodes[pos], "temperature\0", 12); pos += 12;
        opcodes[pos++] = JSONB_DOUBLE;
        double temp = 22.5;
        memcpy(&opcodes[pos], &temp, 8); pos += 8;

        opcodes[pos++] = JSONB_ITEM;
        memcpy(&opcodes[pos], "humidity\0", 9); pos += 9;
        opcodes[pos++] = JSONB_INT32;
        int32_t hum = 60;
        memcpy(&opcodes[pos], &hum, 4); pos += 4;

        opcodes[pos++] = JSONB_END_OBJECT;
    }
    // Default: empty object (hub.set, note.add, card.version, card.status, etc.)

    opcodes[pos++] = JSONB_END_OBJECT;

    // Frame: {: <COBS> :}\n
    if (2 + pos + (pos / 254) + 1 + 3 > buf_size) return 0;
    buf[0] = '{';
    buf[1] = ':';
    uint32_t enc_len = cobs_encode(opcodes, pos, JSONB_COBS_XOR, &buf[2]);
    buf[2 + enc_len] = ':';
    buf[3 + enc_len] = '}';
    buf[4 + enc_len] = '\n';
    return 5 + enc_len;
}

// ── JSON response (unchanged from original) ────────────────────────────────

static const char* json_match_response(const char* req) {
    if (strstr(req, "\"card.temp\""))
        return "{\"value\":22.5}\r\n";
    if (strstr(req, "\"note.template\""))
        return "{\"bytes\":14,\"template\":true}\r\n";
    if (strstr(req, "\"note.get\""))
        return "{\"body\":{\"temperature\":22.5,\"humidity\":60}}\r\n";
    return "{}\r\n";
}

// ── Chip state and UART handling ───────────────────────────────────────────

typedef struct {
    uart_dev_t uart;
    uint8_t rx_buf[BUF_SIZE];
    uint32_t rx_pos;
} chip_state_t;

static void on_uart_rx_data(void *user_data, uint8_t byte) {
    chip_state_t *chip = (chip_state_t*)user_data;

    if (byte == '\n') {
        chip->rx_buf[chip->rx_pos] = '\0';

        // Check for actual content (not just \r)
        bool has_content = false;
        for (uint32_t i = 0; i < chip->rx_pos; i++) {
            if (chip->rx_buf[i] != '\r') {
                has_content = true;
                break;
            }
        }

        if (!has_content) {
            // Reset handshake
            printf("[mock] reset handshake\n");
            const char *resp = "\r\n";
            uart_write(chip->uart, (uint8_t*)resp, strlen(resp));
        } else if (chip->rx_pos >= 2 && chip->rx_buf[0] == '{' && chip->rx_buf[1] == ':') {
            // JSONB request: {:<COBS payload>:}
            printf("[mock] JSONB request (%u bytes):", chip->rx_pos);
            for (uint32_t i = 0; i < chip->rx_pos && i < 40; i++)
                printf(" %02x", chip->rx_buf[i]);
            if (chip->rx_pos > 40) printf(" ...");
            printf("\n");

            // Strip {: header and :} trailer
            uint8_t *payload = chip->rx_buf + 2;
            uint32_t payload_len = chip->rx_pos - 2;
            if (payload_len >= 2 &&
                payload[payload_len - 2] == ':' && payload[payload_len - 1] == '}') {
                payload_len -= 2;
            }

            // COBS-decode in place
            uint8_t decoded[BUF_SIZE];
            uint32_t dec_len = cobs_decode(payload, payload_len, JSONB_COBS_XOR, decoded);

            printf("[mock] decoded (%u bytes):", dec_len);
            for (uint32_t i = 0; i < dec_len && i < 40; i++)
                printf(" %02x", decoded[i]);
            if (dec_len > 40) printf(" ...");
            printf("\n");

            // Find req field
            const char *req_name = jsonb_find_req(decoded, dec_len);
            printf("[mock] req: %s\n", req_name ? req_name : "(none)");

            // Build JSONB response
            uint8_t resp_buf[BUF_SIZE];
            uint32_t resp_len = jsonb_build_response(
                req_name ? req_name : "", resp_buf, sizeof(resp_buf));
            if (resp_len > 0) {
                printf("[mock] JSONB response (%u bytes)\n", resp_len);
                uart_write(chip->uart, resp_buf, resp_len);
            }
        } else if (chip->rx_buf[0] == '{') {
            // JSON request
            const char *resp = json_match_response((const char*)chip->rx_buf);
            printf("[mock] JSON request: %s\n", (char*)chip->rx_buf);
            printf("[mock] responding: %s", resp);
            uart_write(chip->uart, (uint8_t*)resp, strlen(resp));
        }

        chip->rx_pos = 0;
        return;
    }

    if (chip->rx_pos < BUF_SIZE - 1) {
        chip->rx_buf[chip->rx_pos++] = byte;
    }
}

static void on_uart_write_done(void *user_data) {
    (void)user_data;
}

void chip_init(void) {
    chip_state_t *chip = malloc(sizeof(chip_state_t));
    memset(chip, 0, sizeof(chip_state_t));
    printf("[mock] chip_init v3 (JSON + JSONB)\n");

    const uart_config_t uart_config = {
        .tx = pin_init("TX", INPUT_PULLUP),
        .rx = pin_init("RX", INPUT),
        .baud_rate = 9600,
        .rx_data = on_uart_rx_data,
        .write_done = on_uart_write_done,
        .user_data = chip,
    };
    chip->uart = uart_init(&uart_config);
    printf("[mock] uart initialized: %u\n", chip->uart);
}
