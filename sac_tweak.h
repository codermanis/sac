/* sac_tweak - v0.1 - public domain - no warranty implied; use at your own risk

    Live hot-reload of variables for C/C++

    Supported Platforms:
     - Linux (depends on pthread)

    Do this:
       #define SAC_TWEAK_IMPLEMENTATION
    before you include this file in *one* C or C++ file to create the implementation.

    3 supported value types:
      TWEAK(int, sec) = 1;
      TWEAK(char_ptr, who) = "world";
      TWEAK(float, value) = 2500;

    TBD:
      - other platforms
      - optionnal threading
      - remove char_ptr hack
*/
typedef const char* char_ptr;

#ifndef TWEAK

#ifdef __cplusplus
extern "C" {
#endif

/* Public API */
/* Supported types of variable. Use one of these as the first argument to TWEAK */
enum __sac_tweak_type {
    type_int,
    type_float,
    type_char_ptr
};
/* Possible usages:
       TWEAK(int, my_int) = 3;
       TWEAK(float, my_float_var) = 1.2f;
       TWEAK(char_ptr, my_string) = "hello";
    Note: use char_ptr because char* will break macro expansions.
*/
#define TWEAK(T, name) static T __SAC_UNIQUE_NAME; static int __SAC_UNIQUE_ID = __sac_tweak_init(#name, __FILE__, type_##T); T name = __sac_tweak_##T(#name, __FILE__, __SAC_UNIQUE_ID, __SAC_UNIQUE_NAME); __SAC_UNIQUE_NAME



/* Private macro foo */
#define __SAC_MERGE_(a,b)  a##b
#define __SAC_LABEL_(a, b) __SAC_MERGE_(a, b)
#define __SAC_UNIQUE_NAME __SAC_LABEL_(__SAC_unique_name_, __LINE__)
#define __SAC_UNIQUE_ID __SAC_LABEL_(id_, __LINE__)



extern int __sac_tweak_init(const char* name, const char* file, enum __sac_tweak_type type);

extern float __sac_tweak_float(const char* name, const char* file, int id, float);
extern int   __sac_tweak_int(const char* name, const char* file, int id, int);
extern char* __sac_tweak_char_ptr(const char* name, const char* file, int id, const char*);


#ifdef SAC_TWEAK_IMPLEMENTATION

#include <sys/inotify.h>
#include <string.h>
#include <unistd.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>


union __sac_tweak_value {
    int i;
    float f;
    char* s;
};

struct __sac_tweak_datas {
    int watch;
    const char* name;
    const char* filename;
    enum __sac_tweak_type type;
    union __sac_tweak_value {
        int i;
        float f;
        char* s;
    } value;
};

#define __SAC_MAX_TWEAKS 1024
static struct __sac_global_datas {
    pthread_t th;
    int fd;
    struct __sac_tweak_datas tweaks[__SAC_MAX_TWEAKS];
    int tweak_count;
    int self_pipe[2];
}* datas;

static pthread_mutex_t __sac_fastmutex = PTHREAD_MUTEX_INITIALIZER;

static char* __sac_remove_suffix_spaces(char* text) {
    while(*text == ' ' || *text == '\t') text--;
    return text;
}
static char* __sac_remove_prefix_spaces(char* text) {
    while(*text == ' ' || *text == '\t') text++;
    return text;
}

static const char* __sac_read_value_from_file(const char* name, const char* file) {
    char* content;

    FILE* f = fopen(file, "r");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    int content_size = ftell(f);
    content = (char*)malloc(content_size + 1);
    if (content == NULL) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    fread(content, 1, content_size, f);
    content[content_size] = '\0';

    char* tweak_start = content;
    char* result = NULL;
    while ((tweak_start = strstr(tweak_start, "TWEAK"))) {
        tweak_start++;

        /* next '(' */
        char* paren_start = strchr(tweak_start, '(');
        if (!paren_start) {
            continue;
        }

        /* next ')' */
        char* paren_end = strchr(paren_start, ')');
        if (!paren_end) {
            continue;
        }
        paren_end = __sac_remove_suffix_spaces(paren_end - 1);

        char* comma = (char*)memchr(paren_start + 1, ',', paren_end - paren_start + 1);

        if (!comma) {
            continue;
        }
        char* parsed_name = __sac_remove_prefix_spaces(comma + 1);

        /* is correct id? */
        if (strncmp(parsed_name, name, strlen(name)) == 0) {
            /* value is between next = and ; */
            char* next_equal = strchr(paren_end, '=');
            if (!next_equal) {
                continue;
            }
            char* next_semi_colon = strchr(next_equal, ';');
            if (!next_semi_colon) {
                continue;
            }
            next_equal++;
            next_semi_colon--;
            int length = next_semi_colon - next_equal + 1;
            result = strndup(next_equal, length);
            break;
        }
    }

    fclose(f);
    free(content);
    return result;
}

static void __sac_update_tweak(int id) {
    const char* raw = __sac_read_value_from_file(datas->tweaks[id].name, datas->tweaks[id].filename);
    if (raw) {
        switch (datas->tweaks[id].type) {
            case type_int: {
                char* endptr = NULL;
                long val = strtol(raw, &endptr, 0);
                if (endptr != raw) {
                    datas->tweaks[id].value.i = (int)val;
                }
            } break;
            case type_float: {
                char* endptr = NULL;
                float val = strtof(raw, &endptr);
                if (endptr != raw) {
                    datas->tweaks[id].value.f = val;
                }
            } break;
            case type_char_ptr:
                const char* start = strchr(raw, '"');
                datas->tweaks[id].value.s = strndup(start + 1, strlen(start) - 2);
                break;
        }
    }
}

static void* __sac_update_loop(void*);

static bool __sac_tweak_global_init() {
    datas = (struct __sac_global_datas*) malloc(sizeof(struct __sac_global_datas));
    if (datas == NULL) {
        goto error_malloc;
    }
    datas->fd = inotify_init();
    if (datas->fd == -1) {
        goto error_inotify;
    }
    datas->tweak_count = 0;

    if (pipe2(datas->self_pipe, O_NONBLOCK) != 0) {
        goto error_pipe;
    }

    if (pthread_create(&datas->th, NULL, __sac_update_loop, NULL) != 0) {
        goto error_thread;
    }
    pthread_detach(datas->th);

    return true;

error_thread:
    close(datas->self_pipe[0]);
    close(datas->self_pipe[1]);
error_pipe:
    close(datas->fd);
error_inotify:
    free (datas);
error_malloc:
    return false;

}

int __sac_tweak_init(const char* name, const char* file, enum __sac_tweak_type type) {
    pthread_mutex_lock(&__sac_fastmutex);
    if (!datas) {
        if (!__sac_tweak_global_init()) {
            pthread_mutex_unlock(&__sac_fastmutex);
            return 0;
        }
    }
    /* unblock select */
    write(datas->self_pipe[1], "a", 1);

    int id = datas->tweak_count++;
    if (id == __SAC_MAX_TWEAKS) {
        pthread_mutex_unlock(&__sac_fastmutex);
        return 0;
    }
    datas->tweaks[id].watch = inotify_add_watch(datas->fd, file, IN_CLOSE_WRITE);
    datas->tweaks[id].name = name;
    datas->tweaks[id].filename = file;
    datas->tweaks[id].type = type;

    __sac_update_tweak(id);
    pthread_mutex_unlock(&__sac_fastmutex);
    return id;
}

static void* __sac_update_loop(void*) {
    fd_set fds;
    int nfds;
    char buffer[sizeof(struct inotify_event)];

    nfds = 1 + ((datas->self_pipe[0] > datas->fd) ? datas->self_pipe[0] : datas->fd);

    while (1) {

        FD_ZERO(&fds);
        FD_SET(datas->fd, &fds);
        FD_SET(datas->self_pipe[0], &fds);

        int modified = select(nfds, &fds, NULL, NULL, NULL);
        pthread_mutex_lock(&__sac_fastmutex);
        if (FD_ISSET(datas->self_pipe[0], &fds)) {
            while (read(datas->self_pipe[0], buffer, 1) > 0) { }
            modified--;
        }
        for (int i=0; i<modified; i++) {
            if (read(datas->fd, buffer, sizeof(struct inotify_event)) > 0) {
                struct inotify_event *event = (struct inotify_event *) buffer;

                for (int j=0; j<datas->tweak_count; j++) {
                    if (datas->tweaks[j].watch == event->wd) {
                        __sac_update_tweak(j);
                    }
                }

            }
        }
        pthread_mutex_unlock(&__sac_fastmutex);
    }
}

float __sac_tweak_float(const char* name, const char* file, int id, float) {
    pthread_mutex_lock(&__sac_fastmutex);
    float result = datas->tweaks[id].value.f;
    pthread_mutex_unlock(&__sac_fastmutex);
    return result;
}

int __sac_tweak_int(const char* name, const char* file, int id, int) {
    pthread_mutex_lock(&__sac_fastmutex);
    int result = datas->tweaks[id].value.i;
    pthread_mutex_unlock(&__sac_fastmutex);
    return result;
}

char* __sac_tweak_char_ptr(const char* name, const char* file, int id, const char*) {
    pthread_mutex_lock(&__sac_fastmutex);
    char* result = datas->tweaks[id].value.s;
    pthread_mutex_unlock(&__sac_fastmutex);
    return result;
}

#endif /* SAC_TWEAK_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* TWEAK */