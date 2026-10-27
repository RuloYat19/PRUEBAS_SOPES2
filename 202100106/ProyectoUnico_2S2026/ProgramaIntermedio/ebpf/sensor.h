#ifndef SENSOR_H
#define SENSOR_H

#define SENSOR_RUTA_MAX		256
#define SENSOR_COMM_MAX		16
#define SENSOR_FLAGS_CREAT	01101

struct config_sensor {
	unsigned int largo;
	char ruta[SENSOR_RUTA_MAX];
};

struct evento_archivo {
	unsigned int pid;
	unsigned int flags;
	char proceso[SENSOR_COMM_MAX];
	char ruta[SENSOR_RUTA_MAX];
};

#endif
