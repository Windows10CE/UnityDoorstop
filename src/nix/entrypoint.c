#include "../bootstrap.h"
#include "../config/config.h"
#include "../crt.h"
#include "../util/logging.h"
#include "../util/paths.h"
#include "../util/util.h"
#include "./plthook/plthook.h"

void capture_mono_path(void *handle) {
    char_t *result;
    get_module_path(handle, &result, NULL, 0);
    setenv("DOORSTOP_RUNTIME_LIB_PATH", result, TRUE);
}

static bool_t initialized = FALSE;
void *dlsym_hook(void *handle, const char *name) {
#define REDIRECT_INIT(init_name, init_func, target, extra_init)                \
    if (!strcmp(name, init_name)) {                                            \
        if (!initialized) {                                                    \
            initialized = TRUE;                                                \
            init_func(handle);                                                 \
            extra_init;                                                        \
        }                                                                      \
        return (void *)target;                                                 \
    }

    REDIRECT_INIT("il2cpp_init", load_il2cpp_funcs, init_il2cpp, {});
    REDIRECT_INIT("mono_jit_init_version", load_mono_funcs, init_mono,
                  capture_mono_path(handle));
    REDIRECT_INIT("mono_image_open_from_data_with_name", load_mono_funcs,
                  hook_mono_image_open_from_data_with_name,
                  capture_mono_path(handle));
    REDIRECT_INIT("mono_jit_parse_options", load_mono_funcs,
                  hook_mono_jit_parse_options, capture_mono_path(handle));

#undef REDIRECT_INIT
    return dlsym(handle, name);
}

int fclose_hook(FILE *stream) {
    // Some versions of Unity wrongly close stdout, which prevents writing
    // to console
    if (stream == stdout)
        return F_OK;
    return fclose(stream);
}

__attribute__((constructor)) void doorstop_ctor() {
    init_logger();
    load_config();

    if (!config.enabled) {
        LOG("Doorstop not enabled! Skipping!\n");
        return;
    }

    plthook_t *hook;

    void *unity_player = plthook_handle_by_name("UnityPlayer");

#if defined(__linux__)
    if (!unity_player) {
        unity_player = dlopen("UnityPlayer.so", RTLD_LAZY);
    }
#endif

    if (unity_player && plthook_open_by_handle(&hook, unity_player) == 0) {
        LOG("Found UnityPlayer, hooking into it instead\n");
    } else if (plthook_open(&hook, NULL) != 0) {
        LOG("Failed to open current process PLT! Cannot run Doorstop! "
            "Error: "
            "%s\n",
            plthook_error());
        return;
    }

    if (plthook_replace(hook, "dlsym", &dlsym_hook, NULL) != 0)
        printf("Failed to hook dlsym, ignoring it. Error: %s\n",
               plthook_error());

    if (plthook_replace(hook, "fclose", &fclose_hook, NULL) != 0)
        printf("Failed to hook fclose, ignoring it. Error: %s\n",
               plthook_error());

#if defined(__APPLE__)
    /*
        On older Unity versions, Mono methods are resolved by the OS's
       loader directly. Because of this, there is no dlsym, in which case we
       need to apply a PLT hook.
    */
    void *mono_handle = plthook_handle_by_name("libmono");

    if (plthook_replace(hook, "mono_jit_init_version", &init_mono, NULL) != 0)
        printf("Failed to hook jit_init_version, ignoring it. Error: %s\n",
               plthook_error());
    else if (mono_handle)
        load_mono_funcs(mono_handle);
#endif

    plthook_close(hook);
}