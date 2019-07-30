#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define AST_INCLUDE_GLOB 1
#ifdef AST_INCLUDE_GLOB
#if defined(__Darwin__) || defined(__CYGWIN__)
#define GLOB_ABORTED GLOB_ABEND
#endif
#include <glob.h>
#endif

#include "readconfig.h"
#define warnx printf
char RUN_CONFIG_DIR[256] = "/etc/";

extern char RUN_CONFIG_DIR[256];

#define MAX_NESTED_COMMENTS 128
#define COMMENT_META ';'
#define COMMENT_TAG '-'
#define MAX_INCLUDE_LEVEL 10

pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;

void ast_copy_string(char *dst, const char *src, size_t size)
{
    while (*src && size)
    {
        *dst++ = *src++;
        size--;
    }
    if (__builtin_expect(!size, 0))
        dst--;
    *dst = '\0';
}

char *ast_skip_blanks(char *str)
{
    while (*str && *str < 33)
        str++;
    return str;
}

char *ast_trim_blanks(char *str)
{
    char *work = str;

    if (work)
    {
        work += strlen(work) - 1;
        while ((work >= str) && *work < 33)
            *(work--) = '\0';
    }
    return str;
}

int ast_strlen_zero(const char *s)
{
    return (!s || (*s == '\0'));
}

char *ast_skip_nonblanks(char *str)
{
    while (*str && *str > 32)
        str++;
    return str;
}

char *ast_strip(char *s)
{
    s = ast_skip_blanks(s);
    if (s)
        ast_trim_blanks(s);
    return s;
}

struct ast_variable *ast_variable_new(const char *name, const char *value)
{
    struct ast_variable *variable;

    int length = strlen(name) + strlen(value) + 2 + sizeof(struct ast_variable);
    variable = (struct ast_variable *)malloc(length);
    if (variable)
    {
        memset(variable, 0, length);
        variable->name = variable->stuff;
        variable->value = variable->stuff + strlen(name) + 1;
        strcpy(variable->name, name);
        strcpy(variable->value, value);
    }

    return variable;
}

void ast_variable_append(struct ast_category *category, struct ast_variable *variable)
{
    if (category->last)
        category->last->next = variable;
    else
        category->root = variable;
    category->last = variable;
}

void ast_variables_destroy(struct ast_variable *v)
{
    struct ast_variable *vn;

    while (v)
    {
        vn = v;
        v = v->next;
        memset(vn, 0, sizeof(struct ast_variable));
        free(vn);
    }
}

struct ast_variable *ast_variable_browse(const struct ast_config *config, const char *category)
{
    struct ast_category *cat = NULL;

    if (category && config->last_browse && (config->last_browse->name == category))
        cat = config->last_browse;
    else
        cat = ast_category_get(config, category);

    if (cat)
        return cat->root;
    else
        return NULL;
}

char *ast_variable_retrieve(const struct ast_config *config, const char *category, const char *variable)
{
    struct ast_variable *v;

    if (category)
    {
        for (v = ast_variable_browse(config, category); v; v = v->next)
        {
            if (!strcasecmp(variable, v->name))
                return v->value;
        }
    }
    else
    {
        struct ast_category *cat;

        for (cat = config->root; cat; cat = cat->next)
            for (v = cat->root; v; v = v->next)
                if (!strcasecmp(variable, v->name))
                    return v->value;
    }

    return NULL;
}

static struct ast_variable *variable_clone(const struct ast_variable *old)
{
    struct ast_variable *newtemp = ast_variable_new(old->name, old->value);

    if (newtemp)
    {
        newtemp->lineno = old->lineno;
        newtemp->object = old->object;
        newtemp->blanklines = old->blanklines;
        /* TODO: clone comments? */
    }
    return newtemp;
}

static void move_variables(struct ast_category *old, struct ast_category *newtemp)
{
    struct ast_variable *var;
    struct ast_variable *next;

    next = old->root;
    old->root = NULL;
    for (var = next; var; var = next)
    {
        next = var->next;
        var->next = NULL;
        ast_variable_append(newtemp, var);
    }
}

struct ast_category *ast_category_new(const char *name)
{
    struct ast_category *category;

    category = (struct ast_category *)malloc(sizeof(struct ast_category));
    if (category)
    {
        memset(category, 0, sizeof(struct ast_category));
        ast_copy_string(category->name, name, sizeof(category->name));
    }

    return category;
}

static struct ast_category *category_get(const struct ast_config *config, const char *category_name, int ignored)
{
    struct ast_category *cat;

    for (cat = config->root; cat; cat = cat->next)
    {
        if (cat->name == category_name && (ignored || !cat->ignored))
            return cat;
    }

    for (cat = config->root; cat; cat = cat->next)
    {
        if (!strcasecmp(cat->name, category_name) && (ignored || !cat->ignored))
            return cat;
    }

    return NULL;
}

struct ast_category *ast_category_get(const struct ast_config *config, const char *category_name)
{
    return category_get(config, category_name, 0);
}

void ast_category_append(struct ast_config *config, struct ast_category *category)
{
    if (config->last)
        config->last->next = category;
    else
        config->root = category;
    config->last = category;
    config->current = category;
}

void ast_category_destroy(struct ast_category *cat)
{
    ast_variables_destroy(cat->root);
    free(cat);
}

static void inherit_category(struct ast_category *newtemp, const struct ast_category *base)
{
    struct ast_variable *var;

    for (var = base->root; var; var = var->next)
    {
        struct ast_variable *v;

        v = variable_clone(var);
        if (v)
            ast_variable_append(newtemp, v);
    }
}

struct ast_config *ast_config_new(void)
{
    struct ast_config *config;

    config = (struct ast_config *)malloc(sizeof(*config));
    if (config)
    {
        memset(config, 0, sizeof(*config));
        config->max_include_level = MAX_INCLUDE_LEVEL;
    }

    return config;
}

void ast_config_destroy(struct ast_config *cfg)
{
    struct ast_category *cat, *catn;

    if (!cfg)
        return;

    cat = cfg->root;
    while (cat)
    {
        ast_variables_destroy(cat->root);
        catn = cat;
        cat = cat->next;
        free(catn);
    }
    free(cfg);
}

static int process_text_line(struct ast_config *cfg, struct ast_category **cat, char *buf, int lineno, const char *configfile)
{
    char *c;
    char *cur = buf;
    struct ast_variable *v;
    int object;

    /* Actually parse the entry */
    if (cur[0] == '[')
    {
        struct ast_category *newcat = NULL;
        char *catname;

        /* A category header */
        c = strchr(cur, ']');
        if (!c)
        {
            warnx("parse error: no closing ']', line %d of %s", lineno, configfile);
            return -1;
        }
        *c++ = '\0';
        cur++;
        if (*c++ != '(')
            c = NULL;
        catname = cur;
        *cat = newcat = ast_category_new(catname);
        if (!newcat)
        {
            warnx("Out of memory, line %d of %s", lineno, configfile);
            return -1;
        }
        /* If there are options or categories to inherit from, process them now */
        if (c)
        {
            if (!(cur = strchr(c, ')')))
            {
                warnx("parse error: no closing ')', line %d of %s", lineno, configfile);
                return -1;
            }
            *cur = '\0';
            while ((cur = strsep(&c, ",")))
            {
                if (!strcasecmp(cur, "!"))
                {
                    (*cat)->ignored = 1;
                }
                else if (!strcasecmp(cur, "+"))
                {
                    *cat = category_get(cfg, catname, 1);
                    if (!*cat)
                    {
                        ast_config_destroy(cfg);
                        if (newcat)
                            ast_category_destroy(newcat);
                        warnx("Category addition requested, but category '%s' does not exist, line %d of %s", catname, lineno, configfile);
                        return -1;
                    }
                    if (newcat)
                    {
                        move_variables(newcat, *cat);
                        ast_category_destroy(newcat);
                        newcat = NULL;
                    }
                }
                else
                {
                    struct ast_category *base;

                    base = category_get(cfg, cur, 1);
                    if (!base)
                    {
                        warnx("Inheritance requested, but category '%s' does not exist, line %d of %s", cur, lineno, configfile);
                        return -1;
                    }
                    inherit_category(*cat, base);
                }
            }
        }
        if (newcat)
            ast_category_append(cfg, *cat);
    }
    else if (cur[0] == '#')
    {
        return 0;
    }
    else
    {
        /* Just a line (variable = value) */
        if (!*cat)
        {
            warnx("parse error: No category context for line %d of %s", lineno, configfile);
            return -1;
        }
        c = strchr(cur, '=');
        if (c)
        {
            *c = 0;
            c++;
            /* Ignore > in => */
            if (*c == '>')
            {
                object = 1;
                c++;
            }
            else
                object = 0;
            v = ast_variable_new(ast_strip(cur), ast_strip(c));
            if (v)
            {
                v->lineno = lineno;
                v->object = object;
                /* Put and reset comments */
                v->blanklines = 0;
                ast_variable_append(*cat, v);
            }
            else
            {
                warnx("Out of memory, line %d", lineno);
                return -1;
            }
        }
        else
        {
            warnx("No '=' (equal sign) in line %d of %s", lineno, configfile);
        }
    }
    return 0;
}

static struct ast_config *config_text_file_load(const char *filename, struct ast_config *cfg)
{
    char fn[256];
    char buf[8192];
    char *new_buf, *comment_p, *process_buf;
    FILE *f;
    int lineno = 0;
    int comment = 0, nest[MAX_NESTED_COMMENTS];
    struct ast_category *cat = NULL;
    int count = 0;
    struct stat statbuf;

    cat = cfg->current;
    if (filename[0] == '/')
        ast_copy_string(fn, filename, sizeof(fn));
    else
        snprintf(fn, sizeof(fn), "/%s/etc/%s", RUN_CONFIG_DIR, filename);

#ifdef AST_INCLUDE_GLOB
    {
        int glob_ret;
        glob_t globbuf;
        globbuf.gl_offs = 0; // initialize it to silence gcc
#ifdef SOLARIS
        glob_ret = glob(fn, GLOB_NOCHECK, NULL, &globbuf);
#else
        glob_ret = glob(fn, GLOB_NOMAGIC | GLOB_BRACE, NULL, &globbuf);
#endif
        if (glob_ret == GLOB_NOSPACE)
        {
            warnx("Glob Expansion of pattern '%s' failed: Not enough memory", fn);
        }
        else if (glob_ret == GLOB_ABORTED)
        {
            warnx("Glob Expansion of pattern '%s' failed: Read error", fn);
        }
        else
        {
            /* loop over expanded files */
            unsigned int i;
            for (i = 0; i < globbuf.gl_pathc; i++)
            {
                ast_copy_string(fn, globbuf.gl_pathv[i], sizeof(fn));
#endif
                do
                {
                    if (stat(fn, &statbuf))
                        continue;

                    if (!S_ISREG(statbuf.st_mode))
                    {
                        warnx("'%s' is not a regular file, ignoring", fn);
                        continue;
                    }

                    if (!(f = fopen(fn, "r")))
                    {
                        warnx("No file to parse: %s", fn);
                        continue;
                    }
                    count++;
                    warnx("Parsing file:%s", fn);
                    while (!feof(f))
                    {
                        lineno++;
                        if (fgets(buf, sizeof(buf), f))
                        {
                            new_buf = buf;
                            if (comment)
                                process_buf = NULL;
                            else
                                process_buf = buf;
                            while ((comment_p = strchr(new_buf, COMMENT_META)))
                            {
                                if ((comment_p > new_buf) && (*(comment_p - 1) == '\\'))
                                {
                                    /* Yuck, gotta memmove */
                                    memmove(comment_p - 1, comment_p, strlen(comment_p) + 1);
                                    new_buf = comment_p;
                                }
                                else if (comment_p[1] == COMMENT_TAG && comment_p[2] == COMMENT_TAG && (comment_p[3] != '-'))
                                {
                                    /* Meta-Comment start detected ";--" */
                                    if (comment < MAX_NESTED_COMMENTS)
                                    {
                                        *comment_p = '\0';
                                        new_buf = comment_p + 3;
                                        comment++;
                                        nest[comment - 1] = lineno;
                                    }
                                    else
                                    {
                                        warnx("Maximum nest limit of %d reached.", MAX_NESTED_COMMENTS);
                                    }
                                }
                                else if ((comment_p >= new_buf + 2) &&
                                         (*(comment_p - 1) == COMMENT_TAG) &&
                                         (*(comment_p - 2) == COMMENT_TAG))
                                {
                                    /* Meta-Comment end detected */
                                    comment--;
                                    new_buf = comment_p + 1;
                                    if (!comment)
                                    {
                                        /* Back to non-comment now */
                                        if (process_buf)
                                        {
                                            /* Actually have to move what's left over the top, then continue */
                                            char *oldptr;
                                            oldptr = process_buf + strlen(process_buf);
                                            memmove(oldptr, new_buf, strlen(new_buf) + 1);
                                            new_buf = oldptr;
                                        }
                                        else
                                            process_buf = new_buf;
                                    }
                                }
                                else
                                {
                                    if (!comment)
                                    {
                                        /* If ; is found, and we are not nested in a comment, 
										   we immediately stop all comment processing */
                                        *comment_p = '\0';
                                        new_buf = comment_p;
                                    }
                                    else
                                        new_buf = comment_p + 1;
                                }
                            }
                            if (process_buf)
                            {
                                char *buf = ast_strip(process_buf);
                                if (!ast_strlen_zero(buf))
                                {
                                    if (process_text_line(cfg, &cat, buf, lineno, filename))
                                    {
                                        cfg = NULL;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    fclose(f);
                } while (0);
                if (comment)
                    warnx("Unterminated comment detected beginning on line %d", nest[comment]);
#ifdef AST_INCLUDE_GLOB
                if (!cfg)
                    break;
            }
            globfree(&globbuf);
        }
    }
#endif
    if (count == 0)
        return NULL;

    return cfg;
}

struct ast_config *ast_config_internal_load(const char *filename, struct ast_config *cfg)
{
    struct ast_config *result;

    if (cfg->include_level == cfg->max_include_level)
    {
        warnx("Maximum Include level (%d) exceeded", cfg->max_include_level);
        return NULL;
    }

    cfg->include_level++;
    result = config_text_file_load(filename, cfg);

    if (result)
        result->include_level--;

    return result;
}

struct ast_config *ast_config_load(const char *filename)
{
    struct ast_config *cfg;
    struct ast_config *result;

    cfg = ast_config_new();
    if (!cfg)
        return NULL;

    result = ast_config_internal_load(filename, cfg);
    if (!result)
        ast_config_destroy(cfg);

    return result;
}