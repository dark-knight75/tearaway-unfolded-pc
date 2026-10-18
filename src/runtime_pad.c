/* libScePad on SDL3 gamepads, keyboard and mouse. SDL events are pumped by the window thread
 * (gpu/shim/window.cpp); here state is only sampled. The controller, keyboard and mouse work
 * together; their bindings come from input.ini (TW_INPUT_CONFIG, written by the launcher):
 *
 *   key.<action>=<key or mouse name>[|<second>]   SDL key names ("Space", "Left Shift", "W"),
 *       "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5", "Wheel Up",
 *       "Wheel Down"; actions: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad
 *       up down left right, ls_up ls_down ls_left ls_right, rs_up rs_down rs_left rs_right.
 *   pad.<action>=<controller button>   SDL names (a b x y leftshoulder rightshoulder
 *       lefttrigger righttrigger leftstick rightstick back start dpup dpdown dpleft dpright
 *       touchpad misc1 paddle1..paddle4) or "none", for the button actions above.
 *   mouse_look=1  mouse_sensitivity=1.0  mouse_invert_y=0  pad_invert_y=0
 *   motion=1  gyro_sensitivity=1.0   controller gyro/accelerometer (DS4, DualSense, Switch Pro)
 *   touch_sensitivity=1.0   touchpad pixels per mouse pixel while key.touch is held
 *
 * Defaults (no file): WASD move, mouse look, left button R2 (fire), right button L2 (aim),
 * Space Cross, C Circle, F Square, E Triangle, Q L1, G R1, Left Shift L3, V R3, Escape/Enter
 * Options, Tab touchpad, arrow keys and the wheel the d-pad, IJKL the right stick, middle mouse
 * held: a finger on the touchpad that the mouse drags (Tearaway's pokes and swipes). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/twgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) {
                ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad));
                /* SDL only reports sensors that are enabled. */
                SDL_SetGamepadSensorEnabled(gamepad,SDL_SENSOR_GYRO,true);
                SDL_SetGamepadSensorEnabled(gamepad,SDL_SENSOR_ACCEL,true);
            }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Degrees of camera turn per mouse pixel at sensitivity 1 (0.08 felt "very slow"; the
 * sensitivity goes from 0.1 to 10 times this). */
#define MOUSE_DEGREES_PER_PIXEL 0.15f

/* ---- bindings (input.ini) ---------------------------------------------------------------- */
enum { BIND_KEY=1, BIND_MOUSE, BIND_WHEEL };
typedef struct { uint8_t kind; int16_t code; } Bind;
typedef struct { const char *name; uint32_t ps; const char *keys; const char *pad; } Action;
enum { A_LS_UP=16, A_LS_DOWN, A_LS_LEFT, A_LS_RIGHT, A_RS_UP, A_RS_DOWN, A_RS_LEFT, A_RS_RIGHT, A_TOUCH, ACTIONS };
static const Action actions[ACTIONS]={
    {"cross",BTN_CROSS,"Space","a"}, {"circle",BTN_CIRCLE,"C","b"}, {"square",BTN_SQUARE,"F","x"},
    {"triangle",BTN_TRIANGLE,"E","y"}, {"l1",BTN_L1,"Q","leftshoulder"}, {"r1",BTN_R1,"G","rightshoulder"},
    {"l2",BTN_L2,"Mouse Right","lefttrigger"}, {"r2",BTN_R2,"Mouse Left","righttrigger"},
    {"l3",BTN_L3,"Left Shift","leftstick"}, {"r3",BTN_R3,"V","rightstick"},
    {"options",BTN_OPTIONS,"Escape|Return","start"}, {"touchpad",BTN_TOUCHPAD,"Tab","back"},
    {"up",BTN_UP,"Up","dpup"}, {"down",BTN_DOWN,"Down","dpdown"},
    {"left",BTN_LEFT,"Left|Wheel Up","dpleft"}, {"right",BTN_RIGHT,"Right|Wheel Down","dpright"},
    {"ls_up",0,"W",NULL}, {"ls_down",0,"S",NULL}, {"ls_left",0,"A",NULL}, {"ls_right",0,"D",NULL},
    {"rs_up",0,"I",NULL}, {"rs_down",0,"K",NULL}, {"rs_left",0,"J",NULL}, {"rs_right",0,"L",NULL},
    {"touch",0,"Mouse Middle",NULL},
};
#define PAD_TRIGGER_LEFT (-2)
#define PAD_TRIGGER_RIGHT (-3)
static struct {
    Bind keys[ACTIONS][2];
    int pad[16];               /* SDL_GamepadButton, PAD_TRIGGER_*, or -1 */
    int mouse_look, mouse_invert_y, pad_invert_y, motion;
    float mouse_sensitivity, gyro_sensitivity, touch_sensitivity;
} input;

static Bind parse_bind(const char *name) {
    static const struct { const char *name; uint8_t kind; int16_t code; } mouse[]={
        {"Mouse Left",BIND_MOUSE,1}, {"Mouse Right",BIND_MOUSE,2}, {"Mouse Middle",BIND_MOUSE,4},
        {"Mouse 4",BIND_MOUSE,8}, {"Mouse 5",BIND_MOUSE,16}, {"Wheel Up",BIND_WHEEL,1}, {"Wheel Down",BIND_WHEEL,-1},
    };
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i)
        if (!SDL_strcasecmp(name,mouse[i].name)) return (Bind){mouse[i].kind,mouse[i].code};
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    return code==SDL_SCANCODE_UNKNOWN ? (Bind){0,0} : (Bind){BIND_KEY,(int16_t)code};
}
static void set_keys(int action, const char *value) {
    char text[128];
    snprintf(text,sizeof(text),"%s",value);
    input.keys[action][0]=input.keys[action][1]=(Bind){0,0};
    char *second=strchr(text,'|');
    if (second) *second++=0;
    if (*text) input.keys[action][0]=parse_bind(text);
    if (second && *second) input.keys[action][1]=parse_bind(second);
}
static int parse_pad(const char *value) {
    if (!SDL_strcasecmp(value,"lefttrigger")) return PAD_TRIGGER_LEFT;
    if (!SDL_strcasecmp(value,"righttrigger")) return PAD_TRIGGER_RIGHT;
    const SDL_GamepadButton button=SDL_GetGamepadButtonFromString(value);
    return button==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)button;
}
static void load_input(void) {
    for (int a=0;a<ACTIONS;++a) {
        set_keys(a,actions[a].keys);
        if (a<16) input.pad[a]=parse_pad(actions[a].pad);
    }
    input.mouse_look=1; input.mouse_sensitivity=1.0f;
    input.motion=1; input.gyro_sensitivity=1.0f; input.touch_sensitivity=1.0f;
    const char *path=getenv("TW_INPUT_CONFIG");
    FILE *f=path && *path ? fopen(path,"r") : NULL;
    if (f) {
        char line[256];
        while (fgets(line,sizeof(line),f)) {
            line[strcspn(line,"\r\n")]=0;
            char *eq=strchr(line,'=');
            if (line[0]=='#' || !eq) continue;
            *eq=0;
            const char *key=line, *value=eq+1;
            if (!strcmp(key,"mouse_look")) input.mouse_look=atoi(value)!=0;
            else if (!strcmp(key,"mouse_sensitivity")) input.mouse_sensitivity=(float)atof(value);
            else if (!strcmp(key,"mouse_invert_y")) input.mouse_invert_y=atoi(value)!=0;
            else if (!strcmp(key,"pad_invert_y")) input.pad_invert_y=atoi(value)!=0;
            else if (!strcmp(key,"motion")) input.motion=atoi(value)!=0;
            else if (!strcmp(key,"gyro_sensitivity")) input.gyro_sensitivity=(float)atof(value);
            else if (!strcmp(key,"touch_sensitivity")) input.touch_sensitivity=(float)atof(value);
            else for (int a=0;a<ACTIONS;++a) {
                if (!strncmp(key,"key.",4) && !strcmp(key+4,actions[a].name)) set_keys(a,value);
                else if (a<16 && !strncmp(key,"pad.",4) && !strcmp(key+4,actions[a].name)) input.pad[a]=parse_pad(value);
            }
        }
        fclose(f);
        printf("Runtime: controls from %s (mouse look %s, sensitivity %.2f)\n",path,
               input.mouse_look ? "on" : "off",input.mouse_sensitivity);
    }
    if (input.mouse_sensitivity<=0.0f) input.mouse_sensitivity=1.0f;
    twgpu_mouse_configure(input.mouse_look);
}

/* Mouse motion as a right stick: the filtered speed (pixels per second) scaled to a deflection,
 * past the game's dead zone as soon as the mouse moves. */
static float mouse_vx, mouse_vy;
static uint64_t mouse_last_us;
static uint64_t wheel_until_us[2]; /* wheel up, wheel down: held briefly for the game to see */
/* A turn rate (degrees/s) as a stick byte, through the inverse of a typical third-person camera's
 * response below its speed ramp: about 240 x^2.5 degrees/s (x = 0.25: 7, 0.5: 37). */
static uint8_t mouse_axis(float rate) {
    const float r=rate<0 ? -rate : rate;
    if (r<0.5f) return 128;
    float out=SDL_powf(r/240.0f,0.4f);
    if (out>1.0f) out=1.0f;
    int x=(int)(128.0f+(rate<0 ? -out : out)*127.5f);
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static int bind_down(const Bind *b, const bool *k, uint32_t mouse, uint64_t now) {
    switch (b->kind) {
    case BIND_KEY: return k && k[b->code];
    case BIND_MOUSE: return (mouse & (uint32_t)b->code)!=0;
    case BIND_WHEEL: return now<wheel_until_us[b->code>0 ? 0 : 1];
    default: return 0;
    }
}
static int action_down(int a, const bool *k, uint32_t mouse, uint64_t now) {
    return bind_down(&input.keys[a][0],k,mouse,now) || bind_down(&input.keys[a][1],k,mouse,now);
}

/* Game callbacks (runtime_guest_callbacks): none for this game yet; the mouse moves the right stick. */
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count) {
    *count=0;
    return NULL;
}

/* ---- motion sensors -----------------------------------------------------------------------
 * SDL and libScePad share the axes (x right, y up, z toward the player); SDL gives m/s^2 and
 * rad/s, the pad G and rad/s. The orientation is the gyro integrated since the last
 * scePadResetOrientation.
 * ponytail: no tilt correction from the accelerometer, so the orientation drifts over minutes;
 * blend toward gravity (complementary filter) if a game holds a pose that long. */
static float orientation[4]={0,0,0,1}; /* x y z w */
static uint64_t orientation_us;
static void sample_motion(PadData *d, SDL_Gamepad *g, uint64_t now) {
    float accel[3], gyro[3];
    if (!input.motion || !g) { orientation_us=0; return; }
    if (SDL_GetGamepadSensorData(g,SDL_SENSOR_ACCEL,accel,3))
        for (int i=0;i<3;++i) d->acceleration[i]=accel[i]/SDL_STANDARD_GRAVITY;
    if (!SDL_GetGamepadSensorData(g,SDL_SENSOR_GYRO,gyro,3)) { orientation_us=0; return; }
    for (int i=0;i<3;++i) d->angular_velocity[i]=gyro[i]*input.gyro_sensitivity;
    if (orientation_us) {
        float dt=(float)(now-orientation_us)*1e-6f;
        if (dt>0.1f) dt=0.1f;
        const float *w=d->angular_velocity, x=orientation[0], y=orientation[1], z=orientation[2], q=orientation[3];
        const float h=0.5f*dt; /* q' = q + q*(w,0)*dt/2, body-frame rates */
        orientation[0]+=h*( q*w[0]+y*w[2]-z*w[1]);
        orientation[1]+=h*( q*w[1]+z*w[0]-x*w[2]);
        orientation[2]+=h*( q*w[2]+x*w[1]-y*w[0]);
        orientation[3]+=h*(-x*w[0]-y*w[1]-z*w[2]);
        const float n=SDL_sqrtf(orientation[0]*orientation[0]+orientation[1]*orientation[1]+
                                orientation[2]*orientation[2]+orientation[3]*orientation[3]);
        for (int i=0;i<4;++i) orientation[i]/=n;
    }
    orientation_us=now;
    memcpy(d->orientation,orientation,sizeof(orientation));
}

/* key.touch held: a finger on the touchpad, put down in the middle and dragged by the mouse
 * (in touchpad pixels, 1920x943). Released, the mouse goes back to the camera. */
static float touch_x, touch_y;
static int touch_down;
static void mouse_touch(PadData *d, int held, float *mdx, float *mdy) {
    if (!held) { touch_down=0; return; }
    if (!touch_down) { touch_down=1; touch_x=960; touch_y=471; }
    touch_x+=*mdx*input.touch_sensitivity; touch_y+=*mdy*input.touch_sensitivity;
    touch_x=touch_x<0 ? 0 : touch_x>1919 ? 1919 : touch_x;
    touch_y=touch_y<0 ? 0 : touch_y>942 ? 942 : touch_y;
    *mdx=*mdy=0; /* the camera stays put while the finger moves */
    if (d->touch_count<2) d->touches[d->touch_count++]=(PadTouch){.x=(uint16_t)touch_x,.y=(uint16_t)touch_y,.id=2};
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    memcpy(d->orientation,orientation,sizeof(orientation));
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    static int loaded;
    if (!loaded) { load_input(); loaded=1; }
    SDL_Gamepad *g=current_gamepad();
    float mdx=0, mdy=0;
    uint32_t mouse=0;
    int wheel=0, captured=twgpu_mouse_sample(&mdx,&mdy,&mouse,&wheel);
    if (twgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    /* Scripted runs (TW_PAD_FILE) ignore the player's controller and keyboard: a pad in use on
     * the same PC reached the test window (TW_PAD_HOST=1 keeps them). */
    static int scripted=-1;
    if (scripted<0) {
        const char *file=getenv("TW_PAD_FILE"), *host=getenv("TW_PAD_HOST");
        scripted=file && *file && !(host && host[0]=='1');
    }
    if (scripted) return;
    const uint64_t now=d->timestamp;
    if (wheel>0) wheel_until_us[0]=now+80000;
    if (wheel<0) wheel_until_us[1]=now+80000;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        for (int a=0;a<16;++a) {
            const int b=input.pad[a];
            if (b>=0 && SDL_GetGamepadButton(g,(SDL_GamepadButton)b)) d->buttons|=actions[a].ps;
            else if (b==PAD_TRIGGER_LEFT || b==PAD_TRIGGER_RIGHT) {
                const uint8_t v=trigger(SDL_GetGamepadAxis(g,b==PAD_TRIGGER_LEFT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                                              : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
                if (v>30) d->buttons|=actions[a].ps;
                if (actions[a].ps==BTN_L2 && v>d->l2) d->l2=v;
                if (actions[a].ps==BTN_R2 && v>d->r2) d->r2=v;
            }
        }
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_TOUCHPAD)) d->buttons|=BTN_TOUCHPAD; /* DualSense/DS4 click */
        if ((d->buttons & BTN_L2) && !d->l2) d->l2=255;
        if ((d->buttons & BTN_R2) && !d->r2) d->r2=255;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        if (input.pad_invert_y) d->right_y=(uint8_t)(255-d->right_y);
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    sample_motion(d,g,now);
    /* Keyboard and mouse, together with the controller. */
    for (int a=0;a<16;++a) if (action_down(a,k,mouse,now)) d->buttons|=actions[a].ps;
    mouse_touch(d,action_down(A_TOUCH,k,mouse,now),&mdx,&mdy);
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    // The touchpad button without a finger on a touch surface is a left-side click.
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    const int lx=action_down(A_LS_RIGHT,k,mouse,now)-action_down(A_LS_LEFT,k,mouse,now);
    const int ly=action_down(A_LS_DOWN,k,mouse,now)-action_down(A_LS_UP,k,mouse,now);
    if (lx) d->left_x=(uint8_t)(lx<0 ? 0 : 255);
    if (ly) d->left_y=(uint8_t)(ly<0 ? 0 : 255);
    const int rx=action_down(A_RS_RIGHT,k,mouse,now)-action_down(A_RS_LEFT,k,mouse,now);
    const int ry=action_down(A_RS_DOWN,k,mouse,now)-action_down(A_RS_UP,k,mouse,now);
    if (rx) d->right_x=(uint8_t)(rx<0 ? 0 : 255);
    if (ry) d->right_y=(uint8_t)(ry<0 ? 0 : 255);
    if (captured) {
        float dt=mouse_last_us ? (float)(now-mouse_last_us)*1e-6f : 0.0f;
        mouse_last_us=now;
        if (dt>0.0f) {
            if (dt>0.1f) dt=0.1f;
            /* turn rate in degrees/s, smoothed over 12 ms (mouse reports arrive in bursts) */
            const float alpha=1.0f-SDL_expf(-dt/0.012f), scale=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
            mouse_vx+=alpha*(mdx/dt*scale-mouse_vx);
            mouse_vy+=alpha*((input.mouse_invert_y ? -mdy : mdy)/dt*scale-mouse_vy);
            if (mdx==0 && mouse_vx>-1.0f && mouse_vx<1.0f) mouse_vx=0;
            if (mdy==0 && mouse_vy>-1.0f && mouse_vy<1.0f) mouse_vy=0;
        }
        if (mouse_vx!=0 && !rx) d->right_x=mouse_axis(mouse_vx);
        if (mouse_vy!=0 && !ry) d->right_y=mouse_axis(mouse_vy);
    } else {
        mouse_vx=mouse_vy=0; mouse_last_us=0;
    }
}

/* TW_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a TW_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("TW_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* TW_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d (t=%.4f)\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3],now*1e-6);
}
/* TW_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; TW_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in TW_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("TW_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("TW_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (twgpu_overlay_captures_input()) { hold_after_capture=1; return; }
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Light bar {r, g, b}: shown on controllers that have one (DualShock 4, DualSense). */
static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    pthread_mutex_lock(&lock);
    SDL_Gamepad *pad=current_gamepad();
    if (pad) SDL_SetGamepadLED(pad,r,g,b);
    pthread_mutex_unlock(&lock);
}
static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!rgb) return ERR_INVALID_ARG;
    set_led(rgb[0],rgb[1],rgb[2]);
    return 0;
}
static ABI int32_t pad_reset_light_bar(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    set_led(0,0,255); /* user 1 colour */
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_reset_orientation(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    pthread_mutex_lock(&lock);
    orientation[0]=orientation[1]=orientation[2]=0; orientation[3]=1;
    pthread_mutex_unlock(&lock);
    return 0;
}

/* ---- PlayStation Camera: never attached ---------------------------------------------------
 * Games that ask find no camera; the rest behaves as shadPS4's camera library does without one
 * (an open handle, then NOT_CONNECTED on start and frame reads), so camera features switch off
 * instead of the game stopping on an unresolved import. */
#define CAMERA_ERR_PARAM ((int32_t)0x802E0000)
#define CAMERA_ERR_NOT_CONNECTED ((int32_t)0x802E0010)
static ABI int32_t camera_is_attached(int32_t index) { return index ? CAMERA_ERR_PARAM : 0; }
static ABI int32_t camera_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)user; (void)type; (void)param;
    return index ? CAMERA_ERR_PARAM : 1;
}
static ABI int32_t camera_ok(void) { return 0; }
static ABI int32_t camera_not_connected(void) { return CAMERA_ERR_NOT_CONNECTED; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_reset_orientation},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_reset_light_bar},
    {"sceCameraIsAttached",camera_is_attached}, {"sceCameraOpen",camera_open},
    {"sceCameraClose",camera_ok}, {"sceCameraStop",camera_ok}, {"sceCameraSetConfig",camera_ok},
    {"sceCameraSetAttribute",camera_ok}, {"sceCameraSetVideoSync",camera_ok},
    {"sceCameraSetCalibData",camera_ok}, {"sceCameraSetLensCorrection",camera_ok},
    {"sceCameraStart",camera_not_connected}, {"sceCameraGetFrameData",camera_not_connected},
    {"sceCameraGetConfig",camera_not_connected}, {"sceCameraGetAttribute",camera_not_connected},
    {"sceCameraGetDeviceInfo",camera_not_connected}, {"sceCameraGetDeviceConfig",camera_not_connected},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
