#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

/* Definiciones de tipos para las funciones del runtime de Rockchip */
typedef int (*rknn_init_func)(int *ctx, void *model, uint32_t size, uint32_t flag, void *ext);
typedef int (*rknn_run_func)(int ctx, void *ext);
typedef int (*rknn_destroy_func)(int ctx);

int main() {
    printf("[INIT] Cargando librknnrt.so local...\n");
    void *handle = dlopen("./librknnrt.so", RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "[ERROR] No se pudo cargar librknnrt.so: %s\n", dlerror());
        return 1;
    }

    rknn_init_func rknn_init = (rknn_init_func)dlsym(handle, "rknn_init");
    rknn_run_func rknn_run = (rknn_run_func)dlsym(handle, "rknn_run");
    rknn_destroy_func rknn_destroy = (rknn_destroy_func)dlsym(handle, "rknn_destroy");

    if (!rknn_init || !rknn_run || !rknn_destroy) {
        fprintf(stderr, "[ERROR] No se pudieron resolver los símbolos necesarios: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    /* 1. Cargar el modelo mobilenet.rknn en memoria */
    const char *model_path = "./mobilenet.rknn";
    FILE *fp = fopen(model_path, "rb");
    if (!fp) {
        perror("[ERROR] No se pudo abrir mobilenet.rknn");
        dlclose(handle);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    int model_size = ftell(fp);
    rewind(fp);

    void *model_data = malloc(model_size);
    if (!model_data) {
        fprintf(stderr, "[ERROR] Fallo de memoria al asignar el modelo\n");
        fclose(fp);
        dlclose(handle);
        return 1;
    }

    int read_bytes = fread(model_data, 1, model_size, fp);
    fclose(fp);

    if (read_bytes != model_size) {
        fprintf(stderr, "[ERROR] Error leyendo el archivo del modelo completo\n");
        free(model_data);
        dlclose(handle);
        return 1;
    }
    printf("[MODEL] Modelo cargado: %s (%d bytes)\n", model_path, model_size);

    /* 2. Inicializar el contexto de la NPU */
    int ctx = 0;
    int ret = rknn_init(&ctx, model_data, model_size, 0, NULL);
    if (ret < 0) {
        fprintf(stderr, "[ERROR] rknn_init falló con código: %d\n", ret);
        free(model_data);
        dlclose(handle);
        return 1;
    }
    printf("[SUCCESS] Contexto de NPU inicializado correctamente. ID: %d\n", ctx);

    /* 3. Ejecutar la inferencia (rknn_run) */
    printf("[RUN] Disparando rknn_run en la NPU...\n");
    ret = rknn_run(ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "[ERROR] rknn_run falló con código: %d\n", ret);
    } else {
        printf("[SUCCESS] ¡Inferencia completada con éxito mediante rknn_run!\n");
    }

    /* 4. Limpieza de recursos */
    rknn_destroy(ctx);
    free(model_data);
    dlclose(handle);
    printf("\nPrueba de inferencia finalizada.\n");
    return 0;
}
