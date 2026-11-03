// MinGW ships lower-case Windows SDK headers; JUCE includes the MSVC spelling. include_next skips
// this folder (on a case-insensitive disk <dbghelp.h> would otherwise find this file again).
#include_next <dbghelp.h>
