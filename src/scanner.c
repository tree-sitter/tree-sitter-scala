#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <wctype.h>

#define DEBUG

#ifdef DEBUG
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...)
#endif

enum TokenType {
  AUTOMATIC_SEMICOLON,
  INDENT,
  INTERPOLATED_STRING_MIDDLE,
  INTERPOLATED_STRING_END,
  INTERPOLATED_MULTILINE_STRING_MIDDLE,
  INTERPOLATED_MULTILINE_STRING_END,
  OUTDENT,
  SIMPLE_MULTILINE_STRING,
  SIMPLE_STRING,
  OPEN_PAREN,
  CLOSE_PAREN,
  OPEN_BRACK,
  CLOSE_BRACK,
  OPEN_BRACE,
  CLOSE_BRACE,
  ELSE,
  CATCH,
  FINALLY,
  EXTENDS,
  DERIVES,
  WITH,
};

typedef struct {
  Array(int16_t) indents;
} IndentationFrame;

typedef struct {
  Array(IndentationFrame *) frames;  // Array of pointers to indentation frames
  bool just_did_outdent;
} Scanner;

static inline void debug_indents(Scanner *scanner) {
  LOG("    Indentaion Frames(%d):\n", scanner->frames.size);
  for (unsigned i = 0; i < scanner->frames.size; i++) {
    IndentationFrame *frame = scanner->frames.contents[i];
    LOG("      Frame[%d] (%d): ", i, frame->indents.size);
    for (unsigned j = 0; j < frame->indents.size; j++) {
      LOG("%d ", frame->indents.contents[j]);
    }
    LOG("\n");
  }
}

static void debug_valid_symbols(const bool *valid_symbols) {
  LOG("Valid symbols: ");
  if (valid_symbols[AUTOMATIC_SEMICOLON]) LOG("AUTOMATIC_SEMICOLON ");
  if (valid_symbols[INDENT]) LOG("INDENT ");
  if (valid_symbols[INTERPOLATED_STRING_MIDDLE]) LOG("INTERPOLATED_STRING_MIDDLE ");
  if (valid_symbols[INTERPOLATED_STRING_END]) LOG("INTERPOLATED_STRING_END ");
  if (valid_symbols[INTERPOLATED_MULTILINE_STRING_MIDDLE]) LOG("INTERPOLATED_MULTILINE_STRING_MIDDLE ");
  if (valid_symbols[INTERPOLATED_MULTILINE_STRING_END]) LOG("INTERPOLATED_MULTILINE_STRING_END ");
  if (valid_symbols[OUTDENT]) LOG("OUTDENT ");
  if (valid_symbols[SIMPLE_MULTILINE_STRING]) LOG("SIMPLE_MULTILINE_STRING ");
  if (valid_symbols[SIMPLE_STRING]) LOG("SIMPLE_STRING ");
  if (valid_symbols[OPEN_PAREN]) LOG("OPEN_PAREN ");
  if (valid_symbols[CLOSE_PAREN]) LOG("CLOSE_PAREN ");
  if (valid_symbols[OPEN_BRACK]) LOG("OPEN_BRACK ");
  if (valid_symbols[CLOSE_BRACK]) LOG("CLOSE_BRACK ");
  if (valid_symbols[OPEN_BRACE]) LOG("OPEN_BRACE ");
  if (valid_symbols[CLOSE_BRACE]) LOG("CLOSE_BRACE ");
  if (valid_symbols[ELSE]) LOG("ELSE ");
  if (valid_symbols[CATCH]) LOG("CATCH ");
  if (valid_symbols[FINALLY]) LOG("FINALLY ");
  if (valid_symbols[EXTENDS]) LOG("EXTENDS ");
  if (valid_symbols[DERIVES]) LOG("DERIVES ");
  if (valid_symbols[WITH]) LOG("WITH ");
  LOG("\n");
}

// --- Stack Management ---
static void push_indent_group(Scanner *scanner) {
  IndentationFrame *frame = ts_malloc(sizeof(IndentationFrame));
  array_init(&frame->indents);
  array_push(&scanner->frames, frame);  // Push pointer
}

static void pop_indent_group(Scanner *scanner) {
  IndentationFrame *frame = *array_back(&scanner->frames);
  array_delete(&frame->indents);  // Free array inside frame
  ts_free(frame);  // Free struct
  array_pop(&scanner->frames);  // Remove pointer from list
}

static void push_indent_level(Scanner *scanner, int16_t indent) {
  IndentationFrame *frame = *array_back(&scanner->frames);
  array_push(&frame->indents, indent);
}

static void pop_indent_level(Scanner *scanner) {
  IndentationFrame *frame = *array_back(&scanner->frames);
  array_pop(&frame->indents);
}

static int16_t get_latest_indent(Scanner *scanner) {
  IndentationFrame *frame = *array_back(&scanner->frames);
  return *array_back(&frame->indents);
}

static void set_latest_indent(Scanner *scanner, int16_t indent) {
  IndentationFrame *frame = *array_back(&scanner->frames);
  *array_back(&frame->indents) = indent;
}

void *tree_sitter_scala_external_scanner_create() {
  Scanner *scanner = ts_calloc(1, sizeof(Scanner));
  array_init(&scanner->frames);

  // Allocate and initialize the first indentation group
  IndentationFrame *frame = ts_malloc(sizeof(IndentationFrame));
  array_init(&frame->indents);
  array_push(&frame->indents, 0);
  array_push(&scanner->frames, frame);
  scanner->just_did_outdent = false;
  return scanner;
}

void tree_sitter_scala_external_scanner_destroy(void *payload) {
  Scanner *scanner = (Scanner *)payload;

  // Free each IndentStack in the array
  for (unsigned i = 0; i < scanner->frames.size; i++) {
    IndentationFrame *frame = scanner->frames.contents[i];
    array_delete(&frame->indents);  // Free indent array
    ts_free(frame);  // Free the stack itself
  }
  
  array_delete(&scanner->frames);  // Free the array of pointers
  ts_free(scanner);
}

unsigned tree_sitter_scala_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = (Scanner*)payload;
  size_t size = 0;

  if (scanner->frames.size * sizeof(int16_t) + 2 * sizeof(bool) > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    return 0;
  }

  memcpy(buffer + size, &scanner->just_did_outdent, sizeof(bool));
  size += sizeof(bool);

  memcpy(buffer + size, &scanner->frames.size, sizeof(uint32_t));
  size += sizeof(uint32_t);

  for (unsigned i = 0; i < scanner->frames.size; i++) {
    IndentationFrame *frame = scanner->frames.contents[i];

    memcpy(buffer + size, &frame->indents.size, sizeof(uint32_t));
    size += sizeof(uint32_t);

    for (unsigned j = 0; j < frame->indents.size; j++) {
      memcpy(buffer + size, &frame->indents.contents[j], sizeof(int16_t));
      size += sizeof(int16_t);
    }
  }

  return size;
}

void tree_sitter_scala_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = (Scanner *)payload;
  array_clear(&scanner->frames);  // Clear any existing state
  scanner->just_did_outdent = false;

  if (length == 0) {
      // Reinitialize with base indentation group
      push_indent_group(scanner);
      push_indent_level(scanner, 0);
      return;
  }

  size_t size = 0;

  scanner->just_did_outdent = *(bool *)&buffer[size];
  size += sizeof(bool);

  // Deserialize number of indentation stacks
  uint32_t num_stacks;
  memcpy(&num_stacks, buffer + size, sizeof(uint32_t));
  size += sizeof(uint32_t);

  for (uint32_t i = 0; i < num_stacks; i++) {
    // Allocate new indent stack
    IndentationFrame *frame = ts_malloc(sizeof(IndentationFrame));
    array_init(&frame->indents);

    // Deserialize number of indents in this stack
    uint32_t num_indents;
    memcpy(&num_indents, buffer + size, sizeof(uint32_t));
    size += sizeof(uint32_t);

    for (uint32_t j = 0; j < num_indents; j++) {
        int16_t indent_value;
        memcpy(&indent_value, buffer + size, sizeof(int16_t));
        size += sizeof(int16_t);
        array_push(&frame->indents, indent_value);
    }

    // Push the deserialized stack into the scanner
    array_push(&scanner->frames, frame);
  }

  assert(size == length);
}

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool scan_string_content(TSLexer *lexer, bool is_multiline, bool has_interpolation) {
  unsigned closing_quote_count = 0;
  for (;;) {
    if (lexer->lookahead == '"') {
      advance(lexer);
      closing_quote_count++;
      if (!is_multiline) {
        lexer->result_symbol = has_interpolation ? INTERPOLATED_STRING_END : SIMPLE_STRING;
        return true;
      }
      if (closing_quote_count >= 3 && lexer->lookahead != '"') {
        lexer->result_symbol = has_interpolation ? INTERPOLATED_MULTILINE_STRING_END : SIMPLE_MULTILINE_STRING;
        return true;
      }
    } else if (lexer->lookahead == '$') {
      if (is_multiline && has_interpolation) {
        lexer->result_symbol =  INTERPOLATED_MULTILINE_STRING_MIDDLE;
        return true;
      }
      if (has_interpolation) {
        lexer->result_symbol = INTERPOLATED_STRING_MIDDLE;
        return true;
      }
      advance(lexer);
    } else {
      closing_quote_count = 0;
      if (lexer->lookahead == '\\') {
        advance(lexer);
        if (!lexer->eof(lexer)) {
          advance(lexer);
        }
      } else if (lexer->lookahead == '\n') {
        if (is_multiline) {
          advance(lexer);
        } else {
          return false;
        }
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
  return !iswalnum(lexer->lookahead);
}

static bool detect_comment_start(TSLexer *lexer) {
  // Comments should not affect indentation
  if (lexer->lookahead == '/') {
    advance(lexer);
    if (lexer->lookahead == '/' || lexer -> lookahead == '*') {
      return true;
    }
  }
  return false;
}

static bool detect_continuation(TSLexer *lexer, const bool *valid_symbols) {
  // Check for obvious multi-line continuation symbols
  if (lexer->lookahead == '.' || lexer->lookahead == ')' || lexer->lookahead == ']' || lexer->lookahead == ',') {
    return true;
  }

  // Check for keywords that indicate continuation
  return 
    (valid_symbols[ELSE] && scan_word(lexer, "else")) ||
    (valid_symbols[CATCH] && scan_word(lexer, "catch")) ||
    (valid_symbols[FINALLY] && scan_word(lexer, "finally")) ||
    (valid_symbols[EXTENDS] && scan_word(lexer, "extends")) ||
    (valid_symbols[WITH] && scan_word(lexer, "with")) ||
    (valid_symbols[DERIVES] && scan_word(lexer, "derives"));
}

static int skip_whitespace(TSLexer *lexer) {
  int newline_count = 0;
  // Skip all whitespace characters, counting newlines
  while (iswspace(lexer->lookahead)) {
    if (lexer->lookahead == '\n') {
      newline_count++;
    }
    skip(lexer);
  }
  return newline_count;
}

static int skip_comment_and_whitespace(TSLexer *lexer) {
  int newline_count = 0;

  while (true) {
    // Skip all whitespace characters, counting newlines
    newline_count += skip_whitespace(lexer);

    // Handle single-line comments: `// ...`
    if (lexer->lookahead == '/') {
      advance(lexer);
      if (lexer->lookahead == '/') {
        // Consume characters until the end of the line or EOF
        while (lexer->lookahead != '\n' && lexer->lookahead != 0) {
          advance(lexer);
        }
        continue; // Go back to checking for whitespace/comments
      }

      // Handle multi-line (possibly nested) comments: `/* ... */`
      if (lexer->lookahead == '*') {
        advance(lexer);
        int depth = 1; // Track nested depth

        while (depth > 0 && !lexer->eof(lexer)) {
          if (lexer->lookahead == '/') {
            advance(lexer);
            if (lexer->lookahead == '*') { // Found `/*`
              advance(lexer);
              depth++; // Increase nesting depth
            }
          } else if (lexer->lookahead == '*') {
            advance(lexer);
            if (lexer->lookahead == '/') { // Found `*/`
              advance(lexer);
              depth--; // Decrease nesting depth
            }
          } else {
            if (lexer->lookahead == '\n') {
              newline_count++;
            }
            advance(lexer);
          }
        }
        continue; // Go back to checking for whitespace/comments
      }
    }
    // If no more whitespace or comments, break out
    break;
  }

  return newline_count;
}

// --- Helper function to handle opening a new indentation group ---
static bool open_group(Scanner *scanner, TSLexer *lexer, int16_t symbol, char c) {
  advance(lexer);
  int16_t latest_indent = lexer->get_column(lexer);
  int16_t newline_count = skip_comment_and_whitespace(lexer);
  int16_t initial_indent = lexer->eof(lexer) ? 0 : newline_count > 0 ? lexer->get_column(lexer) : lexer->get_column(lexer) - latest_indent;
  push_indent_group(scanner);
  push_indent_level(scanner, initial_indent);
  debug_indents(scanner);
  lexer->result_symbol = symbol;
  lexer->mark_end(lexer);
  return true;
}

// --- Helper function to handle closing an indentation group ---
static bool close_group(Scanner *scanner, TSLexer *lexer, int16_t symbol, char c) {
  advance(lexer);
  pop_indent_group(scanner);
  lexer->result_symbol = symbol;
  lexer->mark_end(lexer);
  return true;
}

// --- Checks whether the current indentation level can be popped ---
static bool can_pop_indent(Scanner *scanner) {
  if (scanner->frames.size == 0) {
    return false;  // No frames exist
  }
  IndentationFrame *frame = *array_back(&scanner->frames);
  return frame->indents.size > 1;  // Only pop if there's more than one indent
}

// --- Checks whether the current indentation frame can be popped ---
static bool can_pop_frame(Scanner *scanner) {
  return scanner->frames.size > 1;  // Don't pop the base frame
}

bool tree_sitter_scala_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *scanner = (Scanner *)payload;
  int16_t latest_indent = get_latest_indent(scanner);

  LOG("\n");
  LOG("initial lexer->lookahead: '%c'\n", lexer->lookahead);
  int16_t newline_count = skip_comment_and_whitespace(lexer);
  int16_t current_indent = 
    lexer->eof(lexer) 
    ? 0
    : newline_count > 0 || scanner->just_did_outdent
      ? lexer->get_column(lexer)
      : lexer->get_column(lexer) - latest_indent;

  LOG("lexer->lookahead: '%c'\n", lexer->lookahead);
  LOG("latest_indent: '%d'\n", latest_indent);
  LOG("current_indent: '%d'\n", current_indent);
  LOG("newline_count: '%d'\n", newline_count);
  LOG("scanner->just_did_outdent: '%d'\n", scanner->just_did_outdent);
  
  debug_indents(scanner);
  debug_valid_symbols(valid_symbols);
  LOG("\n");

  if (valid_symbols[INDENT] && newline_count > 0 && current_indent > latest_indent) {
    lexer->mark_end(lexer);
    
    LOG("    INDENT\n");
    push_indent_level(scanner, current_indent);
    lexer->result_symbol = INDENT;
    scanner->just_did_outdent = false;
    return true;
  }

  // https://github.com/tree-sitter/tree-sitter-scala/commit/38137ff97ff3c7874e26ba8cd8a36ba58b5d957a
  bool force_outdent = lexer->eof(lexer) || lexer->lookahead == ')'  || lexer->lookahead == '}';
  if (valid_symbols[OUTDENT] && (force_outdent || ((newline_count > 0 || scanner->just_did_outdent) && current_indent < latest_indent)) && can_pop_indent(scanner)) {
    lexer->mark_end(lexer);

    LOG("    OUTDENT\n");
    pop_indent_level(scanner);
    lexer->result_symbol = OUTDENT;
    // Set saved_seen_newline if we have seen new lines as part of this OUTDENT
    scanner->just_did_outdent = true;
    return true;
  }

  if (valid_symbols[AUTOMATIC_SEMICOLON] && (newline_count > 0 || scanner->just_did_outdent)) {
    lexer->mark_end(lexer);

    // AUTOMATIC_SEMICOLON should not be issued in the middle of expressions
    // Thus, we exit this branch when encountering comments, else/catch clauses, etc.
    if (detect_continuation(lexer, valid_symbols)) return false;

    LOG("    AUTOMATIC SEMICOLON\n");
    // if (!can_pop_indent(scanner)) {
    //   // We allow synthetic auto-semicolon before e.g. ':'  in some rules, so
    //   // 
    //   set_latest_indent(scanner, current_indent);
    // }
    lexer->result_symbol = AUTOMATIC_SEMICOLON;
    scanner->just_did_outdent = false;
    return true;
  }

  scanner->just_did_outdent = false;

  // Handle opening tokens: '(', '[', '{'
  if (valid_symbols[OPEN_PAREN] && lexer->lookahead == '(') {
    return open_group(scanner, lexer, OPEN_PAREN, '(');
  }
  if (valid_symbols[OPEN_BRACK] && lexer->lookahead == '[') {
    return open_group(scanner, lexer, OPEN_BRACK, '[');
  }
  if (valid_symbols[OPEN_BRACE] && lexer->lookahead == '{') {
    return open_group(scanner, lexer, OPEN_BRACE, '{');
  }

  // Handle closing tokens: ')', ']', '}'
  if (valid_symbols[CLOSE_PAREN] && lexer->lookahead == ')' && can_pop_frame(scanner)) {
    return close_group(scanner, lexer, CLOSE_PAREN, ')');
  }
  if (valid_symbols[CLOSE_BRACK] && lexer->lookahead == ']' && can_pop_frame(scanner)) {
    return close_group(scanner, lexer, CLOSE_BRACK, ']');
  }
  if (valid_symbols[CLOSE_BRACE] && lexer->lookahead == '}' && can_pop_frame(scanner)) {
    return close_group(scanner, lexer, CLOSE_BRACE, '}');
  }

  while (iswspace(lexer->lookahead)) {
    skip(lexer);
  }

  if (valid_symbols[SIMPLE_STRING] && lexer->lookahead == '"') {
    advance(lexer);

    bool is_multiline = false;
    if (lexer->lookahead == '"') {
      advance(lexer);
      if (lexer->lookahead == '"') {
        advance(lexer);
        is_multiline = true;
      } else {
        lexer->result_symbol = SIMPLE_STRING;
        return true;
      }
    }

    return scan_string_content(lexer, is_multiline, false);
  }

  if (valid_symbols[INTERPOLATED_STRING_MIDDLE]) {
    return scan_string_content(lexer, false, true);
  }

  if (valid_symbols[INTERPOLATED_MULTILINE_STRING_MIDDLE]) {
    return scan_string_content(lexer, true, true);
  }

  return false;
}

//
