#ifndef HSTEX_TOKEN_H
#define HSTEX_TOKEN_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t hstex_token;
typedef uint32_t hstex_cs_id;

enum hstex_token_kind {
    HSTEX_TOKEN_CHARACTER = 0,
    HSTEX_TOKEN_PARAMETER = 1,
    HSTEX_TOKEN_CONTROL_SEQUENCE = 2,
    HSTEX_TOKEN_FROZEN_CONTROL_SEQUENCE = 3,
};

#define HSTEX_TOKEN_KIND_SHIFT UINT32_C(30)
#define HSTEX_TOKEN_PAYLOAD_MASK UINT32_C(0x3fffffff)
#define HSTEX_TOKEN_UNEXPANDED_EXECUTABLE_FLAG UINT32_C(0x20000000)
#define HSTEX_TOKEN_CS_ID_MASK UINT32_C(0x1fffffff)
#define HSTEX_CS_ID_MAX HSTEX_TOKEN_CS_ID_MASK

static inline enum hstex_token_kind hstex_token_kind_of(hstex_token token)
{
    return (enum hstex_token_kind)(token >> HSTEX_TOKEN_KIND_SHIFT);
}

static inline hstex_token hstex_token_character(uint8_t category, uint8_t character)
{
    return ((uint32_t)category << 8U) | (uint32_t)character;
}

static inline uint8_t hstex_token_category(hstex_token token)
{
    return (uint8_t)((token >> 8U) & UINT32_C(0x0f));
}

static inline uint8_t hstex_token_character_code(hstex_token token)
{
    return (uint8_t)(token & UINT32_C(0xff));
}

static inline hstex_token hstex_token_control_sequence(hstex_cs_id identifier)
{
    return (UINT32_C(2) << HSTEX_TOKEN_KIND_SHIFT) | identifier;
}

static inline hstex_token
hstex_token_frozen_control_sequence(hstex_cs_id identifier)
{
    return (UINT32_C(3) << HSTEX_TOKEN_KIND_SHIFT) | identifier;
}

static inline hstex_token
hstex_token_unexpanded_control_sequence(hstex_cs_id identifier)
{
    return (UINT32_C(3) << HSTEX_TOKEN_KIND_SHIFT) |
           HSTEX_TOKEN_UNEXPANDED_EXECUTABLE_FLAG | identifier;
}

static inline hstex_cs_id hstex_token_control_sequence_id(hstex_token token)
{
    return token & HSTEX_TOKEN_CS_ID_MASK;
}

static inline hstex_token hstex_token_parameter(uint8_t number)
{
    return (UINT32_C(1) << HSTEX_TOKEN_KIND_SHIFT) | (uint32_t)number;
}

static inline uint8_t hstex_token_parameter_number(hstex_token token)
{
    return (uint8_t)(token & UINT32_C(0xff));
}

static inline bool hstex_token_is_character(hstex_token token)
{
    return hstex_token_kind_of(token) == HSTEX_TOKEN_CHARACTER;
}

static inline bool hstex_token_is_control_sequence(hstex_token token)
{
    return hstex_token_kind_of(token) == HSTEX_TOKEN_CONTROL_SEQUENCE;
}

static inline bool hstex_token_is_frozen_control_sequence(hstex_token token)
{
    return hstex_token_kind_of(token) == HSTEX_TOKEN_FROZEN_CONTROL_SEQUENCE;
}

static inline bool
hstex_token_is_unexpanded_control_sequence(hstex_token token)
{
    return hstex_token_is_frozen_control_sequence(token) &&
           (token & HSTEX_TOKEN_UNEXPANDED_EXECUTABLE_FLAG) != 0U;
}

static inline bool hstex_token_is_parameter(hstex_token token)
{
    return hstex_token_kind_of(token) == HSTEX_TOKEN_PARAMETER;
}

#endif
