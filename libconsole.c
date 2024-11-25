//#include "quickjs-libc.h"
#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <ctype.h>

/*
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>
#include <arpa/inet.h>
*/

#define COUNTOF(x) (sizeof(x) / sizeof((x)[0]))


enum argtype {
    t_null,
    t_bool,
    t_number,
    t_string,
    t_string_or_null,
    t_function,
};

static bool check_args(JSContext* ctx, int argc, JSValueConst* argv, enum argtype argtype_list[], int argtype_count) {
    if (argc != argtype_count) {
        JS_ThrowTypeError(ctx, "argc must be %d, got %d", argtype_count, argc);
        return false;
    }
    for (int i = 0; i < argtype_count; i++) {
        switch (argtype_list[i]) {
        case t_null:
            if (!JS_IsNull(argv[i])) {
                JS_ThrowTypeError(ctx, "argv[%d] must be null", i);
                return false;
            }
            break;
        case t_bool:
            if (!JS_IsBool(argv[i])) {
                JS_ThrowTypeError(ctx, "argv[%d] must be boolean", i);
                return false;
            }
            break;
        case t_number:
            if (!JS_IsNumber(argv[i])) {
                JS_ThrowTypeError(ctx, "argv[%d] must be number", i);
                return false;
            }
            break;
        case t_string:
            if (!JS_IsString(argv[i])) {
                JS_ThrowTypeError(ctx, "argv[%d] must be string", i);
                return false;
            }
            break;
        case t_string_or_null:
            if (!(JS_IsString(argv[i]) || JS_IsNull(argv[i]))) {
                JS_ThrowTypeError(ctx, "argv[%d] must be string or null", i);
                return false;
            }
            break;
        case t_function:
            if (!JS_IsFunction(ctx, argv[i])) {
                JS_ThrowTypeError(ctx, "argv[%d] must be function", i);
                return false;
            }
            break;
        default:
            JS_ThrowTypeError(ctx, "argv[%d] type definition is not yet supported", i);
            return false;
        }
    }
    return true;
}

#define CHECK_ARGS(ctx, argc, argv, tlist)                       \
    if (!check_args(ctx, argc, argv, (tlist), COUNTOF(tlist))) { \
        return JS_EXCEPTION;                                     \
    }


#define CHECK_ERROR(b)  \
    if(!(b)) { goto err; }

#define SAFE_FREE(p)    \
    if(p) { free(p); (p) = NULL; }


static JSValue js_log(FILE* const f, const char* level, JSContext* ctx, int argc, JSValueConst* argv)
{
    fprintf(f, "[%s]", level);

    for (int i = 0; i < argc; i++)
    {
        fprintf(f, " ");

        JSValue json = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);

        size_t len = 0;
        const char* str = JS_ToCStringLen(ctx, &len, json);

        JS_FreeValue(ctx, json);

        if (!str) {
            return JS_EXCEPTION;
        }

        fwrite(str, 1, len, f);
        JS_FreeCString(ctx, str);
    }

    fprintf(f, "\n");
    fflush(f);
    return JS_UNDEFINED;
}

static JSValue js_libc_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return js_log(stdout, "log", ctx, argc, argv);
}
static JSValue js_libc_console_info(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return js_log(stdout, "info", ctx, argc, argv);
}
static JSValue js_libc_console_warn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return js_log(stdout, "warn", ctx, argc, argv);
}
static JSValue js_libc_console_error(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return js_log(stdout, "error", ctx, argc, argv);
}
static JSValue js_libc_console_trace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return js_log(stdout, "trace", ctx, argc, argv);
}


static JSCFunctionListEntry js_console_funcs[] = {
    JS_CFUNC_DEF("log", 1, js_libc_console_log),
    JS_CFUNC_DEF("info", 1, js_libc_console_info),
    JS_CFUNC_DEF("warn", 1, js_libc_console_warn),
    JS_CFUNC_DEF("error", 1, js_libc_console_error),
    JS_CFUNC_DEF("trace", 1, js_libc_console_trace)
};

static int js_console_init(JSContext* ctx, JSModuleDef* m) {
    JS_SetModuleExportList(ctx, m, js_console_funcs, COUNTOF(js_console_funcs));
    return 0;
}

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_console
#endif

JSModuleDef* JS_INIT_MODULE(JSContext* ctx, const char* module_name) {
    JSModuleDef* m;
    m = JS_NewCModule(ctx, module_name, js_console_init);
    if (!m)
        return NULL;
    JS_AddModuleExportList(ctx, m, js_console_funcs, COUNTOF(js_console_funcs));
    return m;
}