#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "rknpu_ioctl.h"

int main() {
    int fd = open("/dev/dri/renderD129", O_RDWR);
    if (fd < 0) {
        perror("Error abriendo /dev/dri/renderD129");
        return 1;
    }
    printf("[INIT] ¡Éxito! renderD129 abierto correctamente (fd = %d)\n", fd);

    /* 1. Creación de memoria GEM con DRM_IOCTL_RKNPU_MEM_CREATE */
    struct rknpu_mem_create mem_arg;
    memset(&mem_arg, 0, sizeof(mem_arg));
    
    mem_arg.size = 65536; /* 64 KB */
    mem_arg.flags = RKNPU_MEM_NON_CONTIGUOUS | RKNPU_MEM_WRITE_COMBINE | RKNPU_MEM_KERNEL_MAPPING | RKNPU_MEM_IOMMU;
    mem_arg.core_mask = 0;
    mem_arg.iommu_domain_id = 0;

    if (ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, &mem_arg) != 0) {
        perror("[GEM] Fallo al crear memoria GEM");
        close(fd);
        return 1;
    }

    /* Respaldo: si dma_addr es 0, usamos obj_addr como base DMA */
    if (mem_arg.dma_addr == 0 && mem_arg.obj_addr != 0) {
        mem_arg.dma_addr = mem_arg.obj_addr;
        printf("[GEM-FIX] dma_addr ajustado mediante obj_addr: 0x%llx\n", (unsigned long long)mem_arg.dma_addr);
    }

    printf("[GEM] Buffer creado -> Handle: 0x%x, Dir DMA: 0x%llx, Obj Kernel: 0x%llx\n", 
           mem_arg.handle, (unsigned long long)mem_arg.dma_addr, (unsigned long long)mem_arg.obj_addr);

    if (mem_arg.dma_addr == 0) {
        fprintf(stderr, "[ERROR] La dirección DMA sigue siendo 0x0.\n");
        close(fd);
        return 1;
    }

    /* 2. Mapeo de memoria con mmap para espacio de usuario */
    struct rknpu_mem_map map_arg;
    memset(&map_arg, 0, sizeof(map_arg));
    map_arg.handle = mem_arg.handle;

    if (ioctl(fd, DRM_IOCTL_RKNPU_MEM_MAP, &map_arg) != 0) {
        perror("[MAP] Fallo al obtener offset de mapeo GEM");
        close(fd);
        return 1;
    }

    void *mapped_ptr = mmap(NULL, mem_arg.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_arg.offset);
    if (mapped_ptr == MAP_FAILED) {
        perror("[MAP] Fallo en mmap del buffer GEM");
        close(fd);
        return 1;
    }

    /* 3. Escribir estructura de tarea en el inicio y comandos en un offset seguro (256 bytes) */
    struct rknpu_task *task = (struct rknpu_task *)mapped_ptr;
    memset(task, 0, sizeof(struct rknpu_task));
    task->flags = 0;
    task->op_idx = 0;
    task->enable_mask = 0x1;
    task->int_mask = 0x3;
    task->int_clear = RKNPU_INT_CLEAR;
    task->int_status = 0;
    task->regcfg_amount = 0;
    task->regcfg_offset = 0;
    task->regcmd_addr = mem_arg.dma_addr + 256; // Comando separado de la estructura de tarea

    uint32_t *cmd_stream = (uint32_t *)((char *)mapped_ptr + 256);
    cmd_stream[0] = 0x00000000; // NOP / Fin de instrucciones

    printf("[TASK] Estructura de tarea y stream separados correctamente en el búfer GEM.\n");

    /* 4. Envío de Job Submit usando DRM_IOCTL_RKNPU_SUBMIT */
    struct rknpu_submit submit_arg;
    memset(&submit_arg, 0, sizeof(submit_arg));

    submit_arg.flags = RKNPU_JOB_PC;
    submit_arg.timeout = 1000;
    submit_arg.task_start = 0;
    submit_arg.task_number = 1;
    submit_arg.task_counter = 1;
    submit_arg.priority = 0;
    submit_arg.core_mask = 1;
    submit_arg.iommu_domain_id = 0;
    submit_arg.task_base_addr = mem_arg.dma_addr;
    submit_arg.task_obj_addr = 0;

    if (ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &submit_arg) == 0) {
        printf("[SUBMIT] ¡Job enviado y procesado con éxito por el driver!\n");
    } else {
        perror("[SUBMIT] Error en la ejecución del Job Submit");
    }

    /* 5. Limpieza */
    munmap(mapped_ptr, mem_arg.size);

    struct drm_gem_close close_arg;
    memset(&close_arg, 0, sizeof(close_arg));
    close_arg.handle = mem_arg.handle;
    ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);

    close(fd);
    printf("\nPrueba completa finalizada.\n");
    return 0;
}
