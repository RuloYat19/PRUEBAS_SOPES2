#include <fcntl.h>
#include <linux/openat2.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "rutas.h"

int abrir_sin_enlaces(const char *ruta, int flags, mode_t modo)
{
	struct open_how how;

	memset(&how, 0, sizeof(how));
	how.flags = (unsigned int)flags;
	if (flags & (O_CREAT | O_TMPFILE))
		how.mode = modo;
	how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

	return (int)syscall(SYS_openat2, AT_FDCWD, ruta, &how, sizeof(how));
}
