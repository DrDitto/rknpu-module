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

#define RKNPU_MEM_NON_CONTIGUOUS  (1 << 1)
#define RKNPU_MEM_WRITE_COMBINE   (1 << 2)
#define RKNPU_MEM_KERNEL_MAPPING  (1 << 4)
#define RKNPU_JOB_PC              (1 << 0)

struct rknpu_version {
    uint32_t major;
    uint32_t minor;
    uint32_t patchlevel;
};

struct rknpu_mem_create {
    unsigned int handle;
    unsigned int flags;
    unsigned int size;
    unsigned int sram_size;
    int iommu_domain_id;
    unsigned int core_mask;
    unsigned long long obj_addr;
    unsigned long long dma_addr;
} __attribute__((aligned(8)));

struct rknpu_mem_map {
    unsigned int handle;
    unsigned int pad;
    unsigned long long offset;
} __attribute__((aligned(8)));

struct rknpu_submit {
    unsigned int flags;
    unsigned int timeout;
    unsigned int task_start;
    unsigned int task_number;
    unsigned int task_counter;
    unsigned int core_mask;
    unsigned int priority;
    unsigned int fmt;
    int iommu_domain_id;
    unsigned int pad;
    unsigned long long task_base_addr;
    unsigned long long task_obj_addr;
    int fence_fd;
    unsigned int subcore_task[2];
    unsigned long long reserved[2];
} __attribute__((aligned(8)));

struct rknpu_task {
    unsigned int flags;
    unsigned int size;
    unsigned long long regcmd_addr;
    unsigned long long regcfg_addr;
    unsigned int regcfg_amount;
    unsigned int int_mask;
} __attribute__((aligned(8)));

#define DRM_RKNPU_GET_VERSION 0x00
#define DRM_RKNPU_MEM_CREATE  0x02
#define DRM_RKNPU_MEM_MAP     0x03
#define DRM_RKNPU_SUBMIT      0x01

#define DRM_RKNPU_VERSION_CMD    DRM_IOWR(DRM_COMMAND_BASE + DRM_RKNPU_GET_VERSION, struct rknpu_version)
#define DRM_RKNPU_MEM_CREATE_CMD DRM_IOWR(DRM_COMMAND_BASE + DRM_RKNPU_MEM_CREATE, struct rknpu_mem_create)
#define DRM_RKNPU_MEM_MAP_CMD    DRM_IOWR(DRM_COMMAND_BASE + DRM_RKNPU_MEM_MAP, struct rknpu_mem_map)
#define DRM_RKNPU_SUBMIT_CMD     DRM_IOWR(DRM_COMMAND_BASE + DRM_RKNPU_SUBMIT, struct rknpu_submit)

int main() {
    int fd = open("/dev/dri/renderD129", O_RDWR);
    if (fd < 0) {
        perror("Error abriendo /dev/dri/renderD129");
        return 1;
    }
    printf("[INIT] ¡Éxito! renderD129 abierto correctamente (fd = %d)\n", fd);

    /* 1. Creación de memoria GEM con mapeo de kernel obligatorio */
    struct rknpu_mem_create mem_arg;
    memset(&mem_arg, 0, sizeof(mem_arg));
    
    mem_arg.size = 65536; /* 64 KB */
    mem_arg.flags = RKNPU_MEM_NON_CONTIGUOUS | RKNPU_MEM_WRITE_COMBINE | RKNPU_MEM_KERNEL_MAPPING;
    mem_arg.core_mask = 0;
    mem_arg.iommu_domain_id = 0;

    if (ioctl(fd, DRM_RKNPU_MEM_CREATE_CMD, &mem_arg) != 0) {
        perror("[GEM] Fallo al crear memoria GEM");
        close(fd);
        return 1;
    }
    printf("[GEM] Buffer creado -> Handle: 0x%x, Dir DMA: 0x%llx, Obj Kernel: 0x%llx\n", 
           mem_arg.handle, (unsigned long long)mem_arg.dma_addr, (unsigned long long)mem_arg.obj_addr);

    /* 2. Mapeo de memoria con mmap para espacio de usuario */
    struct rknpu_mem_map map_arg;
    memset(&map_arg, 0, sizeof(map_arg));
    map_arg.handle = mem_arg.handle;

    if (ioctl(fd, DRM_RKNPU_MEM_MAP_CMD, &map_arg) != 0) {
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

    /* 3. Escribir estructura de tarea y stream de comandos */
    struct rknpu_task *task = (struct rknpu_task *)mapped_ptr;
    task->flags = 0;
    task->size = sizeof(struct rknpu_task);
    task->regcmd_addr = mem_arg.dma_addr + 1024;
    task->regcfg_amount = 0;
    task->int_mask = 0x3;

    uint32_t *cmd_stream = (uint32_t *)((char *)mapped_ptr + 1024);
    cmd_stream[0] = 0x00000000;
    cmd_stream[1] = 0x00000000;

    printf("[TASK] Metadatos y stream escritos en el búfer GEM.\n");

    /* 4. Envío de Job Submit */
    struct rknpu_submit submit_arg;
    memset(&submit_arg, 0, sizeof(submit_arg));

    submit_arg.flags = RKNPU_JOB_PC;
    submit_arg.timeout = 1000;
    submit_arg.task_start = 0;
    submit_arg.task_number = 1;
    submit_arg.task_counter = 1;
    submit_arg.core_mask = 1;
    submit_arg.iommu_domain_id = 0;
    submit_arg.task_base_addr = mem_arg.dma_addr;
    submit_arg.task_obj_addr = mem_arg.obj_addr;

    if (ioctl(fd, DRM_RKNPU_SUBMIT_CMD, &submit_arg) == 0) {
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
