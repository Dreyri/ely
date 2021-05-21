#include "ely/reader.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ely/defines.h"
#include "ely/lexer.h"
#include "ely/util.h"

#define CHAR_LIT_OFFSET 2
#define KEYWORD_LIT_OFFSET 2

#define ELY_PARENS_LIST_NAME "paren-list"
#define ELY_BRACKET_LIST_NAME "bracket-list"
#define ELY_BRACE_LIST_NAME "brace-list"

typedef struct SkipResult
{
    uint32_t tokens_skipped;
    uint32_t bytes_skipped;
} SkipResult;

/// returns the number of tokens that were skipped
static ELY_ALWAYS_INLINE SkipResult skip_atmosphere(const ElyToken* tokens,
                                                    uint32_t        len)
{
    SkipResult res;
    res.bytes_skipped  = 0;
    res.tokens_skipped = 0;

    for (; res.tokens_skipped < len; ++res.tokens_skipped)
    {
        ElyToken tok = tokens[res.tokens_skipped];

        if (!ely_token_is_atmosphere(tok.kind))
        {
            return res;
        }

        res.bytes_skipped += tok.len;
    }

    return res;
}

static inline ElyReadResult reader_read_impl(ElyReader* reader,
                                             const char* __restrict__ src,
                                             ElyNode*        parent,
                                             const ElyToken* tokens,
                                             uint32_t        len,
                                             uint32_t        idx);

static inline ElyReadResult
continue_read_parens_list(ElyReader* reader,
                          const char* __restrict__ src,
                          ElyNode*        self,
                          const ElyToken* tokens,
                          uint32_t        len,
                          uint32_t        idx)
{
    ElyNodeParensList* plist = &self->parens_list;

    for (; idx < len; ++idx)
    {
        SkipResult skipped = skip_atmosphere(&tokens[idx], len - idx);
        idx += skipped.tokens_skipped;
        reader->current_byte += skipped.bytes_skipped;

        ElyToken tok = tokens[idx];

        switch (tok.kind)
        {
        case ELY_TOKEN_WHITESPACE:
        case ELY_TOKEN_TAB:
        case ELY_TOKEN_NEWLINE_CR:
        case ELY_TOKEN_NEWLINE_LF:
        case ELY_TOKEN_NEWLINE_CRLF:
        case ELY_TOKEN_COMMENT:
            __builtin_unreachable();
            assert(false && "There shouldn't be any atmosphere here");
        case ELY_TOKEN_RPAREN: {
            ++idx;
            reader->current_byte += tok.len;
            self->loc.end_byte = reader->current_byte;
            ElyReadResult res  = {
                .node            = self,
                .tokens_consumed = idx,
            };
            return res;
        }
        case ELY_TOKEN_RBRACE:
        case ELY_TOKEN_RBRACKET:
            // TODO: handle errors
            assert(false && "expected ')', didn't get that");
            __builtin_unreachable();
            break;
        case ELY_TOKEN_EOF:
            assert(false && "missing ')'");
            break;
        default: {
            // TODO: make this iterative instead of recursive by manually
            // keeping track of nesting depth. This would minimize any chance of
            // stack exhaustion.
            ElyReadResult next_node =
                reader_read_impl(reader, src, self, tokens, len, idx);
            ely_list_insert(plist->list.prev, &next_node.node->link);
            idx = next_node.tokens_consumed - 1; // this sets the correct index
            break;
        }
        }
    }

    reader->unfinished_node = self;

    ElyReadResult res = {
        .node            = ((void*) 0x1),
        .tokens_consumed = idx,
    };

    return res;
}

static inline ElyReadResult read_parens_list(ElyReader* reader,
                                             const char* __restrict__ src,
                                             ElyNode*        parent,
                                             const ElyToken* tokens,
                                             uint32_t        len,
                                             uint32_t        idx)
{
    // '(' is 1 byte therefore subtract to get start
    uint32_t start_byte = reader->current_byte - 1;

    ElyStxLocation stx_loc = {
        .filename = reader->filename, .start_byte = start_byte
        // .end_byte = undef
    };

    ElyNode*           node  = malloc(sizeof(ElyNode));
    ElyNodeParensList* plist = &node->parens_list;
    ely_node_create(node, parent, ELY_STX_PARENS_LIST, stx_loc);
    ely_list_create(&plist->list);

    ELY_MUSTTAIL return continue_read_parens_list(
        reader, src, node, tokens, len, idx);
}

static inline uint32_t ely_stx_location_write_length(const ElyStxLocation* loc)
{
    // [0 12]
    return 1 + ely_u32_count_numbers(loc->start_byte) + 1 +
           ely_u32_count_numbers(loc->end_byte) + 1;
}

static inline uint32_t
ely_stx_location_write_to_buffer(const ElyStxLocation* loc, char* buffer)
{
    buffer[0]       = '[';
    uint32_t offset = 1;
    offset += ely_u32_write_to_buffer(loc->start_byte, buffer + offset);
    buffer[offset++] = ' ';
    offset += ely_u32_write_to_buffer(loc->end_byte, buffer + offset);
    buffer[offset++] = ']';
    return offset;
}

void ely_node_create(ElyNode*       node,
                     ElyNode*       parent,
                     enum ElyStx    type,
                     ElyStxLocation loc)
{
    node->link.next = NULL;
    node->link.prev = NULL;
    node->parent    = parent;
    node->type      = type;
    node->loc       = loc;
}

void ely_node_destroy(ElyNode* node)
{
    switch (node->type)
    {
    case ELY_STX_PARENS_LIST: {
        ElyNodeParensList* plist = &node->parens_list;

        ElyNode *e, *tmp;
        ely_list_for_each_safe(e, tmp, &plist->list, link)
        {
            ely_node_destroy(e);
        }
    }
        ELY_FALLTHROUGH;
    default:
        free(node);
    }
}

uint32_t node_to_string_len(const ElyNode* node, uint32_t indent)
{
    // start and end parens, whitespace, indentation and location info
    uint32_t len = 3 + indent + ely_stx_location_write_length(&node->loc);
    switch (node->type)
    {
    case ELY_STX_IDENTIFIER:
        return len + ely_token_as_string(ELY_TOKEN_ID).len;
    case ELY_STX_STRING_LIT:
        return len + ely_token_as_string(ELY_TOKEN_STRING_LIT).len;
    case ELY_STX_INT_LIT:
        return len + ely_token_as_string(ELY_TOKEN_INT_LIT).len;
    case ELY_STX_FLOAT_LIT:
        return len + ely_token_as_string(ELY_TOKEN_FLOAT_LIT).len;
    case ELY_STX_CHAR_LIT:
        return len + ely_token_as_string(ELY_TOKEN_CHAR_LIT).len;
    case ELY_STX_KEYWORD_LIT:
        return len + ely_token_as_string(ELY_TOKEN_KEYWORD_LIT).len;
    case ELY_STX_TRUE_LIT:
        return len + ely_token_as_string(ELY_TOKEN_TRUE_LIT).len;
    case ELY_STX_FALSE_LIT:
        return len + ely_token_as_string(ELY_TOKEN_FALSE_LIT).len;
    case ELY_STX_PARENS_LIST: {
        const ElyNodeParensList* plist = &node->parens_list;

        ElyNode* e;
        ely_list_for_each(e, &plist->list, link)
        {
            len += 1; // for newline
            len += node_to_string_len(e, indent + 2);
        }

        return len + strlen(ELY_PARENS_LIST_NAME);
    }
    case ELY_STX_BRACKET_LIST: {
        const ElyNodeBracketList* blist = &node->bracket_list;

        ElyNode* e;
        ely_list_for_each(e, &blist->list, link)
        {
            len += 1; // for newline
            len += node_to_string_len(e, indent + 2);
        }

        return len + strlen(ELY_BRACKET_LIST_NAME);
    }
    case ELY_STX_BRACE_LIST: {
        const ElyNodeBraceList* blist = &node->brace_list;

        ElyNode* e;
        ely_list_for_each(e, &blist->list, link)
        {
            len += 1; // for newline
            len += node_to_string_len(e, indent + 2);
        }

        return len + strlen(ELY_BRACE_LIST_NAME);
    }
    default:
        __builtin_unreachable();
    }
}

uint32_t node_print_generic(uint32_t       indent,
                            ElyStringView  strv,
                            ElyStxLocation loc,
                            char*          buffer)
{
    const char* start = buffer;

    for (uint32_t i = 0; i != indent; ++i)
    {
        buffer[i] = ' ';
    }
    buffer += indent;

    *buffer++ = '(';
    memcpy(buffer, strv.data, strv.len);
    buffer += strv.len;

    *buffer++ = ' ';

    uint32_t loc_len = ely_stx_location_write_to_buffer(&loc, buffer);
    buffer += loc_len;
    *buffer++ = ')';
    return buffer - start;
}

uint32_t node_to_string_impl(const ElyNode* node, char* buffer, uint32_t indent)
{
    switch (node->type)
    {
    case ELY_STX_IDENTIFIER:
        return node_print_generic(
            indent, ely_token_as_string(ELY_TOKEN_ID), node->loc, buffer);
    case ELY_STX_KEYWORD_LIT:
        return node_print_generic(indent,
                                  ely_token_as_string(ELY_TOKEN_KEYWORD_LIT),
                                  node->loc,
                                  buffer);
    case ELY_STX_STRING_LIT:
        return node_print_generic(indent,
                                  ely_token_as_string(ELY_TOKEN_STRING_LIT),
                                  node->loc,
                                  buffer);
    case ELY_STX_INT_LIT:
        return node_print_generic(
            indent, ely_token_as_string(ELY_TOKEN_INT_LIT), node->loc, buffer);
    case ELY_STX_FLOAT_LIT:
        return node_print_generic(indent,
                                  ely_token_as_string(ELY_TOKEN_FLOAT_LIT),
                                  node->loc,
                                  buffer);
    case ELY_STX_CHAR_LIT:
        return node_print_generic(
            indent, ely_token_as_string(ELY_TOKEN_CHAR_LIT), node->loc, buffer);
    case ELY_STX_TRUE_LIT:
        return node_print_generic(
            indent, ely_token_as_string(ELY_TOKEN_TRUE_LIT), node->loc, buffer);
    case ELY_STX_FALSE_LIT:
        return node_print_generic(indent,
                                  ely_token_as_string(ELY_TOKEN_FALSE_LIT),
                                  node->loc,
                                  buffer);
    case ELY_STX_PARENS_LIST: {
        const ElyNodeParensList* plist = &node->parens_list;
        const char*              start = buffer;

        for (uint32_t i = 0; i != indent; ++i)
        {
            buffer[i] = ' ';
        }
        buffer += indent;

        *buffer++       = '(';
        size_t name_len = strlen(ELY_PARENS_LIST_NAME);
        memcpy(buffer, ELY_PARENS_LIST_NAME, name_len);
        buffer += name_len;

        *buffer++ = ' ';

        uint32_t loc_len = ely_stx_location_write_to_buffer(&node->loc, buffer);
        buffer += loc_len;

        const ElyNode* e;
        ely_list_for_each(e, &plist->list, link)
        {
            *buffer++         = '\n';
            uint32_t node_len = node_to_string_impl(e, buffer, indent + 2);
            buffer += node_len;
        }

        *buffer++ = ')';
        return buffer - start;
    }
    default:
        __builtin_unreachable();
    }
}

ElyString ely_node_to_string(const ElyNode* node)
{
    uint32_t buffer_size = node_to_string_len(node, 0);
    char*    data        = malloc(buffer_size);

    uint32_t written = node_to_string_impl(node, data, 0);

    ElyString res = {
        .data = data,
        .len  = written,
    };

    return res;
}

void ely_reader_create(ElyReader* reader, const char* filename)
{
    reader->filename        = filename;
    reader->current_byte    = 0;
    reader->unfinished_node = NULL;
}

static inline ElyNode* read_identifier(const char* __restrict__ src,
                                       ElyNode*       parent,
                                       ElyToken       tok,
                                       ElyStxLocation stx_loc)
{
    // we shouldn't blindly allocate the length of the token since we probably
    // don't need near that much capacity.
    // Therefore we'll scan for the vertical tab character 0x7c

    bool contains_vbar = false;

    for (uint32_t i = 0; i < tok.len; ++i)
    {
        char c = src[stx_loc.start_byte + i];

        if (c == '|')
        {
            contains_vbar = true;
            break;
        }
    }

    ElyNode* node;

    if (!contains_vbar)
    {
        size_t id_str_start = offsetof(ElyNode, id) + sizeof(ElyNodeIdentifier);
        size_t alloc_size   = id_str_start + tok.len;
        node                = malloc(alloc_size);
        ElyNodeIdentifier* ident = &node->id;
        memcpy(ident->str, &src[stx_loc.start_byte], tok.len);
        ely_node_create(node, parent, ELY_STX_IDENTIFIER, stx_loc);
    }
    else
    {
        assert(false && "cannot yet handle literal identifiers");
        __builtin_unreachable();
    }

    return node;
}

static inline ElyNode* read_string_lit(const char* __restrict__ src,
                                       ElyNode*       parent,
                                       ElyToken       tok,
                                       ElyStxLocation stx_loc)
{
    size_t   alloc_size = offsetof(ElyNode, str_lit) + sizeof(ElyNodeStringLit);
    uint32_t str_len    = tok.len - 2; // for leading and trailing quotes

    ElyNode*          node     = malloc(alloc_size + str_len);
    ElyNodeStringLit* str_node = &node->str_lit;

    memcpy(str_node->str, &src[stx_loc.start_byte + 1], str_len);
    ely_node_create(node, parent, ELY_STX_STRING_LIT, stx_loc);
    return node;
}

static inline ElyNode* read_int_lit(const char* __restrict__ src,
                                    ElyNode*       parent,
                                    ElyToken       tok,
                                    ElyStxLocation stx_loc)
{
    uint32_t alloc_size = offsetof(ElyNode, int_lit) + sizeof(ElyNodeIntLit);
    uint32_t str_len    = tok.len;

    ElyNode*       node     = malloc(alloc_size + str_len);
    ElyNodeIntLit* int_node = &node->int_lit;

    ely_node_create(node, parent, ELY_STX_INT_LIT, stx_loc);
    memcpy(int_node->str, &src[stx_loc.start_byte], str_len);
    return node;
}

static inline ElyNode* read_float_lit(const char* __restrict__ src,
                                      ElyNode*       parent,
                                      ElyToken       tok,
                                      ElyStxLocation stx_loc)
{
    uint32_t alloc_size =
        offsetof(ElyNode, float_lit) + sizeof(ElyNodeFloatLit);
    uint32_t str_len = tok.len;

    ElyNode*         node       = malloc(alloc_size + str_len);
    ElyNodeFloatLit* float_node = &node->float_lit;

    ely_node_create(node, parent, ELY_STX_FLOAT_LIT, stx_loc);
    memcpy(float_node->str, &src[stx_loc.start_byte], str_len);
    return node;
}

static inline ElyNode* read_char_lit(const char* __restrict__ src,
                                     ElyNode*       parent,
                                     ElyToken       tok,
                                     ElyStxLocation stx_loc)
{
    uint32_t alloc_size = offsetof(ElyNode, char_lit) + sizeof(ElyNodeCharLit);
    uint32_t str_len    = tok.len - CHAR_LIT_OFFSET;

    ElyNode*        node      = malloc(alloc_size + str_len);
    ElyNodeCharLit* char_node = &node->char_lit;

    ely_node_create(node, parent, ELY_STX_CHAR_LIT, stx_loc);
    memcpy(char_node->str, &src[stx_loc.start_byte + CHAR_LIT_OFFSET], str_len);
    return node;
}

static inline ElyNode* read_keyword_lit(const char* __restrict__ src,
                                        ElyNode*       parent,
                                        ElyToken       tok,
                                        ElyStxLocation stx_loc)
{
    uint32_t alloc_size = offsetof(ElyNode, kw_lit) + sizeof(ElyNodeKeywordLit);
    uint32_t str_len    = tok.len - KEYWORD_LIT_OFFSET;

    ElyNode*           node    = malloc(alloc_size + str_len);
    ElyNodeKeywordLit* kw_node = &node->kw_lit;

    ely_node_create(node, parent, ELY_STX_KEYWORD_LIT, stx_loc);
    memcpy(
        kw_node->str, &src[stx_loc.start_byte + KEYWORD_LIT_OFFSET], str_len);
    return node;
}

static inline ElyNode* read_true_lit(ElyNode* parent, ElyStxLocation stx_loc)
{
    _Static_assert(sizeof(ElyNodeTrueLit) == 0,
                   "ElyNodeTrueLit is no longer empty");

    ElyNode* node = malloc(sizeof(ElyNode));
    ely_node_create(node, parent, ELY_STX_TRUE_LIT, stx_loc);
    return node;
}

static inline ElyNode* read_false_lit(ElyNode* parent, ElyStxLocation stx_loc)
{
    _Static_assert(sizeof(ElyNodeFalseLit) == 0,
                   "ElyNodeFalseLit is no longer empty");

    ElyNode* node = malloc(sizeof(ElyNode));
    ely_node_create(node, parent, ELY_STX_FALSE_LIT, stx_loc);
    return node;
}

// after all atmosphere has been skipped
static inline ElyReadResult reader_read_impl(ElyReader* reader,
                                             const char* __restrict__ src,
                                             ElyNode*        parent,
                                             const ElyToken* tokens,
                                             uint32_t        len,
                                             uint32_t        idx)
{
    ElyToken tok = tokens[idx++];

    uint32_t end_byte = reader->current_byte + tok.len;

    ElyStxLocation stx_loc = {
        .filename   = reader->filename,
        .start_byte = reader->current_byte,
        .end_byte   = end_byte,
    };

    reader->current_byte = end_byte;

    switch (tok.kind)
    {
    case ELY_TOKEN_WHITESPACE:
    case ELY_TOKEN_TAB:
    case ELY_TOKEN_NEWLINE_CR:
    case ELY_TOKEN_NEWLINE_LF:
    case ELY_TOKEN_NEWLINE_CRLF:
    case ELY_TOKEN_COMMENT:
        __builtin_unreachable();
        assert(false && "There shouldn't be any atmosphere here");

    case ELY_TOKEN_LPAREN:
        ELY_MUSTTAIL return read_parens_list(
            reader, src, parent, tokens, len, idx);
    case ELY_TOKEN_LBRACKET:
    case ELY_TOKEN_LBRACE: {
        assert(false && "not yet implemented");
        ElyReadResult res = {.node = NULL, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_RPAREN:
    case ELY_TOKEN_RBRACKET:
    case ELY_TOKEN_RBRACE: {
        assert(false && "Unexpected closing");
        ElyReadResult res = {.node = NULL, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_ID: {
        ElyNode*      node = read_identifier(src, parent, tok, stx_loc);
        ElyReadResult res  = {.node = node, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_INT_LIT: {
        ElyNode*      node = read_int_lit(src, parent, tok, stx_loc);
        ElyReadResult res  = {.node = node, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_FLOAT_LIT: {
        ElyNode*      node = read_float_lit(src, parent, tok, stx_loc);
        ElyReadResult res  = {.node = node, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_STRING_LIT: {
        ElyNode*      node = read_string_lit(src, parent, tok, stx_loc);
        ElyReadResult res  = {.node = node, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_CHAR_LIT: {
        ElyNode*      node = read_char_lit(src, parent, tok, stx_loc);
        ElyReadResult res  = {.node = node, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_KEYWORD_LIT: {
        ElyNode*      node = read_keyword_lit(src, parent, tok, stx_loc);
        ElyReadResult res  = {.node = node, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_TRUE_LIT: {
        ElyNode*      node = read_true_lit(parent, stx_loc);
        ElyReadResult res  = {.node = node, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_FALSE_LIT: {
        ElyNode*      node = read_false_lit(parent, stx_loc);
        ElyReadResult res  = {.node = node, .tokens_consumed = idx};
        return res;
    }
    case ELY_TOKEN_EOF: {
        ElyReadResult res = {.node = NULL, .tokens_consumed = idx};
        return res;
    }
    default:
        __builtin_unreachable();
    }
}

ElyReadResult reader_continue_unfinished(ElyReader* reader,
                                         const char* __restrict__ src,
                                         const ElyToken* tokens,
                                         uint32_t        len)
{
    switch (reader->unfinished_node->type)
    {
    case ELY_STX_PARENS_LIST:
        return continue_read_parens_list(
            reader, src, reader->unfinished_node, tokens, len, 0);
    case ELY_STX_BRACKET_LIST:

    case ELY_STX_BRACE_LIST:

    default:
        __builtin_unreachable();
    }
}

ElyReadResult ely_reader_read(ElyReader* reader,
                              const char* __restrict__ src,
                              const ElyToken* tokens,
                              uint32_t        len)
{
    if (len == 0)
    {
        ElyReadResult res = {.node = NULL, .tokens_consumed = 0};
        return res;
    }

    if (reader->unfinished_node)
    {
        reader_continue_unfinished(reader, src, tokens, len);
    }

    SkipResult skipped = skip_atmosphere(tokens, len);
    reader->current_byte += skipped.bytes_skipped;

    return reader_read_impl(
        reader, src, NULL, tokens, len, skipped.tokens_skipped);
}

void ely_reader_read_all(ElyReader* reader,
                         const char* __restrict__ src,
                         const ElyToken* tokens,
                         uint32_t        len,
                         ElyList*        list)
{
    while (true)
    {
        ElyReadResult res = ely_reader_read(reader, src, tokens, len);

        if (res.node)
        {
            ely_list_insert(list->prev, &res.node->link);
            tokens += res.tokens_consumed;
            len -= res.tokens_consumed;
            //ELY_MUSTTAIL return ely_reader_read_all(
            //    reader, src, tokens, len, list);
        }
        else
        {
            return;
        }
    }
}