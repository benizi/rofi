#ifndef ROFI_HELPER_H
#define ROFI_HELPER_H
#define SN_API_NOT_YET_FROZEN
/* This function is declared as sn_launcher_context_set_application_id but implemented as sn_launcher_set_application_id */
#define sn_launcher_context_set_application_id    sn_launcher_set_application_id
#include <libsn/sn.h>
#include "rofi.h"

/**
 * @defgroup HELPERS Helpers
 */
/**
 * @defgroup HELPER Helper
 * @ingroup HELPERS
 *
 * @{
 */

/**
 * @param args The arguments of the command to exec.
 * @param sncontext The startup notification context if any.
 * @param error_precmd Prefix to error message command.
 * @param error_cmd Error message command
 *
 * Executes the command
 *
 * @returns TRUE when successful, FALSE when failed.
 */
gboolean helper_exec ( char **args, SnLauncherContext *sncontext, const char *error_precmd, const char *error_cmd );

/**
 * @param cmd The command to run.
 * @param run_in_term Whether to run the command in a terminal.
 * @param sn Whether to use startup notification or not.
 * @param id Startup notification application id, if any.
 * @param wmclass Startup notification WM class, if any.
 *
 * Executes the command
 *
 * @returns TRUE when successful, FALSE when failed.
 */
gboolean helper_exec_sh ( const char *cmd, gboolean run_in_term, gboolean sn, const char *id, const char *wmclass );

/**
 * @param string The input string.
 * @param output Pointer to 2 dimensional array with parsed string.
 * @param length Length of 2 dimensional array.
 * @param ...    Key, value parse. Replaces the string Key with value.
 *
 * Parses a string into arguments. While replacing keys with values.
 *
 * @returns TRUE when successful, FALSE when failed.
 */
int helper_parse_setup ( char * string, char ***output, int *length, ... );

/**
 * @param token The string for which we want a collation key.
 * @param case_sensitive Whether case is significant.
 *
 * Get a collation key for @p token. @p token must be a null-terminated string.
 * This collation key can be used for matching the user input against the list
 * of commands, windows, or ssh commands.
 *
 * @returns A newly allocated string containing the collation key.
 */
char *token_collate_key ( const char *token, int case_sensitive );

/**
 * @param input The input string.
 * @param case_sensitive Whether case is significant.
 *
 * Tokenize the string on spaces.
 *
 * @returns a newly allocated 2 dimensional array of strings.
 */
char **tokenize ( const char *input, int case_sensitive );
void tokenize_free ( char ** tokens );

/**
 * @param key The key to search for
 * @param val Pointer to the string to set to the key value (if found)
 *
 * Parse command line argument 'key' to character.
 * This one supports character escaping.
 *
 * @returns TRUE if key was found and val was set.
 */
int find_arg_char ( const char * const key, char *val );

/**
 * @param key The key to search for
 * @param val Pointer to the string to set to the key value (if found)
 *
 * Parse command line argument 'key' to unsigned int.
 *
 * @returns TRUE if key was found and val was set.
 */
int find_arg_uint ( const char * const key, unsigned int *val );

/**
 * @param key The key to search for
 * @param val Pointer to the string to set to the key value (if found)
 *
 * Parse command line argument 'key' to int.
 *
 * @returns TRUE if key was found and val was set.
 */
int find_arg_int ( const char * const key, int *val );

/**
 * @param key The key to search for
 * @param val Pointer to the string to set to the key value (if found)
 *
 * Parse command line argument 'key' to string.
 *
 * @returns TRUE if key was found and val was set.
 */
int find_arg_str ( const char * const key, char** val );

/**
 * @param key The key to search for
 *
 * Check if key is passed as argument.
 *
 * @returns return position of string or -1 if not found.
 */
int find_arg ( const char * const key );

/**
 * @params tokens
 * @param tokens  List of (input) tokens to match.
 * @param input   The entry to match against.
 * @param case_sensitive Whether case is significant.
 * @param index   The current selected index.
 * @param data    User data.
 *
 * Tokenized match, match tokens to line input.
 *
 * @returns 1 when matches, 0 otherwise
 */
int token_match ( char **tokens, const char *input, int not_ascii, int case_sensitive );
/**
 * @param cmd The command to execute.
 *
 * Execute cmd using config.run_command and outputs the result (stdout) to the opened file
 * descriptor.
 *
 * @returns a valid file descriptor on success, or -1 on failure.
 */
int execute_generator ( const char * cmd ) __attribute__( ( nonnull ) );

/**
 * @param pidfile The pidfile to create.
 *
 * returns file descriptor (or -1 when failed)
 */
int create_pid_file ( const char *pidfile );

/**
 * Remove pid file
 */
void remove_pid_file ( int fd );

/**
 * Do some input validation, especially the first few could break things.
 * It is good to catch them beforehand.
 *
 * This functions exits the program with 1 when it finds an invalid configuration.
 */
int config_sanity_check ( void );

/**
 * @param arg string to parse.
 *
 * Parses a string into an character.
 *
 * @returns character.
 */
char helper_parse_char ( const char *arg );

/**
 * @param argc number of arguments.
 * @param argv Array of arguments.
 *
 * Set the application arguments.
 */
void cmd_set_arguments ( int argc, char **argv );

/**
 * @param input The path to expand
 *
 * Expand path, both `~` and `~<user>`
 *
 * @returns path
 */
char *rofi_expand_path ( const char *input );
unsigned int levenshtein ( const char *needle, const char *haystack );

/*@}*/
#endif // ROFI_HELPER_H
