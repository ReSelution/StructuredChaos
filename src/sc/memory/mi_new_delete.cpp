// Routes operator new/delete to mimalloc. Compiled into every executable that
// uses sc_dep, because a static mimalloc cannot override them on Windows.
#include <mimalloc-new-delete.h>
