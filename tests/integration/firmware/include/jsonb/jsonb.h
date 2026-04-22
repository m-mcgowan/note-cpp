// Copyright 2024 Blues Inc.  All rights reserved.
// Use of this source code is governed by licenses granted by the
// copyright holder including that found in the LICENSE file.
//
// Source: https://github.com/blues/note-c-zero
// JSONB binary wire format for Notecard communication.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#pragma once

// JSONB signature that begins every jsonb object
#define JSONB_HEADER                "{:"
#define JSONB_TRAILER               ":}"
#define JSONB_TERMINATOR            '\n'
#define jsonbPresent(x,y)             (((y) > sizeof(JSONB_HEADER)-1) && memcmp((x), JSONB_HEADER, sizeof(JSONB_HEADER)-1) == 0)

// JSONB opcodes used for formatting and parsing
#define JSONB_INVALID               0x00

#define JSONB_BEGIN_OBJECT          0x10
#define JSONB_END_OBJECT            0x11
#define JSONB_BEGIN_ARRAY           0x12
#define JSONB_END_ARRAY             0x13

#define JSONB_NULL                  0x20
#define JSONB_TRUE                  0x21
#define JSONB_FALSE                 0x22

// A UTF-8 JSON item name, null-terminated
#define JSONB_ITEM                  0x30

// A UTF-8 JSON string, null-terminated
#define JSONB_STRING                0x40

// A binary buffer, prefixed by its length
#define JSONB_BIN8                  0x51
#define JSONB_BIN16                 0x52
#define JSONB_BIN24                 0x53
#define JSONB_BIN32                 0x54

// Signed integers, occupying JSON_OPCODE_LEN bytes
#define JSONB_INT8                  0x61
#define JSONB_INT16                 0x62
#define JSONB_INT32                 0x64
#define JSONB_INT64                 0x68

// Unsigned integers, occupying JSON_OPCODE_LEN bytes
#define JSONB_UINT8                 0x71
#define JSONB_UINT16                0x72
#define JSONB_UINT32                0x74
#define JSONB_UINT64                0x78

// IEEE Reals, occupying JSON_OPCODE_LEN bytes
#define JSONB_FLOAT                 0x84
#define JSONB_DOUBLE                0x88

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*bufGrowFn) (uint8_t **buf, uint32_t *buflen, uint32_t growBytes);

typedef struct {
    bool overrun;
    bool error;
    uint8_t opcode;
    bufGrowFn growFn;
    uint8_t *buf;
    uint32_t buflen;
    uint32_t bufused;
} jsonbContext;

void jsonbObjectBegin(jsonbContext *ctx, uint8_t *buf, uint32_t buflen, bufGrowFn bufGrow);
uint32_t jsonbObjectEnd(jsonbContext *ctx);
void jsonbAddStringToObject(jsonbContext *ctx, const char *itemName, const char *str);

bool jsonbParse(jsonbContext *ctx, uint8_t *buf, uint32_t buflen);
char *jsonbGetString(jsonbContext *ctx, const char *itemName);
char *jsonbGetErr(jsonbContext *ctx);

#ifdef __cplusplus
}
#endif
