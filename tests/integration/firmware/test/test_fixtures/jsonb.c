// Copyright 2024 Blues Inc.  All rights reserved.
// Use of this source code is governed by licenses granted by the
// copyright holder including that found in the LICENSE file.
//
// Source: https://github.com/blues/note-c-zero
// Only the functions needed for the JSONB spike test are included here.

#include "jsonb/jsonb.h"

// Internal helpers
static void jbAppendBytes(jsonbContext *ctx, uint8_t opcode, uint8_t *buf, uint32_t buflen);
static uint32_t jbCobsEncode(uint8_t *ptr, uint32_t length, uint8_t xor_byte, uint8_t *dst);
static uint32_t jbCobsDecode(uint8_t *ptr, uint32_t length, uint8_t xor_byte, uint8_t *dst);
static uint32_t jbCobsGuaranteedFit(uint32_t buflen);

static void jsonbFormatBegin(jsonbContext *ctx, uint8_t *buf, uint32_t buflen, bufGrowFn bufGrow)
{
    ctx->growFn = bufGrow;
    ctx->buf = buf;
    ctx->buflen = buflen;
    ctx->bufused = 0;
    ctx->overrun = false;
    ctx->error = false;
}

static uint32_t jsonbFormatEnd(jsonbContext *ctx)
{
    if (ctx->overrun || ctx->error) {
        return 0;
    }
    uint32_t siglen = (sizeof(JSONB_HEADER)-1) + (sizeof(JSONB_TRAILER)-1) + 1;
    uint32_t buflenWithoutSig = ctx->buflen - siglen;
    uint32_t maxExpansionByEncoding = buflenWithoutSig - jbCobsGuaranteedFit(buflenWithoutSig);
    if (ctx->bufused + maxExpansionByEncoding > buflenWithoutSig) {
        return 0;
    }
    uint8_t *movedPayload = &ctx->buf[maxExpansionByEncoding + siglen];
    memmove(movedPayload, ctx->buf, ctx->bufused);
    memcpy(ctx->buf, JSONB_HEADER, sizeof(JSONB_HEADER)-1);
    int32_t cobslen = (int32_t) jbCobsEncode(movedPayload, ctx->bufused, (uint8_t) JSONB_TERMINATOR, &ctx->buf[sizeof(JSONB_HEADER)-1]);
    memcpy(&ctx->buf[(sizeof(JSONB_HEADER)-1)+cobslen], JSONB_TRAILER, sizeof(JSONB_TRAILER)-1);
    ctx->bufused = (sizeof(JSONB_HEADER)-1) + cobslen + (sizeof(JSONB_TRAILER)-1);
    ctx->buf[ctx->bufused++] = JSONB_TERMINATOR;
    return ctx->bufused;
}

static void jsonbAddObjectBegin(jsonbContext *ctx)
{
    jbAppendBytes(ctx, JSONB_BEGIN_OBJECT, NULL, 0);
}

static void jsonbAddObjectEnd(jsonbContext *ctx)
{
    jbAppendBytes(ctx, JSONB_END_OBJECT, NULL, 0);
}

void jsonbObjectBegin(jsonbContext *ctx, uint8_t *buf, uint32_t buflen, bufGrowFn bufGrow)
{
    jsonbFormatBegin(ctx, buf, buflen, bufGrow);
    jsonbAddObjectBegin(ctx);
}

uint32_t jsonbObjectEnd(jsonbContext *ctx)
{
    jsonbAddObjectEnd(ctx);
    return jsonbFormatEnd(ctx);
}

static void jsonbAddString(jsonbContext *ctx, const char *str)
{
    jbAppendBytes(ctx, JSONB_STRING, (uint8_t *) str, strlen(str)+1);
}

static void jsonbAddItemToObject(jsonbContext *ctx, const char *itemName)
{
    jbAppendBytes(ctx, JSONB_ITEM, (uint8_t *) itemName, strlen(itemName)+1);
}

void jsonbAddStringToObject(jsonbContext *ctx, const char *itemName, const char *str)
{
    jsonbAddItemToObject(ctx, itemName);
    jsonbAddString(ctx, str);
}

// Parsing

bool jsonbParse(jsonbContext *ctx, uint8_t *buf, uint32_t buflen)
{
    while (buflen > 0 && buf[0] < ' ') { buf++; buflen--; }
    while (buflen > 0 && buf[buflen-1] < ' ') { buflen--; }
    if (buflen == 0) return false;
    if (buflen < sizeof(JSONB_HEADER)-1 || memcmp(buf, JSONB_HEADER, sizeof(JSONB_HEADER)-1) != 0) {
        return false;
    }
    buf += sizeof(JSONB_HEADER)-1;
    buflen -= sizeof(JSONB_HEADER)-1;
    if (buflen < sizeof(JSONB_TRAILER)-1 || memcmp(&buf[buflen-(sizeof(JSONB_TRAILER)-1)], JSONB_TRAILER, sizeof(JSONB_TRAILER)-1) != 0) {
        return false;
    }
    buflen -= sizeof(JSONB_TRAILER)-1;
    ctx->buflen = jbCobsDecode(buf, buflen, JSONB_TERMINATOR, buf);
    ctx->buf = buf;
    ctx->bufused = 0;
    return true;
}

static void jsonbEnum(jsonbContext *ctx)
{
    ctx->bufused = 0;
    ctx->opcode = JSONB_INVALID;
}

static bool jsonbEnumNext(jsonbContext *ctx, bool *firstInObjectOrArray, uint8_t *opcode, const char **item, void *v)
{
    if (ctx->bufused >= ctx->buflen) return false;
    if (firstInObjectOrArray != NULL) {
        *firstInObjectOrArray = (ctx->opcode == JSONB_BEGIN_OBJECT || ctx->opcode == JSONB_BEGIN_ARRAY || ctx->opcode == JSONB_INVALID);
    }
    ctx->opcode = ctx->buf[ctx->bufused++];
    if (opcode != NULL) *opcode = ctx->opcode;
    *item = NULL;
    if (ctx->opcode == JSONB_ITEM) {
        *item = (const char *) &ctx->buf[ctx->bufused];
        uint32_t namelen = 0;
        bool nullTerminated = false;
        for (uint32_t i=0; i<(ctx->buflen-ctx->bufused); i++) {
            namelen++;
            if (ctx->buf[ctx->bufused+i] == '\0') { nullTerminated = true; break; }
        }
        if (!nullTerminated) return false;
        ctx->bufused += namelen;
        ctx->opcode = ctx->buf[ctx->bufused++];
        if (opcode != NULL) *opcode = ctx->opcode;
    }
    uint32_t len = 0;
    switch (ctx->opcode) {
    case JSONB_BEGIN_OBJECT: case JSONB_END_OBJECT:
    case JSONB_BEGIN_ARRAY: case JSONB_END_ARRAY:
    case JSONB_NULL: case JSONB_TRUE: case JSONB_FALSE:
        break;
    case JSONB_STRING: {
        bool nullTerminated = false;
        for (uint32_t i=0; i<(ctx->buflen-ctx->bufused); i++) {
            len++;
            if (ctx->buf[ctx->bufused+i] == '\0') { nullTerminated = true; break; }
        }
        if (!nullTerminated) return false;
        break;
    }
    case JSONB_INT8: case JSONB_UINT8: len = 1; break;
    case JSONB_INT16: case JSONB_UINT16: len = 2; break;
    case JSONB_INT32: case JSONB_UINT32: case JSONB_FLOAT: len = 4; break;
    case JSONB_INT64: case JSONB_UINT64: case JSONB_DOUBLE: len = 8; break;
    default: return false;
    }
    * (void **) v = &ctx->buf[ctx->bufused];
    ctx->bufused += len;
    return true;
}

static bool jsonbGetObjectItem(jsonbContext *ctx, const char *itemName, uint8_t *itemType, void *itemValue)
{
    uint8_t type;
    const char *key;
    void *value;
    int nesting = 0;
    jsonbEnum(ctx);
    while (jsonbEnumNext(ctx, NULL, &type, &key, &value)) {
        switch (type) {
        case JSONB_BEGIN_OBJECT: nesting++; break;
        case JSONB_END_OBJECT: nesting--; break;
        }
        if (nesting == 0) break;
        if (nesting != 1) continue;
        if (key != NULL) {
            int l1 = strlen(itemName);
            int l2 = strlen(key);
            if (l1 == l2 && memcmp(itemName, key, l1) == 0) {
                *itemType = type;
                * ((void **)itemValue) = value;
                return true;
            }
        }
    }
    return false;
}

char *jsonbGetString(jsonbContext *ctx, const char *itemName)
{
    uint8_t itemType;
    char *itemValue;
    if (!jsonbGetObjectItem(ctx, itemName, &itemType, &itemValue)) return (char *) "";
    if (itemType != JSONB_STRING) return (char *) "";
    return itemValue;
}

char *jsonbGetErr(jsonbContext *ctx)
{
    return jsonbGetString(ctx, "err");
}

// Internal utilities

static void jbAppendBytes(jsonbContext *ctx, uint8_t opcode, uint8_t *buf, uint32_t buflen)
{
    uint32_t needed = buflen;
    if (opcode != JSONB_INVALID) needed++;
    if (ctx->bufused + needed > ctx->buflen) {
        if (ctx->growFn == NULL || !ctx->growFn(&ctx->buf, &ctx->buflen, needed)) {
            ctx->overrun = true;
        }
    }
    if (!ctx->overrun) {
        if (opcode != JSONB_INVALID) ctx->buf[ctx->bufused++] = opcode;
        if (buflen > 0) {
            memcpy(&ctx->buf[ctx->bufused], buf, buflen);
            ctx->bufused += buflen;
        }
    }
}

static uint32_t jbCobsEncode(uint8_t *ptr, uint32_t length, uint8_t xor_byte, uint8_t *dst)
{
    uint8_t ch;
    uint8_t *start = dst;
    uint8_t code = 1;
    uint8_t *code_ptr = dst++;
    while (length--) {
        ch = *ptr++;
        if (ch != 0) { *dst++ = ch ^ xor_byte; code++; }
        if (ch == 0 || code == 0xFF) { *code_ptr = code ^ xor_byte; code = 1; code_ptr = dst++; }
    }
    *code_ptr = code ^ xor_byte;
    return (dst - start);
}

static uint32_t jbCobsDecode(uint8_t *ptr, uint32_t length, uint8_t xor_byte, uint8_t *dst)
{
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
    return dst - start;
}

static uint32_t jbCobsGuaranteedFit(uint32_t buflen)
{
    uint32_t cobsOverhead = 1 + (buflen / 254) + 1;
    return (cobsOverhead > buflen ? 0 : (buflen - cobsOverhead));
}
