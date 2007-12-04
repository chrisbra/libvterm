#include "ecma48_internal.h"

#include <stdio.h>

void ecma48_state_setmode(ecma48_t *e48, ecma48_mode mode, int val)
{
  ecma48_state_t *state = e48->state;

  int done = 0;
  if(state->callbacks && state->callbacks->setmode)
    done = (*state->callbacks->setmode)(e48, mode, val);

  switch(mode) {
  case ECMA48_MODE_NONE:
    break;

  case ECMA48_MODE_KEYPAD:
    e48->mode.keypad = val;
    break;

  case ECMA48_MODE_DEC_CURSOR:
    e48->mode.cursor = val;
    break;

  case ECMA48_MODE_DEC_CURSORVISIBLE:
    e48->mode.cursor_visible = val;
    break;

  }
}