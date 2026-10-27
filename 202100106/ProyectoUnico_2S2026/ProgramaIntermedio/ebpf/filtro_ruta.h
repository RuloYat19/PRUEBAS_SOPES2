#ifndef FILTRO_RUTA_H
#define FILTRO_RUTA_H

#include "sensor.h"

static inline int ruta_en_directorio(const char *ruta, const char *dir, unsigned int largo)
{
	unsigned int i;
	int encontrado_fin = 0;

	if (!ruta || !dir || largo == 0 || largo >= SENSOR_RUTA_MAX)
		return 0;

	for (i = 0; i < SENSOR_RUTA_MAX; i++) {
		if (i < largo) {
			if (ruta[i] != dir[i])
				return 0;
		} else {
			if (ruta[i] == '\0') {
				if (i == largo)
					return 0;
				encontrado_fin = 1;
				break;
			}
			if (ruta[i] == '/')
				return 0;
		}
	}

	return encontrado_fin;
}

#endif
