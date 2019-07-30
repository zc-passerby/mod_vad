#ifndef _READ_CONFIG_H_
#define _READ_CONFIG_H_

#if defined(__cplusplus) || defined(c_plusplus)
extern "C"
{
#endif

#include <stdarg.h>
#include <pthread.h>
#include <stdbool.h>

    struct ast_category
    {
        char name[80];
        int ignored; /* do not let user of the config see this category */
        struct ast_variable *root;
        struct ast_variable *last;
        struct ast_category *next;
    };

    struct ast_config
    {
        struct ast_category *root;
        struct ast_category *last;
        struct ast_category *current;
        struct ast_category *last_browse; /* used to cache the last category supplied via category_browse */
        int include_level;
        int max_include_level;
    };

    struct ast_variable
    {
        char *name;
        char *value;
        int lineno;
        int object;     /*!< 0 for variable, 1 for object */
        int blanklines; /*!< Number of blanklines following entry */
        struct ast_variable *next;
        char stuff[0];
    };

    /*! \brief Load a config file 
 * \param filename path of file to open.  If no preceding '/' character, path is considered relative to AST_CONFIG_DIR
 * Create a config structure from a given configuration file.
 *
 * Returns NULL on error, or an ast_config data structure on success
 */
    struct ast_config *ast_config_load(const char *filename);

    /*! \brief Destroys a config 
 * \param config pointer to config data structure
 * Free memory associated with a given config
 *
 */
    void ast_config_destroy(struct ast_config *config);

    /*! \brief Goes through variables
 * Somewhat similar in intent as the ast_category_browse.
 * List variables of config file category
 *
 * Returns ast_variable list on success, or NULL on failure
 */
    struct ast_variable *ast_variable_browse(const struct ast_config *config, const char *category);

    /*! \brief Gets a variable 
 * \param config which (opened) config to use
 * \param category category under which the variable lies
 * \param variable which variable you wish to get the data for
 * Goes through a given config file in the given category and searches for the given variable
 *
 * Returns the variable value on success, or NULL if unable to find it.
 */
    char *ast_variable_retrieve(const struct ast_config *config, const char *category, const char *variable);

    /*! \brief Retrieve a category if it exists
 * \param config which config to use
 * \param category_name name of the category you're looking for
 * This will search through the categories within a given config file for a match.
 *
 * Returns pointer to category if found, NULL if not.
 */
    struct ast_category *ast_category_get(const struct ast_config *config, const char *category_name);

    /*! \brief Free variable list 
 * \param var the linked list of variables to free
 * This function frees a list of variables.
 */
    void ast_variables_destroy(struct ast_variable *var);

    struct ast_config *ast_config_new(void);
    struct ast_category *ast_category_new(const char *name);
    void ast_category_append(struct ast_config *config, struct ast_category *cat);
    int ast_category_delete(struct ast_config *cfg, char *category);
    void ast_category_destroy(struct ast_category *cat);

    struct ast_variable *ast_variable_new(const char *name, const char *value);
    void ast_variable_append(struct ast_category *category, struct ast_variable *variable);
    int ast_variable_delete(struct ast_config *cfg, char *category, char *variable, char *value);

    struct ast_config *ast_config_internal_load(const char *configfile, struct ast_config *cfg);
    void ast_copy_string(char *dst, const char *src, size_t size);
    char *ast_skip_blanks(char *str);
    int ast_strlen_zero(const char *s);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif