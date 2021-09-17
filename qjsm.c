#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * QuickJS stand alone interpreter
 *
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following states:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include <signal.h>
#include <sys/poll.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

#if 1 // def HAVE_QUICKJS_CONFIG_H
#include "quickjs-config.h"
#endif

#ifdef USE_WORKER
#include <pthread.h>
#include <stdatomic.h>

static int
atomic_add_int(int* ptr, int v) {
  return atomic_fetch_add((_Atomic(uint32_t)*)ptr, v) + v;
}
#endif

#include "list.h"
#include "cutils.h"
#include "utils.h"
#include "vector.h"
#include "quickjs-libc.h"
#include "quickjs-internal.h"
#include "buffer-utils.h"

typedef struct pollhandler {
  struct pollfd pf;
  void (*handler)(void* opaque, struct pollfd*);
  void* opaque;
  struct list_head link;
} pollhandler_t;

thread_local uint64_t jsm_pending_signals = 0;
struct list_head pollhandlers;

void js_std_set_module_loader_func(JSModuleLoaderFunc* func);

#ifdef HAVE_MALLOC_USABLE_SIZE
#ifndef HAVE_MALLOC_USABLE_SIZE_DEFINITION
extern size_t malloc_usable_size();
#endif
#endif

#define trim_dotslash(str) (!strncmp((str), "./", 2) ? (str) + 2 : (str))

#define jsm_declare_module(name)                                                                                                                                                                       \
  extern const uint8_t qjsc_##name[];                                                                                                                                                                  \
  extern const uint32_t qjsc_##name##_size;                                                                                                                                                            \
  JSModuleDef* js_init_module_##name(JSContext*, const char*);

jsm_declare_module(console);
jsm_declare_module(events);
jsm_declare_module(fs);
jsm_declare_module(perf_hooks);
jsm_declare_module(process);
jsm_declare_module(repl);
jsm_declare_module(require);
jsm_declare_module(tty);
jsm_declare_module(util);

#ifdef CONFIG_BIGNUM
jsm_declare_module(qjscalc);
static int bignum_ext = 1;
#endif

void js_std_set_worker_new_context_func(JSContext* (*func)(JSRuntime* rt));

void jsm_std_dump_error(JSContext* ctx, JSValue exception_val);

static BOOL debug_module_loader = FALSE;

static Vector module_debug = VECTOR_INIT();
static Vector module_list = VECTOR_INIT();
static Vector builtins = VECTOR_INIT();

JSValue package_json;

static JSValue
jsm_load_package_json(JSContext* ctx, const char* file) {
  if(JS_IsUndefined(package_json)) {
    uint8_t* buf;
    size_t len;
    if(file == 0)
      file = "package.json";
    if(!(buf = js_load_file(ctx, &len, file)))
      package_json = JS_NULL;
    else
      package_json = JS_ParseJSON(ctx, buf, len, file);
  }
  return JS_DupValue(ctx, package_json);
}

char*
jsm_find_module_ext(JSContext* ctx, const char* module, const char* ext) {
  const char *path, *p, *q;
  char* file = NULL;
  size_t n, m;
  struct stat st;

  if((path = getenv("QUICKJS_MODULE_PATH")) == NULL)
    path = js_default_module_path;

  for(p = path; *p; p = q) {
    if((q = strchr(p, ':')) == NULL)
      q = p + strlen(p);
    n = q - p;
    file = js_malloc(ctx, n + 1 + strlen(module) + 3 + 1);
    strncpy(file, p, n);
    file[n] = '/';
    strcpy(&file[n + 1], module);
    m = strlen(module);
    if(!(m >= 3 && !strcmp(&module[m - 3], ext)))
      strcpy(&file[n + 1 + m], ext);
    if(!stat(file, &st))
      return file;
    js_free(ctx, file);
    if(*q == ':')
      ++q;
  }
  return NULL;
}

char*
jsm_find_module(JSContext* ctx, const char* module) {
  char* path = NULL;
  size_t len;

  while(!strncmp(module, "./", 2)) module = trim_dotslash(module);
  len = strlen(module);

  if(strchr(module, '/') == NULL || (len >= 3 && !strcmp(&module[len - 3], ".so")))
    path = jsm_find_module_ext(ctx, module, ".so");

  if(path == NULL)
    path = jsm_find_module_ext(ctx, module, ".js");
  return path;
}

char*
jsm_normalize_module(JSContext* ctx, const char* base_name, const char* name, void* opaque) {
  size_t p;
  const char* r;
  DynBuf file = {0, 0, 0};
  size_t n;
  if(name[0] != '.')
    return js_strdup(ctx, name);

  js_dbuf_init(ctx, &file);

  n = base_name[(p = str_rchr(base_name, '/'))] ? p : 0;

  dbuf_put(&file, base_name, n);
  dbuf_0(&file);

  for(r = name;;) {
    if(r[0] == '.' && r[1] == '/') {
      r += 2;
    } else if(r[0] == '.' && r[1] == '.' && r[2] == '/') {
      /* remove the last path element of file, except if "." or ".." */
      if(file.size == 0)
        break;
      if((p = byte_rchr(file.buf, file.size, '/')) < file.size)
        p++;
      else
        p = 0;
      if(!strcmp(&file.buf[p], ".") || !strcmp(&file.buf[p], ".."))
        break;
      if(p > 0)
        p--;
      file.size = p;
      r += 3;
    } else {
      break;
    }
  }
  if(file.size == 0)
    dbuf_putc(&file, '.');

  dbuf_putc(&file, '/');
  dbuf_putstr(&file, r);
  dbuf_0(&file);

  // printf("jsm_normalize_module\x1b[1;48;5;27m(1)\x1b[0m %-40s %-40s -> %s\n", base_name, name,
  // file.buf);

  return file.buf;
}

static JSModuleDef*
jsm_module_loader_so(JSContext* ctx, const char* module) {
  JSModuleDef* m;
  void* hd;
  JSModuleDef* (*init)(JSContext*, const char*);
  char* file;

  if(!strchr(module, '/')) {
    /* must add a '/' so that the DLL is not searched in the system library paths */
    if(!(file = js_malloc(ctx, strlen(module) + 2 + 1)))
      return NULL;
    strcpy(file, "./");
    strcpy(file + 2, module);
  } else {
    file = (char*)module;
  }
  /* C module */
  hd = dlopen(file, RTLD_NOW | RTLD_LOCAL);
  if(file != module)
    js_free(ctx, file);
  if(!hd) {
    JS_ThrowReferenceError(ctx, "could not load module file '%s' as shared library: %s", module, dlerror());
    goto fail;
  }

  init = dlsym(hd, "js_init_module");
  if(!init) {
    JS_ThrowReferenceError(ctx, "could not load module file '%s': js_init_module not found", module);
    goto fail;
  }

  m = init(ctx, module);
  if(!m) {
    JS_ThrowReferenceError(ctx, "could not load module file '%s': initialization error", module);
  fail:
    if(hd)
      dlclose(hd);
    return NULL;
  }
  return m;
}

JSModuleDef*
jsm_module_loader_path(JSContext* ctx, const char* name, void* opaque) {
  char *module, *file = 0;
  JSModuleDef* ret = NULL;
  module = js_strdup(ctx, trim_dotslash(name));
  for(;;) {
    if(!strchr(module, '/') && (ret = js_module_search(ctx, module))) {
      goto end;
    }
    if(debug_module_loader) {
      if(file)
        printf("jsm_module_loader_path[%x] \x1b[48;5;220m(2)\x1b[0m %-20s '%s'\n", pthread_self(), trim_dotslash(name), file);
      /*  else  printf("jsm_module_loader_path[%x] \x1b[48;5;124m(1)\x1b[0m %-20s -> %s\n",
       * pthread_self(), trim_dotslash(name), trim_dotslash(module));*/
    }
    if(!has_suffix(name, ".so") && !file) {
      JSValue package = jsm_load_package_json(ctx, 0);
      if(!JS_IsNull(package)) {
        JSValue aliases = JS_GetPropertyStr(ctx, package, "_moduleAliases");
        JSValue target = JS_UNDEFINED;
        if(!JS_IsUndefined(aliases)) {
          target = JS_GetPropertyStr(ctx, aliases, module);
        }
        JS_FreeValue(ctx, aliases);
        JS_FreeValue(ctx, package);
        if(!JS_IsUndefined(target)) {
          const char* str = JS_ToCString(ctx, target);
          if(str) {
            js_free(ctx, module);
            module = js_strdup(ctx, str);
            JS_FreeCString(ctx, str);
            continue;
          }
        }
      }
    }
    if(!file) {
      if(strchr("./", module[0]))
        file = js_strdup(ctx, module);
      else if(!(file = jsm_find_module(ctx, module)))
        break;
      continue;
    }
    break;
  }
  if(file) {
    if(debug_module_loader)
      if(strcmp(trim_dotslash(name), trim_dotslash(file)))
        printf("jsm_module_loader_path[%x] \x1b[48;5;28m(3)\x1b[0m %-20s -> %s\n", pthread_self(), module, file);
    ret = has_suffix(file, ".so") ? jsm_module_loader_so(ctx, file) : js_module_loader(ctx, file, opaque);
  }
end:
  if(vector_finds(&module_debug, "import") != -1) {
    fprintf(stderr, (!file || strcmp(module, file)) ? "!!! IMPORT %s -> %s\n" : "!!! IMPORT %s\n", module, file);
  }
  if(!ret)
    printf("jsm_module_loader_path(\"%s\") = %p\n", name, ret);
  if(module)
    js_free(ctx, module);
  if(file)
    js_free(ctx, file);
  return ret;
}

static JSValue
jsm_eval_buf(JSContext* ctx, const char* buf, int buf_len, const char* filename, int flags) {
  JSValue val;

  if(flags & JS_EVAL_TYPE_MODULE) {
    /* for the modules, we compile then run to be able to set import.meta */
    val = JS_Eval(ctx, buf, buf_len, filename, flags | JS_EVAL_FLAG_COMPILE_ONLY);

    if(JS_IsException(val)) {
      if(JS_IsNull(JS_GetRuntime(ctx)->current_exception)) {
        JS_GetException(ctx);
        val = JS_UNDEFINED;
      }
    }

    if(!JS_IsException(val)) {
      js_module_set_import_meta(ctx, val, FALSE, TRUE);
      /*val =*/JS_EvalFunction(ctx, val);
    }
  } else {
    val = JS_Eval(ctx, buf, buf_len, filename, flags & (~(JS_EVAL_TYPE_MODULE)));
  }

  return val;
}

static JSValue
jsm_eval_file(JSContext* ctx, const char* file, int module) {
  uint8_t* buf;
  size_t len;
  int flags;
  if(!(buf = js_load_file(ctx, &len, file))) {
    fprintf(stderr, "Failed loading '%s': %s\n", file, strerror(errno));
    return JS_ThrowInternalError(ctx, "Failed loading '%s': %s", file, strerror(errno));
  }
  if(module < 0)
    module = (has_suffix(file, ".mjs") || JS_DetectModule((const char*)buf, len));
  flags = module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
  return jsm_eval_buf(ctx, buf, len, file, flags);
}

static int
jsm_load_script(JSContext* ctx, const char* filename, BOOL module) {
  JSValue val;
  int32_t ret = 0;
  val = jsm_eval_file(ctx, filename, module);
  if(JS_IsException(val)) {
    js_value_fwrite(ctx, val, stderr);
    return -1;
  }
  if(JS_IsNumber(val))
    JS_ToInt32(ctx, &ret, val);
  if(JS_VALUE_GET_TAG(val) != JS_TAG_MODULE && JS_VALUE_GET_TAG(val) != JS_TAG_EXCEPTION)
    JS_FreeValue(ctx, val);
  return ret;
}

/*static JSModuleDef*
jsm_load_module(JSContext* ctx, const char* name) {
  DynBuf buf;
  js_dbuf_init(ctx, &buf);
  dbuf_printf(&buf, "import * as %s from '%s'; globalThis.%s = %s;", name, name, name, name);
  dbuf_0(&buf);
  jsm_eval_buf(ctx, buf.buf, buf.size, "<input>", JS_EVAL_TYPE_MODULE);
  return js_module_search(ctx, name);
}*/

/* also used to initialize the worker context */
static JSContext*
jsm_context_new(JSRuntime* rt) {
  JSContext* ctx;
  ctx = JS_NewContext(rt);
  if(!ctx)
    return NULL;
#ifdef CONFIG_BIGNUM
  if(bignum_ext) {
    JS_AddIntrinsicBigFloat(ctx);
    JS_AddIntrinsicBigDecimal(ctx);
    JS_AddIntrinsicOperators(ctx);
    JS_EnableBignumExt(ctx, TRUE);
  }
#endif

#define jsm_module_native(name) js_init_module_##name(ctx, #name);

  jsm_module_native(std);
  jsm_module_native(os);
  jsm_module_native(child_process);
  jsm_module_native(deep);
  jsm_module_native(inspect);
  jsm_module_native(lexer);
  jsm_module_native(misc);
  jsm_module_native(mmap);
  jsm_module_native(path);
  jsm_module_native(pointer);
  jsm_module_native(predicate);
  jsm_module_native(repeater);
  jsm_module_native(tree_walker);
  jsm_module_native(xml);
  return ctx;
}

#if defined(__APPLE__)
#define MALLOC_OVERHEAD 0
#else
#define MALLOC_OVERHEAD 8
#endif

struct trace_malloc_data {
  uint8_t* base;
};

static void
dump_vector(const Vector* vec, size_t start) {
  size_t i, len = vector_size(vec, sizeof(char*));
  for(i = start; i < len; i++) {
    const char* str = *(char**)vector_at(vec, sizeof(char*), i);
    fputs(i > start ? "',\n  '" : "[\n  '", stdout);
    fputs(str, stdout);
    if(i + 1 == len)
      puts("'\n]");
  }
}

static inline unsigned long long
jsm_trace_malloc_ptr_offset(uint8_t* ptr, struct trace_malloc_data* dp) {
  return ptr - dp->base;
}

/* default memory allocation functions with memory limitation */
static inline size_t
jsm_trace_malloc_usable_size(void* ptr) {
#if defined(__APPLE__)
  return malloc_size(ptr);
#elif defined(_WIN32)
  return _msize(ptr);
#elif defined(EMSCRIPTEN) || defined(__dietlibc__) || defined(__MSYS__) || defined(DONT_HAVE_MALLOC_USABLE_SIZE)
  return 0;
#elif defined(__linux__) || defined(HAVE_MALLOC_USABLE_SIZE)
  return malloc_usable_size(ptr);
#else
  /* change this to `return 0;` if compilation fails */
  return malloc_usable_size(ptr);
#endif
}

static void
#ifdef _WIN32
    /* mingw printf is used */
    __attribute__((format(gnu_printf, 2, 3)))
#else
    __attribute__((format(printf, 2, 3)))
#endif
    jsm_trace_malloc_printf(JSMallocState* s, const char* fmt, ...) {
  va_list ap;
  int c;

  va_start(ap, fmt);
  while((c = *fmt++) != '\0') {
    if(c == '%') {
      /* only handle %p and %zd */
      if(*fmt == 'p') {
        uint8_t* ptr = va_arg(ap, void*);
        if(ptr == NULL) {
          printf("NULL");
        } else {
          printf("H%+06lld.%zd", jsm_trace_malloc_ptr_offset(ptr, s->opaque), jsm_trace_malloc_usable_size(ptr));
        }
        fmt++;
        continue;
      }
      if(fmt[0] == 'z' && fmt[1] == 'd') {
        size_t sz = va_arg(ap, size_t);
        printf("%zd", sz);
        fmt += 2;
        continue;
      }
    }
    putc(c, stdout);
  }
  va_end(ap);
}

static void
jsm_trace_malloc_init(struct trace_malloc_data* s) {
  free(s->base = malloc(8));
}

static void*
jsm_trace_malloc(JSMallocState* s, size_t size) {
  void* ptr;

  /* Do not allocate zero bytes: behavior is platform dependent */
  assert(size != 0);

  if(unlikely(s->malloc_size + size > s->malloc_limit))
    return NULL;
  ptr = malloc(size);
  jsm_trace_malloc_printf(s, "A %zd -> %p\n", size, ptr);
  if(ptr) {
    s->malloc_count++;
    s->malloc_size += jsm_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  }
  return ptr;
}

static void
jsm_trace_free(JSMallocState* s, void* ptr) {
  if(!ptr)
    return;

  jsm_trace_malloc_printf(s, "F %p\n", ptr);
  s->malloc_count--;
  s->malloc_size -= jsm_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  free(ptr);
}

static void*
jsm_trace_realloc(JSMallocState* s, void* ptr, size_t size) {
  size_t old_size;

  if(!ptr) {
    if(size == 0)
      return NULL;
    return jsm_trace_malloc(s, size);
  }
  old_size = jsm_trace_malloc_usable_size(ptr);
  if(size == 0) {
    jsm_trace_malloc_printf(s, "R %zd %p\n", size, ptr);
    s->malloc_count--;
    s->malloc_size -= old_size + MALLOC_OVERHEAD;
    free(ptr);
    return NULL;
  }
  if(s->malloc_size + size - old_size > s->malloc_limit)
    return NULL;

  jsm_trace_malloc_printf(s, "R %zd %p", size, ptr);

  ptr = realloc(ptr, size);
  jsm_trace_malloc_printf(s, " -> %p\n", ptr);
  if(ptr) {
    s->malloc_size += jsm_trace_malloc_usable_size(ptr) - old_size;
  }
  return ptr;
}

static const JSMallocFunctions trace_mf = {
    jsm_trace_malloc,
    jsm_trace_free,
    jsm_trace_realloc,
#if defined(__APPLE__)
    malloc_size,
#elif defined(_WIN32)
    (size_t(*)(const void*))_msize,
#elif defined(EMSCRIPTEN) || defined(__dietlibc__) || defined(__MSYS__) || defined(DONT_HAVE_MALLOC_USABLE_SIZE_DEFINITION)
    NULL,
#elif defined(__linux__) || defined(HAVE_MALLOC_USABLE_SIZE)
    (size_t(*)(const void*))malloc_usable_size,
#else
    /* change this to `NULL,` if compilation fails */
    malloc_usable_size,
#endif
};

#define PROG_NAME "qjsm"

void
jsm_help(void) {
  printf("QuickJS version " CONFIG_VERSION "\n"
         "usage: " PROG_NAME " [options] [file [args]]\n"
         "-h  --help         list options\n"
         "-e  --eval EXPR    evaluate EXPR\n"
         "-i  --interactive  go to interactive mode\n"
         "-m  --module NAME  load an ES6 module\n"
         "-I  --include file include an additional file\n"
         "    --std          make 'std' and 'os' available to the loaded script\n"
#ifdef CONFIG_BIGNUM
         "    --no-bignum    disable the bignum extensions (BigFloat, BigDecimal)\n"
         "    --qjscalc      load the QJSCalc runtime (default if invoked as qjscalc)\n"
#endif
         "-T  --trace        trace memory allocation\n"
         "-d  --dump         dump the memory usage stats\n"
         "    --memory-limit n       limit the memory usage to 'n' bytes\n"
         "    --stack-size n         limit the stack size to 'n' bytes\n"
         "    --unhandled-rejection  dump unhandled promise rejections\n"
         "-q  --quit         just instantiate the interpreter and quit\n");
  exit(1);
}

static JSValue
js_eval_script(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  const char* str;
  size_t len;
  JSValue ret;
  int32_t module;
  str = JS_ToCStringLen(ctx, &len, argv[0]);
  if(argc > 1)
    JS_ToInt32(ctx, &module, argv[1]);
  else
    module = str_ends(str, ".mjs");
  switch(magic) {
    case 0: {
      ret = jsm_eval_file(ctx, str, module);
      break;
    }
    case 1: {
      ret = jsm_eval_buf(ctx, str, len, "<input>", module);
      break;
    }
  }
  if(JS_IsException(ret)) {
    if(JS_IsNull(JS_GetRuntime(ctx)->current_exception)) {
      JS_GetException(ctx);
      ret = JS_UNDEFINED;
    }
  }
  if(JS_VALUE_GET_TAG(ret) == JS_TAG_MODULE) {
    JSModuleDef* module = JS_VALUE_GET_PTR(ret);
    JSValue exports, obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name", js_module_name(ctx, ret));
    JS_SetPropertyStr(ctx, obj, "exports", js_module_exports(ctx, ret));
    ret = obj;
  }
  JS_FreeCString(ctx, str);
  return ret;
}

enum { FIND_MODULE, LOAD_MODULE, RESOLVE_MODULE, GET_MODULE_NAME, GET_MODULE_OBJECT, GET_MODULE_EXPORTS, GET_MODULE_NAMESPACE, GET_MODULE_FUNCTION, GET_MODULE_EXCEPTION, GET_MODULE_META_OBJ };

static JSValue
jsm_module_func(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_EXCEPTION;
  JSModuleDef* m;
  switch(magic) {

    case FIND_MODULE: {
      const char* name = JS_ToCString(ctx, argv[0]);
      m = js_module_search(ctx, name);
      JS_FreeCString(ctx, name);
      ret = JS_DupValue(ctx, JS_MKPTR(JS_TAG_MODULE, m));
      break;
    }
    case LOAD_MODULE: {
      const char* name = JS_ToCString(ctx, argv[0]);
      JSModuleDef* m;

      if((m = js_load_module(ctx, name)))
        ret = JS_MKPTR(JS_TAG_MODULE, m);

      JS_FreeCString(ctx, name);
      break;
    }
    case RESOLVE_MODULE: {
      ret = JS_NewInt32(ctx, JS_ResolveModule(ctx, argv[0]));
      break;
    }
    case GET_MODULE_NAME: {
      if((m = js_module_get(ctx, argv[0])))
        ret = js_module_name(ctx, argv[0]);
      break;
    }
    case GET_MODULE_OBJECT: {
      if((m = js_module_get(ctx, argv[0]))) {
        ret = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, ret, "name", js_module_name(ctx, argv[0]));
        JS_SetPropertyStr(ctx, ret, "resolved", JS_NewBool(ctx, m->resolved));
        JS_SetPropertyStr(ctx, ret, "func_created", JS_NewBool(ctx, m->func_created));
        JS_SetPropertyStr(ctx, ret, "instantiated", JS_NewBool(ctx, m->instantiated));
        JS_SetPropertyStr(ctx, ret, "evaluated", JS_NewBool(ctx, m->evaluated));
        if(m->eval_has_exception)
          JS_SetPropertyStr(ctx, ret, "exception", JS_DupValue(ctx, m->eval_exception));
        if(!JS_IsUndefined(m->module_ns))
          JS_SetPropertyStr(ctx, ret, "namespace", JS_DupValue(ctx, m->module_ns));
        if(!JS_IsUndefined(m->func_obj))
          JS_SetPropertyStr(ctx, ret, "func", JS_DupValue(ctx, m->func_obj));
        if(!JS_IsUndefined(m->meta_obj))
          JS_SetPropertyStr(ctx, ret, "meta", JS_DupValue(ctx, m->meta_obj));
      }
      break;
    }
    case GET_MODULE_EXPORTS: {
      //      if((m = js_module_get(ctx, argv[0])))
      ret = js_module_exports(ctx, argv[0]);
      break;
    }
    case GET_MODULE_NAMESPACE: {
      if((m = js_module_get(ctx, argv[0])))
        ret = JS_DupValue(ctx, m->module_ns);
      break;
    }
    case GET_MODULE_FUNCTION: {
      if((m = js_module_get(ctx, argv[0]))) {
        if(TRUE || m->func_created)
          ret = JS_DupValue(ctx, m->func_obj);
        else
          ret = JS_NULL;
      }
      break;
    }
    case GET_MODULE_EXCEPTION: {
      if((m = js_module_get(ctx, argv[0]))) {
        if(m->eval_has_exception)
          ret = JS_DupValue(ctx, m->eval_exception);
        else
          ret = JS_NULL;
      }
      break;
    }
    case GET_MODULE_META_OBJ: {
      if((m = js_module_get(ctx, argv[0])))
        ret = JS_DupValue(ctx, m->meta_obj);
      break;
    }
  }
  return ret;
}

static const JSCFunctionListEntry jsm_global_funcs[] = {
    JS_CFUNC_MAGIC_DEF("evalFile", 1, js_eval_script, 0),
    JS_CFUNC_MAGIC_DEF("evalScript", 1, js_eval_script, 1),
    JS_CGETSET_DEF("moduleList", js_module_list, 0),
    JS_CFUNC_MAGIC_DEF("findModule", 1, jsm_module_func, FIND_MODULE),
    JS_CFUNC_MAGIC_DEF("loadModule", 1, jsm_module_func, LOAD_MODULE),
    JS_CFUNC_MAGIC_DEF("resolveModule", 1, jsm_module_func, RESOLVE_MODULE),
    JS_CFUNC_MAGIC_DEF("getModuleName", 1, jsm_module_func, GET_MODULE_NAME),
    JS_CFUNC_MAGIC_DEF("getModuleObject", 1, jsm_module_func, GET_MODULE_OBJECT),
    JS_CFUNC_MAGIC_DEF("getModuleExports", 1, jsm_module_func, GET_MODULE_EXPORTS),
    JS_CFUNC_MAGIC_DEF("getModuleNamespace", 1, jsm_module_func, GET_MODULE_NAMESPACE),
    JS_CFUNC_MAGIC_DEF("getModuleFunction", 1, jsm_module_func, GET_MODULE_FUNCTION),
    JS_CFUNC_MAGIC_DEF("getModuleException", 1, jsm_module_func, GET_MODULE_EXCEPTION),
    JS_CFUNC_MAGIC_DEF("getModuleMetaObject", 1, jsm_module_func, GET_MODULE_META_OBJ),
};

int
main(int argc, char** argv) {
  JSRuntime* rt;
  JSContext* ctx;
  struct trace_malloc_data trace_data = {NULL};
  int optind;
  char* expr = NULL;
  int interactive = 0;
  int dump_memory = 0;
  int trace_memory = 0;
  int empty_run = 0;
  int module = 1;
  int load_std = 1;
  int dump_unhandled_promise_rejection = 0;
  size_t memory_limit = 0;
  char* include_list[32];
  int i, include_count = 0;
#ifdef CONFIG_BIGNUM
  int load_jscalc;
#endif
  size_t stack_size = 0;
  const char* exename;

  package_json = JS_UNDEFINED;

  init_list_head(&pollhandlers);

  {
    const char* p;
    exename = argv[0];
    p = strrchr(exename, '/');
    if(p)
      exename = p + 1;
    /* load jscalc runtime if invoked as 'qjscalc' */
    load_jscalc = !strcmp(exename, "qjscalc");
  }

  /* cannot use getopt because we want to pass the command line to
     the script */
  optind = 1;
  while(optind < argc && *argv[optind] == '-') {
    char* arg = argv[optind] + 1;
    const char* longopt = "";
    const char* optarg;

    /* a single - is not an option, it also stops argument scanning */
    if(!*arg)
      break;

    if(arg[1]) {
      optarg = &arg[1];
    } else {
      optarg = argv[++optind];
    }

    if(*arg == '-') {
      longopt = arg + 1;
      arg += strlen(arg);
      /* -- stops argument scanning */
      if(!*longopt)
        break;
    }
    for(; *arg || *longopt; longopt = "") {
      char opt = *arg;
      if(opt)
        arg++;
      if(opt == 'h' || opt == '?' || !strcmp(longopt, "help")) {
        jsm_help();
        continue;
      }
      if(opt == 'e' || !strcmp(longopt, "eval")) {
        if(*arg) {
          expr = arg;
          break;
        }
        if(optind < argc) {
          expr = argv[optind++];
          break;
        }
        fprintf(stderr, "%s: missing expression for -e\n", exename);
        exit(2);
      }
      if(opt == 'I' || !strcmp(longopt, "include")) {
        if(optind >= argc) {
          fprintf(stderr, "expecting filename");
          exit(1);
        }
        if(include_count >= countof(include_list)) {
          fprintf(stderr, "too many included files");
          exit(1);
        }
        include_list[include_count++] = optarg;
        break;
      }
      if(opt == 'i' || !strcmp(longopt, "interactive")) {
        interactive++;
        break;
      }
      if(opt == 'm' || !strcmp(longopt, "module")) {
        const char* modules = argv[optind];
        size_t i, len;

        for(i = 0; modules[i]; i += len) {
          len = str_chr(&modules[i], ',');
          vector_putptr(&module_list, str_ndup(&modules[i], len));

          if(modules[i + len] == ',')
            len++;
        }

        break;
      }
      if(opt == 'd' || !strcmp(longopt, "dump")) {
        dump_memory++;
        break;
      }
      if(opt == 'T' || !strcmp(longopt, "trace")) {
        trace_memory++;
        break;
      }
      if(!strcmp(longopt, "std")) {
        load_std = 1;
        break;
      }
      if(!strcmp(longopt, "unhandled-rejection")) {
        dump_unhandled_promise_rejection = 1;
        break;
      }
#ifdef CONFIG_BIGNUM
      if(!strcmp(longopt, "no-bignum")) {
        bignum_ext = 0;
        break;
      }
      if(!strcmp(longopt, "bignum")) {
        bignum_ext = 1;
        break;
      }
      if(!strcmp(longopt, "qjscalc")) {
        load_jscalc = 1;
        break;
      }
#endif
      if(opt == 'q' || !strcmp(longopt, "quit")) {
        empty_run++;
        break;
      }
      if(!strcmp(longopt, "memory-limit")) {
        if(optind >= argc) {
          fprintf(stderr, "expecting memory limit");
          exit(1);
        }
        memory_limit = (size_t)strtod(argv[optind++], NULL);
        break;
      }
      if(!strcmp(longopt, "stack-size")) {
        if(optind >= argc) {
          fprintf(stderr, "expecting stack size");
          exit(1);
        }
        stack_size = (size_t)strtod(argv[optind++], NULL);
        break;
      }
      if(opt) {
        fprintf(stderr, "%s: unknown option '-%c'\n", exename, opt);
      } else {
        fprintf(stderr, "%s: unknown option '--%s'\n", exename, longopt);
      }
      jsm_help();
    }
    optind++;
  }

  {
    const char* modules;

    if((modules = getenv("DEBUG"))) {
      size_t i, len;
      for(i = 0; modules[i]; i += len) {
        len = str_chr(&modules[i], ',');
        vector_putptr(&module_debug, str_ndup(&modules[i], len));

        if(modules[i + len] == ',')
          len++;
      }

      if(vector_finds(&module_debug, "modules") != -1)
        debug_module_loader = TRUE;
    }
  }

  if(load_jscalc)
    bignum_ext = 1;

  if(trace_memory) {
    jsm_trace_malloc_init(&trace_data);
    rt = JS_NewRuntime2(&trace_mf, &trace_data);
  } else {
    rt = JS_NewRuntime();
  }
  if(!rt) {
    fprintf(stderr, "%s: cannot allocate JS runtime\n", exename);
    exit(2);
  }

  JS_SetModuleLoaderFunc(rt, 0, jsm_module_loader_path, 0);

  if(memory_limit != 0)
    JS_SetMemoryLimit(rt, memory_limit);
  // if (stack_size != 0)
  JS_SetMaxStackSize(rt, stack_size != 0 ? stack_size : 256 * 1048576);

  js_std_set_worker_new_context_func(jsm_context_new);

  js_std_init_handlers(rt);
  ctx = jsm_context_new(rt);
  if(!ctx) {
    fprintf(stderr, "%s: cannot allocate JS context\n", exename);
    exit(2);
  }

  /* loader for ES6 modules */
  JS_SetModuleLoaderFunc(rt, jsm_normalize_module, jsm_module_loader_path, NULL);

  if(dump_unhandled_promise_rejection) {
    JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker, NULL);
  }

  if(!empty_run) {
#ifdef CONFIG_BIGNUM
    if(load_jscalc) {
      js_eval_binary(ctx, qjsc_qjscalc, qjsc_qjscalc_size, 0);
    }
#endif
    js_std_add_helpers(ctx, argc - optind, argv + optind);

    int num_native, num_compiled;

#define jsm_builtin_native(name) vector_putptr(&builtins, #name)

    jsm_builtin_native(std);
    jsm_builtin_native(os);
    jsm_builtin_native(child_process);
    jsm_builtin_native(deep);
    jsm_builtin_native(inspect);
    jsm_builtin_native(lexer);
    jsm_builtin_native(misc);
    jsm_builtin_native(mmap);
    jsm_builtin_native(path);
    jsm_builtin_native(pointer);
    jsm_builtin_native(predicate);
    jsm_builtin_native(repeater);
    jsm_builtin_native(tree_walker);
    jsm_builtin_native(xml);
    num_native = vector_size(&builtins, sizeof(char*));

    // printf("native builtins: "); dump_vector(&builtins, 0);

#define jsm_builtin_compiled(name)                                                                                                                                                                     \
  js_eval_binary(ctx, qjsc_##name, qjsc_##name##_size, 0);                                                                                                                                             \
  vector_putptr(&builtins, #name)

    jsm_builtin_compiled(console);
    jsm_builtin_compiled(events);
    jsm_builtin_compiled(fs);
    jsm_builtin_compiled(perf_hooks);
    jsm_builtin_compiled(process);
    // jsm_builtin_compiled(repl);
    jsm_builtin_compiled(require);
    jsm_builtin_compiled(tty);
    jsm_builtin_compiled(util);

    num_compiled = vector_size(&builtins, sizeof(char*)) - num_native;

    {
      const char* str = "import process from 'process';\nglobalThis.process = process;\n";
      js_eval_str(ctx, str, "<input>", JS_EVAL_TYPE_MODULE);
    }

    JS_SetPropertyFunctionList(ctx, JS_GetGlobalObject(ctx), jsm_global_funcs, countof(jsm_global_funcs));
    if(load_std) {
      const char* str = "import * as std from 'std';\nimport * as os from 'os';\nglobalThis.std = "
                        "std;\nglobalThis.os "
                        "= os;\nglobalThis.setTimeout = os.setTimeout;\nglobalThis.clearTimeout = "
                        "os.clearTimeout;\n";
      js_eval_str(ctx, str, "<input>", JS_EVAL_TYPE_MODULE);
    }

    // jsm_list_modules(ctx);

    {
      char** name;
      JSModuleDef* m;
      vector_foreach_t(&module_list, name) {
        if(!(m = js_load_module(ctx, *name))) {
          fprintf(stderr, "error loading module '%s'\n", *name);
          exit(1);
        }
        free(*name);
      }
      vector_free(&module_list);
    }

    for(i = 0; i < include_count; i++) {
      if(jsm_load_script(ctx, include_list[i], module) == -1)
        goto fail;
    }

    if(expr) {
      if(js_eval_str(ctx, expr, "<cmdline>", 0) == -1)
        goto fail;
    } else if(optind >= argc) {
      /* interactive mode */
      interactive = 1;
    } else {
      const char* filename;
      filename = argv[optind];
      if(jsm_load_script(ctx, filename, module) == -1) {
        js_value_fwrite(ctx, JS_GetException(ctx), stderr);
        goto fail;
      }
    }
    if(interactive) {
      const char* str = "import REPL from 'repl'; globalThis.repl = new REPL('qjsm').runSync();\n";
      js_eval_binary(ctx, qjsc_repl, qjsc_repl_size, 0);
      js_eval_str(ctx, str, "<input>", JS_EVAL_TYPE_MODULE);
    }

    js_std_loop(ctx);
  }

  {

    JSValue exception = JS_GetException(ctx);

    if(!JS_IsNull(exception)) {
      js_std_dump_error(ctx);
    }
  }

  if(dump_memory) {
    JSMemoryUsage stats;
    JS_ComputeMemoryUsage(rt, &stats);
    JS_DumpMemoryUsage(stdout, &stats, rt);
  }
  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);

  if(empty_run && dump_memory) {
    clock_t t[5];
    double best[5];
    int i, j;
    for(i = 0; i < 100; i++) {
      t[0] = clock();
      rt = JS_NewRuntime();
      t[1] = clock();
      ctx = JS_NewContext(rt);
      t[2] = clock();
      JS_FreeContext(ctx);
      t[3] = clock();
      JS_FreeRuntime(rt);
      t[4] = clock();
      for(j = 4; j > 0; j--) {
        double ms = 1000.0 * (t[j] - t[j - 1]) / CLOCKS_PER_SEC;
        if(i == 0 || best[j] > ms)
          best[j] = ms;
      }
    }
    printf("\nInstantiation times (ms): %.3f = %.3f+%.3f+%.3f+%.3f\n", best[1] + best[2] + best[3] + best[4], best[1], best[2], best[3], best[4]);
  }
  return 0;
fail:
  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return 1;
}
