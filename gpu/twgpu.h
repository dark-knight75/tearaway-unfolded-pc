/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef TWGPU_H
#define TWGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} TwGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void twgpu_register_kernel(void);
/* Co-op: the game folder and param.sfo identity for the network and PSN libraries. */
void twnet_configure(const char *app0, const char *serial, const char *title, const char *app_ver);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int twgpu_init(const TwGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t twgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int twgpu_handle_fault(void *ucontext, void *address);
/* TW_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void twgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int twgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int twgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int twgpu_overlay_captures_input(void);
/* Mouse controls: look=1 captures the cursor while the game window has the focus (relative
 * mode). sample returns the motion (pixels) and wheel steps since the last call and the held
 * buttons (1 left, 2 right, 4 middle, 8 X1, 16 X2); 1 while the cursor is captured. */
void twgpu_mouse_configure(int look);
/* Heading (degrees, about the vertical axis) of the camera the game last rendered with and how old
 * it is; 0 before the first frame with a known camera. */
int twgpu_camera_yaw(float *yaw_degrees, double *age_seconds);
int twgpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned twgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
