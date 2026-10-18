// SPDX-License-Identifier: GPL-2.0-or-later
// Tests for tools/bbmouse (tools/bbmouse/test.sh): the virtual gamepad the add-on presents, driven
// by a fake keyboard (bbgpu_split_keyboard below) and the shared mouse state.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
typedef struct { uint32_t magic, captured, buttons, wheel_up, wheel_down, reserved; int64_t dx_milli, dy_milli; } Shared;
static bool keys[512];
__attribute__((visibility("default"),used)) const bool *bbgpu_split_keyboard(void){ return keys; }
static int fails;
#define CHECK(c,msg) do{ int ok=!!(c); printf("%s  %s\n", ok?"PASS":"FAIL", msg); fails+=!ok; }while(0)
int main(void){
  int fd=open(getenv("BB_MOUSE_SHM"),O_RDWR|O_CREAT,0600); ftruncate(fd,4096);
  Shared *m=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  int n=0; SDL_JoystickID *ids=SDL_GetGamepads(&n);
  CHECK(n==1 && ids && ids[0]==0x7ffffff0, "virtual gamepad offered when no controller");
  SDL_Gamepad *g=SDL_OpenGamepad(ids[0]); SDL_free(ids);
  CHECK(g!=NULL, "virtual gamepad opens");
  keys[SDL_SCANCODE_X]=1; keys[SDL_SCANCODE_W]=1; keys[SDL_SCANCODE_ESCAPE]=1;
  m->captured=1; m->dx_milli=20000;
  CHECK(SDL_GamepadConnected(g), "connected");
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_SOUTH), "X = cross");
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_START), "Esc = options");
  CHECK(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY)==-32768, "W = stick up");
  usleep(16000); m->dx_milli=20000; SDL_GamepadConnected(g);
  Sint16 rx=SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX); printf("      mouse 20px right -> right stick %d\n",rx);
  CHECK(rx>3000, "mouse right turns camera right");
  for(int i=0;i<40;i++){ usleep(16000); SDL_GamepadConnected(g); }
  CHECK(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)==0 || SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)==-128, "camera stops when mouse stops");
  for(int i=0;i<512;i++) keys[i]=0;
  m->buttons=1; SDL_GamepadConnected(g); usleep(300000); SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)==32767, "hold left click = R2");
  CHECK(!SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), "hold is not also R1");
  m->buttons=0; SDL_GamepadConnected(g);
  CHECK(!SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), "releasing a hold gives no R1");
  usleep(200000);
  m->buttons=1; SDL_GamepadConnected(g); usleep(50000); m->buttons=0; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), "quick click = R1");
  CHECK(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)==0, "quick click is not R2");
  usleep(100000); SDL_GamepadConnected(g);
  CHECK(!SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), "R1 tap releases");
  m->buttons=2; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)==32767, "right click = L2");
  m->buttons=4; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_STICK) && !SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_DPAD_DOWN), "middle click = R3 lock on");
  m->buttons=0; m->wheel_up++; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_DPAD_RIGHT), "scroll up = D-pad right");
  m->wheel_down++; usleep(200000); SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_DPAD_LEFT), "scroll down = D-pad left");
  m->captured=0; m->buttons=1; usleep(400000); SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)==0, "mouse ignored when not captured");
  for(int i=0;i<512;i++) keys[i]=0; m->captured=1; m->buttons=0; usleep(300000); SDL_GamepadConnected(g);
  keys[SDL_SCANCODE_UP]=1; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_DPAD_UP), "arrow up = D-pad up (menus)");
  CHECK(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY)==0, "arrow up no longer moves the camera");
  keys[SDL_SCANCODE_UP]=0; keys[SDL_SCANCODE_LEFT]=1; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_DPAD_LEFT), "arrow left = D-pad left");
  keys[SDL_SCANCODE_LEFT]=0;
  keys[SDL_SCANCODE_W]=1; SDL_GamepadConnected(g); keys[SDL_SCANCODE_SPACE]=1; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_EAST) && !SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK), "Space while moving: sprint first");
  usleep(380000); SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_EAST) && SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK), "then L3 while sprinting = jump");
  usleep(150000); SDL_GamepadConnected(g);
  CHECK(!SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_EAST) && !SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK), "then both released");
  keys[SDL_SCANCODE_SPACE]=0; SDL_GamepadConnected(g); usleep(200000);
  keys[SDL_SCANCODE_C]=1; SDL_GamepadConnected(g); keys[SDL_SCANCODE_SPACE]=1; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK), "Space while holding C = instant jump");
  for(int i=0;i<512;i++) keys[i]=0; SDL_GamepadConnected(g); usleep(200000);
  keys[SDL_SCANCODE_W]=1; keys[SDL_SCANCODE_LSHIFT]=1; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_EAST), "Shift held = sprint (circle)");
  keys[SDL_SCANCODE_SPACE]=1; SDL_GamepadConnected(g);
  CHECK(SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK), "Shift + Space = instant jump");
  printf("%s\n", fails?"SOME TESTS FAILED":"ALL PASSED"); return fails;
}
