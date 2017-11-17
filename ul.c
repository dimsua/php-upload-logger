#include "php_ul.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <Zend/zend.h>
#include <Zend/zend_virtual_cwd.h>
#include <ext/standard/info.h>
#include <ext/standard/file.h>
#include <main/rfc1867.h>
#include <main/SAPI.h>
#include <main/spprintf.h>

static int  (*old_rfc1867_callback)(unsigned int, void*, void**)    = NULL;
static void (*old_move_uploaded_file)(INTERNAL_FUNCTION_PARAMETERS) = NULL;

ZEND_DECLARE_MODULE_GLOBALS(uploadlogger);

PHP_INI_BEGIN()
    STD_PHP_INI_BOOLEAN("ul.enabled",              "0",    PHP_INI_PERDIR, OnUpdateBool,   enabled,        zend_uploadlogger_globals, uploadlogger_globals)
    STD_PHP_INI_ENTRY("ul.verification_script",    "",     PHP_INI_PERDIR, OnUpdateString, script,         zend_uploadlogger_globals, uploadlogger_globals)
PHP_INI_END()

static int verify_file(const char* script, const char* file)
{
    FILE* f;
    int outcome;

    while (isspace(*script)) {
        ++script;
    }

    if (!*script) {
        return SUCCESS;
    }

    {
        struct stat st;
        if (VCWD_STAT(script, &st) < 0) {
            zend_error(E_WARNING, "Unable to stat() the upload verification script (%s), dropping the file", script);
            return FAILURE;
        }
    }

    {
        char* buf;
        spprintf(&buf, 0, "%s %s 2>&1", script, file);
        if (UNEXPECTED(!buf)) {
            zend_error(E_WARNING, "Out of memory");
            return FAILURE;
        }

        f = VCWD_POPEN(buf, "r");
        efree(buf);
    }

    if (!f) {
        zend_error(E_WARNING, "Unable to run the upload verification script (%s), dropping the file", script);
        return FAILURE;
    }

    {
        char c;
        size_t n = fread(&c, 1, sizeof(c), f);
        if (!n) {
            pclose(f);
            zend_error(E_WARNING, "Upload verification script returned no data, dropping the file");
            return FAILURE;
        }

        if (c != '1') {
            outcome = FAILURE;
        }
        else {
            outcome = SUCCESS;
        }
    }

    pclose(f);
    return outcome;
}

static int my_rfc1867_callback(unsigned int event, void* event_data, void** extra)
{
    if UL_G(enabled) {
        if (MULTIPART_EVENT_FILE_END == event) {
                multipart_event_file_end* data = (multipart_event_file_end*)event_data;
                int fail                       = 0;

                if (!data->cancel_upload && UL_G(script) && EXPECTED(data->temp_filename)) {
                    if (FAILURE == verify_file(UL_G(script), data->temp_filename)) {
                        fail = 1;
                    }
                }

                if (fail) {
                    return FAILURE;
                }
        }
    }

    return old_rfc1867_callback ? old_rfc1867_callback(event, event_data, extra) : SUCCESS;
}

static PHP_FUNCTION(move_uploaded_file)
{
    assert(old_move_uploaded_file != NULL);
    old_move_uploaded_file(INTERNAL_FUNCTION_PARAM_PASSTHRU);

    if UL_G(enabled) {
        char* path;
        char* new_path;
        size_t path_len, new_path_len;
        if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &path, &path_len, &new_path, &new_path_len) == FAILURE) {
            return;
        }
    }
}

static PHP_GINIT_FUNCTION(uploadlogger)
{
    uploadlogger_globals->script         = NULL;
    uploadlogger_globals->enabled        = 0;
}

static PHP_MINIT_FUNCTION(uploadlogger)
{
    zend_internal_function* func = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("move_uploaded_file"));
    if (func) {
        old_move_uploaded_file = func->handler;
        func->handler          = PHP_FN(move_uploaded_file);
    }

    old_rfc1867_callback = php_rfc1867_callback;
    php_rfc1867_callback = my_rfc1867_callback;

    REGISTER_INI_ENTRIES();
    return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(uploadlogger)
{
    php_rfc1867_callback = old_rfc1867_callback;

    if (old_move_uploaded_file) {
        zend_internal_function* func = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("move_uploaded_file"));
        if (func) {
            func->handler = old_move_uploaded_file;
        }
    }

    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

static PHP_MINFO_FUNCTION(uploadlogger)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "Track File Uploads", "enabled");
    php_info_print_table_row(2, "version", PHP_UL_EXTVER);
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

static PHP_RSHUTDOWN_FUNCTION(uploadlogger)
{
    return SUCCESS;
}

zend_module_entry uploadlogger_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_UL_EXTNAME,
    NULL,
    PHP_MINIT(uploadlogger),
    PHP_MSHUTDOWN(uploadlogger),
    NULL,
    PHP_RSHUTDOWN(uploadlogger),
    PHP_MINFO(uploadlogger),
    PHP_UL_EXTVER,
    PHP_MODULE_GLOBALS(uploadlogger),
    PHP_GINIT(uploadlogger),
    NULL,
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_UPLOADLOGGER
ZEND_GET_MODULE(uploadlogger)
#endif
