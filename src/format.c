#include "hstex/engine.h"

#include "hstex/borrowed.h"
#include "hstex/filedb.h"
#include "hstex/input.h"

#include <sys/mman.h>
#include "internal.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A format is the engine's state once the format source has been read, put
   by so that the next run need not read it again: the names the engine
   knows and what they mean, the registers, the parameters, the fonts and the
   hyphenation patterns, and glyph-to-Unicode mappings. What a run builds as
   it goes is not in it, and a format is only written where none of that has
   begun. See
   docs/DECISIONS.md, the-format-a-run-starts-from. */

/* The name the file gives itself and how wide its records are both live in
   hstex/engine.h, so that the driver can key a cached format on them without
   linking this. */
static const char hstex_format_magic[] = HSTEX_FORMAT_MAGIC;

static int format_error(char *error, size_t capacity, const char *format, ...)
    HSTEX_PRINTF_FORMAT(3, 4);

static int format_error(char *error, size_t capacity, const char *format, ...)
{
    if (error != NULL && capacity != 0U) {
        va_list arguments;
        va_start(arguments, format);
        (void)vsnprintf(error, capacity, format, arguments);
        va_end(arguments);
    }
    return -1;
}

/* One stream serves both directions, so that what is written and what is
   read cannot come apart. */
struct format_stream {
    FILE *file;
    const uint8_t *bytes;
    size_t length;
    size_t at;
    /* How much has been written, so that what needs aligning can be. */
    size_t written;
    bool writing;
    bool failed;
    /* Reading, leave the bodies of definitions where they lie in `bytes'
       rather than copying them out: the bytes outlive the engine. */
    bool in_place;
};

static void transfer(struct format_stream *stream, void *value, size_t length)
{
    if (stream->failed || length == 0U) {
        return;
    }
    if (stream->writing) {
        if (fwrite(value, 1U, length, stream->file) != length) {
            stream->failed = true;
        }
        stream->written += length;
        return;
    }
    if (length > stream->length - stream->at) {
        stream->failed = true;
        return;
    }
    memcpy(value, stream->bytes + stream->at, length);
    stream->at += length;
}

#define TRANSFER_VALUE(stream, value) \
    transfer((stream), &(value), sizeof(value))

/* An array the format carries: how many there are, then the bytes of them.
   Reading takes the room for them; the capacity it reports is what was
   written, since a run grows it again as it needs to. `owned` says whether
   what the pointer holds now is this run's to give back -- an array inside a
   record that has itself just been read holds nothing of ours. */
static void transfer_array(struct format_stream *stream, void **base,
                           size_t *count, size_t *capacity, size_t size,
                           bool owned)
{
    TRANSFER_VALUE(stream, *count);
    if (stream->failed) {
        return;
    }
    if (!stream->writing) {
        if (owned) {
            free(*base);
        }
        *base = NULL;
        if (*count != 0U) {
            if (*count > SIZE_MAX / size) {
                stream->failed = true;
                return;
            }
            *base = malloc(*count * size);
            if (*base == NULL) {
                stream->failed = true;
                return;
            }
        }
        if (capacity != NULL) {
            *capacity = *count;
        }
    }
    transfer(stream, *base, *count * size);
}

/* A POINTER MEANS NOTHING IN A FILE, AND NEITHER DOES PADDING. These arrays are written as they sit
   in memory and what they point at is written after them, so a reader fills
   the pointers in from what follows and never reads the ones in the file.
   Writing them anyway put this run's own addresses into the format: two
   builds from the same source, by the same binary, came out differing in
   91,406 bytes, every one of them an address that had been handed out by a
   different allocation. They are cleared in a copy on the way out, which
   costs a copy per element of three arrays and makes a format the same file
   every time it is built from the same source. */
struct format_hole {
    size_t at;
    size_t length;
};

static void transfer_array_cleared(struct format_stream *stream, void **base,
                                   size_t *count, size_t *capacity, size_t size,
                                   bool owned, const struct format_hole *holes,
                                   size_t hole_count)
{
    if (!stream->writing) {
        transfer_array(stream, base, count, capacity, size, owned);
        return;
    }
    unsigned char element[512];
    if (size > sizeof(element)) {
        stream->failed = true;
        return;
    }
    TRANSFER_VALUE(stream, *count);
    const unsigned char *from = *base;
    for (size_t index = 0U; index < *count && !stream->failed; ++index) {
        memcpy(element, from + index * size, size);
        for (size_t which = 0U; which < hole_count; ++which) {
            memset(element + holes[which].at, 0, holes[which].length);
        }
        transfer(stream, element, size);
    }
}

/* HOW MANY OF A REGISTER BANK ARE WORTH WRITING. The banks are as long as
   the register number an engine accepts -- some thirty-two thousand of each
   -- and a format has set almost none of them: a fresh engine callocs them,
   so an element that is all zero is one the reader would have made anyway.
   Everything past the last one that is not is left out of the file, and the
   reader gets it back from calloc without reading, copying, or touching a
   page of it. Measured on a stock LaTeX format, that is 3.4 MB of 13 MB.

   The bytes a compiler leaves between fields are not the engine's, so an
   element is judged by the same holes the writer clears. */
static size_t used_prefix(const void *base, size_t count, size_t size,
                          const struct format_hole *holes, size_t hole_count)
{
    unsigned char element[512];
    if (base == NULL || size == 0U || size > sizeof(element)) {
        return count;
    }
    const unsigned char *from = base;
    for (size_t index = count; index != 0U; --index) {
        memcpy(element, from + (index - 1U) * size, size);
        for (size_t which = 0U; which < hole_count; ++which) {
            memset(element + holes[which].at, 0, holes[which].length);
        }
        for (size_t byte = 0U; byte < size; ++byte) {
            if (element[byte] != 0U) {
                return index;
            }
        }
    }
    return 0U;
}

/* One register bank: `prefix' elements in the file, `capacity' in memory. */
/* A bank of registers, zero from the kernel and paid for a page at a time
   as it is set. Taken from the heap it came from whatever a run had given
   back, and was zeroed by hand: the bank of boxes cost half a millisecond. */
static void *bank_map(struct hstex_engine *engine, size_t bytes)
{
    if (engine->mapped_bank_count ==
        sizeof(engine->mapped_banks) / sizeof(engine->mapped_banks[0])) {
        return calloc(bytes, 1U);
    }
    void *bank = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bank == MAP_FAILED) {
        return calloc(bytes, 1U);
    }
    engine->mapped_banks[engine->mapped_bank_count] = bank;
    engine->mapped_bank_bytes[engine->mapped_bank_count] = bytes;
    ++engine->mapped_bank_count;
    hstex_borrowed_register(bank, bytes);
    return bank;
}

static void transfer_registers(struct format_stream *stream,
                               struct hstex_engine *engine, void **base,
                               size_t capacity, size_t existing_capacity,
                               size_t prefix, size_t size,
                               const struct format_hole *holes,
                               size_t hole_count)
{
    if (stream->failed) {
        return;
    }
    if (stream->writing) {
        if (hole_count == 0U) {
            transfer(stream, *base, prefix * size);
            return;
        }
        unsigned char element[512];
        if (size > sizeof(element)) {
            stream->failed = true;
            return;
        }
        const unsigned char *from = *base;
        for (size_t index = 0U; index < prefix && !stream->failed; ++index) {
            memcpy(element, from + index * size, size);
            for (size_t which = 0U; which < hole_count; ++which) {
                memset(element + holes[which].at, 0, holes[which].length);
            }
            transfer(stream, element, size);
        }
        return;
    }
    /* The engine already holds a bank of this size, zero throughout: it was
       taken at initialisation and nothing has been set in it. Giving it
       back and taking another cost every bank a mapping and an unmapping,
       and the bank of boxes half a millisecond; what was written goes into
       the one there is. */
    if (capacity == 0U) {
        hstex_release(*base);
        *base = NULL;
        return;
    }
    if (capacity > SIZE_MAX / size) {
        stream->failed = true;
        return;
    }
    if (*base == NULL || capacity != existing_capacity) {
        hstex_release(*base);
        *base = bank_map(engine, capacity * size);
        if (*base == NULL) {
            stream->failed = true;
            return;
        }
    }
    transfer(stream, *base, prefix * size);
}

/* Where a kind keeps an address, and how much of it there is to clear. */
#define FORMAT_ADDRESS(type, field) \
    {offsetof(struct type, field), sizeof(void *)}

#define FORMAT_FIELD(type, field)                                      \
    {offsetof(struct type, field), sizeof(((struct type *)0)->field)}

/* The room a compiler leaves after a struct's last field, worked out from
   the field rather than written down, so that it stays right if the fields
   move and is nothing at all where a compiler leaves nothing. */
#define FORMAT_TAIL(outer, inner, type, last)                              \
    {offsetof(struct outer, inner) + offsetof(struct type, last) +         \
         sizeof(((struct type *)0)->last),                                 \
     sizeof(struct type) - offsetof(struct type, last) -                   \
         sizeof(((struct type *)0)->last)}

/* The room the engine's token pool would have given a run of this many: the
   least power of two that holds it, never fewer than eight. A body the pool
   may later be given back must have exactly this much, because the pool
   works out which list a body belongs on from its length alone. */
static size_t token_room(size_t count)
{
    size_t room = 8U;
    while (room < count) {
        room *= 2U;
    }
    return room;
}

/* A BLOCK TO CUT BODIES FROM. Everything a format's definitions hold is read
   in one pass and none of it is given back before the engine is, so it is
   cut from a few blocks rather than asked for one body at a time. A block is
   never grown or moved, so what is cut from it stays where it was put.

   The room asked for is exactly what the body needs. A body cut from here
   never goes back to the token pool, whose lists are kept by length, so
   nothing later reads the room around it and it need not be rounded up to a
   length the pool would recognize. */
enum { HSTEX_FORMAT_BODY_BLOCK = 1U << 18 };

static void *body_take(struct hstex_engine *engine, size_t bytes)
{
    if (bytes == 0U) {
        return NULL;
    }
    /* Every body is a run of tokens, so a block cut in whole tokens stays as
       aligned as the block itself. */
    bytes = (bytes + (sizeof(hstex_token) - 1U)) & ~(sizeof(hstex_token) - 1U);
    if (bytes > engine->body_left) {
        size_t wanted = bytes > (size_t)HSTEX_FORMAT_BODY_BLOCK
                            ? bytes
                            : (size_t)HSTEX_FORMAT_BODY_BLOCK;
        if (engine->body_block_count == engine->body_block_capacity) {
            size_t capacity = engine->body_block_capacity == 0U
                                  ? 16U
                                  : engine->body_block_capacity * 2U;
            if (capacity > SIZE_MAX / sizeof(*engine->body_blocks)) {
                return NULL;
            }
            void **grown = realloc(engine->body_blocks,
                                   capacity * sizeof(*engine->body_blocks));
            if (grown == NULL) {
                return NULL;
            }
            engine->body_blocks = grown;
            engine->body_block_capacity = capacity;
        }
        void *block = malloc(wanted);
        if (block == NULL) {
            return NULL;
        }
        engine->body_blocks[engine->body_block_count++] = block;
        engine->body_next = block;
        engine->body_left = wanted;
    }
    void *taken = engine->body_next;
    engine->body_next += bytes;
    engine->body_left -= bytes;
    return taken;
}

/* One definition's body, cut from the block above where it can be. Where it
   cannot -- the block could not be grown -- it is asked for the way every
   other body read from a format used to be, and the record is not told it
   borrowed anything, so it gives that one back as it always did. */
/* A body begins at a multiple of a token's width in the file, so that a run
   reading the file where it lies can point at it. The count before it is
   wider than a token, so what precedes a body settles nothing about where
   it starts; the pad is written and skipped by position in the stream,
   which both sides count the same way. */
static void transfer_pad(struct format_stream *stream, size_t width)
{
    if (stream->failed) {
        return;
    }
    size_t at = stream->writing ? stream->written : stream->at;
    size_t pad = (width - at % width) % width;
    if (pad == 0U) {
        return;
    }
    if (stream->writing) {
        uint8_t zeros[16] = {0};
        transfer(stream, zeros, pad);
        return;
    }
    if (pad > stream->length - stream->at) {
        stream->failed = true;
        return;
    }
    stream->at += pad;
}

static void transfer_pad_tokens(struct format_stream *stream)
{
    transfer_pad(stream, sizeof(hstex_token));
}

/* An array nothing writes to once it is read -- a font's metrics, the
   hyphenation trie -- which a run reading the file where it lies points at
   rather than copies. Written at an alignment of eight, so that any record
   in it is aligned. Elsewhere it is read as any array is. */
static void transfer_array_in_place(struct format_stream *stream,
                                    void **base, size_t *count, size_t size,
                                    bool owned)
{
    TRANSFER_VALUE(stream, *count);
    if (stream->failed) {
        return;
    }
    transfer_pad(stream, 8U);
    if (stream->writing) {
        transfer(stream, *base, *count * size);
        return;
    }
    if (owned) {
        hstex_release(*base);
    }
    *base = NULL;
    if (*count == 0U) {
        return;
    }
    if (*count > SIZE_MAX / size) {
        stream->failed = true;
        return;
    }
    size_t bytes = *count * size;
    if (stream->failed || bytes > stream->length - stream->at) {
        stream->failed = true;
        return;
    }
    if (stream->in_place) {
        *base = (void *)(uintptr_t)(stream->bytes + stream->at);
        stream->at += bytes;
        return;
    }
    *base = malloc(bytes);
    if (*base == NULL) {
        stream->failed = true;
        return;
    }
    transfer(stream, *base, bytes);
}

/* A TABLE THE RUN GROWS, READ WHERE IT LIES. Written with the room it had
   as well as what was in it, the tail zero, so that a run pointing at it
   has the same room to grow into before it must copy the table out; a
   table read with only what was in it copied itself out at the first name
   or meaning a document added. Written at an alignment of eight. */
static void transfer_table_in_place(struct format_stream *stream,
                                    void **base, size_t *count,
                                    size_t *capacity, size_t size, bool owned)
{
    TRANSFER_VALUE(stream, *count);
    TRANSFER_VALUE(stream, *capacity);
    if (stream->failed) {
        return;
    }
    if (*capacity < *count || (*capacity != 0U && *capacity > SIZE_MAX / size)) {
        stream->failed = true;
        return;
    }
    transfer_pad(stream, 8U);
    if (stream->writing) {
        transfer(stream, *base, *count * size);
        static const uint8_t zeros[4096];
        size_t tail = (*capacity - *count) * size;
        while (tail != 0U && !stream->failed) {
            size_t piece = tail < sizeof(zeros) ? tail : sizeof(zeros);
            transfer(stream, (void *)(uintptr_t)(const void *)zeros, piece);
            tail -= piece;
        }
        return;
    }
    if (owned) {
        hstex_release(*base);
    }
    *base = NULL;
    if (*capacity == 0U) {
        return;
    }
    size_t bytes = *capacity * size;
    if (bytes > stream->length - stream->at) {
        stream->failed = true;
        return;
    }
    if (stream->in_place) {
        *base = (void *)(uintptr_t)(stream->bytes + stream->at);
        stream->at += bytes;
        return;
    }
    *base = malloc(bytes);
    if (*base == NULL) {
        stream->failed = true;
        return;
    }
    transfer(stream, *base, bytes);
}

static void transfer_body(struct format_stream *stream,
                          struct hstex_engine *engine, hstex_token **tokens,
                          size_t *count, uint8_t *borrowed, uint8_t bit)
{
    /* How long the body is was written with the record it belongs to,
       and is in the record already when this is reached. */
    if (stream->writing) {
        if (*count != 0U) {
            transfer_pad_tokens(stream);
            transfer(stream, *tokens, *count * sizeof(**tokens));
        }
        return;
    }
    if (stream->failed) {
        return;
    }
    *tokens = NULL;
    *borrowed = (uint8_t)(*borrowed & (uint8_t)~bit);
    if (*count == 0U) {
        return;
    }
    if (*count > SIZE_MAX / sizeof(**tokens)) {
        stream->failed = true;
        return;
    }
    transfer_pad_tokens(stream);
    size_t bytes = *count * sizeof(**tokens);
    if (stream->failed || bytes > stream->length - stream->at) {
        stream->failed = true;
        return;
    }
    /* WHERE IT LIES. A run that keeps the file mapped needs no copy of a
       body: it is a run of tokens in the file exactly as it is in memory,
       and nothing writes to a body once it is defined. The record is told
       it borrowed the body, as it is for one cut from a block, and gives
       it back to no one. Only the pages a run expands are ever read in. */
    if (stream->in_place) {
        *tokens = (hstex_token *)(uintptr_t)(stream->bytes + stream->at);
        stream->at += bytes;
        *borrowed = (uint8_t)(*borrowed | bit);
        return;
    }
    void *room = body_take(engine, bytes);
    if (room != NULL) {
        *borrowed = (uint8_t)(*borrowed | bit);
    } else {
        room = malloc(token_room(*count) * sizeof(**tokens));
        if (room == NULL) {
            stream->failed = true;
            return;
        }
    }
    *tokens = room;
    transfer(stream, *tokens, bytes);
}

/* A body read from a format is put in the room the engine's own pool would
   have given it -- the least power of two that holds it, never fewer than
   eight -- so that when nothing holds it any more it can go back on the
   list its length names. See docs/DECISIONS.md, where-a-body-is-kept. */
static void transfer_tokens(struct format_stream *stream, hstex_token **tokens,
                            size_t *count)
{
    if (stream->writing) {
        void *base = (void *)*tokens;
        transfer_array(stream, &base, count, NULL, sizeof(**tokens), false);
        *tokens = base;
        return;
    }
    TRANSFER_VALUE(stream, *count);
    if (stream->failed) {
        return;
    }
    *tokens = NULL;
    if (*count == 0U) {
        return;
    }
    if (*count > SIZE_MAX / 4U) {
        stream->failed = true;
        return;
    }
    size_t capacity = token_room(*count);
    if (capacity > SIZE_MAX / sizeof(**tokens)) {
        stream->failed = true;
        return;
    }
    *tokens = malloc(capacity * sizeof(**tokens));
    if (*tokens == NULL) {
        stream->failed = true;
        return;
    }
    transfer(stream, *tokens, *count * sizeof(**tokens));
}

/* What one font was loaded as: its name, its metrics, and the programs the
   metrics refer to. */
static void transfer_font(struct format_stream *stream, struct hstex_font *font)
{
    /* What the record holds now is only ours to look at where it is being
       written; what has just been read holds the other run's pointers. */
    size_t name_length = 0U;
    if (stream->writing && font->name != NULL) {
        name_length = strlen(font->name) + 1U;
    }
    void *name = stream->writing ? (void *)font->name : NULL;
    transfer_array_in_place(stream, &name, &name_length, 1U, false);
    font->name = name;
    size_t attribute_length = 0U;
    if (stream->writing && font->pdf_attribute != NULL) {
        attribute_length = strlen(font->pdf_attribute) + 1U;
    }
    void *attribute = stream->writing ? (void *)font->pdf_attribute : NULL;
    transfer_array(stream, &attribute, &attribute_length, NULL, 1U, false);
    font->pdf_attribute = attribute;
    size_t characters = 0U;
    if (stream->writing && font->characters != NULL) {
        characters = (size_t)HSTEX_FONT_CHARACTER_COUNT;
    }
    void *metrics = stream->writing ? (void *)font->characters : NULL;
    transfer_array_in_place(stream, &metrics, &characters,
                            sizeof(*font->characters), false);
    font->characters = metrics;
    void *lig_kern = stream->writing ? (void *)font->lig_kern : NULL;
    transfer_array_in_place(stream, &lig_kern, &font->lig_kern_count,
                            sizeof(*font->lig_kern), false);
    font->lig_kern = lig_kern;
    void *kerns = stream->writing ? (void *)font->kerns : NULL;
    transfer_array_in_place(stream, &kerns, &font->kern_count,
                            sizeof(*font->kerns), false);
    font->kerns = kerns;
    void *extensibles = stream->writing ? (void *)font->extensibles : NULL;
    transfer_array_in_place(stream, &extensibles, &font->extensible_count,
                            sizeof(*font->extensibles), false);
    font->extensibles = extensibles;
    void *dimens = stream->writing ? (void *)font->dimens : NULL;
    transfer_table_in_place(stream, &dimens, &font->dimen_count,
                            &font->dimen_capacity, sizeof(*font->dimens),
                            false);
    font->dimens = dimens;
    TRANSFER_VALUE(stream, font->design_size);
    TRANSFER_VALUE(stream, font->identifier_cs);
    TRANSFER_VALUE(stream, font->size);
    TRANSFER_VALUE(stream, font->hyphen_character);
    TRANSFER_VALUE(stream, font->skew_character);
    TRANSFER_VALUE(stream, font->checksum);
    if (!stream->writing) {
        font->virtual_font = NULL;
        font->virtual_state = 0U;
    }
}

static void transfer_string(struct format_stream *stream, char **string)
{
    size_t length = 0U;
    if (stream->writing && *string != NULL) {
        length = strlen(*string) + 1U;
    }
    void *bytes = stream->writing ? (void *)*string : NULL;
    transfer_array(stream, &bytes, &length, NULL, 1U, false);
    *string = bytes;
}

/* Everything a format carries, in one order for both directions. */
static void transfer_format(struct format_stream *stream,
                            struct hstex_engine *engine)
{
    struct hstex_symbol_table *symbols = &engine->lexical_state.symbols;
    void *entries = symbols->entries;
    transfer_table_in_place(stream, &entries, &symbols->entry_count,
                            &symbols->entry_capacity,
                            sizeof(*symbols->entries), true);
    symbols->entries = entries;
    void *slots = symbols->slots;
    size_t slot_capacity = symbols->slot_capacity;
    transfer_array_in_place(stream, &slots, &slot_capacity,
                            sizeof(*symbols->slots), true);
    symbols->slots = slots;
    symbols->slot_capacity = slot_capacity;
    void *bytes = symbols->bytes;
    transfer_table_in_place(stream, &bytes, &symbols->byte_count,
                            &symbols->byte_capacity,
                            sizeof(*symbols->bytes), true);
    symbols->bytes = bytes;
    TRANSFER_VALUE(stream, engine->lexical_state.catcodes);
    TRANSFER_VALUE(stream, engine->lexical_state.end_line_character);
    TRANSFER_VALUE(stream, engine->lexical_state.paragraph_control_sequence);
    TRANSFER_VALUE(stream, engine->catcode_levels);

    void *meanings = engine->meanings;
    size_t meaning_capacity = engine->meaning_capacity;
    transfer_array_in_place(stream, &meanings, &meaning_capacity,
                            sizeof(*engine->meanings), true);
    engine->meanings = meanings;
    engine->meaning_capacity = meaning_capacity;

    TRANSFER_VALUE(stream, engine->macro_free_list);
    TRANSFER_VALUE(stream, engine->macro_definitions);
    size_t macro_count = engine->macro_count;
    void *macros = engine->macros;
    static const struct format_hole macro_holes[] = {
        FORMAT_ADDRESS(hstex_macro, parameter_text),
        FORMAT_ADDRESS(hstex_macro, replacement),
        /* Where a body was cut from is this run's business and not the
           file's; it is written as nothing and settled again on the way in. */
        FORMAT_FIELD(hstex_macro, bodies_borrowed),
    };
    transfer_array_cleared(stream, &macros, &macro_count,
                           &engine->macro_capacity, sizeof(*engine->macros),
                           true, macro_holes,
                           sizeof(macro_holes) / sizeof(*macro_holes));
    engine->macros = macros;
    engine->macro_count = macro_count;
    for (size_t index = 0U; index < macro_count && !stream->failed; ++index) {
        struct hstex_macro *macro = &engine->macros[index];
        transfer_body(stream, engine, &macro->parameter_text,
                      &macro->parameter_count_tokens, &macro->bodies_borrowed,
                      (uint8_t)HSTEX_MACRO_PARAMETER_BORROWED);
        transfer_body(stream, engine, &macro->replacement,
                      &macro->replacement_count, &macro->bodies_borrowed,
                      (uint8_t)HSTEX_MACRO_REPLACEMENT_BORROWED);
    }

    size_t register_capacity = engine->count_capacity;
    TRANSFER_VALUE(stream, register_capacity);
    /* A box ends in a glue set whose last field does not reach the end of
       it; what the compiler leaves there is written with the box. */
    static const struct format_hole box_holes[] = {
        FORMAT_TAIL(hstex_box, glue, hstex_glue_set, order),
    };
    struct {
        void **base;
        size_t size;
        const struct format_hole *holes;
        size_t hole_count;
    } registers[] = {
        {(void **)&engine->counts, sizeof(*engine->counts), NULL, 0U},
        {(void **)&engine->count_levels, sizeof(*engine->count_levels), NULL,
         0U},
        {(void **)&engine->dimens, sizeof(*engine->dimens), NULL, 0U},
        {(void **)&engine->dimen_levels, sizeof(*engine->dimen_levels), NULL,
         0U},
        {(void **)&engine->glues, sizeof(*engine->glues), NULL, 0U},
        {(void **)&engine->glue_levels, sizeof(*engine->glue_levels), NULL, 0U},
        {(void **)&engine->muglues, sizeof(*engine->muglues), NULL, 0U},
        {(void **)&engine->muglue_levels, sizeof(*engine->muglue_levels), NULL,
         0U},
        {(void **)&engine->token_registers, sizeof(*engine->token_registers),
         NULL, 0U},
        {(void **)&engine->token_register_levels,
         sizeof(*engine->token_register_levels), NULL, 0U},
        {(void **)&engine->boxes, sizeof(*engine->boxes), box_holes,
         sizeof(box_holes) / sizeof(*box_holes)},
        {(void **)&engine->box_levels, sizeof(*engine->box_levels), NULL, 0U},
    };
    /* How far into the banks anything has been set. One length serves all of
       them: they are indexed alike, and a register that is set is usually set
       in more than one of them. */
    size_t register_prefix = 0U;
    if (stream->writing) {
        for (size_t index = 0U;
             index < sizeof(registers) / sizeof(registers[0]); ++index) {
            size_t reached = used_prefix(*registers[index].base,
                                         register_capacity,
                                         registers[index].size,
                                         registers[index].holes,
                                         registers[index].hole_count);
            if (reached > register_prefix) {
                register_prefix = reached;
            }
        }
    }
    TRANSFER_VALUE(stream, register_prefix);
    if (!stream->writing && register_prefix > register_capacity) {
        stream->failed = true;
    }
    for (size_t index = 0U;
         index < sizeof(registers) / sizeof(registers[0]) && !stream->failed;
         ++index) {
        transfer_registers(stream, engine, registers[index].base,
                           register_capacity, engine->count_capacity,
                           register_prefix, registers[index].size,
                           registers[index].holes,
                           registers[index].hole_count);
    }
    if (!stream->writing) {
        engine->count_capacity = register_capacity;
    }

    void *nodes = engine->nodes;
    transfer_array(stream, &nodes, &engine->node_count, &engine->node_capacity,
                   sizeof(*engine->nodes), true);
    engine->nodes = nodes;
    void *list_items = engine->list_items;
    transfer_array(stream, &list_items, &engine->list_item_count,
                   &engine->list_item_capacity, sizeof(*engine->list_items), true);
    engine->list_items = list_items;
    /* What the insertion nodes point at; see struct hstex_insert_detail. */
    void *insert_details = engine->insert_details;
    transfer_array(stream, &insert_details, &engine->insert_detail_count,
                   &engine->insert_detail_capacity,
                   sizeof(*engine->insert_details), true);
    engine->insert_details = insert_details;

    size_t token_list_count = engine->token_list_count;
    void *token_lists = engine->token_lists;
    static const struct format_hole list_holes[] = {
        FORMAT_ADDRESS(hstex_token_list, tokens),
    };
    transfer_array_cleared(stream, &token_lists, &token_list_count,
                           &engine->token_list_capacity,
                           sizeof(*engine->token_lists), true, list_holes,
                           sizeof(list_holes) / sizeof(*list_holes));
    engine->token_lists = token_lists;
    engine->token_list_count = token_list_count;
    for (size_t index = 0U; index < token_list_count && !stream->failed;
         ++index) {
        struct hstex_token_list *list = &engine->token_lists[index];
        transfer_tokens(stream, &list->tokens, &list->count);
    }

    size_t font_count = engine->font_count;
    void *fonts = engine->fonts;
    static const struct format_hole font_holes[] = {
        FORMAT_ADDRESS(hstex_font, name),
        FORMAT_ADDRESS(hstex_font, pdf_attribute),
        FORMAT_ADDRESS(hstex_font, characters),
        FORMAT_ADDRESS(hstex_font, lig_kern),
        FORMAT_ADDRESS(hstex_font, kerns),
        FORMAT_ADDRESS(hstex_font, extensibles),
        FORMAT_ADDRESS(hstex_font, dimens),
        FORMAT_ADDRESS(hstex_font, virtual_font),
        FORMAT_FIELD(hstex_font, virtual_state),
    };
    transfer_array_cleared(stream, &fonts, &font_count, &engine->font_capacity,
                           sizeof(*engine->fonts), true, font_holes,
                           sizeof(font_holes) / sizeof(*font_holes));
    engine->fonts = fonts;
    engine->font_count = font_count;
    for (size_t index = 0U; index < font_count && !stream->failed; ++index) {
        transfer_font(stream, &engine->fonts[index]);
    }
    TRANSFER_VALUE(stream, engine->current_font);
    TRANSFER_VALUE(stream, engine->current_font_level);

    size_t hyphen_root_count = 0U;
    if (stream->writing && engine->hyphen_roots != NULL) {
        hyphen_root_count = 256U;
    }
    void *hyphen_roots = engine->hyphen_roots;
    transfer_array_in_place(stream, &hyphen_roots, &hyphen_root_count,
                            sizeof(*engine->hyphen_roots), true);
    engine->hyphen_roots = hyphen_roots;
    void *hyphen_nodes = engine->hyphen_nodes;
    transfer_array_in_place(stream, &hyphen_nodes, &engine->hyphen_node_count,
                            sizeof(*engine->hyphen_nodes), true);
    if (!stream->writing) {
        engine->hyphen_node_capacity = engine->hyphen_node_count;
    }
    engine->hyphen_nodes = hyphen_nodes;
    void *hyphen_values = engine->hyphen_values;
    transfer_array_in_place(stream, &hyphen_values, &engine->hyphen_value_count,
                            sizeof(*engine->hyphen_values), true);
    if (!stream->writing) {
        engine->hyphen_value_capacity = engine->hyphen_value_count;
    }
    engine->hyphen_values = hyphen_values;
    TRANSFER_VALUE(stream, engine->hyphen_pattern_count);
    void *hyphen_exceptions = engine->hyphen_exceptions;
    transfer_array(stream, &hyphen_exceptions, &engine->hyphen_exception_count,
                   &engine->hyphen_exception_capacity,
                   sizeof(*engine->hyphen_exceptions), true);
    engine->hyphen_exceptions = hyphen_exceptions;
    void *hyphen_exception_data = engine->hyphen_exception_data;
    transfer_array(stream, &hyphen_exception_data,
                   &engine->hyphen_exception_data_count,
                   &engine->hyphen_exception_data_capacity,
                   sizeof(*engine->hyphen_exception_data), true);
    engine->hyphen_exception_data = hyphen_exception_data;

    TRANSFER_VALUE(stream, engine->dimen_parameters);
    TRANSFER_VALUE(stream, engine->dimen_parameter_levels);
    TRANSFER_VALUE(stream, engine->glue_parameters);
    TRANSFER_VALUE(stream, engine->glue_parameter_levels);
    TRANSFER_VALUE(stream, engine->muglue_parameters);
    TRANSFER_VALUE(stream, engine->muglue_parameter_levels);
    TRANSFER_VALUE(stream, engine->token_parameters);
    TRANSFER_VALUE(stream, engine->token_parameter_levels);
    TRANSFER_VALUE(stream, engine->integer_parameters);
    TRANSFER_VALUE(stream, engine->integer_parameter_levels);
    TRANSFER_VALUE(stream, engine->code_tables);
    TRANSFER_VALUE(stream, engine->code_levels);
    TRANSFER_VALUE(stream, engine->math_fonts);
    TRANSFER_VALUE(stream, engine->math_font_levels);
    TRANSFER_VALUE(stream, engine->interaction_mode);
    TRANSFER_VALUE(stream, engine->group_level);
    TRANSFER_VALUE(stream, engine->dump_requested);

    /* \pdfglyphtounicode is normally executed while a macro format is
       built. Its mappings must therefore travel with that format: they are
       later consumed while document fonts and their ToUnicode CMaps are
       written. */
    size_t glyph_unicode_count = engine->glyph_unicode_count;
    void *glyph_unicode = engine->glyph_unicode;
    static const struct format_hole glyph_unicode_holes[] = {
        FORMAT_ADDRESS(hstex_glyph_unicode, glyph),
        FORMAT_ADDRESS(hstex_glyph_unicode, unicode),
    };
    transfer_array_cleared(
        stream, &glyph_unicode, &glyph_unicode_count,
        &engine->glyph_unicode_capacity, sizeof(*engine->glyph_unicode), true,
        glyph_unicode_holes,
        sizeof(glyph_unicode_holes) / sizeof(*glyph_unicode_holes));
    engine->glyph_unicode = glyph_unicode;
    engine->glyph_unicode_count = glyph_unicode_count;
    for (size_t index = 0U;
         index < glyph_unicode_count && !stream->failed; ++index) {
        transfer_string(stream, &engine->glyph_unicode[index].glyph);
        transfer_string(stream, &engine->glyph_unicode[index].unicode);
    }

    /* The names the state digest passes over -- the macro package's own
       scratch -- travel with the format, because they describe the package
       the format is. A list given by the environment is the experimenter's
       and outranks the format's. */
    if (!stream->writing && engine->soft_names_from_environment) {
        size_t discarded_count = 0U;
        void *discarded = NULL;
        transfer_array(stream, &discarded, &discarded_count, NULL,
                       sizeof(uint64_t), false);
        free(discarded);
    } else {
        void *names = engine->soft_names;
        transfer_array(stream, &names, &engine->soft_name_count, NULL,
                       sizeof(uint64_t), false);
        engine->soft_names = names;
    }

    /* Where the installation keeps its filename databases, so that a run
       starting from this format need not start a child to be told. It is
       used only where the stamp still describes the trees on disk. */
    transfer_string(stream, &engine->texmf_trees);
    TRANSFER_VALUE(stream, engine->texmf_trees_stamp);
}

/* A format is written only from a run that has built nothing of its own, so
   that what is left out of it is what a fresh engine already has. */
static bool engine_is_quiet(const struct hstex_engine *engine)
{
    return engine->save_count == 0U && engine->conditional_count == 0U &&
           engine->group_level == 0U && engine->shipped_pages == 0U &&
           engine->dvi_file == NULL && engine->pdf_file == NULL;
}

/* What a format is worth telling the next run about the installation it was
   built against. Asking costs a child process, which a format build has
   already paid for by the time it gets here, and saves one in every run
   that starts from what is written. */
static void remember_trees(struct hstex_engine *engine)
{
    const char *trees = hstex_file_db_trees();
    if (trees == NULL || trees[0] == '\0') {
        return;
    }
    char *kept = malloc(strlen(trees) + 1U);
    if (kept == NULL) {
        return;
    }
    memcpy(kept, trees, strlen(trees) + 1U);
    free(engine->texmf_trees);
    engine->texmf_trees = kept;
    engine->texmf_trees_stamp = hstex_file_db_trees_stamp(kept);
}

int hstex_engine_write_format(struct hstex_engine *engine, const char *path,
                              char *error, size_t error_capacity)
{
    if (engine == NULL || path == NULL) {
        return format_error(error, error_capacity, "invalid format write");
    }
    if (!engine_is_quiet(engine)) {
        return format_error(error, error_capacity,
                               "a format cannot be written from a run that "
                               "has begun a document");
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return format_error(error, error_capacity, "cannot write %s", path);
    }
    remember_trees(engine);
    struct format_stream stream = {0};
    stream.file = file;
    stream.writing = true;
    transfer(&stream, (void *)(uintptr_t)(const void *)hstex_format_magic,
             sizeof(hstex_format_magic));
    uint64_t layout = hstex_format_layout();
    transfer(&stream, &layout, sizeof(layout));
    /* Where the filename database begins, filled in once it is written;
       zero where the format carries none. */
    long trailer_word = ftell(file);
    uint64_t trailer_at = 0U;
    transfer(&stream, &trailer_at, sizeof(trailer_at));
    transfer_format(&stream, engine);
    if (!stream.failed && trailer_word >= 0) {
        size_t image_length = 0U;
        uint8_t *image = hstex_file_db_image(
            engine->texmf_trees, engine->texmf_trees_stamp, &image_length);
        if (image != NULL) {
            long here = ftell(file);
            while (here >= 0 && here % 8 != 0) {
                if (fputc(0, file) == EOF) {
                    here = -1;
                    break;
                }
                ++here;
            }
            if (here > 0) {
                trailer_at = (uint64_t)here;
                transfer(&stream, image, image_length);
                if (!stream.failed &&
                    (fseek(file, trailer_word, SEEK_SET) != 0 ||
                     fwrite(&trailer_at, sizeof(trailer_at), 1U, file) !=
                         1U)) {
                    stream.failed = true;
                }
            }
            free(image);
        }
    }
    if (fclose(file) != 0 || stream.failed) {
        return format_error(error, error_capacity, "cannot write %s", path);
    }
    return 0;
}

int hstex_engine_format_to_file(struct hstex_engine *engine, FILE *file)
{
    if (engine == NULL || file == NULL) {
        return -1;
    }
    remember_trees(engine);
    struct format_stream stream = {0};
    stream.file = file;
    stream.writing = true;
    transfer(&stream, (void *)(uintptr_t)(const void *)hstex_format_magic,
             sizeof(hstex_format_magic));
    uint64_t layout = hstex_format_layout();
    transfer(&stream, &layout, sizeof(layout));
    uint64_t trailer_at = 0U;
    transfer(&stream, &trailer_at, sizeof(trailer_at));
    transfer_format(&stream, engine);
    return stream.failed ? -1 : 0;
}

int hstex_engine_format_from_buffer(struct hstex_engine *engine,
                                    const uint8_t *bytes, size_t length,
                                    size_t *consumed, bool in_place,
                                    char *error, size_t error_capacity)
{
    if (engine == NULL || bytes == NULL) {
        return format_error(error, error_capacity, "invalid format buffer");
    }
    struct format_stream stream = {0};
    stream.bytes = bytes;
    stream.length = length;
    stream.in_place =
        in_place && ((uintptr_t)bytes % sizeof(hstex_token)) == 0U;
    char magic[sizeof(hstex_format_magic)];
    transfer(&stream, magic, sizeof(magic));
    if (stream.failed ||
        memcmp(magic, hstex_format_magic, sizeof(magic)) != 0) {
        return format_error(error, error_capacity,
                            "not a checkpoint (bad format header)");
    }
    uint64_t layout = 0U;
    transfer(&stream, &layout, sizeof(layout));
    if (stream.failed || layout != hstex_format_layout()) {
        return format_error(error, error_capacity,
                            "checkpoint was written by a build laid out "
                            "differently");
    }
    uint64_t trailer_at = 0U;
    transfer(&stream, &trailer_at, sizeof(trailer_at));
    transfer_format(&stream, engine);
    if (stream.failed) {
        return format_error(error, error_capacity, "corrupt checkpoint format");
    }
    if (consumed != NULL) {
        *consumed = stream.at;
    }
    hstex_file_db_offer_trees(engine->texmf_trees, engine->texmf_trees_stamp);
    return hstex_rebuild_glyph_unicode_slots(engine, error, error_capacity);
}

/* The filename database a format carries, taken where it lies. Taken, the
   mapping is the engine's for the rest of the run; not taken -- no trailer,
   a stamp the trees on disk no longer match, a database already built --
   the mapping is let go here. */
static bool adopt_carried_files(const struct hstex_input *mapped,
                                uint64_t trailer_at)
{
    return trailer_at != 0U && trailer_at % 8U == 0U &&
           trailer_at < mapped->length &&
           hstex_file_db_adopt_image(
               (const uint8_t *)mapped->data + trailer_at,
               mapped->length - (size_t)trailer_at, true);
}

/* The mapping is the engine's from here on; taken by the database as well,
   it is the process's, and the engine does not unmap it. */
static void keep_mapping(struct hstex_engine *engine,
                         const struct hstex_input *mapped, bool shared)
{
    struct hstex_input *kept = malloc(sizeof(*kept));
    if (kept == NULL) {
        /* Nowhere to keep the record of it: the pages stay mapped for the
           process, which is what everything pointing into them needs. */
        return;
    }
    *kept = *mapped;
    if (engine->format_mapping != NULL) {
        if (!engine->format_mapping_shared) {
            hstex_borrowed_forget(engine->format_mapping->data);
            hstex_input_close(engine->format_mapping);
        }
        free(engine->format_mapping);
    }
    engine->format_mapping = kept;
    engine->format_mapping_shared = shared;
    hstex_borrowed_register(kept->data, kept->length);
}

int hstex_engine_read_format(struct hstex_engine *engine, const char *path,
                             char *error, size_t error_capacity)
{
    if (engine == NULL || path == NULL) {
        return format_error(error, error_capacity, "invalid format read");
    }
    /* THE FORMAT IS READ WHERE IT LIES. What a run takes out of a format it
       takes a record at a time, into room of its own; reading the file into
       a second buffer first only moves the bytes twice and asks the kernel
       for pages it then has to clear. Mapping the file hands the same bytes
       out of the page cache, so a format is neither copied nor counted
       against this run twice. */
    struct hstex_input mapped;
    char opened[256];
    if (hstex_input_open_private(path, &mapped, opened, sizeof(opened)) != 0) {
        return format_error(error, error_capacity, "cannot read %s", path);
    }
    struct format_stream stream = {0};
    stream.bytes = mapped.data;
    stream.length = mapped.length;
    stream.in_place = ((uintptr_t)mapped.data % sizeof(hstex_token)) == 0U;
    char magic[sizeof(hstex_format_magic)];
    transfer(&stream, magic, sizeof(magic));
    if (stream.failed || memcmp(magic, hstex_format_magic, sizeof(magic)) != 0) {
        hstex_input_close(&mapped);
        return format_error(error, error_capacity, "%s is not a format",
                               path);
    }
    uint64_t layout = 0U;
    transfer(&stream, &layout, sizeof(layout));
    if (stream.failed || layout != hstex_format_layout()) {
        hstex_input_close(&mapped);
        return format_error(error, error_capacity,
                            "%s was written by a build whose records are laid "
                            "out differently; build the format again",
                            path);
    }
    uint64_t trailer_at = 0U;
    transfer(&stream, &trailer_at, sizeof(trailer_at));
    static const enum hstex_integer_parameter process_clock[] = {
        HSTEX_INTEGER_TIME,
        HSTEX_INTEGER_DAY,
        HSTEX_INTEGER_MONTH,
        HSTEX_INTEGER_YEAR,
    };
    int32_t clock_values[sizeof(process_clock) / sizeof(*process_clock)];
    uint32_t clock_levels[sizeof(process_clock) / sizeof(*process_clock)];
    for (size_t index = 0U;
         index < sizeof(process_clock) / sizeof(*process_clock); ++index) {
        clock_values[index] = engine->integer_parameters[process_clock[index]];
        clock_levels[index] =
            engine->integer_parameter_levels[process_clock[index]];
    }
    transfer_format(&stream, engine);
    for (size_t index = 0U;
         index < sizeof(process_clock) / sizeof(*process_clock); ++index) {
        engine->integer_parameters[process_clock[index]] = clock_values[index];
        engine->integer_parameter_levels[process_clock[index]] =
            clock_levels[index];
    }
    if (stream.failed) {
        /* Whatever was read points into this mapping; the engine is done
           for, and its destruction gives back nothing that was borrowed. */
        hstex_input_close(&mapped);
        return format_error(error, error_capacity, "%s is not a format",
                               path);
    }
    keep_mapping(engine, &mapped, adopt_carried_files(&mapped, trailer_at));
    /* What this format was told about the installation, offered to the
       lookup before the first name is asked for. */
    hstex_file_db_offer_trees(engine->texmf_trees, engine->texmf_trees_stamp);
    return hstex_rebuild_glyph_unicode_slots(engine, error, error_capacity);
}

int hstex_engine_adopt_format_files(struct hstex_engine *engine,
                                    const char *path)
{
    if (engine == NULL || path == NULL || engine->format_mapping != NULL) {
        return -1;
    }
    struct hstex_input mapped;
    char opened[256];
    if (hstex_input_open_private(path, &mapped, opened, sizeof(opened)) != 0) {
        return -1;
    }
    struct format_stream stream = {0};
    stream.bytes = mapped.data;
    stream.length = mapped.length;
    char magic[sizeof(hstex_format_magic)];
    transfer(&stream, magic, sizeof(magic));
    uint64_t layout = 0U;
    transfer(&stream, &layout, sizeof(layout));
    uint64_t trailer_at = 0U;
    transfer(&stream, &trailer_at, sizeof(trailer_at));
    if (stream.failed ||
        memcmp(magic, hstex_format_magic, sizeof(magic)) != 0 ||
        layout != hstex_format_layout() ||
        !adopt_carried_files(&mapped, trailer_at)) {
        hstex_input_close(&mapped);
        return -1;
    }
    keep_mapping(engine, &mapped, true);
    return 0;
}
