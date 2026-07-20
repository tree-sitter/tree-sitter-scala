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
  MATCH,
  COLON_EOL,
  XML_TAG_START,
  OPERATOR_EOL,
  BLOCK_COMMENT,
  SUPPRESS_BLOCK_COMMENT,
  ERROR_SENTINEL
};

// Designated initializers keep the names in sync with the enum by
// construction (a plain positional list silently desyncs when a token is
// added mid-enum).
const char* token_name[] = {
  [AUTOMATIC_SEMICOLON] = "AUTOMATIC_SEMICOLON",
  [INDENT] = "INDENT",
  [OUTDENT] = "OUTDENT",
  [COMMA_OUTDENT] = "COMMA_OUTDENT",
  [SIMPLE_STRING_START] = "SIMPLE_STRING_START",
  [SIMPLE_STRING_MIDDLE] = "SIMPLE_STRING_MIDDLE",
  [SIMPLE_MULTILINE_STRING_START] = "SIMPLE_MULTILINE_STRING_START",
  [INTERPOLATED_STRING_MIDDLE] = "INTERPOLATED_STRING_MIDDLE",
  [INTERPOLATED_MULTILINE_STRING_MIDDLE] = "INTERPOLATED_MULTILINE_STRING_MIDDLE",
  [RAW_STRING_START] = "RAW_STRING_START",
  [RAW_STRING_MIDDLE] = "RAW_STRING_MIDDLE",
  [RAW_STRING_MULTILINE_MIDDLE] = "RAW_STRING_MULTILINE_MIDDLE",
  [SINGLE_LINE_STRING_END] = "SINGLE_LINE_STRING_END",
  [MULTILINE_STRING_END] = "MULTILINE_STRING_END",
  [ELSE] = "ELSE",
  [CATCH] = "CATCH",
  [FINALLY] = "FINALLY",
  [EXTENDS] = "EXTENDS",
  [DERIVES] = "DERIVES",
  [WITH] = "WITH",
  [MATCH] = "MATCH",
  [COLON_EOL] = "COLON_EOL",
  [XML_TAG_START] = "XML_TAG_START",
  [OPERATOR_EOL] = "OPERATOR_EOL",
  [BLOCK_COMMENT] = "BLOCK_COMMENT",
  [SUPPRESS_BLOCK_COMMENT] = "SUPPRESS_BLOCK_COMMENT",
  [ERROR_SENTINEL] = "ERROR_SENTINEL"
};

typedef struct {
  Array(int16_t) indents;
  int16_t last_indentation_size;
  int16_t last_newline_count;
  int16_t last_column;
  // Set while the last returned token is COLON_EOL (comments in between keep
  // it). A colon argument's body may then start at the SAME width as the
  // current region (dotty allows it on continuation lines):
  //   noNotes:            <- region width 10 (body of an outer colon arg)
  //     ... elems.exists(...)
  //   && ifNotTried(x):   <- leading-infix continuation line
  //     val xelems = ...  <- body at width 10 == region width
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

// Advances past horizontal whitespace; returns whether any was consumed.
static bool advance_past_blanks(TSLexer *lexer) {
  bool found = false;
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    advance(lexer);
    found = true;
  }
  return found;
}

// Indent-stack entries carry this flag when the region holds `case` clauses
// aligned with their `match`/`catch` (allowed since Scala 3): unlike a normal
// region, it also closes on a line at the SAME width when that line does not
// start another `case` clause. The flag rides in the serialized int16 widths.
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

static bool scan_word(TSLexer *lexer, const char* const word) {
  for (uint8_t i = 0; word[i] != '\0'; i++) {
    if (lexer->lookahead != word[i]) {
      return false;
    }
    advance(lexer);
  }
  // `_` and `$` continue an identifier too: `match_` is not `match`.
  return !(iswalnum(lexer->lookahead) || lexer->lookahead == '_' ||
           lexer->lookahead == '$');
}

// Reads an identifier-like word (letters, digits, `_`, `$`) into `buf`
// (capacity `cap` including the NUL). Returns its length, or -1 when the
// word overflows the buffer or contains a non-ASCII character (it cannot be
// an ASCII keyword then); the whole word is consumed either way. Unlike a
// chain of scan_word calls, a failed keyword comparison never leaves the
// lexer mid-identifier (`cobject` must not be misread as `c` + `object`).
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

// Is `word` one of the `count` strings in `words`?
static bool word_in(const char *word, const char *const words[],
                    unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    if (strcmp(word, words[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Core of the block-comment skippers (nesting, SLS 1.4); the caller has
// consumed the leading "/*". Returns true when the matching "*/" was
// consumed; false when stopped early — at EOF, at a newline (when
// stop_at_newline; the newline is not consumed), or on budget exhaustion.
// Charges *budget per consumed character when budget is non-NULL.
static bool consume_block_comment_body_ex(TSLexer *lexer,
                                          bool stop_at_newline, int *budget) {
  unsigned depth = 1;
  while (depth > 0) {
    if (lexer->eof(lexer) || (budget != NULL && *budget <= 0) ||
        (stop_at_newline && lexer->lookahead == '\n')) {
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
    if (budget != NULL) {
      (*budget)--;
    }
  }
  return true;
}

// Stops just past the matching "*/" (or at EOF).
static void consume_block_comment_body(TSLexer *lexer) {
  consume_block_comment_body_ex(lexer, false, NULL);
}

// Stops at a newline: returns whether the comment ended on the line it
// started on. Used where a comment crossing the line end means "the line
// ends here" and the rest of the comment does not matter.
static bool consume_block_comment_body_on_line(TSLexer *lexer) {
  return consume_block_comment_body_ex(lexer, true, NULL);
}

// After a token that must end its line: is the rest only blanks and
// comments? A line comment, or a block comment running past the line end,
// counts as yes (comments are transparent to scalac's line handling); code
// after a same-line block comment counts as no.
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

// Skips a double-quoted string body; the caller has consumed the opening
// quote. Stops after the closing quote or at EOL/EOF (unterminated).
// Charges *budget per consumed character when non-NULL. Shared by the
// line scanners so their string handling cannot drift.
static void skip_string_tail(TSLexer *lexer, int *budget) {
  while (!lexer->eof(lexer) && lexer->lookahead != '"' &&
         lexer->lookahead != '\n') {
    if (lexer->lookahead == '\\') {
      advance(lexer);
      if (budget != NULL) {
        (*budget)--;
      }
    }
    advance(lexer);
    if (budget != NULL) {
      (*budget)--;
    }
  }
  if (lexer->lookahead == '"') {
    advance(lexer);
    if (budget != NULL) {
      (*budget)--;
    }
  }
}

// Skips a back-quoted identifier body; the caller has consumed the opening
// back-tick. Stops after the closing tick or at EOL/EOF.
static void skip_backticked_tail(TSLexer *lexer, int *budget) {
  while (!lexer->eof(lexer) && lexer->lookahead != '`' &&
         lexer->lookahead != '\n') {
    advance(lexer);
    if (budget != NULL) {
      (*budget)--;
    }
  }
  if (lexer->lookahead == '`') {
    advance(lexer);
    if (budget != NULL) {
      (*budget)--;
    }
  }
}

// After an opening `'`: a character literal (`'('`, `'\n'`) is skipped
// whole and reveals no code, while quote syntax (`'{ q }`, `'sym`) reveals
// its first character, which IS code the caller must process (most
// importantly for bracket-depth bookkeeping). Distinguishing the two needs
// two characters of lookahead. Returns the revealed character, or 0.
static int32_t skip_char_or_quote_tail(TSLexer *lexer, int *budget) {
  if (lexer->lookahead == '\\') {
    advance(lexer);
    if (!lexer->eof(lexer) && lexer->lookahead != '\n') {
      advance(lexer);
    }
    if (lexer->lookahead == '\'') {
      advance(lexer);
    }
    if (budget != NULL) {
      (*budget) -= 3;
    }
    return 0;
  }
  if (lexer->eof(lexer) || lexer->lookahead == '\n') {
    return 0;
  }
  int32_t quoted = lexer->lookahead;
  advance(lexer);
  if (budget != NULL) {
    (*budget)--;
  }
  if (lexer->lookahead == '\'') {
    advance(lexer);  // a character literal: `quoted` was not code
    if (budget != NULL) {
      (*budget)--;
    }
    return 0;
  }
  return quoted;
}

// After a leading operator and its whitespace: does an operand follow?
// Comments and line breaks are transparent — scalac reads the next real
// token, so `||  // but consider NotGiven` + newline + `tps.hasAnnotation`
// is a continuation — and only EOF (a comment-only tail) means there is
// none. An inline comment does not count as the operand itself, though:
// `|| /* c */ x` finds `x`, not the `/`.
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

// Scans the rest of the line and reports whether it ENDS in a `then` or
// `do` keyword at bracket depth 0 (skipping strings, characters, quotes,
// back-ticked identifiers and comments; a trailing comment still counts as
// the line end). A dedented leading infix operator whose line ends in
// `then`/`do` continues an enclosing conditional, so the indented regions
// between them must close:
//   if xs.indexSatisfying(from = index): imp =>
//       imp.selectors.exists(p)
//   < nextImport then
// Advances the lexer; the caller must have called mark_end.
static bool line_ends_conditional(TSLexer *lexer) {
  int depth = 0;
  // Whether the last thing seen is a depth-0 `then`/`do`; any code after it
  // clears the flag again, so only a line-final keyword reports true.
  bool pending = false;
  while (!lexer->eof(lexer) && lexer->lookahead != '\n' &&
         lexer->lookahead != '\r') {
    int32_t c = lexer->lookahead;
    if (c == ' ' || c == '\t') {
      advance(lexer);
    } else if (c == '(' || c == '[' || c == '{') {
      depth++;
      pending = false;
      advance(lexer);
    } else if (c == ')' || c == ']' || c == '}') {
      depth--;
      pending = false;
      advance(lexer);
    } else if (c == '"') {
      pending = false;
      advance(lexer);
      skip_string_tail(lexer, NULL);
    } else if (c == '\'') {
      pending = false;
      advance(lexer);
      int32_t quoted = skip_char_or_quote_tail(lexer, NULL);
      if (quoted == '(' || quoted == '[' || quoted == '{') {
        depth++;
      } else if (quoted == ')' || quoted == ']' || quoted == '}') {
        depth--;
      }
    } else if (c == '`') {
      // A back-ticked identifier is never the `then`/`do` keyword.
      pending = false;
      advance(lexer);
      skip_backticked_tail(lexer, NULL);
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
        pending = false;  // a lone '/' is code
      }
    } else if (iswalpha(c) || c == '_' || c == '$') {
      char word[sizeof "then"];
      int len = read_word(lexer, word, (int)sizeof word);
      pending = depth == 0 && len > 0 &&
                (strcmp(word, "then") == 0 || strcmp(word, "do") == 0);
    } else {
      pending = false;
      advance(lexer);
    }
  }
  return pending;
}

// Peeks whether the next word is a keyword that can only continue the
// enclosing expression (else/catch/finally): an indented block never starts
// with one. Advances the lexer; the caller must have called mark_end (or
// return false right away). The mid-line OUTDENT gate in scan() dispatches
// on the same first letters (e/c/f) before calling this; keep them in sync.
static bool is_block_closing_keyword(TSLexer *lexer) {
  switch (lexer->lookahead) {
    case 'e': return scan_word(lexer, "else");
    case 'c': return scan_word(lexer, "catch");
    case 'f': return scan_word(lexer, "finally");
    default: return false;
  }
}

// Scans the rest of the line for a `=>` (or `⇒`) arrow at bracket depth 0.
// This is what distinguishes a `case` CLAUSE from an enum `case A, B` /
// `case C(...) extends D` definition when deciding whether a freshly
// opened region holds case clauses (only clauses carry the arrow on their
// line). Advances the lexer; the caller must have called mark_end.
static bool line_has_case_arrow(TSLexer *lexer) {
  int depth = 0;
  while (!lexer->eof(lexer) && lexer->lookahead != '\n' &&
         lexer->lookahead != '\r') {
    int32_t c = lexer->lookahead;
    if (c == '(' || c == '[' || c == '{') {
      depth++;
      advance(lexer);
    } else if (c == ')' || c == ']' || c == '}') {
      depth--;
      advance(lexer);
    } else if (c == '"') {
      advance(lexer);
      skip_string_tail(lexer, NULL);
    } else if (c == '\'') {
      advance(lexer);
      int32_t quoted = skip_char_or_quote_tail(lexer, NULL);
      if (quoted == '(' || quoted == '[' || quoted == '{') {
        depth++;
      } else if (quoted == ')' || quoted == ']' || quoted == '}') {
        depth--;
      }
    } else if (c == '`') {
      advance(lexer);
      skip_backticked_tail(lexer, NULL);
    } else if (c == '/') {
      advance(lexer);
      if (lexer->lookahead == '/') {
        return false;  // rest of the line is a comment
      }
      if (lexer->lookahead == '*') {
        advance(lexer);
        if (!consume_block_comment_body_on_line(lexer)) {
          return false;  // the comment runs past the line end
        }
      }
    } else if (c == 0x21D2) {  // `⇒`, the Scala 2 spelling of `=>`
      advance(lexer);
      if (depth == 0) {
        return true;
      }
    } else if (is_op_char(c)) {
      bool starts_eq = c == '=';
      advance(lexer);
      bool second_gt = lexer->lookahead == '>';
      int extra = 0;
      while (is_op_char(lexer->lookahead)) {
        advance(lexer);
        extra++;
      }
      // Exactly `=>`: not `==>`, not `=>>` (extra counts what follows the
      // first character, so a plain arrow has exactly the `>`).
      if (depth == 0 && starts_eq && second_gt && extra == 1) {
        return true;
      }
    } else {
      advance(lexer);
    }
  }
  return false;
}

// After the word `case`: does `class`/`object` follow (a definition, not a
// clause)? Reads the next word whole — chained scan_word calls would
// misread an identifier like `cobject` as `c` + `object` after the partial
// match. Advances the lexer.
static bool is_case_definition_word(TSLexer *lexer) {
  advance_past_blanks(lexer);
  char word[sizeof "object"];
  int len = read_word(lexer, word, (int)sizeof word);
  return len > 0 &&
         (strcmp(word, "class") == 0 || strcmp(word, "object") == 0);
}

// Peeks whether the line starts a `case` clause — the word `case` not
// followed by `class`/`object` (which begin a definition instead). Advances
// the lexer; the caller must have called mark_end (or return false).
static bool is_case_clause_intro(TSLexer *lexer) {
  return scan_word(lexer, "case") && !is_case_definition_word(lexer);
}

// Lexes a whole block comment as the BLOCK_COMMENT token. The caller has
// consumed the leading "/*". Block comments are lexed externally rather than
// in the grammar so that the `/*` token does not occupy a column in every
// parse-table row (~0.6MiB of parser.c).
static bool finish_block_comment(TSLexer *lexer) {
  lexer->mark_end(lexer);
  lexer->result_symbol = BLOCK_COMMENT;
  LOG("    BLOCK_COMMENT\n");
  return true;
}

static bool lex_block_comment(TSLexer *lexer) {
  consume_block_comment_body(lexer);
  return finish_block_comment(lexer);
}

// Result of looking for a comment at a layout boundary (INDENT/OUTDENT).
typedef enum {
  COMMENT_NONE,   // not a comment; the lexer may have advanced past a lone '/'
  COMMENT_LEXED,  // a block comment was lexed as BLOCK_COMMENT: return true
  COMMENT_ABORT,  // a comment starts here: give up on the layout token
  COMMENT_SAME_LINE_CODE,  // block comment(s) skipped, code follows on the
                           // same line; the lexer stands at that code and
                           // nothing was marked — the caller must make its
                           // layout decision with the code's column as the
                           // effective indentation (a comment does not set
                           // the indentation of its line, the code does)
} CommentAtLayout;

// Comments should not affect indentation: detect one at a layout boundary
// (as the old detect_comment_start guard did). Line comments are internal
// tokens; block comments must be lexed here since the internal lexer no
// longer knows them. The caller must have called mark_end.
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
          // A trailing line comment: no more code on this line. Consume it
          // into the comment token (the extent cannot be re-marked back).
          while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            advance(lexer);
          }
          finish_block_comment(lexer);
          return COMMENT_LEXED;
        }
        // A lone '/' is code (the column is off by one; harmless).
        return COMMENT_SAME_LINE_CODE;
      }
      return COMMENT_SAME_LINE_CODE;
    }
  }
  if (lexer->lookahead == '/' || lexer->lookahead == '*') {
    return COMMENT_ABORT;
  }
  // A lone '/' is not a comment; the lexer stays advanced past it.
  return COMMENT_NONE;
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
  // Track "the last returned token was a fewer-braces colon" (comments in
  // between keep it; a false return does not persist state anyway).
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

  // Separate from OUTDENT because the scanner cannot distinguish a comma that
  // terminates an indented block (e.g. `map: x => f(x),`) from one that is
  // internal to it (e.g. `case EnumCase1, EnumCase2`). By using a distinct
  // token, tree-sitter only makes it valid in grammar contexts where comma
  // termination is expected (colon_argument, _indentable_expression).
  if (valid_symbols[COMMA_OUTDENT] && lexer->lookahead == ',' && prev != -1) {
    // A comma closes the indented block only when it separates elements of
    // an enclosing bracketed list: a `)`/`]` at bracket depth 0 follows —
    // on the same line (`... else expr, more)`) or within the next few
    // lines (`g(x), h(y)` + `)`), including the trailing-comma case. A
    // depth-0 `}` instead closes the enclosing brace block whose last
    // statement the comma is internal to (`foo { _ => import a.b, c.d }`),
    // and a statement continuing with no closer in sight is internal too
    // (`import a.b, c.d`, `case A, B`): no token is returned, the internal
    // ',' wins. With lookahead ',' no other external token can fire, making
    // the early false return safe.
    lexer->mark_end(lexer);
    if (valid_symbols[ERROR_SENTINEL]) {
      // Error recovery: every symbol looks valid — keep the old O(1)
      // pop-and-fire instead of scanning ahead from every comma.
      array_pop(&scanner->indents);
      lexer->result_symbol = COMMA_OUTDENT;
      return true;
    }
    advance(lexer);
    bool fire = false;
    bool content_seen = false;
    int depth = 0;
    // Bounds the closer search: a wrapped argument list closes within a few
    // lines, while an unbounded scan would make each internal comma (enum
    // `case A, B`, import lists) cost the rest of the file.
    int budget = 800;
    for (;;) {
      if (lexer->eof(lexer) || budget <= 0) {
        break;
      }
      int32_t c = lexer->lookahead;
      if (c == '\n' && !content_seen) {
        // Trailing comma: whatever follows on the next lines is the next
        // list element (or the closer), however far away.
        fire = true;
        break;
      }
      if (c == '(' || c == '[' || c == '{') {
        depth++;
        content_seen = true;
        advance(lexer);
        budget--;
      } else if (c == ')' || c == ']') {
        if (depth == 0) {
          fire = true;
          break;
        }
        depth--;
        advance(lexer);
        budget--;
      } else if (c == '}') {
        if (depth == 0) {
          break;
        }
        depth--;
        advance(lexer);
        budget--;
      } else if (c == '/') {
        advance(lexer);
        budget--;
        if (lexer->lookahead == '/') {
          while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            advance(lexer);
            budget--;
          }
        } else if (lexer->lookahead == '*') {
          advance(lexer);
          consume_block_comment_body_ex(lexer, false, &budget);
        } else {
          content_seen = true;
        }
      } else if (c == '"') {
        content_seen = true;
        advance(lexer);
        budget--;
        skip_string_tail(lexer, &budget);
      } else if (c == '`') {
        // A back-ticked identifier may contain brackets; they are not code.
        content_seen = true;
        advance(lexer);
        budget--;
        skip_backticked_tail(lexer, &budget);
      } else if (c == '\'') {
        content_seen = true;
        advance(lexer);
        budget--;
        int32_t quoted = skip_char_or_quote_tail(lexer, &budget);
        if (quoted == '(' || quoted == '[' || quoted == '{') {
          depth++;
        } else if (quoted == ')' || quoted == ']') {
          if (depth == 0) {
            fire = true;
            break;
          }
          depth--;
        } else if (quoted == '}') {
          if (depth == 0) {
            break;
          }
          depth--;
        }
      } else {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
          content_seen = true;
        }
        advance(lexer);
        budget--;
      }
    }
    if (!fire) {
      return false;
    }
    array_pop(&scanner->indents);
    lexer->result_symbol = COMMA_OUTDENT;
    return true;
  }

  // Mid-cascade close of a same-width case region: a deeper region was just
  // popped and the pending line sits exactly at this region's width. The
  // region stays open only when the line starts another `case` clause; the
  // false return is safe because the internal `case` keyword is the only
  // token that can follow (mirrors the same-width INDENT at the bottom).
  // The column check pins this to the cascade position: the saved
  // last_indentation_size can go stale mid-line when the states in between
  // have no external tokens (e.g. right after a case clause's `=>`).
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

  // The plain INDENT geometry: deeper than the current region (an empty
  // stack rides on prev_width == -1: any width is deeper), or same-width
  // right after a fewer-braces colon (see after_colon_eol in Scanner).
  bool indent_geometry =
      indentation_size > prev_width ||
      (scanner->after_colon_eol && indentation_size == prev_width);
  if (
      valid_symbols[INDENT] &&
      newline_count > 0 &&
      // An indented block cannot start with a closing delimiter, e.g. the
      // `}` after an empty-bodied lambda: `foo { x =>\n  }`
      lexer->lookahead != '}' &&
      lexer->lookahead != ')' &&
      lexer->lookahead != ']' &&
      (
        indent_geometry ||
        // A comment sitting exactly at the region width can hide a deeper
        // line behind it (`x match` + `  /* c */ case a => b`): probe it,
        // the code's own column decides below.
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
        // The code does not open a block after all; lex the comment.
        return finish_block_comment(lexer);
      }
      case COMMENT_NONE:
        if (!indent_geometry) {
          return false;  // entered only to probe a comment; this is code
        }
        break;
    }
    // An indented block cannot start with a keyword that continues the
    // enclosing expression (the `finally` in `try if (c) a.removeIf(b)` +
    // `finally d.set(false)`), nor with `yield`/`do`: they belong to an
    // enclosing `for (...)` whose body production is also indentable — an
    // INDENT would commit to a bare indented-block body and orphan the
    // keyword. Read the word whole: chained scan_word calls would leave the
    // lexer mid-identifier on a partial match and the later guards would
    // misread its tail (`cdo`, `eyield`, `e+ x`).
    int16_t entry = indentation_size;
    switch (lexer->lookahead) {
      // Only these letters can start one of the stopper keywords (or
      // `case`); any other word start pushes the INDENT with no word scan.
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
        // At top level the indent stack is empty and this branch accepts
        // any width, so the same-width `case` branch at the bottom of
        // scan() never sees `x match` + width-0 `case` lines: flag the
        // region here instead, so a same-width non-case line closes it
        // (CASE_INDENT_FLAG). Only when the line carries a depth-0 `=>`:
        // an enum body's `case A, B` / `case C(...) extends D` opens
        // through this branch too, and flagging it would break the enum
        // (the flagged close fires at the first non-case member, and the
        // clause-continuation check blocks the semicolon between cases).
        if (scanner->indents.size == 0 && len == 4 &&
            strcmp(word, "case") == 0 && !is_case_definition_word(lexer) &&
            line_has_case_arrow(lexer)) {
          entry |= CASE_INDENT_FLAG;
        }
        break;
      }
      case '|':
      case '&':
        // Nor with a leading `|`/`&` infix operator: it continues the
        // previous expression (`if (false)` + `  || true` + `then ...`),
        // and an INDENT here would commit the parse to an indented block
        // before the infix reading can be tried (mirrors the ASI/OUTDENT
        // guards). Only `|`/`&` runs: they cannot start a statement, while
        // a general symbolic head is a legal first statement of the new
        // block (`??? ++ 1`, `*> io`, a back-ticked DSL word) — and after
        // a fewer-braces colon the line can only be the body (a `:` line
        // cannot be continued by an infix).
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
        // effective indentation ( ` /** doc */ override def f...` inside a
        // braced body must not close the region at the comment's column).
        int16_t effective = (int16_t)lexer->get_column(lexer);
        if (effective < prev_width) {
          indentation_size = effective;
          break;
        }
        // Not an outdent after all — but the automatic semicolon has
        // suppression rules of its own (leading `.`/infix operator,
        // else/catch/finally/match), so it must not be emitted here
        // sight unseen. Lex the comment and let the next scan decide,
        // carrying the pending newline through the last_newline_count /
        // last_column recovery below (` /* c */ .map(g)` and
        // ` /* c */ else 2` continue their statement; ` /* c */ val b = 2`
        // still gets its semicolon).
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
    // Don't close the indented block when the next line starts with a leading
    // infix operator: that operator continues the previous expression inside
    // the current region even when the operator line is dedented.
    // Exception: when the line ends in a depth-0 `then`/`do`, the operator
    // continues an enclosing conditional and the regions between do close
    // (see line_ends_conditional).
    if (lexer->lookahead != 0 && is_leading_infix_continuation(lexer) &&
        !line_ends_conditional(lexer)) {
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

  // Recover newline_count from the outdent reset. Only when this scan did
  // not cross a newline itself: a scan that consumed its own newline is at a
  // fresh line's decision point, and the saved count would be stale — the
  // column pin alone cannot tell a later line at the same column apart (a
  // false-returning scan does not persist the reset below, so the saved
  // values can survive across a whole line).
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

    // Single-line and multi-line comments
    if (lexer->lookahead == '/') {
      advance(lexer);
      if (lexer->lookahead == '/') {
        return false;
      }
      if (lexer->lookahead == '*' && valid_symbols[BLOCK_COMMENT]) {
        advance(lexer);
        // Peek through the comment. When the rest of the line is blank the
        // decision is deferred: consume the comment as a token and let the
        // newline after it re-trigger this branch. When code follows on the
        // same line, the pending newline must not be lost once the comment
        // token is consumed (`val a = 1` + `/* c */ val b = 2` still needs
        // its semicolon) — but the suppression rules must apply to that
        // code too (`/* c */ else 2` must not be split), so the comment is
        // lexed first and the newline carried to the next scan through the
        // last_newline_count / last_column recovery above. (Either way the
        // token extent includes any trailing blanks: the end-of-comment
        // position cannot be re-marked once the lexer has peeked past it.)
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

    // Don't insert the automatic semicolon before a leading infix operator:
    // symbolic (`|| b`) or back-ticked (`a` \`in\` `b`), followed by
    // horizontal whitespace and an operand on the same line. Checked before
    // the keyword scans below so neither ever reads a position the other
    // has advanced past (`m|| x` must not have its `||` misread as leading
    // infix after a failed `match` scan) — the two first-char classes are
    // disjoint. A blank line still separates the statements.
    if (is_op_char(lexer->lookahead) || lexer->lookahead == '`') {
      if (newline_count == 1 && is_leading_infix_continuation(lexer)) {
        return false;
      }
      return true;
    }

    // A following keyword that continues the enclosing expression suppresses
    // the automatic semicolon, even when several such keywords are valid at
    // once (e.g. a dangling `if` waiting for `else` inside a `try` whose
    // `finally` comes next). Dispatch on the first character so scan_word
    // never consumes a prefix shared by two keywords ("else"/"extends");
    // a partial match may leave the lexer mid-identifier, but nothing after
    // the switch reads the position again.
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
      case 'c':
        if (valid_symbols[CATCH] && scan_word(lexer, "catch")) {
          return false;
        }
        break;
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
        // `match` never starts a statement, so when it is valid it always
        // continues the previous expression (Scala 3 allows the chain
        // `xs.filter(p)` + newline + `match ...`).
        if (valid_symbols[MATCH] && scan_word(lexer, "match")) {
          return false;
        }
        break;
      default:
        break;
    }

    return true;
  }

  // A mid-line else/catch/finally that is not directly shiftable must first
  // close the open indented block, e.g. the `else` in:
  //   if (c)
  //     x match {
  //       case 1 => 1
  //     } else 2
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
    // The lexer has advanced past an identifier starting with e/c/f; nothing
    // else in this invocation could match it, so give up on external tokens.
    return false;
  }

  while (iswspace(lexer->lookahead)) {
    if (lexer->lookahead == '\n') {
      newline_count++;
    }
    skip(lexer);
  }

  // XML mode (SLS §10) is entered only where an expression or pattern can
  // start (that is where the grammar makes XML_TAG_START valid) AND the `<`
  // is immediately followed by a name-start character. `< b` and `<-`, `<:`,
  // `<=` etc. fall through to the regular operator tokens.
  if (valid_symbols[XML_TAG_START] && lexer->lookahead == '<') {
    advance(lexer);
    if (iswalpha(lexer->lookahead) || lexer->lookahead == '_') {
      lexer->mark_end(lexer);
      lexer->result_symbol = XML_TAG_START;
      return true;
    }
    return false;
  }

  // A symbolic infix operator that ends its line continues the expression on
  // the next line (SLS 1.2: no semicolon inference after an infix operator).
  // Lexed here because the decision needs the rest of the line: the internal
  // lexer would hand the same characters to the regular operator token, whose
  // statement would then be split by the automatic semicolon / outdent at the
  // newline (misreading the operator as postfix). Skipped in error recovery,
  // where every symbol looks valid.
  if ((valid_symbols[OPERATOR_EOL] || valid_symbols[COLON_EOL]) &&
      !valid_symbols[ERROR_SENTINEL] && is_op_char(lexer->lookahead)) {
    // Collect the operator; anything longer than 3 chars cannot be one of the
    // reserved sequences checked below.
    int32_t op[3] = {0, 0, 0};
    int op_len = 0;
    while (is_op_char(lexer->lookahead)) {
      if (op_len < 3) {
        op[op_len] = lexer->lookahead;
      }
      op_len++;
      advance(lexer);
    }
    lexer->mark_end(lexer);
    // Reserved sequences are not infix operators: `=`, `=>`/`?=>`/`=>>`,
    // `<-`, `<:`, `>:`, `<%`, `:` (colon argument), `#`, `@`.
    if (op_len == 1 && op[0] == ':') {
      // A lone `:` that ends its line is the fewer-braces colon (scalac's
      // COLONeol). Deciding it here kills the across-the-newline ascription
      // reading at the token level; without this, every nesting level of
      // `a:` > `b:` > `c:` keeps a doomed GLR fork alive to the end of the
      // region, and past ~4 levels the version cap discards the correct one.
      // Trailing comments count as end of line; code on the same line
      // makes it a plain (ascription/lambda) colon.
      if (valid_symbols[COLON_EOL] &&
          rest_of_line_is_blank_or_comments(lexer)) {
        lexer->result_symbol = COLON_EOL;
        return true;
      }
      return false;
    }
    if (op_len == 1 && (op[0] == '=' || op[0] == '#' || op[0] == '@')) {
      return false;
    }
    if (op_len == 2 &&
        ((op[0] == '=' && op[1] == '>') || (op[0] == '<' && op[1] == '-') ||
         (op[0] == '<' && op[1] == ':') || (op[0] == '>' && op[1] == ':') ||
         (op[0] == '<' && op[1] == '%'))) {
      return false;
    }
    if (op_len == 3 && ((op[0] == '?' && op[1] == '=' && op[2] == '>') ||
                        (op[0] == '=' && op[1] == '>' && op[2] == '>'))) {
      return false;
    }
    // The branch is also entered for COLON_EOL-only states; everything from
    // here on lexes OPERATOR_EOL, which must be valid.
    if (!valid_symbols[OPERATOR_EOL]) {
      return false;
    }
    // The operator must end its line; trailing comments count as the line
    // end (they are transparent to scalac's newline handling: `p || // c`
    // still continues on the next line).
    if (!rest_of_line_is_blank_or_comments(lexer)) {
      return false;
    }
    // The right operand must be able to start an expression; if the next
    // non-blank character closes a delimiter or list instead (e.g. a vararg
    // splice `xs*` before a `)` on its own line), this is not a continuation.
    // Comments between the operator and the operand are skipped.
    for (;;) {
      while (iswspace(lexer->lookahead)) {
        advance(lexer);
      }
      if (lexer->lookahead == '/') {
        advance(lexer);
        if (lexer->lookahead == '/') {
          while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            advance(lexer);
          }
          continue;
        }
        if (lexer->lookahead == '*') {
          advance(lexer);
          consume_block_comment_body(lexer);
          continue;
        }
        // A lone '/' cannot start an operand, so the operator was postfix.
        return false;
      }
      break;
    }
    if (lexer->eof(lexer) || lexer->lookahead == ')' ||
        lexer->lookahead == ']' || lexer->lookahead == '}' ||
        lexer->lookahead == ',' || lexer->lookahead == ';' ||
        lexer->lookahead == '.' || lexer->lookahead == '=' ||
        // No expression starts with `@` either: the next line is an
        // annotated definition (`x --?` + `@deprecated val y = 1`).
        lexer->lookahead == '@') {
      return false;
    }
    // Nor can the operand be a keyword that only continues or starts a
    // statement. This matters for symbolic identifiers used as expressions:
    // in `if (c) ???` + `else d` the `???` must stay a plain expression.
    if (lexer->lookahead >= 'a' && lexer->lookahead <= 'z') {
      static const char *const non_operand_words[] = {
          "case", "catch",  "do",   "else",  "finally", "for",   "if",
          "match", "return", "then", "throw", "try",     "while", "yield",
          // Definition/modifier keywords: a line starting with one of these
          // begins a new statement, so the operator was postfix (`x --?` +
          // `val y = ...`, Scala 2 postfixOps).
          "abstract", "class", "def",    "enum",      "export", "final",
          "given",    "import", "implicit", "lazy",   "object", "override",
          "package",  "private", "protected", "sealed", "trait", "type",
          "val",      "var",
      };
      // Sized for the longest word above; read_word reports a longer (or
      // non-ASCII) identifier as -1, which correctly skips the check.
      char word[sizeof "protected"];
      int len = read_word(lexer, word, (int)sizeof word);
      if (len > 0 &&
          word_in(word, non_operand_words,
                  sizeof(non_operand_words) / sizeof(non_operand_words[0]))) {
        return false;
      }
      lexer->result_symbol = OPERATOR_EOL;
      return true;
    }
    // A leading infix operator on the next line continues the postfix
    // reading of the previous line instead: `a ???` + `|| b`.
    if (is_leading_infix_continuation(lexer)) {
      return false;
    }
    lexer->result_symbol = OPERATOR_EOL;
    return true;
  }

  // Mid-line block comments (no layout decision pending). Suppressed where
  // the slashes are ordinary text instead: after `//` (SUPPRESS_BLOCK_COMMENT, a
  // dead grammar alternative that marks that state) and where a
  // string-content token is expected (`s"a /* b"`). During error recovery
  // all symbols look valid, and there lexing the comment is the safe choice.
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
    // A lone '/' or a line comment: nothing else external can match here.
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

  // Scala 3 allows the `case` clauses of a `match`/`catch` to align with the
  // enclosing region instead of being indented deeper (`x match` + newline +
  // `case ...` at the same width). Open the case region here — flagged, so
  // the OUTDENT logic above also closes it on a same-width non-case line.
  // Only the states right after `match`/`catch`(/`=>`. of a case) have
  // INDENT valid with a same-width `case` line, so the grammar needs no new
  // production. Checked last: with lookahead 'c' no other external token can
  // fire, making the false return safe.
  if (valid_symbols[INDENT] && !valid_symbols[ERROR_SENTINEL] &&
      newline_count > 0 && lexer->lookahead == 'c' &&
      // The empty stack needs no arm here: the generic INDENT branch accepts
      // any width there and sets CASE_INDENT_FLAG itself.
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
