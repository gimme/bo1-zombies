#pragma once
#include "windows.h"
#include <unistd.h>
BO1_EXTERN_C uintptr_t _beginthreadex(void *security, unsigned stack_size, unsigned(__stdcall *start_address)(void *), void *arglist,
    unsigned initflag, unsigned *thrdaddr);
BO1_EXTERN_C void _endthreadex(unsigned retval);
