// ed2.c
//
// An ed-like text editor.
//
// Usage:
//   ed2 [filename]
//
// Opens filename if present, or a new buffer if no filename is given.
// Edit/save the buffer with essentially the same commands as the original
// ed text editor. See plan.md for a summary of which ed commands have been
// implemented.
//

// Local library includes.
#include "cstructs/cstructs.h"

// Library includes.
#include <readline/readline.h>

// Standard includes.
#include <assert.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


///////////////////////////////////////////////////////////////////////////
// Debug.
///////////////////////////////////////////////////////////////////////////

#define show_debug_output 0

#if show_debug_output
#define dbg_printf(...) printf(__VA_ARGS__)
#else
#define dbg_printf(...)
#endif


///////////////////////////////////////////////////////////////////////////
// Constants.
///////////////////////////////////////////////////////////////////////////

// TODO Define error strings here.

// The user can't undo when backup_current_line == no_valid_backup.
#define no_valid_backup -1

#define string_capacity 1024

///////////////////////////////////////////////////////////////////////////
// Globals.
///////////////////////////////////////////////////////////////////////////

char   last_error[string_capacity];
int    do_print_errors = 0;

// The current filename.
char   filename[string_capacity];

// The lines are held in an array. The array frees removed lines for us.
// The byte stream can be formed by joining this array with "\n".
Array  lines = NULL;

int    current_line;
int    is_modified;  // This is set in save_state; it's called for edits.

// Data used for undos.
Array  backup_lines = NULL;
int    backup_current_line;


///////////////////////////////////////////////////////////////////////////
// Macros.
///////////////////////////////////////////////////////////////////////////

// This can be used for both setting and getting.
// Don't forget to free the old value if setting.
#define line_at(index) array__item_val(lines, index, char *)


///////////////////////////////////////////////////////////////////////////
// Internal functions.
///////////////////////////////////////////////////////////////////////////

void error(const char *err_str) {
  strcpy(last_error, err_str);
  printf("?\n");
  if (do_print_errors) printf("%s\n", last_error);
}

void line_releaser(void *line_vp, void *context) {
  char *line = *(char **)line_vp;
  assert(line);
  free(line);
}

Array new_lines_array() {
  Array new_array = array__new(64, sizeof(char *));
  new_array->releaser = line_releaser;
  return new_array;
}

// Initialize our data structures.
void init() {
  lines               = new_lines_array();
  is_modified         = 0;

  backup_lines        = new_lines_array();
  backup_current_line = no_valid_backup;

  // TODO This default only matters for new files. Study ed's behavior to
  //      determine what the current line effectively is for new files.
  //      This isn't obvious to me because I have the sense that 'a' will
  //      append after line 0, while 'i' will insert before line 1.
  // This variable is the same as what the user considers it; this is
  // non-obvious in that our lines array is 0-indexed, so we have to be careful
  // when indexing into `lines`.
  current_line = 0;

  strcpy(last_error, "");
}

// Backup functionality.

void deep_copy_array(Array src, Array dst) {
  array__clear(dst);
  array__for(char **, line, src, i) {
    array__new_val(dst, char *) = strdup(*line);
  }
}

void save_state(Array saved_lines, int *saved_current_line) {
  is_modified = 1;
  *saved_current_line = current_line;
  deep_copy_array(lines, saved_lines);
}

void load_state_from_backup() {
  deep_copy_array(backup_lines, lines);
  current_line = backup_current_line;
}

int last_line() {
  // If the last entry in `lines` is the empty string, then the file ends in a
  // newline; pay attention to this to avoid an off-by-one error.
  if (*line_at(lines->count - 1) == '\0') {
    return lines->count - 1;
  }
  return lines->count;
}

// Separate a raw buffer into a sequence of indexed lines.
// Destroys the buffer in the process.
void find_lines(char *buffer) {
  assert(lines);  // Check that lines has been initialized.
  assert(buffer);

  array__clear(lines);
  backup_current_line = no_valid_backup;
  is_modified = 0;

  char *buffer_ptr = buffer;
  char *line;
  while ((line = strsep(&buffer_ptr, "\n"))) {
    array__new_val(lines, char *) = strdup(line);
  }

  current_line = last_line();
}

// Load a file. Use the global `filename` unless `new_filename` is non-NULL, in
// which case, the new name replaces the global filename and is loaded.
void load_file(char *new_filename) {

  if (is_modified) {
    error("warning: file modified");
    return;
  }

  if (new_filename) strlcpy(filename, new_filename, string_capacity);
  if (strlen(filename) == 0) {
    error("no current filename");
    return;
  }

  FILE *f = fopen(filename, "rb");

  if (f == NULL) {
    printf("%s: No such file or directory\n", filename);
    return;
  }

  struct stat file_stats;
  int is_err = fstat(fileno(f), &file_stats);
  // TODO Handle is_err != 0.

  size_t buffer_size = file_stats.st_size;
  char * buffer = malloc(buffer_size + 1);  // + 1 for the final null character.

  is_err = fread(buffer,       // buffer ptr
                 1,            // item size
                 buffer_size,  // num items
                 f);           // stream
  buffer[buffer_size] = '\0';  // Manually add a final null character.

  // TODO Handle is_err != 0.

  fclose(f);

  find_lines(buffer);
  free(buffer);
  printf("%zd\n", buffer_size);  // Report how many bytes we read.
}

// Save the buffer. If filename is NULL, save it to the current filename.
// This returns the number of bytes written on success and -1 on error.
int save_file(char *new_filename) {
  if (new_filename) strlcpy(filename, new_filename, string_capacity);
  if (strlen(filename) == 0) {
    error("no current filename");
    return -1;
  }

  FILE *f = fopen(filename, "wb");
  // TODO Handle the case that f is NULL.

  int nbytes_written = 0;
  int was_error = 0;
  array__for(char **, line, lines, i) {
    int nbytes_this_line = 0;
    if (i) nbytes_this_line += fwrite("\n", 1, 1, f);  // 1, 1 = size, nitems
    size_t len = strlen(*line);
    nbytes_this_line += fwrite(*line,  // buffer
                               1,      // size
                               len,    // nitems
                               f);     // stream
    if (nbytes_this_line < len + (i ? 1 : 0)) was_error = 1;
    nbytes_written += nbytes_this_line;
  }

  if (was_error) error("error while writing");

  is_modified = 0;
  fclose(f);
  return nbytes_written;
}

// Parsing functions.

// This returns the number of characters scanned.
int scan_number(char *command, int *num) {
  int num_chars_parsed;
  int num_items_parsed = sscanf(command, "%d%n", num, &num_chars_parsed);
  return (num_items_parsed ? num_chars_parsed : 0);
}
// TODO Consider using scan_number from parse_range.

// This parses out any initial line range from a command, returning the number
// of characters parsed. If a range is successfully parsed, then current_line is
// updated to the end of this range.
int parse_range(char *command, int *start, int *end) {

  // For now, we'll parse ranges of the following types:
  //  * <no range>
  //  * ,
  //  * %
  //  * <int>
  //  * <int>,
  //  * <int>,<int>

  // Set up the default range.
  *start = *end = current_line;

  int parsed = 0;

  // The ',' and '%' cases.
  if (*command == ',' || *command == '%') {
    *start = 1;
    current_line = *end = last_line();
    return 1;  // Parsed 1 character.
  }

  int num_chars_parsed;
  int num_parsed = sscanf(command, "%d%n", start, &num_chars_parsed);
  parsed += num_chars_parsed * (num_parsed > 0);

  // The <no range> case.
  if (num_parsed == 0) return parsed;

  // The <int> case.
  current_line = *end = *start;
  if (*(command + parsed) != ',') return parsed;

  parsed++;  // Skip over the ',' character.
  num_parsed = sscanf(command + parsed, "%d%n", end, &num_chars_parsed);
  parsed += num_chars_parsed * (num_parsed > 0);
  if (num_parsed > 0) current_line = *end;

  // The <int>,<int> and <int>, cases.
  return parsed;
}

// This expects to receive a string of the form "/regex/repl/", which it parses
// and places into pattern and repl, allocating new space for the copies. The
// return value is true iff the parse was successful. The caller only needs to
// call free on pattern and repl when the return value is true.
int parse_subst_params(char *command, char **pattern, char **repl) {
  char *cursor = command;

  if (*cursor != '/') {
    error("expected '/' after s command");
    return 0;  // 0 = did not work
  }

  // TODO Allow backslash-escaped characters in the pattern and repl.

  // `pattern` will have offsets [p_start, p_start + p_len).
  cursor++;  // Skip the current '/'.
  int p_start = cursor - command;
  while (*cursor && *cursor != '/') cursor++;
  if (*cursor == '\0') {
    error("expected '/' to end regular expression");
    return 0;  // 0 = did not work
  }
  int p_len = cursor - command - p_start;

  // `repl` will have offsets [r_start, r_end).
  cursor++;  // Skip the current '/'.
  int r_start = cursor - command;
  while (*cursor && *cursor != '/') cursor++;
  // Be chill if there's no trailing '/'.
  int r_len = cursor - command - r_start;

  // Allocate and copy our output strings.
  *pattern = calloc(p_len + 1, 1);  // + 1 for the final null, 1 = size
  memcpy(*pattern, command + p_start, p_len);
  *repl    = calloc(r_len + 1, 1);  // + 1 for the final null, 1 = size
  memcpy(*repl,    command + r_start, r_len);

  return 1;  // 1 = did work
}

// This accepts *line_ptr = <prefix> <match> <suffix> and <repl>, where <match>
// has offsets [start, end). It allocates a new string just long enough to hold
// <prefix> <repl> <suffix>, frees *line_ptr, and reassigns *line_ptr to the new
// string.
void substring_repl(char **line_ptr, size_t start, size_t end, char *repl) {
  assert(line_ptr && *line_ptr && repl);
  int orig_line_len = strlen(*line_ptr);
  assert(    0 <= start && start <  orig_line_len);
  assert(start <= end   &&   end <= orig_line_len);

  // The + 1 here is for the terminating null.
  size_t new_size = orig_line_len - (start - end) + strlen(repl) + 1;
  char *new_line = malloc(new_size);

  // *line_ptr = <prefix> <match> <suffix>
  memcpy(new_line, *line_ptr, start);             // new_line  = <prefix>
  char *cursor = stpcpy(new_line + start, repl);  // new_line += <repl>
  strcpy(cursor, *line_ptr + end);                // new_line += <suffix>

  free(*line_ptr);
  *line_ptr = new_line;
}

void substitute_on_lines(char *pattern, char *repl, int start, int end) {
  regex_t compiled_re;
  int compile_flags = 0;
  int was_error = regcomp(&compiled_re, pattern, compile_flags);
  if (was_error) {
    // TODO Nicify this and all regex-returned error messages with regerror.
    error("invalid regular expression");
    return;
  }
  size_t max_matches = compiled_re.re_nsub + 1;  // + 1 for a full-pattern match
  regmatch_t *matches = malloc(sizeof(regmatch_t) * max_matches);

  int exec_flags = 0;
  for (int i = start; i <= end; ++i) {
    int errcode = regexec(&compiled_re, line_at(i - 1),
                          max_matches, matches, exec_flags);
    if (errcode) {
      was_error = 1;
      continue;
    }
    // TODO Respect \1 .. \9 as backreference replacements.
    substring_repl(array__item_ptr(lines, i - 1),       // char ** to update
                   matches[0].rm_so, matches[0].rm_eo,  // start, end offsets
                   repl);                               // replacement
  }
  if (was_error) error("error while matching regular expression");
  regfree(&compiled_re);
}

void print_line(int line_index, int do_add_number) {
  if (do_add_number) printf("%d\t", line_index);
  printf("%s\n", line_at(line_index - 1));
}

// This enters multi-line input mode. It accepts lines of input, including
// meaningful blank lines, until a line with a single period is given.
// The lines are appended to the end of the given `lines` Array.
void read_in_lines(Array lines) {
  while (1) {
    char *line = readline("");
    if (strcmp(line, ".") == 0) return;
    array__new_val(lines, char *) = strdup(line);
  }
}

// This inserts all of `sub` into `arr` so that arr[index] = sub[0].
// TODO Modify cstructs to make this easier.
void insert_subarr_into_arr(Array sub, Array arr, int index) {
  assert(arr->item_size == sub->item_size);
  int arr_count = arr->count;
  int sub_count = sub->count;
  array__add_zeroed_items(arr, sub->count);
  for (int i = arr_count - 1; i >= index; --i) {
    // arr[i + sub_count] = arr[i]
    memcpy(array__item_ptr(arr, i + sub_count),  // dst
           array__item_ptr(arr, i),              // src
           arr->item_size);                      // size
  }
  for (int i = 0; i < sub_count; ++i) {
    // arr[i + index] = sub[i]
    memcpy(array__item_ptr(arr, i + index),  // dst
           array__item_ptr(sub, i),          // src
           arr->item_size);                  // size
  }
}

// Enters line-reading mode and inserts the lines at the given 0-based index.
void read_and_insert_lines_at(int index) {
  // Silently clamp the index to legal values.
  if (index < 0)            index = 0;
  if (index > lines->count) index = lines->count;
  Array new_lines = array__new(16, sizeof(char *));
  read_in_lines(new_lines);
  insert_subarr_into_arr(new_lines, lines, index);
  current_line += new_lines->count;
  array__delete(new_lines);
}

// Returns true iff the range is bad.
int err_if_bad_range(int start, int end) {
  if (start < 1 || end > last_line()) {
    error("invalid address");
    return 1;
  }
  return 0;
}

// Returns true iff the new current line is bad.
int err_if_bad_current_line(int new_current_line) {
  if (new_current_line < 1 || new_current_line > last_line()) {
    error("invalid address");  // TODO Drop magic strings.
    return 1;
  }
  current_line = new_current_line;
  return 0;
}

// Print out the given lines; useful for the p or empty commands.
// This simply produces an error if the range is invalid.
void print_range(int start, int end, int do_number_lines) {
  if (err_if_bad_range(start, end)) return;
  for (int i = start; i <= end; ++i) print_line(i, do_number_lines);
}

void delete_range(int start, int end) {
  if (err_if_bad_range(start, end)) return;
  for (int n = end - start + 1; n > 0; --n) {
    array__remove_item(lines, array__item_ptr(lines, start - 1));
  }
  current_line = (start <= last_line() ? start : last_line());
}

void join_range(int start, int end, int is_default_range) {

  // 1. Establish and check the validity of the range.
  if (is_default_range) {
    start = current_line;
    end   = current_line + 1;
  }
  if (err_if_bad_range(start, end)) return;
  if (start == end) return;

  // 2. Calculate the size we need.
  size_t joined_len = 1;  // Start at 1 for the null terminator.
  for (int i = start; i <= end; ++i) joined_len += strlen(line_at(i - 1));

  // 3. Allocate, join, and set the new line.
  char *new_line = malloc(joined_len);
  new_line[0] = '\0';
  for (int i = start; i <= end; ++i) strcat(new_line, line_at(i - 1));
  free(line_at(start - 1));
  line_at(start - 1) = new_line;
  // This method is valid because of the range checks at the function start.
  for (int i = start + 1; i <= end; ++i) {
    array__remove_item(lines, array__item_ptr(lines, i - 1));
  }

  current_line = start;  // The current line is the newly joined line.
}

// Moves the range [start, end] to after the text currently at line dst.
void move_lines(int start, int end, int dst) {
  // TODO Check that range, dst, and the pair are all valid.

  // 1. Deep copy the lines being moved so we can call delete_range later.
  Array moving_lines = array__new(end - start + 1, sizeof(char *));
  for (int i = start; i <= end; ++i) {
    array__new_val(moving_lines, char *) = strdup(line_at(i - 1));
  }

  // 2. Append the deep copy after dst.
  insert_subarr_into_arr(moving_lines, lines, dst);
  array__delete(moving_lines);

  // 3. Remove the original range.
  int range_len = end - start + 1;
  int offset    = (dst > end ? 0 : range_len);
  delete_range(start + offset, end + offset);

  current_line = dst + offset;
}

void run_command(char *command) {

  dbg_printf("run command: \"%s\"\n", command);

  int start, end;
  int num_range_chars = parse_range(command, &start, &end);
  command += num_range_chars;
  dbg_printf("After parse_range, s=%d e=%d c=\"%s\"\n", start, end, command);
  int is_default_range = (num_range_chars == 0);

  // First consider commands that may have a suffix.
  // This way we can easily give an error to an unexpected suffix in later code.

  switch(*command) {
    case 'm':  // Move the range to right after the line given as a suffix num.
      {
        save_state(backup_lines, &backup_current_line);
        int dst_line;
        int num_chars_parsed = scan_number(command + 1, &dst_line);
        if (num_chars_parsed == 0) dst_line = current_line;
        move_lines(start, end, dst_line);
        return;
      }

    case 'w':  // Save the buffer to a file.
      {
        char *new_filename = NULL;  // NULL makes save_file use the global name.
        if (*++command != '\0') {
          if (*command != ' ') {
            error("unexpected command suffix");
          } else {
            new_filename = ++command;
          }
        }
        // TODO
        //  [ ] Make this printf more similar to the load_file one, code-wise.
        //  [ ] Look for ways to factor out {load,save}_file commonalities.
        int bytes_written = save_file(new_filename);
        if (bytes_written >= 0) printf("%d\n", bytes_written);
        return;
      }

    case 'e':  // Load a file.
      {
        char *new_filename = NULL;  // NULL makes load_file use the global name.
        if (*++command != '\0') {
          if (*command != ' ') {
            error("unexpected command suffix");
          } else {
            new_filename = ++command;
          }
        }
        load_file(new_filename);
        return;
      }

    case 's':  // Make a substitution.
      {
        char *pattern;
        char *repl;
        int   did_work = parse_subst_params(++command, &pattern, &repl);
        if  (!did_work) return;
        save_state(backup_lines, &backup_current_line);
        substitute_on_lines(pattern, repl, start, end);
        free(pattern);
        free(repl);
        return;
      }
  }

  // TODO Check for, and complain about, any command suffix.

  int do_number_lines = 0;

  switch(*command) {

    case 'q':  // Quit.  TODO Error if a range is provided.
      exit(0);

    case '\0': // If no range was given, advence a line. Print current_line.
      {
        if (is_default_range) {
          if (err_if_bad_current_line(current_line + 1)) return;
        }
        print_range(current_line, current_line, 0);  // 0 = don't add line num
        break;
      }

    case '=':  // Print the range's end line num, or last line num on no range.
      printf("%d\n", (is_default_range ? last_line() : end));
      break;

    case 'n':  // Print lines with added line numbers.
      do_number_lines = 1;
      // Purposefully fall through to the next case.

    case 'p':  // Print all lines in the effective range.
      print_range(start, end, do_number_lines);
      break;

    case 'h':  // Print last error, if there was one.
      if (last_error[0]) printf("%s\n", last_error);
      break;
      
    case 'H':  // Toggle error printing.
      do_print_errors = !do_print_errors;
      break;

    case 'a':  // Append new lines.
      save_state(backup_lines, &backup_current_line);
      read_and_insert_lines_at(current_line);
      break;

    case 'i':  // Insert new lines.
      save_state(backup_lines, &backup_current_line);
      read_and_insert_lines_at(current_line - 1);
      break;

    case 'd':  // Delete lines in the effective range.
      save_state(backup_lines, &backup_current_line);
      delete_range(start, end);
      break;

    case 'c':  // Change effective range lines into newly input lines.
      {
        save_state(backup_lines, &backup_current_line);
        int is_ending_range = (end == last_line());
        delete_range(start, end);
        int insert_point = is_ending_range ? last_line() : current_line - 1;
        read_and_insert_lines_at(insert_point);
        break;
      }

    case 'j':  // Join the lines in the effective rnage.
      save_state(backup_lines, &backup_current_line);
      join_range(start, end, is_default_range);
      break;

    // TODO Check that a valid backup exists.
    case 'u':  // Undo the last change, if there was one.
      {
        // 1. Current state -> swap.
        Array swap_lines = new_lines_array();
        int   swap_current_line;
        save_state(swap_lines, &swap_current_line);

        // 2. Backup -> current state.
        load_state_from_backup();

        // 3. Swap -> backup.
        array__delete(backup_lines);
        backup_lines        = swap_lines;
        backup_current_line = swap_current_line;
        break;
      }

    default:  // If we get here, the command wasn't recognized.
      error("unknown command");
  }

  // TODO Clean up this command parsing bit.
  //  * Design carefully about treating the command suffix.
}


///////////////////////////////////////////////////////////////////////////
// Main.
///////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv) {

  // Initialization.
  init();

  if (argc < 2) {
    // The empty string indicates no filename has been given yet.
    filename[0] = '\0';
  } else {
    strlcpy(filename, argv[1], string_capacity);
    load_file(NULL);  // NULL --> use the global `filename`

    if (show_debug_output) {
      printf("File contents:'''\n");
      array__for(char **, line, lines, i) printf(i ? "\n%s" : "%s", *line);
      printf("'''\n");
    }
  }

  // Enter our read-eval-print loop (REPL).
  while (1) {
    char *line = readline("");
    run_command(line);
  }

  return 0;
}
