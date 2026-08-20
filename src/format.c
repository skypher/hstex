#include "hstex/engine.h"

#include "internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A format is the engine's state once the format source has been read, put
   by so that the next run need not read it again: the names the engine
   knows and what they mean, the registers, the parameters, the fonts and the
   hyphenation patterns. What a run builds as it goes is not in it, and a
   format is only written where none of that has begun. See
   docs/DECISIONS.md, the-format-a-run-starts-from. */

static const char hstex_format_magic[] = "HSTEX format 1\n";

/* How wide the records a format carries are. A format written by one build
   and read by another whose records are laid out differently is not a format
   at all, and saying so is better than reading nonsense out of it. */
static uint64_t hstex_format_layout(void)
{
    const size_t widths[] = {
        sizeof(struct hstex_engine),  sizeof(struct hstex_macro),
        sizeof(struct hstex_meaning), sizeof(struct hstex_node),
        sizeof(struct hstex_box),     sizeof(struct hstex_font),
        sizeof(struct hstex_glue),    sizeof(struct hstex_save_entry),
    };
    uint64_t digest = UINT64_C(0xcbf29ce484222325);
    for (size_t index = 0U; index < sizeof(widths) / sizeof(widths[0]);
         ++index) {
        digest = (digest ^ (uint64_t)widths[index]) * UINT64_C(0x100000001b3);
    }
    return digest;
}

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
    bool writing;
    bool failed;
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
    size_t capacity = 8U;
    while (capacity < *count) {
        if (capacity > SIZE_MAX / 2U) {
            stream->failed = true;
            return;
        }
        capacity *= 2U;
    }
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
    transfer_array(stream, &name, &name_length, NULL, 1U, false);
    font->name = name;
    size_t characters = 0U;
    if (stream->writing && font->characters != NULL) {
        characters = (size_t)HSTEX_FONT_CHARACTER_COUNT;
    }
    void *metrics = stream->writing ? (void *)font->characters : NULL;
    transfer_array(stream, &metrics, &characters, NULL,
                   sizeof(*font->characters), false);
    font->characters = metrics;
    void *lig_kern = stream->writing ? (void *)font->lig_kern : NULL;
    transfer_array(stream, &lig_kern, &font->lig_kern_count, NULL,
                   sizeof(*font->lig_kern), false);
    font->lig_kern = lig_kern;
    void *kerns = stream->writing ? (void *)font->kerns : NULL;
    transfer_array(stream, &kerns, &font->kern_count, NULL,
                   sizeof(*font->kerns), false);
    font->kerns = kerns;
    void *extensibles = stream->writing ? (void *)font->extensibles : NULL;
    transfer_array(stream, &extensibles, &font->extensible_count, NULL,
                   sizeof(*font->extensibles), false);
    font->extensibles = extensibles;
    void *dimens = stream->writing ? (void *)font->dimens : NULL;
    transfer_array(stream, &dimens, &font->dimen_count, &font->dimen_capacity,
                   sizeof(*font->dimens), false);
    font->dimens = dimens;
    TRANSFER_VALUE(stream, font->design_size);
    TRANSFER_VALUE(stream, font->identifier_cs);
    TRANSFER_VALUE(stream, font->size);
    TRANSFER_VALUE(stream, font->hyphen_character);
    TRANSFER_VALUE(stream, font->skew_character);
    TRANSFER_VALUE(stream, font->checksum);
}

/* Everything a format carries, in one order for both directions. */
static void transfer_format(struct format_stream *stream,
                            struct hstex_engine *engine)
{
    struct hstex_symbol_table *symbols = &engine->lexical_state.symbols;
    void *entries = symbols->entries;
    transfer_array(stream, &entries, &symbols->entry_count,
                   &symbols->entry_capacity, sizeof(*symbols->entries), true);
    symbols->entries = entries;
    void *slots = symbols->slots;
    size_t slot_capacity = symbols->slot_capacity;
    transfer_array(stream, &slots, &slot_capacity, &symbols->slot_capacity,
                   sizeof(*symbols->slots), true);
    symbols->slots = slots;
    void *bytes = symbols->bytes;
    transfer_array(stream, &bytes, &symbols->byte_count,
                   &symbols->byte_capacity, sizeof(*symbols->bytes), true);
    symbols->bytes = bytes;
    TRANSFER_VALUE(stream, engine->lexical_state.catcodes);
    TRANSFER_VALUE(stream, engine->lexical_state.end_line_character);
    TRANSFER_VALUE(stream, engine->lexical_state.paragraph_control_sequence);
    TRANSFER_VALUE(stream, engine->catcode_levels);

    void *meanings = engine->meanings;
    size_t meaning_capacity = engine->meaning_capacity;
    transfer_array(stream, &meanings, &meaning_capacity,
                   &engine->meaning_capacity, sizeof(*engine->meanings), true);
    engine->meanings = meanings;

    TRANSFER_VALUE(stream, engine->macro_free_list);
    TRANSFER_VALUE(stream, engine->macro_definitions);
    size_t macro_count = engine->macro_count;
    void *macros = engine->macros;
    transfer_array(stream, &macros, &macro_count, &engine->macro_capacity,
                   sizeof(*engine->macros), true);
    engine->macros = macros;
    engine->macro_count = macro_count;
    for (size_t index = 0U; index < macro_count && !stream->failed; ++index) {
        struct hstex_macro *macro = &engine->macros[index];
        transfer_tokens(stream, &macro->parameter_text,
                        &macro->parameter_count_tokens);
        transfer_tokens(stream, &macro->replacement, &macro->replacement_count);
    }

    size_t register_capacity = engine->count_capacity;
    TRANSFER_VALUE(stream, register_capacity);
    struct {
        void **base;
        size_t size;
    } registers[] = {
        {(void **)&engine->counts, sizeof(*engine->counts)},
        {(void **)&engine->count_levels, sizeof(*engine->count_levels)},
        {(void **)&engine->dimens, sizeof(*engine->dimens)},
        {(void **)&engine->dimen_levels, sizeof(*engine->dimen_levels)},
        {(void **)&engine->glues, sizeof(*engine->glues)},
        {(void **)&engine->glue_levels, sizeof(*engine->glue_levels)},
        {(void **)&engine->muglues, sizeof(*engine->muglues)},
        {(void **)&engine->muglue_levels, sizeof(*engine->muglue_levels)},
        {(void **)&engine->token_registers, sizeof(*engine->token_registers)},
        {(void **)&engine->token_register_levels,
         sizeof(*engine->token_register_levels)},
        {(void **)&engine->boxes, sizeof(*engine->boxes)},
        {(void **)&engine->box_levels, sizeof(*engine->box_levels)},
    };
    for (size_t index = 0U;
         index < sizeof(registers) / sizeof(registers[0]) && !stream->failed;
         ++index) {
        size_t count = register_capacity;
        transfer_array(stream, registers[index].base, &count, NULL,
                       registers[index].size, true);
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

    size_t token_list_count = engine->token_list_count;
    void *token_lists = engine->token_lists;
    transfer_array(stream, &token_lists, &token_list_count,
                   &engine->token_list_capacity, sizeof(*engine->token_lists), true);
    engine->token_lists = token_lists;
    engine->token_list_count = token_list_count;
    for (size_t index = 0U; index < token_list_count && !stream->failed;
         ++index) {
        struct hstex_token_list *list = &engine->token_lists[index];
        transfer_tokens(stream, &list->tokens, &list->count);
    }

    size_t font_count = engine->font_count;
    void *fonts = engine->fonts;
    transfer_array(stream, &fonts, &font_count, &engine->font_capacity,
                   sizeof(*engine->fonts), true);
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
    transfer_array(stream, &hyphen_roots, &hyphen_root_count, NULL,
                   sizeof(*engine->hyphen_roots), true);
    engine->hyphen_roots = hyphen_roots;
    void *hyphen_nodes = engine->hyphen_nodes;
    transfer_array(stream, &hyphen_nodes, &engine->hyphen_node_count,
                   &engine->hyphen_node_capacity, sizeof(*engine->hyphen_nodes), true);
    engine->hyphen_nodes = hyphen_nodes;
    void *hyphen_values = engine->hyphen_values;
    transfer_array(stream, &hyphen_values, &engine->hyphen_value_count,
                   &engine->hyphen_value_capacity,
                   sizeof(*engine->hyphen_values), true);
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
}

/* A format is written only from a run that has built nothing of its own, so
   that what is left out of it is what a fresh engine already has. */
static bool engine_is_quiet(const struct hstex_engine *engine)
{
    return engine->save_count == 0U && engine->conditional_count == 0U &&
           engine->group_level == 0U && engine->shipped_pages == 0U &&
           engine->dvi_file == NULL && engine->pdf_file == NULL;
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
    struct format_stream stream = {0};
    stream.file = file;
    stream.writing = true;
    transfer(&stream, (void *)(uintptr_t)(const void *)hstex_format_magic,
             sizeof(hstex_format_magic));
    uint64_t layout = hstex_format_layout();
    transfer(&stream, &layout, sizeof(layout));
    transfer_format(&stream, engine);
    if (fclose(file) != 0 || stream.failed) {
        return format_error(error, error_capacity, "cannot write %s", path);
    }
    return 0;
}

int hstex_engine_read_format(struct hstex_engine *engine, const char *path,
                             char *error, size_t error_capacity)
{
    if (engine == NULL || path == NULL) {
        return format_error(error, error_capacity, "invalid format read");
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return format_error(error, error_capacity, "cannot read %s", path);
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return format_error(error, error_capacity, "cannot read %s", path);
    }
    long length = ftell(file);
    rewind(file);
    if (length < 0) {
        (void)fclose(file);
        return format_error(error, error_capacity, "cannot read %s", path);
    }
    uint8_t *bytes = malloc((size_t)length == 0U ? 1U : (size_t)length);
    if (bytes == NULL ||
        fread(bytes, 1U, (size_t)length, file) != (size_t)length) {
        free(bytes);
        (void)fclose(file);
        return format_error(error, error_capacity, "cannot read %s", path);
    }
    (void)fclose(file);
    struct format_stream stream = {0};
    stream.bytes = bytes;
    stream.length = (size_t)length;
    char magic[sizeof(hstex_format_magic)];
    transfer(&stream, magic, sizeof(magic));
    if (stream.failed || memcmp(magic, hstex_format_magic, sizeof(magic)) != 0) {
        free(bytes);
        return format_error(error, error_capacity, "%s is not a format",
                               path);
    }
    uint64_t layout = 0U;
    transfer(&stream, &layout, sizeof(layout));
    if (stream.failed || layout != hstex_format_layout()) {
        free(bytes);
        return format_error(error, error_capacity,
                            "%s was written by a build whose records are laid "
                            "out differently; build the format again",
                            path);
    }
    transfer_format(&stream, engine);
    free(bytes);
    if (stream.failed) {
        return format_error(error, error_capacity, "%s is not a format",
                               path);
    }
    return 0;
}
