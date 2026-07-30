#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <string.h>
#include <wctype.h>

// #define DEBUG

#ifdef DEBUG
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...)
#endif

enum TokenType {
  AUTOMATIC_SEMICOLON,
  INDENT,
  OUTDENT,
  COMMA_OUTDENT,
  SIMPLE_STRING_START,
  SIMPLE_STRING_MIDDLE,
  SIMPLE_MULTILINE_STRING_START,
  INTERPOLATED_STRING_MIDDLE,
  INTERPOLATED_MULTILINE_STRING_MIDDLE,
  RAW_STRING_START,
  RAW_STRING_MIDDLE,
  RAW_STRING_MULTILINE_MIDDLE,
  SINGLE_LINE_STRING_END,
  MULTILINE_STRING_END,
  ELSE,
  CATCH,
  FINALLY,
  EXTENDS,
  DERIVES,
  WITH,
  BLOCK_COMMENT,
  SUPPRESS_BLOCK_COMMENT,
  ERROR_SENTINEL,
  COLON_EOL,
  OPERATOR_EOL,
  FLOATING_POINT_WITH_SEPARATORS,
  END_KEYWORD
};

const char* token_name[] = {
  "AUTOMATIC_SEMICOLON",
  "INDENT",
  "OUTDENT",
  "COMMA_OUTDENT",
  "SIMPLE_STRING_START",
  "SIMPLE_STRING_MIDDLE",
  "SIMPLE_MULTILINE_STRING_START",
  "INTERPOLATED_STRING_MIDDLE",
  "INTERPOLATED_MULTILINE_STRING_MIDDLE",
  "RAW_STRING_START",
  "RAW_STRING_MIDDLE",
  "RAW_STRING_MULTILINE_MIDDLE",
  "SINGLE_LINE_STRING_END",
  "MULTILINE_STRING_END",
  "ELSE",
  "CATCH",
  "FINALLY",
  "EXTENDS",
  "DERIVES",
  "WITH",
  "BLOCK_COMMENT",
  "SUPPRESS_BLOCK_COMMENT",
  "ERROR_SENTINEL",
  "COLON_EOL",
  "OPERATOR_EOL",
  "FLOATING_POINT_WITH_SEPARATORS",
  "END_KEYWORD"
};

typedef struct {
  Array(int16_t) indents;
  int16_t last_indentation_size;
  int16_t last_newline_count;
  int16_t last_column;
  int16_t after_colon_eol;
} Scanner;

void *tree_sitter_scala_external_scanner_create() {
  Scanner *scanner = ts_calloc(1, sizeof(Scanner));
  array_init(&scanner->indents);
  scanner->last_indentation_size = -1;
  scanner->last_column = -1;
  return scanner;
}

void tree_sitter_scala_external_scanner_destroy(void *payload) {
  Scanner *scanner = payload;
  array_delete(&scanner->indents);
  ts_free(scanner);
}

unsigned tree_sitter_scala_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = (Scanner*)payload;

  if ((scanner->indents.size + 4) * sizeof(int16_t) > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    return 0;
  }

  size_t size = 0;
  memcpy(buffer + size, &scanner->last_indentation_size, sizeof(int16_t));
  size += sizeof(int16_t);
  memcpy(buffer + size, &scanner->last_newline_count, sizeof(int16_t));
  size += sizeof(int16_t);
  memcpy(buffer + size, &scanner->last_column, sizeof(int16_t));
  size += sizeof(int16_t);
  memcpy(buffer + size, &scanner->after_colon_eol, sizeof(int16_t));
  size += sizeof(int16_t);

  for (unsigned i = 0; i < scanner->indents.size; i++) {
    memcpy(buffer + size, &scanner->indents.contents[i], sizeof(int16_t));
    size += sizeof(int16_t);
  }

  return size;
}

void tree_sitter_scala_external_scanner_deserialize(void *payload, const char *buffer,
                                                    unsigned length) {
  Scanner *scanner = (Scanner*)payload;
  array_clear(&scanner->indents);
  scanner->last_indentation_size = -1;
  scanner->last_column = -1;
  scanner->last_newline_count = 0;
  scanner->after_colon_eol = 0;

  if (length == 0) {
    return;
  }

  size_t size = 0;

  scanner->last_indentation_size = *(int16_t *)&buffer[size];
  size += sizeof(int16_t);
  scanner->last_newline_count = *(int16_t *)&buffer[size];
  size += sizeof(int16_t);
  scanner->last_column = *(int16_t *)&buffer[size];
  size += sizeof(int16_t);
  scanner->after_colon_eol = *(int16_t *)&buffer[size];
  size += sizeof(int16_t);

  while (size < length) {
    array_push(&scanner->indents, *(int16_t *)&buffer[size]);
    size += sizeof(int16_t);
  }

  assert(size == length);
}

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool advance_past_blanks(TSLexer *lexer) {
  bool found = false;
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    advance(lexer);
    found = true;
  }
  return found;
}

// Marks an indent region whose `case` clauses align with their `match`/`catch`
// (Scala 3 same-width case). Such a region also closes on a same-width line
// that is not another `case` clause. The flag is packed into the int16 width.
#define CASE_INDENT_FLAG ((int16_t)0x4000)

static inline int16_t indent_width(int16_t entry) {
  return entry == -1 ? -1 : (int16_t)(entry & 0x3FFF);
}

// Does a line at `width` sit exactly at the width of a flagged same-width
// case region `prev`? Shared by every close site of such regions.
static inline bool at_case_region_width(int16_t prev, int16_t width) {
  return prev != -1 && (prev & CASE_INDENT_FLAG) && width == indent_width(prev);
}

// Used to detect leading infix operators on continuation lines.
// See: https://www.scala-lang.org/api/3.x/docs/changed-features/operators.html
static bool is_op_char(int32_t c) {
  switch (c) {
    case '!': case '#': case '%': case '&':
    case '*': case '+': case '-': case '<': 
    case '=': case '>': case '?': case '@':
    case '\\': case '^': case '|': case '~': 
    case ':':
      return true;
    default:
      return false;
  }
}

// We enumerate 3 types of strings that we need to handle differently:
// 1. Simple strings, `"..."` or `"""..."""`
// 2. Interpolated strings, `s"..."` or `f"..."` or `foo"..."` or foo"""...""".
// 3. Raw strings, `raw"..."`
typedef enum {
  STRING_MODE_SIMPLE,
  STRING_MODE_INTERPOLATED,
  STRING_MODE_RAW
} StringMode;

static bool scan_string_content(TSLexer *lexer, bool is_multiline, StringMode string_mode) {
  LOG("scan_string_content(%d, %d, %c)\n", is_multiline, string_mode, lexer->lookahead);
  unsigned closing_quote_count = 0;
  for (;;) {
    if (lexer->lookahead == '"') {
      advance(lexer);
      closing_quote_count++;
      if (!is_multiline) {
        lexer->result_symbol = SINGLE_LINE_STRING_END;
        lexer->mark_end(lexer);
        return true;
      }
      if (closing_quote_count >= 3 && lexer->lookahead != '"') {
        lexer->result_symbol = MULTILINE_STRING_END;
        lexer->mark_end(lexer);
        return true;
      }
    } else if (lexer->lookahead == '$' && string_mode != STRING_MODE_SIMPLE) {
      switch (string_mode) {
        case STRING_MODE_INTERPOLATED:
          lexer->result_symbol = is_multiline ? INTERPOLATED_MULTILINE_STRING_MIDDLE : INTERPOLATED_STRING_MIDDLE;
          break;
        case STRING_MODE_RAW:
          lexer->result_symbol = is_multiline ? RAW_STRING_MULTILINE_MIDDLE : RAW_STRING_MIDDLE;
          break;
        default:
          assert(false);          
      }
      lexer->mark_end(lexer);
      return true;
    } else {
      closing_quote_count = 0;
      if (lexer->lookahead == '\\') {
        // Multiline strings ignore escape sequences
        if (is_multiline || string_mode == STRING_MODE_RAW) {
          // FIXME: In raw string mode, we have to jump over escaped quotes.
          advance(lexer);
          // In single-line raw strings, `\"` is not translated to `"`, but it also does
          // not close the string. Likewise, `\\` is not translated to `\`, but it does
          // stop the second `\` from stopping a double-quote from closing the string.
          if (!is_multiline && string_mode == STRING_MODE_RAW && 
            (lexer->lookahead == '"' || lexer->lookahead == '\\')) {
            advance(lexer);
          }
        } else {
          lexer->result_symbol = string_mode == STRING_MODE_SIMPLE ? SIMPLE_STRING_MIDDLE : INTERPOLATED_STRING_MIDDLE;
          lexer->mark_end(lexer);
          return true;
        }
      // During error recovery and dynamic precedence resolution, the external 
      // scanner will be invoked with all valid_symbols set to true, which means
      // we will be asked to scan a string token when we are not actually in a 
      // string context. Here we detect these cases and return false.
      } else if (lexer->lookahead == '\n' && !is_multiline) {
        return false;
      } else if (lexer->eof(lexer)) {
        return false;
      } else {
        advance(lexer);
      }
    }
  }
}

// Consumes a block comment body, nesting per SLS 1.4. The caller has consumed
// the leading "/*". With stop_at_newline it stops at the first newline.
// Returns whether the comment closed on the line it started on.
static bool consume_block_comment_body_ex(TSLexer *lexer, bool stop_at_newline) {
  unsigned depth = 1;
  while (depth > 0) {
    if (lexer->eof(lexer) || (stop_at_newline && lexer->lookahead == '\n')) {
      return false;
    }
    if (lexer->lookahead == '/') {
      advance(lexer);
      if (lexer->lookahead == '*') {
        advance(lexer);
        depth++;
      }
    } else if (lexer->lookahead == '*') {
      advance(lexer);
      if (lexer->lookahead == '/') {
        advance(lexer);
        depth--;
      }
    } else {
      advance(lexer);
    }
  }
  return true;
}

// Stops just past the matching "*/" (or at EOF).
static void consume_block_comment_body(TSLexer *lexer) {
  consume_block_comment_body_ex(lexer, false);
}

// Stops at a newline. Returns whether the comment ended on the line it
// started on. A comment crossing the line end means the line ends there.
static bool consume_block_comment_body_on_line(TSLexer *lexer) {
  return consume_block_comment_body_ex(lexer, true);
}

static bool finish_block_comment(TSLexer *lexer) {
  lexer->mark_end(lexer);
  lexer->result_symbol = BLOCK_COMMENT;
  LOG("    BLOCK_COMMENT\n");
  return true;
}

// The caller has consumed the leading "/*".
static bool lex_block_comment(TSLexer *lexer) {
  consume_block_comment_body(lexer);
  return finish_block_comment(lexer);
}

static bool scan_word(TSLexer *lexer, const char* const word) {
  for (uint8_t i = 0; word[i] != '\0'; i++) {
    if (lexer->lookahead != word[i]) {
      return false;
    }
    advance(lexer);
  }
  // `match_` must not match the keyword `match`.
  return !(iswalnum(lexer->lookahead) || lexer->lookahead == '_' ||
           lexer->lookahead == '$');
}

// Reads one identifier-like word into `buf`. Returns -1 when the word
// cannot be an ASCII keyword. The whole word is always consumed, so a
// failed keyword check never leaves the lexer mid-identifier.
static int read_word(TSLexer *lexer, char *buf, int cap) {
  int len = 0;
  bool not_keyword = false;
  while (iswalnum(lexer->lookahead) || lexer->lookahead == '_' ||
         lexer->lookahead == '$') {
    if (lexer->lookahead > 127 || len >= cap - 1) {
      not_keyword = true;
    } else {
      buf[len] = (char)lexer->lookahead;
      len++;
    }
    advance(lexer);
  }
  buf[len] = '\0';
  return not_keyword ? -1 : len;
}

static bool word_in(const char *word, const char *const words[],
                    unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    if (strcmp(word, words[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Whether the lookahead word closes the enclosing indented region.
// else/catch/finally all can. The line-leading-else hazard that once
// excluded else is now ruled out by the caller's newline_count == 0 guard.
static bool is_block_closing_keyword(TSLexer *lexer) {
  switch (lexer->lookahead) {
    case 'e': return scan_word(lexer, "else");
    case 'c': return scan_word(lexer, "catch");
    case 'f': return scan_word(lexer, "finally");
    default: return false;
  }
}

// True when `class` or `object` follows `case`, marking a definition not a
// clause. Reads the whole word so an identifier like `cobject` is not misread
// as `c` plus `object` by chained scan_word calls. Advances the lexer.
static bool is_case_definition_word(TSLexer *lexer) {
  advance_past_blanks(lexer);
  char word[sizeof "object"];
  int len = read_word(lexer, word, (int)sizeof word);
  return len > 0 &&
         (strcmp(word, "class") == 0 || strcmp(word, "object") == 0);
}

// True when the line starts a `case` clause and not a `case class`/`case
// object` definition. Advances the lexer. The caller must have called
// mark_end already, or must return false.
static bool is_case_clause_intro(TSLexer *lexer) {
  return scan_word(lexer, "case") && !is_case_definition_word(lexer);
}

// Result of looking for a comment at a layout boundary (INDENT/OUTDENT).
typedef enum {
  COMMENT_NONE,   // not a comment; the lexer may have advanced past a lone '/'
  COMMENT_LEXED,  // a block comment was lexed as BLOCK_COMMENT: return true
  COMMENT_ABORT,  // a comment starts here: give up on the layout token
  COMMENT_SAME_LINE_CODE,  // comments skipped, code follows on the same
                           // line and its column is the indentation
} CommentAtLayout;

// Comments must not affect indentation. Block comments have to be lexed
// here because the internal lexer no longer knows them.
static CommentAtLayout check_comment_at_layout(TSLexer *lexer,
                                               const bool *valid_symbols) {
  if (lexer->lookahead != '/') {
    return COMMENT_NONE;
  }
  advance(lexer);
  if (lexer->lookahead == '*' && valid_symbols[BLOCK_COMMENT]) {
    advance(lexer);
    for (;;) {
      consume_block_comment_body(lexer);
      advance_past_blanks(lexer);
      if (lexer->eof(lexer) || lexer->lookahead == '\n' ||
          lexer->lookahead == '\r') {
        finish_block_comment(lexer);
        return COMMENT_LEXED;
      }
      if (lexer->lookahead == '/') {
        advance(lexer);
        if (lexer->lookahead == '*') {
          advance(lexer);
          continue;  // another block comment on the same line
        }
        if (lexer->lookahead == '/') {
          // The extent cannot be re-marked back, so consume the trailing
          // line comment into the comment token.
          while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            advance(lexer);
          }
          finish_block_comment(lexer);
          return COMMENT_LEXED;
        }
        // A lone '/' is code. The off-by-one column is harmless.
        return COMMENT_SAME_LINE_CODE;
      }
      return COMMENT_SAME_LINE_CODE;
    }
  }
  if (lexer->lookahead == '/' || lexer->lookahead == '*') {
    return COMMENT_ABORT;
  }
  // A lone '/' is not a comment. The lexer stays advanced past it.
  return COMMENT_NONE;
}

// Skips a delimited body (a string or a back-ticked identifier) whose opening
// delimiter the caller has consumed. Stops after the closing `close` or at
// EOL/EOF. With escapes, a backslash escapes the next character.
static void skip_delimited(TSLexer *lexer, int32_t close, bool escapes) {
  while (!lexer->eof(lexer) && lexer->lookahead != close &&
         lexer->lookahead != '\n') {
    if (escapes && lexer->lookahead == '\\') {
      advance(lexer);
    }
    advance(lexer);
  }
  if (lexer->lookahead == close) {
    advance(lexer);
  }
}

// After an opening `'`. A character literal reveals no code and returns 0. A
// quote reveals its first character, which the caller still needs for bracket
// depth, so that character is returned instead.
static int32_t skip_char_or_quote_tail(TSLexer *lexer) {
  if (lexer->lookahead == '\\') {
    advance(lexer);
    if (!lexer->eof(lexer) && lexer->lookahead != '\n') {
      advance(lexer);
    }
    if (lexer->lookahead == '\'') {
      advance(lexer);
    }
    return 0;
  }
  if (lexer->eof(lexer) || lexer->lookahead == '\n') {
    return 0;
  }
  int32_t quoted = lexer->lookahead;
  advance(lexer);
  if (lexer->lookahead == '\'') {
    advance(lexer);  // a character literal: `quoted` was not code
    return 0;
  }
  return quoted;
}

// Whether the rest of the line holds only blanks and comments. A comment
// running past the line end still counts as blank because comments are
// transparent to scalac, but code after a closed block comment does not.
static bool rest_of_line_is_blank_or_comments(TSLexer *lexer) {
  for (;;) {
    advance_past_blanks(lexer);
    if (lexer->eof(lexer) || lexer->lookahead == '\n' ||
        lexer->lookahead == '\r') {
      return true;
    }
    if (lexer->lookahead != '/') {
      return false;
    }
    advance(lexer);
    if (lexer->lookahead == '/') {
      while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
        advance(lexer);
      }
      return true;
    }
    if (lexer->lookahead != '*') {
      return false;  // a lone '/' is code
    }
    advance(lexer);
    if (!consume_block_comment_body_on_line(lexer)) {
      return true;  // the comment runs past the line end
    }
  }
}

// Whether a real operand follows the operator. Comments and line breaks are
// transparent, so scalac reads the next real token across them and only a
// comment-only tail to EOF means there is none.
static bool has_operand(TSLexer *lexer) {
  for (;;) {
    if (lexer->eof(lexer)) {
      return false;
    }
    if (iswspace(lexer->lookahead)) {
      advance(lexer);
      continue;
    }
    if (lexer->lookahead != '/') {
      return true;
    }
    advance(lexer);
    if (lexer->lookahead == '/') {
      while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
        advance(lexer);
      }
      continue;
    }
    if (lexer->lookahead != '*') {
      return true;  // a lone '/' is code
    }
    advance(lexer);
    consume_block_comment_body(lexer);
  }
}

// Returns true if the lookahead starts a leading infix operator — a symbolic
// operator or back-ticked identifier followed by whitespace and then an
// operand. Such a line is a continuation of the previous expression, so
// neither AUTOMATIC_SEMICOLON nor OUTDENT should fire ahead of it. Advances
// the lexer; the caller must not rely on position.
static bool is_leading_infix_continuation(TSLexer *lexer) {
  if (is_op_char(lexer->lookahead)) {
    advance(lexer);
    while (is_op_char(lexer->lookahead)) {
      advance(lexer);
    }
    return advance_past_blanks(lexer) && has_operand(lexer);
  }
  if (lexer->lookahead == '`') {
    advance(lexer);
    while (lexer->lookahead != '`' && !lexer->eof(lexer)) {
      advance(lexer);
    }
    if (lexer->lookahead != '`') {
      return false;
    }
    advance(lexer);
    return advance_past_blanks(lexer) && has_operand(lexer);
  }
  return false;
}

// Walks the rest of the line, tracking bracket depth and skipping strings,
// characters, back-ticks and comments. Reports whether the line ends in a
// depth-0 `then`/`do` and whether it carries a depth-0 `=>` (Scala 2 `⇒`).
typedef struct {
  bool ends_conditional;
  bool has_case_arrow;
  bool closes_bracket;
} LineScan;

static LineScan scan_rest_of_line(TSLexer *lexer) {
  int depth = 0;
  LineScan r = {false, false, false};
  while (!lexer->eof(lexer) && lexer->lookahead != '\n' &&
         lexer->lookahead != '\r') {
    int32_t c = lexer->lookahead;
    if (c == ' ' || c == '\t') {
      advance(lexer);
    } else if (c == '(' || c == '[' || c == '{') {
      depth++;
      r.ends_conditional = false;
      advance(lexer);
    } else if (c == ')' || c == ']' || c == '}') {
      depth--;
      if (depth < 0) {
        r.closes_bracket = true;
      }
      r.ends_conditional = false;
      advance(lexer);
    } else if (c == '"' || c == '`') {
      // A string or a back-ticked identifier; only a string honours escapes.
      r.ends_conditional = false;
      advance(lexer);
      skip_delimited(lexer, c, c == '"');
    } else if (c == '\'') {
      r.ends_conditional = false;
      advance(lexer);
      int32_t quoted = skip_char_or_quote_tail(lexer);
      if (quoted == '(' || quoted == '[' || quoted == '{') {
        depth++;
      } else if (quoted == ')' || quoted == ']' || quoted == '}') {
        depth--;
      }
    } else if (c == '/') {
      advance(lexer);
      if (lexer->lookahead == '/') {
        break;  // rest of the line is a comment
      }
      if (lexer->lookahead == '*') {
        advance(lexer);
        if (!consume_block_comment_body_on_line(lexer)) {
          break;  // the comment runs past the line end
        }
      } else {
        r.ends_conditional = false;  // a lone '/' is code
      }
    } else if (c == 0x21D2) {  // `⇒`, the Scala 2 spelling of `=>`
      if (depth == 0) {
        r.has_case_arrow = true;
      }
      r.ends_conditional = false;
      advance(lexer);
    } else if (is_op_char(c)) {
      bool starts_eq = c == '=';
      advance(lexer);
      bool second_gt = lexer->lookahead == '>';
      int extra = 0;
      while (is_op_char(lexer->lookahead)) {
        advance(lexer);
        extra++;
      }
      // A plain `=>` at depth 0, not `==>` or `=>>`.
      if (depth == 0 && starts_eq && second_gt && extra == 1) {
        r.has_case_arrow = true;
      }
      r.ends_conditional = false;
    } else if (iswalpha(c) || c == '_' || c == '$') {
      char word[sizeof "then"];
      int len = read_word(lexer, word, (int)sizeof word);
      r.ends_conditional = depth == 0 && len > 0 &&
                           (strcmp(word, "then") == 0 || strcmp(word, "do") == 0);
    } else {
      r.ends_conditional = false;
      advance(lexer);
    }
  }
  return r;
}

// Consumes `digit (digit | '_' digit)*`. Sets *saw_sep if a separator appears.
static bool consume_digit_group(TSLexer *lexer, bool *saw_sep) {
  if (lexer->lookahead < '0' || lexer->lookahead > '9') {
    return false;
  }
  advance(lexer);
  for (;;) {
    if (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
      advance(lexer);
    } else if (lexer->lookahead == '_') {
      advance(lexer);
      if (lexer->lookahead < '0' || lexer->lookahead > '9') {
        return false;
      }
      if (saw_sep) {
        *saw_sep = true;
      }
      advance(lexer);
    } else {
      return true;
    }
  }
}

// Lexes a floating point literal whose integer part uses digit separators, e.g.
// `1_000.5`. The internal regex cannot. After an underscore-containing group
// its DFA loses the transition to the following `.`.
static bool scan_float_with_separator(TSLexer *lexer) {
  bool int_sep = false;
  if (!consume_digit_group(lexer, &int_sep)) {
    return false;
  }
  // Only the integer-part separator is ours. Without one the internal lexer
  // lexes the number, so bail before walking the fraction and exponent.
  if (!int_sep) {
    return false;
  }
  bool is_float = false;
  if (lexer->lookahead == '.') {
    advance(lexer);
    if (!consume_digit_group(lexer, NULL)) {
      return false;
    }
    is_float = true;
  }
  if (lexer->lookahead == 'e' || lexer->lookahead == 'E') {
    advance(lexer);
    if (lexer->lookahead == '+' || lexer->lookahead == '-') {
      advance(lexer);
    }
    if (!consume_digit_group(lexer, NULL)) {
      return false;
    }
    is_float = true;
  }
  if (lexer->lookahead == 'd' || lexer->lookahead == 'D' ||
      lexer->lookahead == 'f' || lexer->lookahead == 'F') {
    advance(lexer);
    is_float = true;
  }
  // A bare integer with separators (`1_000`) is the internal lexer's job.
  if (!is_float) {
    return false;
  }
  lexer->mark_end(lexer);
  lexer->result_symbol = FLOATING_POINT_WITH_SEPARATORS;
  return true;
}

static inline void debug_indents(Scanner *scanner) {
  LOG("    indents(%d): ", scanner->indents.size);
  for (unsigned i = 0; i < scanner->indents.size; i++) {
    LOG("%d ", scanner->indents.contents[i]);
  }
  LOG("\n");
}

static bool scan_impl(void *payload, TSLexer *lexer,
                      const bool *valid_symbols);

bool tree_sitter_scala_external_scanner_scan(void *payload, TSLexer *lexer,
                                             const bool *valid_symbols) {
  Scanner *scanner = (Scanner *)payload;
  bool res = scan_impl(payload, lexer, valid_symbols);
  // Track whether the last returned token was a fewer-braces colon. Comments
  // in between keep the flag. A false return does not persist state anyway.
  if (res) {
    if (lexer->result_symbol == COLON_EOL) {
      scanner->after_colon_eol = 1;
    } else if (lexer->result_symbol != BLOCK_COMMENT) {
      scanner->after_colon_eol = 0;
    }
  }
  return res;
}

static bool scan_impl(void *payload, TSLexer *lexer,
                      const bool *valid_symbols) {
  #ifdef DEBUG
  {
    if (valid_symbols[ERROR_SENTINEL]) {
      LOG("entering tree_sitter_scala_external_scanner_scan. ERROR_SENTINEL is valid\n");
    } else {
      char debug_str[1024] = "entering tree_sitter_scala_external_scanner_scan valid symbols: ";
      for (unsigned i = 0; i < ERROR_SENTINEL; i++) {
        if (valid_symbols[i]) {
          strcat(debug_str, token_name[i]);
          strcat(debug_str, ", ");
        }
      }
      strcat(debug_str, "\n");
      LOG("%s", debug_str);
    }
  }
  #endif

  Scanner *scanner = (Scanner *)payload;
  int16_t prev = scanner->indents.size > 0 ? *array_back(&scanner->indents) : -1;
  int16_t prev_width = indent_width(prev);
  int16_t newline_count = 0;
  int16_t indentation_size = 0;

  while (iswspace(lexer->lookahead)) {
    if (lexer->lookahead == '\n') {
      newline_count++;
      indentation_size = 0;
    }
    else {
      indentation_size++;
    }
    skip(lexer);
  }

  // COMMA_OUTDENT is a distinct token so tree-sitter only makes it valid
  // where comma termination is expected (colon_argument,
  // _indentable_expression). A comma terminates the indented block only when
  // it ends its line. A comma with more code after it on the same line
  // continues the statement and is internal to the block (`import a.b, c.d`,
  // `extends A, B`, enum `case A, B`). A trailing comment still ends the line.
  if (valid_symbols[COMMA_OUTDENT] && lexer->lookahead == ',' && prev != -1) {
    lexer->mark_end(lexer);
    // Error recovery makes every symbol valid, so keep the eager pop there.
    if (!valid_symbols[ERROR_SENTINEL]) {
      advance(lexer);
      bool ends_line = false;
      for (;;) {
        advance_past_blanks(lexer);
        if (lexer->eof(lexer) || lexer->lookahead == '\n' ||
            lexer->lookahead == '\r') {
          ends_line = true;
          break;
        }
        if (lexer->lookahead != '/') {
          break;
        }
        advance(lexer);
        if (lexer->lookahead == '/') {
          ends_line = true;  // line comment runs to the line end
          break;
        }
        if (lexer->lookahead != '*') {
          break;  // a lone '/' is code
        }
        advance(lexer);
        consume_block_comment_body(lexer);
      }
      if (!ends_line) {
        return false;
      }
    }
    if (scanner->indents.size > 0) {
      array_pop(&scanner->indents);
    }
    lexer->result_symbol = COMMA_OUTDENT;
    return true;
  }

  // Closes a same-width case region mid-cascade after a deeper region popped and
  // the line sits at this region's width. It stays open only for another `case`
  // clause, and returning false is safe because `case` is the only token that can
  // follow. The column check guards against a stale last_indentation_size when
  // intervening states emit no external token.
  if (valid_symbols[OUTDENT] && !valid_symbols[ERROR_SENTINEL] &&
      scanner->last_indentation_size != -1 &&
      at_case_region_width(prev, scanner->last_indentation_size) &&
      (lexer->eof(lexer)
           ? scanner->last_column == -1
           : (int16_t)lexer->get_column(lexer) == scanner->last_column)) {
    lexer->mark_end(lexer);
    if (is_case_clause_intro(lexer)) {
      return false;
    }
    array_pop(&scanner->indents);
    LOG("    pop\n");
    LOG("    OUTDENT (same-width case region, cascade)\n");
    lexer->result_symbol = OUTDENT;
    return true;
  }

  // Before advancing the lexer, check if we can double outdent
  if (
      valid_symbols[OUTDENT] &&
      (
        lexer->lookahead == 0 ||
        (
          prev != -1 &&
          (
            lexer->lookahead == ')' ||
            lexer->lookahead == ']' ||
            lexer->lookahead == '}'
          )
        ) ||
        (
          scanner->last_indentation_size != -1 &&
          prev != -1 &&
          scanner->last_indentation_size < prev_width
        )
      )
  ) {
    if (scanner->indents.size > 0) {
        array_pop(&scanner->indents);
    }
    LOG("    pop\n");
    LOG("    OUTDENT\n");
    lexer->result_symbol = OUTDENT;
    return true;
  }
  scanner->last_indentation_size = -1;

  // True when the line is deeper than the current region. An empty stack has
  // prev_width == -1, so any width counts as deeper. A same-width line right
  // after a fewer-braces colon also opens a block (the body may sit there).
  bool indent_geometry =
      indentation_size > prev_width ||
      (scanner->after_colon_eol && indentation_size == prev_width);
  if (
      valid_symbols[INDENT] &&
      newline_count > 0 &&
      // An indented block cannot start with a closing delimiter, e.g. the
      // `}` after an empty-bodied lambda: `foo { x =>` + newline + `}`.
      lexer->lookahead != '}' &&
      lexer->lookahead != ')' &&
      lexer->lookahead != ']' &&
      (
        indent_geometry ||
        // A comment at exactly the region width can hide a deeper line
        // behind it. Probe it and let the code's column decide below.
        (indentation_size == prev_width && lexer->lookahead == '/')
      )
  ) {
    lexer->mark_end(lexer);
    switch (check_comment_at_layout(lexer, valid_symbols)) {
      case COMMENT_LEXED: return true;
      case COMMENT_ABORT: return false;
      case COMMENT_SAME_LINE_CODE: {
        // The line's content starts after the comment; its column is the
        // effective indentation (`if x then` + `  /* c */ body`).
        int16_t effective = (int16_t)lexer->get_column(lexer);
        if (effective > prev_width) {
          indentation_size = effective;
          break;
        }
        // The code does not open a block after all, so lex the comment.
        return finish_block_comment(lexer);
      }
      case COMMENT_NONE:
        if (!indent_geometry) {
          return false;  // entered only to probe a comment; this is code
        }
        break;
    }
    // An indented block cannot start with else/catch/finally, nor with
    // yield/do, which belong to an enclosing `for (...)` whose body is also
    // indentable. Read the word whole so a partial match cannot mislead.
    int16_t entry = indentation_size;
    switch (lexer->lookahead) {
      case 'e': case 'c': case 'f': case 'y': case 'd': {
        static const char *const block_opening_stoppers[] = {
            "else", "catch", "finally", "yield", "do"};
        char word[sizeof "finally"];
        int len = read_word(lexer, word, (int)sizeof word);
        if (len > 0 &&
            word_in(word, block_opening_stoppers,
                    sizeof(block_opening_stoppers) /
                        sizeof(block_opening_stoppers[0]))) {
          return false;
        }
        // At top level the stack is empty, so the same-width `case` close at
        // the bottom of scan() never sees a width-0 `match`/`case`. Flag the
        // region here instead. The arrow rules out an enum `case Foo`, which
        // has none and must not open a case region.
        if (scanner->indents.size == 0 && len == 4 &&
            strcmp(word, "case") == 0 && !is_case_definition_word(lexer) &&
            scan_rest_of_line(lexer).has_case_arrow) {
          entry |= CASE_INDENT_FLAG;
        }
        break;
      }
      case '|':
      case '&':
        // Nor with a leading `|`/`&` infix operator, which continues the
        // previous expression. Only these two run here, and not after a colon,
        // where the line can only be the body.
        if (!scanner->after_colon_eol && is_leading_infix_continuation(lexer)) {
          return false;
        }
        break;
      default:
        break;
    }
    array_push(&scanner->indents, entry);
    lexer->result_symbol = INDENT;
    LOG("    INDENT\n");
    return true;
  }

  // This saves the indentation_size and newline_count so it can be used
  // in subsequent calls for multiple outdent or auto-semicolon.
  // A same-width case region also closes on a line at its own width when
  // that line does not start another `case` clause.
  bool case_region_close =
      newline_count > 0 && at_case_region_width(prev, indentation_size);
  if (valid_symbols[OUTDENT] &&
      (lexer->lookahead == 0 ||
      (
        newline_count > 0 &&
        prev != -1 &&
        indentation_size < prev_width
      ) ||
      case_region_close
      )
  ) {
    lexer->mark_end(lexer);
    switch (check_comment_at_layout(lexer, valid_symbols)) {
      case COMMENT_LEXED: return true;
      case COMMENT_ABORT: return false;
      case COMMENT_SAME_LINE_CODE: {
        // The line's content starts after the comment; its column is the
        // effective indentation (` /** doc */ override def f...` inside a
        // braced body must not close the region at the comment's column).
        int16_t effective = (int16_t)lexer->get_column(lexer);
        if (effective < prev_width) {
          indentation_size = effective;
          break;
        }
        // Not an outdent after all. The automatic semicolon has its own
        // suppression rules, so lex the comment and let the next scan
        // decide, carrying the newline through the recovery below.
        scanner->last_newline_count = newline_count;
        scanner->last_column = effective;
        return finish_block_comment(lexer);
      }
      case COMMENT_NONE: break;
    }
    scanner->last_indentation_size = indentation_size;
    scanner->last_newline_count = newline_count;
    if (lexer->eof(lexer)) {
      scanner->last_column = -1;
    } else {
      scanner->last_column = (int16_t)lexer->get_column(lexer);
    }
    // Keep the indented block open when the next line starts with a leading
    // infix operator, which continues the previous expression. But a line
    // ending in depth-0 `then`/`do` closes the regions up to its conditional.
    if (lexer->lookahead != 0 && is_leading_infix_continuation(lexer) &&
        !scan_rest_of_line(lexer).ends_conditional) {
      return false;
    }
    // A same-width `case` line continues the case region instead.
    if (case_region_close && is_case_clause_intro(lexer)) {
      return false;
    }
    if (scanner->indents.size > 0) {
      array_pop(&scanner->indents);
    }
    LOG("    pop\n");
    LOG("    OUTDENT\n");
    lexer->result_symbol = OUTDENT;
    return true;
  }

  // Recover newline_count from the outdent reset. Skipped when this scan
  // crossed a newline itself, because the saved count belongs to an
  // earlier line at the same column.
  bool is_eof = lexer->eof(lexer);
  if (
      (
        scanner->last_newline_count > 0 &&
        (is_eof && scanner->last_column == -1)
      ) ||
      (!is_eof && newline_count == 0 &&
       lexer->get_column(lexer) == (uint32_t)scanner->last_column)
  ) {
    newline_count += scanner->last_newline_count;
  }
  scanner->last_newline_count = 0;

  // END_KEYWORD is only valid right after the semicolon the grammar puts
  // in front of a marker. Emit it when the tag word confirms the marker
  // shape.
  if (valid_symbols[END_KEYWORD] && !valid_symbols[ERROR_SENTINEL] &&
      lexer->lookahead == 'e') {
    if (scan_word(lexer, "end")) {
      for (;;) {
        advance_past_blanks(lexer);
        if (lexer->lookahead != '/') {
          break;
        }
        advance(lexer);
        if (lexer->lookahead != '*') {
          break;
        }
        advance(lexer);
        consume_block_comment_body(lexer);
      }
      if (iswalpha(lexer->lookahead) || lexer->lookahead == '_' ||
          lexer->lookahead == '$' || lexer->lookahead == '`' ||
          lexer->lookahead > 127) {
        lexer->mark_end(lexer);
        lexer->result_symbol = END_KEYWORD;
        return true;
      }
    }
    return false;
  }

  if (valid_symbols[AUTOMATIC_SEMICOLON] && newline_count > 0) {
    // AUTOMATIC_SEMICOLON should not be issued in the middle of expressions
    // Thus, we exit this branch when encountering comments, else/catch clauses, etc.

    lexer->mark_end(lexer);
    lexer->result_symbol = AUTOMATIC_SEMICOLON;

    // Probably, a multi-line field expression, e.g.
    // a
    //  .b
    //  .c
    if (lexer->lookahead == '.') {
      return false;
    }

    // A statement never ends right before a closing bracket, and marker
    // slots would otherwise ask for a semicolon here.
    if (lexer->lookahead == ')' || lexer->lookahead == ']' ||
        lexer->lookahead == ',') {
      return false;
    }

    // Same, and it keeps a braced else-if chain from forking one marker
    // head per nested if. A `}` line that continues with more code
    // (`} | Mate`) keeps the semicolon and the old reading of its tail. A
    // trailing comment counts as the line end.
    if (lexer->lookahead == '}') {
      advance(lexer);
      for (;;) {
        advance_past_blanks(lexer);
        if (lexer->lookahead == '}' || lexer->lookahead == ')' ||
            lexer->lookahead == ']') {
          advance(lexer);
          continue;
        }
        if (lexer->lookahead == '/') {
          advance(lexer);
          if (lexer->lookahead == '/') {
            return false;
          }
          if (lexer->lookahead == '*') {
            advance(lexer);
            consume_block_comment_body(lexer);
            continue;
          }
        }
        break;
      }
      if (lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
          lexer->eof(lexer)) {
        return false;
      }
      return true;
    }

    // Single-line and multi-line comments
    if (lexer->lookahead == '/') {
      advance(lexer);
      if (lexer->lookahead == '/') {
        return false;
      }
      if (lexer->lookahead == '*' && valid_symbols[BLOCK_COMMENT]) {
        advance(lexer);
        // The suppression rules must also see code after the comment
        // (`/* c */ else 2` must not be split). Lex the comment and carry
        // the pending newline to the next scan through the recovery above.
        consume_block_comment_body(lexer);
        advance_past_blanks(lexer);
        if (!(lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
              lexer->eof(lexer))) {
          scanner->last_newline_count = newline_count;
          scanner->last_column = (int16_t)lexer->get_column(lexer);
        }
        return finish_block_comment(lexer);
      }
      // A lone '/' falls through with the lexer advanced past it, matching
      // the old flow.
    }

    // Checked before the keyword scans so neither reads a position the
    // other advanced past (`m|| x` after a failed `match` scan). A blank
    // line still separates the statements.
    if (is_op_char(lexer->lookahead) || lexer->lookahead == '`') {
      if (newline_count == 1 && is_leading_infix_continuation(lexer)) {
        return false;
      }
      return true;
    }

    // A keyword that continues the enclosing expression suppresses the
    // semicolon, even when several are valid at once. The first-character
    // dispatch keeps scan_word from consuming a shared prefix.
    switch (lexer->lookahead) {
      case 'e':
        if (!valid_symbols[ELSE] && !valid_symbols[EXTENDS]) {
          break;
        }
        advance(lexer);
        if (valid_symbols[ELSE] && scan_word(lexer, "lse")) {
          return false;
        }
        if (valid_symbols[EXTENDS] && scan_word(lexer, "xtends")) {
          return false;
        }
        break;
      case 'c': {
        // Read the word whole: `catch` and `case` share a prefix that
        // chained scan_word calls cannot rewind.
        char word[sizeof "catch"];
        int len = read_word(lexer, word, (int)sizeof word);
        if (len <= 0) {
          break;
        }
        if (valid_symbols[CATCH] && strcmp(word, "catch") == 0) {
          return false;
        }
        // A case clause line needs no separator, and suppressing it keeps
        // a long else-if chain from forking one marker head per nested if
        // right before it. A case definition keeps its separator, and so
        // does a clause line that closes an enclosing bracket, whose
        // separator belongs to the surrounding expression.
        if (strcmp(word, "case") == 0 && !is_case_definition_word(lexer)) {
          LineScan line = scan_rest_of_line(lexer);
          if (line.has_case_arrow && !line.closes_bracket) {
            return false;
          }
        }
        break;
      }
      case 'f':
        if (valid_symbols[FINALLY] && scan_word(lexer, "finally")) {
          return false;
        }
        break;
      case 'w':
        if (valid_symbols[WITH] && scan_word(lexer, "with")) {
          return false;
        }
        break;
      case 'd':
        if (valid_symbols[DERIVES] && scan_word(lexer, "derives")) {
          return false;
        }
        break;
      case 'm':
        // `match` is a reserved word that never starts a statement, so it
        // always continues the previous expression.
        if (scan_word(lexer, "match")) {
          return false;
        }
        break;
      default:
        break;
    }

    return true;
  }

  // An else/catch/finally that is not directly shiftable must first close the
  // open indented region (a dedented `else` after a braceless match closes it).
  // Skipped during error recovery, where every symbol looks valid.
  if (
      valid_symbols[OUTDENT] &&
      !valid_symbols[ERROR_SENTINEL] &&
      newline_count == 0 &&
      prev != -1 &&
      (
        (lexer->lookahead == 'e' && !valid_symbols[ELSE]) ||
        (lexer->lookahead == 'c' && !valid_symbols[CATCH]) ||
        (lexer->lookahead == 'f' && !valid_symbols[FINALLY])
      )
  ) {
    lexer->mark_end(lexer);
    if (is_block_closing_keyword(lexer)) {
      if (scanner->indents.size > 0) {
        array_pop(&scanner->indents);
      }
      LOG("    pop\n");
      LOG("    OUTDENT (mid-line closing keyword)\n");
      lexer->result_symbol = OUTDENT;
      return true;
    }
    // The lexer has advanced past an identifier starting with e/c/f.
    // Nothing else external can match it, so give up.
    return false;
  }

  while (iswspace(lexer->lookahead)) {
    if (lexer->lookahead == '\n') {
      newline_count++;
    }
    skip(lexer);
  }

  // A floating point literal whose integer part uses digit separators.
  if (valid_symbols[FLOATING_POINT_WITH_SEPARATORS] &&
      !valid_symbols[ERROR_SENTINEL] && lexer->lookahead >= '0' &&
      lexer->lookahead <= '9') {
    return scan_float_with_separator(lexer);
  }

  // A symbolic infix operator that ends its line continues the expression on
  // the next line (SLS 1.2). Lexed here because the internal operator token
  // would let the newline split the statement, misreading it as postfix.
  if ((valid_symbols[OPERATOR_EOL] || valid_symbols[COLON_EOL]) &&
      !valid_symbols[ERROR_SENTINEL] && is_op_char(lexer->lookahead)) {
    // Collect the operator. Only the first 3 chars are kept, since anything
    // longer cannot be one of the reserved sequences checked below.
    char op[4] = {0};
    int op_len = 0;
    while (is_op_char(lexer->lookahead)) {
      if (op_len < 3) {
        op[op_len] = (char)lexer->lookahead;
      }
      op_len++;
      advance(lexer);
    }
    lexer->mark_end(lexer);
    if (op_len == 1 && op[0] == ':') {
      // A lone `:` ending its line is the fewer-braces colon (scalac's
      // COLONeol). Deciding it at the token level kills the cross-newline
      // ascription fork that GLR would otherwise keep alive across the region.
      if (valid_symbols[COLON_EOL] &&
          rest_of_line_is_blank_or_comments(lexer)) {
        lexer->result_symbol = COLON_EOL;
        return true;
      }
      return false;
    }
    // These sequences are not infix operators, so the line does not continue.
    static const char *const reserved[] = {
        "=", "#", "@", "=>", "<-", "<:", ">:", "<%", "?=>", "=>>"};
    if (op_len <= 3 &&
        word_in(op, reserved, sizeof(reserved) / sizeof(reserved[0]))) {
      return false;
    }
    // The branch is also entered for COLON_EOL-only states. Everything from
    // here on lexes OPERATOR_EOL, which must be valid.
    if (!valid_symbols[OPERATOR_EOL]) {
      return false;
    }
    // The operator must end its line. A trailing comment counts as the line
    // end because scalac treats it as transparent, so `p || // c` still
    // continues on the next line.
    if (!rest_of_line_is_blank_or_comments(lexer)) {
      return false;
    }
    // The right operand must be able to start an expression. Comments and line
    // breaks before it are transparent. A comment-only tail to EOF is postfix.
    if (!has_operand(lexer)) {
      return false;
    }
    // A next real token that closes a delimiter or list (a vararg splice `xs*`
    // before a `)`), or a lone `/`, also means the operator was postfix.
    if (lexer->lookahead == ')' || lexer->lookahead == ']' ||
        lexer->lookahead == '}' || lexer->lookahead == ',' ||
        lexer->lookahead == ';' || lexer->lookahead == '.' ||
        lexer->lookahead == '=' || lexer->lookahead == '/' ||
        // No expression starts with `@` either. The next line is an
        // annotated definition (`x --?` + `@deprecated val y = 1`).
        lexer->lookahead == '@') {
      return false;
    }
    // Nor can the operand be a keyword that only continues or starts a
    // statement. This matters for symbolic identifiers used as expressions.
    // In `if (c) ???` + `else d` the `???` must stay a plain expression.
    if (lexer->lookahead >= 'a' && lexer->lookahead <= 'z') {
      static const char *const non_operand_words[] = {
          "case", "catch",  "do",   "else",  "finally", "for",   "if",
          "match", "return", "then", "throw", "try",     "while", "yield",
          // Definition or modifier keywords. A line starting with one of these
          // begins a new statement, so the operator was postfix (`x --?` +
          // `val y = ...`, Scala 2 postfixOps).
          "abstract", "class", "def",    "enum",      "export", "final",
          "given",    "import", "implicit", "lazy",   "object", "override",
          "package",  "private", "protected", "sealed", "trait", "type",
          "val",      "var",
      };
      // Sized for the longest word above. read_word reports a longer or
      // non-ASCII identifier as -1, which correctly skips the check.
      char word[sizeof "protected"];
      int len = read_word(lexer, word, (int)sizeof word);
      if (len > 0 &&
          word_in(word, non_operand_words,
                  sizeof(non_operand_words) / sizeof(non_operand_words[0]))) {
        return false;
      }
    } else if (is_leading_infix_continuation(lexer)) {
      // A leading infix operator on the next line continues the postfix
      // reading of the previous line instead: `a ???` + `|| b`.
      return false;
    }
    lexer->result_symbol = OPERATOR_EOL;
    return true;
  }

  // Mid-line block comments with no layout decision pending. `/*` is plain
  // text where SUPPRESS_BLOCK_COMMENT or a string-content state is valid.
  // In error recovery all symbols look valid and lexing the comment is safe.
  if (valid_symbols[BLOCK_COMMENT] && lexer->lookahead == '/' &&
      (valid_symbols[ERROR_SENTINEL] ||
       !(valid_symbols[SUPPRESS_BLOCK_COMMENT] ||
         valid_symbols[SIMPLE_STRING_MIDDLE] ||
         valid_symbols[INTERPOLATED_STRING_MIDDLE] ||
         valid_symbols[RAW_STRING_MIDDLE] ||
         valid_symbols[RAW_STRING_MULTILINE_MIDDLE] ||
         valid_symbols[INTERPOLATED_MULTILINE_STRING_MIDDLE] ||
         valid_symbols[MULTILINE_STRING_END]))) {
    advance(lexer);
    if (lexer->lookahead == '*') {
      advance(lexer);
      return lex_block_comment(lexer);
    }
    // A lone '/' or a line comment. Nothing else external can match here.
    return false;
  }

  if (valid_symbols[SIMPLE_STRING_START] && lexer->lookahead == '"') {
    advance(lexer);
    lexer->mark_end(lexer);

    if (lexer->lookahead == '"') {
      advance(lexer);
      if (lexer->lookahead == '"') {
        advance(lexer);
        lexer->result_symbol = SIMPLE_MULTILINE_STRING_START;
        lexer->mark_end(lexer);
        return true;
      }
    }

    lexer->result_symbol = SIMPLE_STRING_START;
    return true;
  }

  // We need two tokens of lookahead to determine if we are parsing a raw string,
  // the `raw` and the `"`, which is why we need to do it in the external scanner.
  if (valid_symbols[RAW_STRING_START] && lexer->lookahead == 'r') {
    advance(lexer);
    if (lexer->lookahead == 'a') {
      advance(lexer);
      if (lexer->lookahead == 'w') {
        advance(lexer);
        if (lexer->lookahead == '"') {
          lexer->mark_end(lexer);
          lexer->result_symbol = RAW_STRING_START;
          return true;
        }
      }
    }
  }

  if (valid_symbols[SIMPLE_STRING_MIDDLE]) {
    return scan_string_content(lexer, false, STRING_MODE_SIMPLE);
  }

  if (valid_symbols[INTERPOLATED_STRING_MIDDLE]) {
    return scan_string_content(lexer, false, STRING_MODE_INTERPOLATED);
  }

  if (valid_symbols[RAW_STRING_MIDDLE]) {
    return scan_string_content(lexer, false, STRING_MODE_RAW);
  }

  if (valid_symbols[RAW_STRING_MULTILINE_MIDDLE]) {
    return scan_string_content(lexer, true, STRING_MODE_RAW);
  }  

  if (valid_symbols[INTERPOLATED_MULTILINE_STRING_MIDDLE]) {
    return scan_string_content(lexer, true, STRING_MODE_INTERPOLATED);
  }

  // We still need to handle the simple multiline string case, but there is
  // no `MULTILINE_STRING_MIDDLE` token, and `MULTILINE_STRING_END` is used
  // by all three of simple raw, and interpolated multiline strings. So this 
  // check needs to come after the `INTERPOLATED_MULTILINE_STRING_MIDDLE` and
  // `RAW_STRING_MULTILINE_MIDDLE` check, so that we can be sure we are in a 
  // simple multiline string context.
  if (valid_symbols[MULTILINE_STRING_END]) {
    return scan_string_content(lexer, true, STRING_MODE_SIMPLE);
  }

  // Scala 3 lets a `match`/`catch`'s `case` clauses align with the enclosing
  // region instead of indenting deeper. Open a flagged region so the OUTDENT
  // logic above closes it on a same-width non-case line. Only states after
  // `match`/`catch` or a case `=>` reach here, so no new grammar production is
  // needed. Returning false is safe because lookahead 'c' lets no other external
  // token fire.
  if (valid_symbols[INDENT] && !valid_symbols[ERROR_SENTINEL] &&
      newline_count > 0 && lexer->lookahead == 'c' &&
      prev != -1 && indentation_size == prev_width) {
    lexer->mark_end(lexer);
    if (is_case_clause_intro(lexer)) {
      array_push(&scanner->indents,
                 (int16_t)(indentation_size | CASE_INDENT_FLAG));
      lexer->result_symbol = INDENT;
      LOG("    INDENT (same-width case region)\n");
      return true;
    }
    return false;
  }

  return false;
}

//
