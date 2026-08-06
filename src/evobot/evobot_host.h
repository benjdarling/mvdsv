#ifndef EVOBOT_HOST_H
#define EVOBOT_HOST_H

typedef void (*evobot_host_print_t)(const char *message);
typedef double (*evobot_host_server_time_t)(void);

typedef struct evobot_host_api_s
{
	evobot_host_print_t print;
	evobot_host_server_time_t server_time;
} evobot_host_api_t;

#endif
