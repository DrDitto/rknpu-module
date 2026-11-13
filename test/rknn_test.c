#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>

/* Definiciones de tipos para las funciones clave del runtime */
typedef int (*rknn_init_func)(void **ctx, void *model, uint32_t size, uint32_t flag, void *ext_context);
typedef int (*rknn_destroy_func)(void *ctx);

int main() {
    printf("[INIT] Cargando librknnrt.so local...\n");

    /* 1. Cargar la biblioteca compartida */
    void *handle = dlopen("./librknnrt.so", RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "[ERROR] No se pudo cargar librknnrt.so: %s\n", dlerror());
        return 1;
    }

    /* 2. Resolver símbolos */
    rknn_init_func rknn_init = (rknn_init_func)dlsym(handle, "rknn_init");
    rknn_destroy_func rknn_destroy = (rknn_destroy_func)dlsym(handle, "rknn_destroy");

    if (!rknn_init || !rknn_destroy) {
        fprintf(stderr, "[ERROR] No se pudieron resolver los símbolos en librknnrt.so: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    printf("[RKNN] Símbolos resueltos con éxito.\n");

    /* 3. Inicializar contexto de la NPU */
    void *ctx = NULL;
    int ret = rknn_init(&ctx, NULL, 0, 0, NULL);
    printf("[RKNN] rknn_init ejecutado -> Retorno: %d, Contexto: %p\n", ret, ctx);

    /* 4. Limpieza */
    if (ctx) {
        rknn_destroy(ctx);
        printf("[RKNN] Contexto destruido correctamente.\n");
    }

    dlclose(handle);
    printf("\nPrueba de runtime finalizada.\n");
    return 0;
}
