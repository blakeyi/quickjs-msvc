//#include "quickjs-libc.h"
#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <ctype.h>

#include <WinSock2.h>
#define close closesocket
#ifdef _MSC_VER
#define strncasecmp strnicmp
#endif

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



static struct sockaddr_in js_libc_http_parse_sockaddr(const char* address) {

    int port = 80;

    char* port_string = strstr(address, ":");
    if (port_string) {
        port = atoi(port_string + 1);
    }
    else {
        port_string = strstr(address, "/");
    }

    assert(port);

    char host_string[256];
    strcpy(host_string, address);
    if (port_string) {
        host_string[port_string - address] = 0;
    }

    struct hostent* host = gethostbyname(host_string);
    assert(host);
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy((char*)&addr.sin_addr.s_addr, (char*)host->h_addr, host->h_length);
    addr.sin_port = htons(port);

    return addr;
}

#define HTTP_MATCH_HEADER(l, h) \
    (!strncasecmp((l), (h), strlen(h)))

#define CHECK_ERROR(b)  \
    if(!(b)) { goto err; }

#define SAFE_FREE(p)    \
    if(p) { free(p); (p) = NULL; }

struct stream {
    char* buff;
    size_t len;
    size_t pos;
};

static struct stream stream_create() {

    struct stream stm;
    stm.pos = 0;
    stm.len = 0;
    stm.buff = NULL;

    return stm;
}

static void stream_free(struct stream* stm) {

    SAFE_FREE(stm->buff);
    stm->pos = 0;
    stm->len = 0;
}

static bool stream_reserve(struct stream* stm, size_t new_len) {

    if (!stm->buff || new_len > stm->len) {
        size_t l = (stm->len * 2 > new_len ? stm->len * 2 : new_len);
        char* p = realloc(stm->buff, l + 1);
        if (!p) {
            return false;
        }
        stm->buff = p;
    }
    return true;
}

static bool stream_printf(struct stream* stm, char* fmt, ...) {

    va_list va;
    va_start(va, fmt);

    int str_len = vsnprintf(NULL, 0, fmt, va);
    if (str_len < 0) {
        return false;
    }

    if (!stream_reserve(stm, stm->pos + str_len)) {
        return false;
    }
   
    vsnprintf(stm->buff + stm->pos, str_len + 1, fmt, va);
    stm->pos += str_len;
    va_end(va);
    return true;
}

static bool stream_write(struct stream* stm, const char* buff, size_t buff_len) {

    if (!buff_len) {
        return true;
    }

    if (!stream_reserve(stm, stm->pos + buff_len)) {
        return false;
    }

    memmove(stm->buff + stm->pos, buff, buff_len);
    stm->pos += buff_len;
    stm->buff[stm->pos] = 0;
    return true;
}

static bool stream_read(struct stream* stm, size_t len) {

    if (!len) {
        return true;
    }

    if (len > stm->pos) {
        return false;
    }

    memmove(stm->buff, stm->buff + len, stm->pos - len);
    stm->pos -= len;
    stm->buff[stm->pos] = 0;
    return true;
}

static char* http_readline(struct stream* stm) {

    char* p = strstr(stm->buff, "\r\n");
    if (!p) {
        return NULL;
    }

    size_t l = p - stm->buff;
    char* s = malloc(l + 1);
    if (!s) {
        return NULL;
    }

    memcpy(s, stm->buff, l);
    s[l] = 0;

    stream_read(stm, l + 2);
    return s;
}

static bool http_send(int sock, 
    const char* method, const char* api, const char* header,
    const char* req_buff, size_t req_len)
{
    struct stream stm_send = stream_create();
    bool ret = false;

    CHECK_ERROR(stream_printf(&stm_send, "%s %s HTTP/1.0\r\n", method, api));
    CHECK_ERROR(stream_printf(&stm_send, "Content-Length: %ld\r\n", req_len));
    CHECK_ERROR(stream_printf(&stm_send, "%s\r\n", header));

    CHECK_ERROR(stream_write(&stm_send, req_buff, req_len));

    while (stm_send.pos) {
        int send_len = send(sock, stm_send.buff, stm_send.pos, 0);
        CHECK_ERROR(send_len != SOCKET_ERROR);
        CHECK_ERROR(stream_read(&stm_send, send_len));
    }

    ret = true;
err:;
    stream_free(&stm_send);
    return ret;
}

static char* http_recv(int sock, size_t* len)
{
    struct stream stm_recv = stream_create();
    struct stream stm_http = stream_create();

    size_t content_len = -1;
    char* header_line = NULL;
    char header_comma = ' ';


    bool recv_end = false;
    bool header_end = false;
    bool body_end = false;

    char* ret = NULL;

    CHECK_ERROR(stream_reserve(&stm_recv, 8192));
    CHECK_ERROR(stream_reserve(&stm_http, 8192)); 
    CHECK_ERROR(stream_printf(&stm_http, "{"));

    while (true)
    {
        if (!recv_end)
        {
            char buff[256];
            int recv_len = recv(sock, buff, COUNTOF(buff), 0);

#ifdef _MSC_VER
            char log[256];
            sprintf(log, "recv_len %d", recv_len);
            OutputDebugString(log);
#endif

            CHECK_ERROR(recv_len != SOCKET_ERROR);
            if (recv_len == 0) {
                recv_end = true;
            }
            else {
                CHECK_ERROR(stream_write(&stm_recv, buff, recv_len));
            }
        }

        while (!header_end)
        {
            if (header_line) { header_comma = ','; }
            SAFE_FREE(header_line);
            header_line = http_readline(&stm_recv);
            if (!header_line) {
                CHECK_ERROR(!recv_end);
                break;
            }
            if (strlen(header_line) == 0) {
                header_end = true;

                CHECK_ERROR(stream_printf(&stm_http, "},"));
                break;
            }

            if (HTTP_MATCH_HEADER(header_line, "HTTP/")) {
                double v = 0;
                int status = 0;
                CHECK_ERROR(2 == sscanf(strchr(header_line, '/') + 1, "%lf %d", &v, &status));

                CHECK_ERROR(stream_printf(&stm_http, "\"status\": %d,", status));
                CHECK_ERROR(stream_printf(&stm_http, "\"header\": {", status));
                SAFE_FREE(header_line);
            }
            else if (HTTP_MATCH_HEADER(header_line, "Content-Length:")) {
                content_len = atoi(strchr(header_line, ':') + 1);
                CHECK_ERROR(stream_printf(&stm_http, "%c\"Content-Length\": \"%d\"", header_comma, content_len));
            }
            else {
                char* p = strchr(header_line, ':');
                CHECK_ERROR(p);
                
                *(p++) = 0;
                while (*p && (!isprint(*p) || isspace(*p))) p++;
                CHECK_ERROR(stream_printf(&stm_http, "%c\"%s\": \"%s\"", header_comma, header_line, p));
            }
        }

        if (!header_end) {
            continue;
        }

        if (!body_end)
        {
            if (content_len == -1) {
                body_end = recv_end;
            }
            else if (content_len == stm_recv.pos) {
                recv_end = true;
                body_end = true;
            }
            else{
                CHECK_ERROR(!recv_end);
            }

            continue;
        }

        CHECK_ERROR(stream_printf(&stm_http, "\"body\": \""));
        for (size_t i = 0; i < stm_recv.pos; i++) {
            CHECK_ERROR(stream_printf(&stm_http, "\\\\x%02x", (uint8_t)stm_recv.buff[i]));
        }
        CHECK_ERROR(stream_printf(&stm_http, "\""));
        break;
    }

    CHECK_ERROR(stream_printf(&stm_http, "}"));
    ret = strdup(stm_http.buff);
    (*len) = stm_http.pos;

#ifdef _MSC_VER
    OutputDebugString(ret);
#endif
err:;
    stream_free(&stm_recv);
    stream_free(&stm_http);
    SAFE_FREE(header_line);

    return ret;
}

static JSValue js_libc_http_post(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    CHECK_ARGS(ctx, argc, argv, ((enum argtype[]){t_string, t_string, t_string}))

    const char* url = JS_ToCString(ctx, argv[0]);
    const char* header = JS_ToCString(ctx, argv[1]);
	size_t req_len = 0;
    const char* req = JS_ToCStringLen(ctx, &req_len, argv[2]);

    const char* protocol = "http://";
    const char* api = NULL;
    const char* msg = "";
    int sock = NULL;

    size_t resp_len = 0;
    char* resp = NULL;

    msg = "protocol error";
    CHECK_ERROR(!strncasecmp(protocol, url, strlen(protocol)));

    msg = "url error";
    api = strstr(url + strlen(protocol), "/");
    CHECK_ERROR(api);

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    struct sockaddr_in addr = js_libc_http_parse_sockaddr(url + strlen(protocol));

    msg = "socket failed";
    sock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_ERROR(sock != INVALID_SOCKET);

    msg = "connect failed";
    CHECK_ERROR(!connect(sock, (const struct sockaddr*)&addr, sizeof(addr)));

    msg = "http_send failed";
    CHECK_ERROR(http_send(sock, "POST", api, header, req, req_len));

    msg = "http_recv failed";
    resp = http_recv(sock, &resp_len);
    CHECK_ERROR(resp);

	close(sock);
    WSACleanup();

    JSValue ret = JS_ParseJSON(ctx, resp, resp_len, "<fromJSON>");
    SAFE_FREE(resp);
    return ret;
err:;
    if (sock > 0) {
        close(sock);
    }

    JS_ThrowInternalError(ctx, "%s", msg);
    return JS_EXCEPTION;
}

static JSValue js_libc_http_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    CHECK_ARGS(ctx, argc, argv, ((enum argtype[]){t_string, t_string}))

    const char* url = JS_ToCString(ctx, argv[0]);
    const char* header = JS_ToCString(ctx, argv[1]);

    const char* protocol = "http://";
    const char* api = NULL;
    const char* msg = "";
    int sock = NULL;

    size_t resp_len = 0;
    char* resp = NULL;

    msg = "protocol error";
    CHECK_ERROR(!strncasecmp(protocol, url, strlen(protocol)));

    msg = "url error";
    api = strstr(url + strlen(protocol), "/");
    CHECK_ERROR(api);

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    struct sockaddr_in addr = js_libc_http_parse_sockaddr(url + strlen(protocol));

    msg = "socket failed";
    sock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_ERROR(sock != INVALID_SOCKET);

    msg = "connect failed";
    CHECK_ERROR(!connect(sock, (const struct sockaddr*)&addr, sizeof(addr)));

    msg = "http_send failed";
    CHECK_ERROR(http_send(sock, "GET", api, header, NULL, 0));

    msg = "http_recv failed";
    resp = http_recv(sock, &resp_len);
    CHECK_ERROR(resp);

    close(sock);
    WSACleanup();

    JSValue ret = JS_ParseJSON(ctx, resp, resp_len, "<fromJSON>");
    SAFE_FREE(resp);
    return ret;
err:;
    if (sock > 0) {
        close(sock);
    }

    JS_ThrowInternalError(ctx, "%s", msg);
    return JS_EXCEPTION;
}

static JSCFunctionListEntry js_http_funcs[] = {
    JS_CFUNC_DEF("post", 1, js_libc_http_post),
    JS_CFUNC_DEF("get", 1, js_libc_http_get)
};

static int js_http_init(JSContext* ctx, JSModuleDef* m) {
    JS_SetModuleExportList(ctx, m, js_http_funcs, COUNTOF(js_http_funcs));
    return 0;
}

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_http
#endif

JSModuleDef* JS_INIT_MODULE(JSContext* ctx, const char* module_name) {
    JSModuleDef* m;
    m = JS_NewCModule(ctx, module_name, js_http_init);
    if (!m)
        return NULL;
    JS_AddModuleExportList(ctx, m, js_http_funcs, COUNTOF(js_http_funcs));
    return m;
}